#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <queue>
#include <mutex>
#include <vector>
#include <iostream>
#include <algorithm>
#include <thread>
#include <atomic>
#include <optional>
#include <cstdlib>

#include "common.h"

using app::json;

struct Node
{
    std::string name;
    int capacity = 1;
    std::string type;
};

struct Edge
{
    std::string name;
    int capacity = 1;
    std::string type; // "carRoad|planeRoad"
    std::string node1;
    std::string node2;
};

struct VehiclePos
{
    enum class Kind { Node, Edge } kind = Kind::Node;

    std::string place; // node name or edge name
    std::string lastNode; // last known node (for edge transit)
};

struct FlightSession
{
    std::string flightId;
    std::string vehicleId;
    std::string parkingNode;

    bool landingApproved = false;
    bool landed = false;
    std::string status = "Created";
    int64_t createdAt = 0;
};

static std::string env_or(const char* key, const std::string& defVal) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : defVal;
}

static int env_or_int(const char* key, int defVal) {
    const char* v = std::getenv(key);
    if (!v || !*v) return defVal;
    try {
        return std::stoi(v);
    } catch (...) {
        return defVal;
    }
}

class GroundControl
{
public:
    GroundControl(std::string panelHost, int panelPort,
              std::string fmHost, int fmPort,
              std::string boardHost, int boardPort,
              std::string selfHost, int selfPort)
    : panelHost_(std::move(panelHost)),
      panelPort_(panelPort),
      fmHost_(std::move(fmHost)),
      fmPort_(fmPort),
      boardHost_(std::move(boardHost)),
      boardPort_(boardPort),
      selfHost_(std::move(selfHost)),
      selfPort_(selfPort)
    {
    }


    bool load_map(const std::string& filePath)
    {
        std::ifstream in(filePath);
        if (!in)
        {
            std::cerr << "[GroundControl] cannot open map file: " << filePath << '\n';
            return false;
        }

        json doc;
        in >> doc;

        if (!doc.contains("nodes") || !doc["nodes"].is_array() ||
            !doc.contains("edges") || !doc["edges"].is_array())
        {
            std::cerr << "[GroundControl] invalid map json\n";
            return false;
        }

        for (const auto& n : doc["nodes"])
        {
            Node node;
            node.name = n.at("name").get<std::string>();
            node.capacity = n.value("capacity", 1);
            node.type = n.value("type", "");
            nodes_[node.name] = node;
        }

        for (const auto& e : doc["edges"])
        {
            Edge edge;
            edge.name = e.at("name").get<std::string>();
            edge.capacity = e.value("capacity", 1);
            edge.type = e.value("type", "");
            edge.node1 = e.at("node1").get<std::string>();
            edge.node2 = e.at("node2").get<std::string>();
            edges_[edge.name] = edge;
            adjacency_[edge.node1].push_back(edge.name);
            adjacency_[edge.node2].push_back(edge.name);
        }

        return true;
    }

    void run(int port)
    {
        httplib::Server svr;

        svr.Get("/health", [&](const httplib::Request&, httplib::Response& res)
        {
            app::reply_json(res, 200, {
                                {"service", "GroundControl"},
                                {"status", "ok"},
                                {"time", app::now_sec()}
                            });
        });

        // Initial vehicle positions from FollowMe
        svr.Post("/v1/vehicles/init", [&](const httplib::Request& req, httplib::Response& res)
        {
            auto bodyOpt = app::parse_json_body(req);
            if (!bodyOpt)
            {
                app::reply_json(res, 400, {{"error", "invalid json"}});
                return;
            }
            const auto& body = *bodyOpt;
            if (!body.contains("vehicles") || !body["vehicles"].is_array())
            {
                app::reply_json(res, 400, {{"error", "'vehicles' array required"}});
                return;
            }

            json accepted = json::array();
            json rejected = json::array();

            std::lock_guard<std::mutex> lk(mtx_);
            for (const auto& v : body["vehicles"])
            {
                const std::string vid = app::s_or(v, "vehicleId");
                const std::string node = v.value("currentNode", std::string("FS-1"));
                if (vid.empty() || nodes_.find(node) == nodes_.end())
                {
                    rejected.push_back({{"vehicleId", vid}, {"reason", "bad_input"}});
                    continue;
                }

                if (!node_has_slot_unsafe(node))
                {
                    rejected.push_back({{"vehicleId", vid}, {"reason", "node_full"}, {"node", node}});
                    continue;
                }

                // remove old if existed
                if (vehiclePos_.count(vid))
                {
                    auto old = vehiclePos_[vid];
                    if (old.kind == VehiclePos::Kind::Node)
                    {
                        nodeOcc_[old.place].erase("vehicle:" + vid);
                    }
                    else
                    {
                        edgeOcc_[old.place].erase("vehicle:" + vid);
                    }
                }

                vehiclePos_[vid] = VehiclePos{VehiclePos::Kind::Node, node, node};
                nodeOcc_[node].insert("vehicle:" + vid);
                accepted.push_back({{"vehicleId", vid}, {"node", node}});
                push_event_unsafe("vehicle.init", {
                                      {"vehicleId", vid},
                                      {"node", node}
                                  });
            }

            app::reply_json(res, 200, {
                                {"ok", true},
                                {"accepted", accepted},
                                {"rejected", rejected}
                            });
        });

        // 1) plane asks landing permission
        svr.Get("/v1/land_permission", [&](const httplib::Request& req, httplib::Response& res)
        {
            if (!req.has_param("flightId"))
            {
                app::reply_json(res, 400, {{"error", "flightId query param is required"}});
                return;
            }
            const std::string flightId = req.get_param_value("flightId");

            // idempotency: if already approved -> return same
            {
                std::lock_guard<std::mutex> lk(mtx_);
                auto it = sessions_.find(flightId);
                if (it != sessions_.end() && it->second.landingApproved)
                {
                    app::reply_json(res, 200, {
                                        {"flightId", flightId},
                                        {"allowed", true},
                                        {"parking", it->second.parkingNode},
                                        {"vehicleId", it->second.vehicleId},
                                        {"reason", "already_approved"}
                                    });
                    return;
                }
            }

            // Check with InformationPanel: expected now/past? + phase must be airborne
            auto panelRes = app::http_get_json(
                panelHost_, panelPort_,
                "/v1/flights/" + flightId + "?ts=" + std::to_string(app::now_sec())
            );

            if (!panelRes.ok()) {
                notify_panel_status(flightId, "Denied", "panel_unreachable");
                app::reply_json(res, 200, {
                    {"flightId", flightId},
                    {"allowed", false},
                    {"retryAfterSec", 30},
                    {"reason", "panel_unreachable"}
                });
                return;
            }

            const bool expectedNowOrPast = panelRes.body.value("expectedNowOrPast", false);
            const std::string phase = panelRes.body.value("phase", std::string("airborne"));

            if (!expectedNowOrPast) {
                notify_panel_status(flightId, "Denied", "flight_not_expected");
                app::reply_json(res, 200, {
                    {"flightId", flightId},
                    {"allowed", false},
                    {"retryAfterSec", 30},
                    {"reason", "flight_not_expected"}
                });
                return;
            }

            if (phase != "airborne") {
                notify_panel_status(flightId, "Denied", "not_airborne");
                app::reply_json(res, 200, {
                    {"flightId", flightId},
                    {"allowed", false},
                    {"retryAfterSec", 30},
                    {"reason", "not_airborne"},
                    {"phase", phase}
                });
                return;
            }


            // Check free followme vehicle + reserve vehicle in FollowMe
            auto hasEmpty = app::http_get_json(fmHost_, fmPort_, "/v1/vehicles/hasEmpty");
            if (!hasEmpty.ok() || !hasEmpty.body.value("hasEmpty", false))
            {
                notify_panel_status(flightId, "Delayed", "no_followme_available");
                app::reply_json(res, 200, {
                                    {"flightId", flightId},
                                    {"allowed", false},
                                    {"retryAfterSec", 30},
                                    {"reason", "no_followme_available"}
                                });
                return;
            }

            auto reserveRes = app::http_post_json(fmHost_, fmPort_, "/v1/vehicles/reserve", {
                                                      {"flightId", flightId}
                                                  });
            if (!reserveRes.ok())
            {
                notify_panel_status(flightId, "Delayed", "followme_reservation_failed");
                app::reply_json(res, 200, {
                                    {"flightId", flightId},
                                    {"allowed", false},
                                    {"retryAfterSec", 30},
                                    {"reason", "followme_reservation_failed"}
                                });
                return;
            }
            const std::string vehicleId = reserveRes.body.value("vehicleId", "");

            // Reserve RE-1 + parking atomically
            bool reserved = false;
            std::string parking;
            {
                std::lock_guard<std::mutex> lk(mtx_);

                if (!node_has_slot_unsafe("RE-1"))
                {
                    reserved = false;
                }
                else
                {
                    parking = choose_free_parking_unsafe();
                    if (!parking.empty())
                    {
                        // reserve parking and RE-1
                        reservedParking_.insert(parking);
                        nodeOcc_["RE-1"].insert("reservation:" + flightId);

                        FlightSession s;
                        s.flightId = flightId;
                        s.vehicleId = vehicleId;
                        s.parkingNode = parking;
                        s.landingApproved = true;
                        s.status = "LandingApproved";
                        s.createdAt = app::now_sec();

                        sessions_[flightId] = s;
                        push_event_unsafe("flight.landing_approved", {
                                              {"flightId", flightId},
                                              {"vehicleId", vehicleId},
                                              {"parking", parking}
                                          });
                        reserved = true;
                    }
                }
            }

            if (!reserved)
            {
                // release reserved vehicle in FollowMe
                if (!vehicleId.empty())
                {
                    app::http_post_json(fmHost_, fmPort_, "/v1/vehicles/release", {
                                            {"vehicleId", vehicleId}
                                        });
                }
                notify_panel_status(flightId, "Delayed", "re1_or_parking_unavailable");
                app::reply_json(res, 200, {
                                    {"flightId", flightId},
                                    {"allowed", false},
                                    {"retryAfterSec", 30},
                                    {"reason", "re1_or_parking_unavailable"}
                                });
                return;
            }

            notify_panel_status(flightId, "LandingApproved", "all_resources_reserved");
            app::reply_json(res, 200, {
                                {"flightId", flightId},
                                {"allowed", true},
                                {"vehicleId", vehicleId},
                                {"parking", parking}
                            });
        });

        svr.Get("/v1/takeoff_permission", [&](const httplib::Request& req, httplib::Response& res)
        {
            if (!req.has_param("flightId"))
            {
                app::reply_json(res, 400, {{"error", "flightId query param is required"}});
                return;
            }
            const std::string flightId = req.get_param_value("flightId");

            // 1) спрашиваем панель (если у неё нет специального поля — просто не блокируем)
            auto panelRes = app::http_get_json(
                panelHost_, panelPort_,
                "/v1/flights/" + flightId + "?ts=" + std::to_string(app::now_sec())
            );

            if (panelRes.ok()) {
                const std::string phase = panelRes.body.value("phase", std::string("grounded"));

                if (phase != "grounded") {
                    app::reply_json(res, 200, {
                        {"flightId", flightId},
                        {"allowed", false},
                        {"retryAfterSec", 10},
                        {"reason", "not_grounded"},
                        {"phase", phase}
                    });
                    return;
                }

                if (panelRes.body.contains("takeoffAllowed") && panelRes.body["takeoffAllowed"].is_boolean()) {
                    if (!panelRes.body["takeoffAllowed"].get<bool>()) {
                        app::reply_json(res, 200, {
                            {"flightId", flightId},
                            {"allowed", false},
                            {"retryAfterSec", 10},
                            {"reason", "panel_denied_takeoff"}
                        });
                        return;
                    }
                }
            }

            std::string planeNode;

            {
                std::lock_guard<std::mutex> lk(mtx_);

                // 2) самолет должен быть на земле (на парковке P-*)
                auto pn = find_plane_node_unsafe(flightId);
                if (!pn)
                {
                    app::reply_json(res, 200, {
                                        {"flightId", flightId},
                                        {"allowed", false},
                                        {"retryAfterSec", 10},
                                        {"reason", "plane_not_on_ground"}
                                    });
                    return;
                }
                planeNode = *pn;

                // 3) RW-1 должен быть свободен для взлета (мы будем резервировать его)
                // idempotent: если уже зарезервировали под этот flight
                if (nodeOcc_[kTakeoffNode].count("takeoff_reservation:" + flightId) > 0)
                {
                    app::reply_json(res, 200, {
                                        {"flightId", flightId},
                                        {"allowed", true},
                                        {"reason", "already_reserved"},
                                        {"from", planeNode},
                                        {"runwayEntrance", kTakeoffNode}
                                    });
                    return;
                }

                if (!node_has_slot_unsafe(kTakeoffNode))
                {
                    app::reply_json(res, 200, {
                                        {"flightId", flightId},
                                        {"allowed", false},
                                        {"retryAfterSec", 10},
                                        {"reason", "re1_busy"}
                                    });
                    return;
                }

                // резервируем RW-1 на время “выруливания/взлета”
                nodeOcc_[kTakeoffNode].insert("takeoff_reservation:" + flightId);

                push_event_unsafe("flight.takeoff_approved", {
                                      {"flightId", flightId},
                                      {"from", planeNode},
                                      {"runwayEntrance", kTakeoffNode}
                                  });
            }

            notify_panel_status(flightId, "TakeoffApproved", "rw1_reserved_for_takeoff");
            app::reply_json(res, 200, {
                                {"flightId", flightId},
                                {"allowed", true},
                                {"from", planeNode},
                                {"runwayEntrance", kTakeoffNode}
                            });
        });


        // plane confirms landed to RE-1
        svr.Post(R"(/v1/flights/([A-Za-z0-9\-_]+)/landed)", [&](const httplib::Request& req, httplib::Response& res)
        {
            const std::string flightId = req.matches[1];

            std::lock_guard<std::mutex> lk(mtx_);
            auto it = sessions_.find(flightId);
            if (it == sessions_.end() || !it->second.landingApproved)
            {
                app::reply_json(res, 404, {{"error", "flight session not found or not approved"}});
                return;
            }

            // replace reservation in RE-1 by plane occupancy
            nodeOcc_[kLandingNode].erase("reservation:" + flightId);
            if (!node_has_plane_slot_unsafe_(kLandingNode))
            {
                app::reply_json(res, 409, {{"error", "RE-1 already has a plane"}});
                return;
            }

            nodeOcc_[kLandingNode].insert("plane:" + flightId);

            it->second.landed = true;
            it->second.status = "LandedAtRE1";
            push_event_unsafe("flight.landed_re1", {{"flightId", flightId}});

            app::reply_json(res, 200, {
                                {"ok", true},
                                {"flightId", flightId},
                                {"status", it->second.status}
                            });
        });

        svr.Post(R"(/v1/flights/([A-Za-z0-9\-_]+)/tookoff)", [&](const httplib::Request& req, httplib::Response& res) {
            const std::string flightId = req.matches[1];

            std::string fromNode;
            {
                std::lock_guard<std::mutex> lk(mtx_);

                auto pn = find_plane_node_unsafe(flightId);
                if (!pn) {
                    app::reply_json(res, 404, {{"error", "plane_not_found_on_ground"}});
                    return;
                }
                fromNode = *pn;

                nodeOcc_[fromNode].erase("plane:" + flightId);
                reservedParking_.erase(fromNode);
                nodeOcc_[kTakeoffNode].erase("takeoff_reservation:" + flightId);

                push_event_unsafe("flight.tookoff", {
                    {"flightId", flightId},
                    {"from", fromNode}
                });
            }

            notify_panel_status(flightId, "Departed", "tookoff");
            app::reply_json(res, 200, {{"ok", true}, {"flightId", flightId}, {"status", "Departed"}});
        });

        // followme asks mission path
        auto mission_path_handler = [&](const std::string& flightId, httplib::Response& res)
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = sessions_.find(flightId);
            if (it == sessions_.end())
            {
                app::reply_json(res, 404, {{"error", "session_not_found"}});
                return;
            }

            const auto& s = it->second;
            std::string vehicleNode = "FS-1";

            auto pIt = vehiclePos_.find(s.vehicleId);
            if (pIt != vehiclePos_.end())
            {
                if (pIt->second.kind == VehiclePos::Kind::Node)
                {
                    vehicleNode = pIt->second.place;
                }
                else
                {
                    vehicleNode = pIt->second.lastNode;
                }
            }

            auto routeToRunway = shortest_path_nodes_unsafe(vehicleNode, "RE-1", "carRoad");
            auto routeWithPlane = shortest_path_nodes_unsafe("RE-1", s.parkingNode, "planeRoad");
            auto routeReturn = shortest_path_nodes_unsafe(s.parkingNode, "FS-1", "carRoad");

            app::reply_json(res, 200, {
                                {"flightId", flightId},
                                {"vehicleId", s.vehicleId},
                                {"from", "RE-1"},
                                {"to", s.parkingNode},
                                {"routeToRunway", routeToRunway},
                                {"routeWithPlane", routeWithPlane},
                                {"routeReturn", routeReturn}
                            });
        };

        svr.Get("/v1/map/followme/path", [&](const httplib::Request& req, httplib::Response& res)
        {
            if (!req.has_param("flightId"))
            {
                app::reply_json(res, 400, {{"error", "flightId required"}});
                return;
            }
            mission_path_handler(req.get_param_value("flightId"), res);
        });

        // backward-compat from your draft:
        svr.Get(R"(/v1/map/followme/path/flightId=([A-Za-z0-9\-_]+))",
                [&](const httplib::Request& req, httplib::Response& res)
                {
                    mission_path_handler(req.matches[1], res);
                });

        auto permission_handler = [&](const std::string& flightId, httplib::Response& res)
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = sessions_.find(flightId);
            if (it == sessions_.end())
            {
                app::reply_json(res, 404, {{"error", "session_not_found"}});
                return;
            }
            app::reply_json(res, 200, {
                                {"flightId", flightId},
                                {"allowed", it->second.landed}
                            });
        };

        svr.Get("/v1/map/followme/permission", [&](const httplib::Request& req, httplib::Response& res)
        {
            if (!req.has_param("flightId"))
            {
                app::reply_json(res, 400, {{"error", "flightId required"}});
                return;
            }
            permission_handler(req.get_param_value("flightId"), res);
        });

        svr.Get(R"(/v1/map/followme/permission/flightId=([A-Za-z0-9\-_]+))",
                [&](const httplib::Request& req, httplib::Response& res)
                {
                    permission_handler(req.matches[1], res);
                });

        // FollowMe asks to enter edge (atomic check edge capacity + current position)
        svr.Post("/v1/map/traffic/enter-edge", [&](const httplib::Request& req, httplib::Response& res)
        {
            auto bodyOpt = app::parse_json_body(req);
            if (!bodyOpt)
            {
                app::reply_json(res, 400, {{"error", "invalid json"}});
                return;
            }
            const auto& body = *bodyOpt;

            const std::string vehicleId = app::s_or(body, "vehicleId");
            const std::string from = app::s_or(body, "from");
            const std::string to = app::s_or(body, "to");

            if (vehicleId.empty() || from.empty() || to.empty())
            {
                app::reply_json(res, 400, {{"error", "vehicleId, from, to required"}});
                return;
            }

            std::lock_guard<std::mutex> lk(mtx_);

            // auto-register if absent
            if (!vehiclePos_.count(vehicleId))
            {
                if (nodes_.count(from) == 0 || !node_has_slot_unsafe(from))
                {
                    app::reply_json(res, 409, {
                                        {"granted", false},
                                        {"reason", "cannot_register_vehicle_on_from_node"}
                                    });
                    return;
                }
                vehiclePos_[vehicleId] = VehiclePos{VehiclePos::Kind::Node, from, from};
                nodeOcc_[from].insert("vehicle:" + vehicleId);
            }

            auto& pos = vehiclePos_[vehicleId];

            // idempotent case: already on edge from->to
            if (pos.kind == VehiclePos::Kind::Edge)
            {
                const auto& e = edges_.at(pos.place);
                const bool same = ((e.node1 == from && e.node2 == to) || (e.node1 == to && e.node2 == from));
                if (same)
                {
                    app::reply_json(res, 200, {{"granted", true}, {"edge", e.name}, {"idempotent", true}});
                    return;
                }
            }

            if (pos.kind != VehiclePos::Kind::Node || pos.place != from)
            {
                app::reply_json(res, 409, {
                                    {"granted", false},
                                    {"reason", "vehicle_not_on_from_node"},
                                    {"current", pos.place}
                                });
                return;
            }

            auto edgeOpt = find_edge_between_unsafe(from, to, "carRoad");
            if (!edgeOpt)
            {
                app::reply_json(res, 404, {{"granted", false}, {"reason", "edge_not_found"}});
                return;
            }
            const std::string edgeName = *edgeOpt;

            if (!edge_has_slot_unsafe(edgeName))
            {
                app::reply_json(res, 409, {
                                    {"granted", false},
                                    {"reason", "edge_busy"},
                                    {"edge", edgeName}
                                });
                return;
            }

            nodeOcc_[from].erase("vehicle:" + vehicleId);
            edgeOcc_[edgeName].insert("vehicle:" + vehicleId);

            pos.kind = VehiclePos::Kind::Edge;
            pos.place = edgeName;
            pos.lastNode = from;

            push_event_unsafe("vehicle.enter_edge", {
                                  {"vehicleId", vehicleId},
                                  {"from", from},
                                  {"to", to},
                                  {"edge", edgeName}
                              });

            app::reply_json(res, 200, {
                                {"granted", true},
                                {"edge", edgeName}
                            });
        });

        // FollowMe asks to leave edge to node
        svr.Post("/v1/map/traffic/leave-edge", [&](const httplib::Request& req, httplib::Response& res)
        {
            auto bodyOpt = app::parse_json_body(req);
            if (!bodyOpt)
            {
                app::reply_json(res, 400, {{"error", "invalid json"}});
                return;
            }
            const auto& body = *bodyOpt;

            const std::string vehicleId = app::s_or(body, "vehicleId");
            const std::string to = app::s_or(body, "to");
            if (vehicleId.empty() || to.empty())
            {
                app::reply_json(res, 400, {{"error", "vehicleId, to required"}});
                return;
            }

            std::lock_guard<std::mutex> lk(mtx_);
            auto it = vehiclePos_.find(vehicleId);
            if (it == vehiclePos_.end())
            {
                app::reply_json(res, 404, {{"granted", false}, {"reason", "vehicle_not_found"}});
                return;
            }

            auto& pos = it->second;

            // idempotent: already in node to
            if (pos.kind == VehiclePos::Kind::Node && pos.place == to)
            {
                app::reply_json(res, 200, {{"granted", true}, {"idempotent", true}});
                return;
            }

            if (pos.kind != VehiclePos::Kind::Edge)
            {
                app::reply_json(res, 409, {{"granted", false}, {"reason", "vehicle_not_on_edge"}});
                return;
            }

            const std::string edgeName = pos.place;
            const auto& e = edges_.at(edgeName);

            const bool connected = (e.node1 == to || e.node2 == to);
            if (!connected)
            {
                app::reply_json(res, 409, {
                                    {"granted", false},
                                    {"reason", "target_node_not_connected_to_current_edge"},
                                    {"edge", edgeName}
                                });
                return;
            }

            bool canEnterTarget = node_has_slot_unsafe(to);

            if (!canEnterTarget)
            {
                const std::string flightId = app::s_or(body, "flightId");

                // Спец-правило: FollowMe может "доковаться" в RE-1 к своему самолету
                if (to == "RE-1" && !flightId.empty())
                {
                    const auto& occ = nodeOcc_["RE-1"];
                    if (occ.count("plane:" + flightId) > 0)
                    {
                        canEnterTarget = true;
                    }
                }
            }

            if (!canEnterTarget)
            {
                app::reply_json(res, 409, {
                                    {"granted", false},
                                    {"reason", "target_node_busy"},
                                    {"node", to}
                                });
                return;
            }

            edgeOcc_[edgeName].erase("vehicle:" + vehicleId);
            nodeOcc_[to].insert("vehicle:" + vehicleId);

            pos.kind = VehiclePos::Kind::Node;
            pos.place = to;
            pos.lastNode = to;

            push_event_unsafe("vehicle.leave_edge", {
                                  {"vehicleId", vehicleId},
                                  {"to", to},
                                  {"edge", edgeName}
                              });

            app::reply_json(res, 200, {{"granted", true}});
        });

        // FollowMe informs: arrived parking with plane
        svr.Post("/v1/vehicles/followme", [&](const httplib::Request& req, httplib::Response& res)
        {
            auto bodyOpt = app::parse_json_body(req);
            if (!bodyOpt)
            {
                app::reply_json(res, 400, {{"error", "invalid json"}});
                return;
            }
            const auto& body = *bodyOpt;

            const std::string flightId = app::s_or(body, "flightId");
            const std::string status = app::s_or(body, "status");
            if (flightId.empty() || status.empty())
            {
                app::reply_json(res, 400, {{"error", "flightId and status required"}});
                return;
            }

            if (status != "arrivedParking")
            {
                app::reply_json(res, 400, {{"error", "unsupported status"}});
                return;
            }

            std::string parking;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                auto it = sessions_.find(flightId);
                if (it == sessions_.end())
                {
                    app::reply_json(res, 404, {{"error", "session_not_found"}});
                    return;
                }

                parking = it->second.parkingNode;
                reservedParking_.erase(parking);

                // sanity
                if (!nodes_.count(parking)) {
                    push_event_unsafe("flight.arrived_parking_error", {
                        {"flightId", flightId},
                        {"parking", parking},
                        {"reason", "unknown_parking_node"}
                    });
                    app::reply_json(res, 409, {{"error", "unknown parking node"}, {"parking", parking}});
                    return;
                }

                // самолёт должен отобразиться на стойке независимо от того, что FollowMe ещё там
                if (!node_has_plane_slot_unsafe_(parking) && nodeOcc_[parking].count("plane:" + flightId) == 0) {
                    // редкий конфликт: на стойке уже другой самолёт
                    push_event_unsafe("flight.arrived_parking_conflict", {
                        {"flightId", flightId},
                        {"parking", parking},
                        {"reason", "parking_already_has_plane"}
                    });
                    // можно решить по-разному; безопасно — не двигать
                    app::reply_json(res, 409, {{"error", "parking already has a plane"}, {"parking", parking}});
                    return;
                }

                place_plane_unsafe_(flightId, parking);

                it->second.status = "ArrivedParking";
                push_event_unsafe("flight.arrived_parking", {
                    {"flightId", flightId},
                    {"parking", parking}
                });
            }

            notify_panel_status(flightId, "ArrivedParking", "plane_on_stand", parking);
            app::reply_json(res, 200, {
                                {"ok", true},
                                {"flightId", flightId},
                                {"parking", parking}
                            });
        });

        // FollowMe mission completed and vehicle returned
        svr.Post("/v1/followme/mission/completed", [&](const httplib::Request& req, httplib::Response& res)
        {
            auto bodyOpt = app::parse_json_body(req);
            if (!bodyOpt)
            {
                app::reply_json(res, 400, {{"error", "invalid json"}});
                return;
            }
            const auto& body = *bodyOpt;
            const std::string flightId = app::s_or(body, "flightId");
            const std::string vehicleId = app::s_or(body, "vehicleId");

            if (flightId.empty() || vehicleId.empty())
            {
                app::reply_json(res, 400, {{"error", "flightId and vehicleId required"}});
                return;
            }

            {
                std::lock_guard<std::mutex> lk(mtx_);
                auto it = sessions_.find(flightId);
                if (it != sessions_.end())
                {
                    it->second.status = "MissionCompleted";
                }
                push_event_unsafe("mission.completed", {
                                      {"flightId", flightId},
                                      {"vehicleId", vehicleId}
                                  });
            }

            notify_panel_status(flightId, "Parked", "followme_returned_to_base");
            app::reply_json(res, 200, {{"ok", true}});
        });

        // Visualizer snapshot API
        svr.Get("/v1/visualizer/snapshot", [&](const httplib::Request&, httplib::Response& res)
        {
            std::lock_guard<std::mutex> lk(mtx_);

            json nodes = json::array();
            for (const auto& [name, n] : nodes_)
            {
                json occ = json::array();
                if (nodeOcc_.count(name))
                {
                    for (const auto& x : nodeOcc_[name]) occ.push_back(x);
                }
                nodes.push_back({
                    {"name", n.name},
                    {"type", n.type},
                    {"capacity", n.capacity},
                    {"occupiedBy", occ}
                });
            }

            json edges = json::array();
            for (const auto& [name, e] : edges_)
            {
                json occ = json::array();
                if (edgeOcc_.count(name))
                {
                    for (const auto& x : edgeOcc_[name]) occ.push_back(x);
                }
                edges.push_back({
                    {"name", e.name},
                    {"type", e.type},
                    {"capacity", e.capacity},
                    {"node1", e.node1},
                    {"node2", e.node2},
                    {"occupiedBy", occ}
                });
            }

            json sessions = json::array();
            for (const auto& [id, s] : sessions_)
            {
                sessions.push_back({
                    {"flightId", s.flightId},
                    {"vehicleId", s.vehicleId},
                    {"parkingNode", s.parkingNode},
                    {"landingApproved", s.landingApproved},
                    {"landed", s.landed},
                    {"status", s.status},
                    {"createdAt", s.createdAt}
                });
            }

            app::reply_json(res, 200, {
                                {"time", app::now_sec()},
                                {"seq", seq_},
                                {"nodes", nodes},
                                {"edges", edges},
                                {"sessions", sessions}
                            });
        });

        // Visualizer incremental events
        svr.Get("/v1/visualizer/events", [&](const httplib::Request& req, httplib::Response& res)
        {
            uint64_t since = 0;
            if (req.has_param("since"))
            {
                try
                {
                    since = static_cast<uint64_t>(std::stoull(req.get_param_value("since")));
                }
                catch (...)
                {
                    since = 0;
                }
            }

            std::lock_guard<std::mutex> lk(mtx_);
            json out = json::array();
            for (const auto& ev : events_)
            {
                if (ev["seq"].get<uint64_t>() > since)
                {
                    out.push_back(ev);
                }
            }

            app::reply_json(res, 200, {
                                {"events", out},
                                {"latestSeq", seq_}
                            });
        });

        std::cout << "[GroundControl] listening on 0.0.0.0:" << port << '\n';
        std::cout << "[GroundControl] panel: " << panelHost_ << ":" << panelPort_
            << ", followme: " << fmHost_ << ":" << fmPort_ << '\n';
        start_panel_sync_thread_();
        svr.listen("0.0.0.0", port);
    }

private:
    bool edge_allows_mode(const Edge& e, const std::string& mode) const
    {
        return e.type.find(mode) != std::string::npos;
    }

    bool node_has_slot_unsafe(const std::string& node)
    {
        auto nIt = nodes_.find(node);
        if (nIt == nodes_.end()) return false;
        const int cap = nIt->second.capacity;
        const int occ = node_occ_for_capacity_unsafe_(node);
        return occ < cap;
    }

    bool edge_has_slot_unsafe(const std::string& edge)
    {
        auto eIt = edges_.find(edge);
        if (eIt == edges_.end()) return false;
        const int cap = eIt->second.capacity;
        const int occ = static_cast<int>(edgeOcc_[edge].size());
        return occ < cap;
    }

    std::optional<std::string> find_edge_between_unsafe(
        const std::string& a,
        const std::string& b,
        const std::string& mode
    )
    {
        if (!adjacency_.count(a)) return std::nullopt;
        for (const auto& edgeName : adjacency_[a])
        {
            const auto& e = edges_.at(edgeName);
            const bool match = (e.node1 == a && e.node2 == b) || (e.node1 == b && e.node2 == a);
            if (match && edge_allows_mode(e, mode))
            {
                return edgeName;
            }
        }
        return std::nullopt;
    }

    std::vector<std::string> shortest_path_nodes_unsafe(
        const std::string& start,
        const std::string& goal,
        const std::string& mode
    )
    {
        if (nodes_.count(start) == 0 || nodes_.count(goal) == 0)
        {
            return {};
        }
        if (start == goal)
        {
            return {start};
        }

        std::queue<std::string> q;
        std::unordered_map<std::string, std::string> prev;
        std::unordered_set<std::string> vis;

        q.push(start);
        vis.insert(start);

        bool found = false;

        while (!q.empty() && !found)
        {
            auto cur = q.front();
            q.pop();

            if (!adjacency_.count(cur)) continue;

            for (const auto& edgeName : adjacency_[cur])
            {
                const auto& e = edges_.at(edgeName);
                if (!edge_allows_mode(e, mode)) continue;

                const std::string next = (e.node1 == cur) ? e.node2 : e.node1;
                if (!vis.count(next))
                {
                    vis.insert(next);
                    prev[next] = cur;
                    if (next == goal)
                    {
                        found = true;
                        break;
                    }
                    q.push(next);
                }
            }
        }

        if (!found)
        {
            return {};
        }

        std::vector<std::string> path;
        for (std::string at = goal; !at.empty();)
        {
            path.push_back(at);
            auto it = prev.find(at);
            if (it == prev.end()) break;
            at = it->second;
        }
        std::reverse(path.begin(), path.end());
        return path;
    }

    std::string choose_free_parking_unsafe()
    {
        std::string best;
        size_t bestLen = std::numeric_limits<size_t>::max();

        for (const auto& [name, n] : nodes_)
        {
            if (n.type != "planeParking") continue;
            if (reservedParking_.count(name)) continue;

            // ВАЖНО: стойка должна быть свободна от самолёта
            if (!node_has_plane_slot_unsafe_(name)) continue;

            // и не должна быть забита “не самолётными” объектами (vehicle/reservation)
            if (!node_has_slot_unsafe(name)) continue;

            auto path = shortest_path_nodes_unsafe(kLandingNode, name, "planeRoad");
            if (path.empty()) continue;

            if (path.size() < bestLen)
            {
                bestLen = path.size();
                best = name;
            }
        }
        return best;
    }

    void push_event_unsafe(const std::string& type, const json& payload)
    {
        ++seq_;
        events_.push_back({
            {"seq", seq_},
            {"ts", app::now_sec()},
            {"type", type},
            {"payload", payload}
        });

        // simple retention
        if (events_.size() > 10000)
        {
            events_.erase(events_.begin(), events_.begin() + 2000);
        }
    }

    void notify_panel_status(const std::string& flightId,
                             const std::string& status,
                             const std::string& reason,
                             const std::string& parkingNode = "")
    {
        json body = {
            {"flightId", flightId},
            {"status", status},
            {"reason", reason}
        };
        if (!parkingNode.empty()) body["parkingNode"] = parkingNode;

        (void)app::http_post_json(panelHost_, panelPort_, "/v1/flights/status", body);
    }

    std::optional<std::string> find_plane_node_unsafe(const std::string& flightId)
    {
        const std::string token = "plane:" + flightId;
        for (const auto& [node, occ] : nodeOcc_)
        {
            if (occ.count(token) > 0) return node;
        }
        return std::nullopt;
    }

    static std::string get_phase_from_panel_item(const json& f) {
        if (f.contains("phase") && f["phase"].is_string()) {
            return f["phase"].get<std::string>();
        }
        // fallback на случай старых данных: infer по статусу
        const std::string status = f.value("status", std::string(""));
        if (status == "Departed" || status == "Cancelled" || status == "Denied") return "terminal";
        if (status == "ArrivedParking" || status == "Parked" || status == "OnStand") return "grounded";
        return "airborne";
    }

    bool is_plane_parking_node_unsafe(const std::string& node) const {
        auto it = nodes_.find(node);
        if (it == nodes_.end()) return false;
        return it->second.type == "planeParking";
    }

///////////////////////////////////////////////////////////////////////////////
    static std::string board_key_(const std::string& flightId, const std::string& kind) {
        return flightId + "|" + kind;
    }

    bool can_try_board_now_unsafe_(const std::string& key) {
        const int64_t now = app::now_sec();
        auto it = boardRetryAfter_.find(key);
        return (it == boardRetryAfter_.end() || now >= it->second);
    }

    void board_fail_unsafe_(const std::string& key,
                            const std::string& flightId,
                            const std::string& kind,
                            const std::string& reason)
    {
        const int64_t now = app::now_sec();
        int& att = boardAttempt_[key];
        att = std::min(att + 1, 10);

        int delay = 1 << std::min(att, 5); // 1,2,4,8,16,32
        delay = std::min(delay, 30);

        boardRetryAfter_[key] = now + delay;

        push_event_unsafe("board.action_failed", {
            {"flightId", flightId},
            {"kind", kind},
            {"reason", reason},
            {"retryInSec", delay}
        });
    }

    void board_ok_unsafe_(const std::string& key,
                          const std::string& flightId,
                          const std::string& kind)
    {
        boardAttempt_.erase(key);
        boardRetryAfter_.erase(key);

        push_event_unsafe("board.action_ok", {
            {"flightId", flightId},
            {"kind", kind}
        });
    }

    // ACK wrappers (важно: возвращают true только если Board ответил ok=true)
    bool spawn_board_airborne_ack_(const std::string& flightId) {
        auto r = app::http_post_json(boardHost_, boardPort_, "/v1/planes/airborne", {
            {"flightId", flightId},
            {"gcHost", selfHost_},
            {"gcPort", selfPort_},
            {"pollSec", 5},
            {"touchdownDelaySec", 2}
        });
        return r.ok() && r.body.value("ok", false);
    }

    bool spawn_board_grounded_ack_(const std::string& flightId, const std::string& parkingNode) {
        auto r = app::http_post_json(boardHost_, boardPort_, "/v1/planes/grounded", {
            {"flightId", flightId},
            {"parkingNode", parkingNode},
            {"gcHost", selfHost_},
            {"gcPort", selfPort_},
            {"pollSec", 3},
            {"handlingSec", 10},
            {"takeoffDelaySec", 2}
        });
        return r.ok() && r.body.value("ok", false);
    }

    bool stop_board_ack_(const std::string& flightId) {
        auto r = app::http_post_json(boardHost_, boardPort_, "/v1/planes/stop", {
            {"flightId", flightId}
        });
        return r.ok() && r.body.value("ok", false);
    }

///////////////////////////////////////////////////////////////////////////////
    void start_panel_sync_thread_()
    {
        panelSyncStop_.store(false);
        panelInitDone_.store(false);
        boardInitDone_.store(false);

        panelSyncThread_ = std::thread([this]()
        {
            while (!panelSyncStop_.load() && !boardInitDone_.load())
            {
                try
                {
                    if (!panelInitDone_.load())
                    {
                        // Фаза 1: только до первого успешного непустого списка
                        if (sync_from_panel_once_()) {
                            panelInitDone_.store(true);
                        }
                        std::this_thread::sleep_for(std::chrono::seconds(panelSyncPeriodSec_));
                    }
                    else
                    {
                        // Фаза 2: НЕ ходим в Panel, но ретраим Board до ACK по всем
                        if (retry_board_init_once_()) {
                            boardInitDone_.store(true);
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::seconds(1));
                    }
                }
                catch (...)
                {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }
            }
        });

        panelSyncThread_.detach();
    }


    bool sync_from_panel_once_() {
        const std::string path = panelFlightsListPath_ + "?ts=" + std::to_string(app::now_sec());
        auto r = app::http_get_json(panelHost_, panelPort_, path);
        if (!r.ok()) return false;

        json flightsJson;
        if (r.body.is_object() && r.body.contains("flights") && r.body["flights"].is_array()) {
            flightsJson = r.body["flights"];
        } else if (r.body.is_array()) {
            flightsJson = r.body;
        } else {
            return false;
        }

        // “до первого успешного получения самолетов” — проверяем, что есть хотя бы один
        if (flightsJson.empty()) {
            return false;
        }

        struct Action {
            std::string kind;      // "airborne" | "grounded" | "stop"
            std::string flightId;
            std::string parkingNode;
        };
        std::vector<Action> actions;

        {
            std::lock_guard<std::mutex> lk(mtx_);

            for (const auto& f : flightsJson) {
                const std::string flightId = app::s_or(f, "flightId");
                if (flightId.empty()) continue;

                const std::string phase = get_phase_from_panel_item(f);
                const std::string status = f.value("status", std::string(""));
                const std::string parkingNode = f.value("parkingNode", std::string(""));
                const bool expectedNowOrPast = f.value("expectedNowOrPast", true);

                // ---- TERMINAL ----
                if (phase == "terminal") {
                    // 1) убрать самолёт с карты GC (это локальная чистка, можно делать сразу)
                    auto pn = find_plane_node_unsafe(flightId);
                    if (pn) {
                        nodeOcc_[*pn].erase("plane:" + flightId);
                        push_event_unsafe("plane.removed_by_panel", {{"flightId", flightId}, {"node", *pn}});
                    }

                    // 2) если Board-агент был — стопаем, но spawnedInBoard_ НЕ трогаем до ACK
                    if (spawnedInBoard_.count(flightId)) {
                        const std::string key = board_key_(flightId, "stop");
                        if (can_try_board_now_unsafe_(key)) {
                            actions.push_back({"stop", flightId, ""});
                        }
                    }
                    continue;
                }

                // ---- GROUNDED ----
                if (phase == "grounded") {
                    std::string stand = parkingNode;

                    if (stand.empty()) {
                        auto pn = find_plane_node_unsafe(flightId);
                        if (pn) stand = *pn;
                    }

                    if (stand.empty()) {
                        push_event_unsafe("plane.grounded_missing_parking", {
                            {"flightId", flightId},
                            {"status", status},
                            {"reason", "panel_phase_grounded_but_no_parkingNode"}
                        });
                        continue;
                    }

                    if (!nodes_.count(stand)) {
                        push_event_unsafe("plane.grounded_bad_parking", {
                            {"flightId", flightId},
                            {"parkingNode", stand},
                            {"reason", "unknown_node"}
                        });
                        continue;
                    }

                    if (!is_plane_parking_node_unsafe(stand)) {
                        push_event_unsafe("plane.grounded_not_parking_type", {
                            {"flightId", flightId},
                            {"node", stand},
                            {"nodeType", nodes_.at(stand).type}
                        });
                    }

                    // перенос plane:* если нужно
                    auto cur = find_plane_node_unsafe(flightId);
                    if (cur && *cur != stand) {
                        nodeOcc_[*cur].erase("plane:" + flightId);
                        push_event_unsafe("plane.relocated_by_panel", {
                            {"flightId", flightId},
                            {"from", *cur},
                            {"to", stand}
                        });
                    }

                    // ставим plane:* на stand
                    bool placed = (nodeOcc_[stand].count("plane:" + flightId) > 0);
                    if (!placed) {
                        if (!node_has_slot_unsafe(stand)) {
                            push_event_unsafe("plane.grounded_place_blocked", {
                                {"flightId", flightId},
                                {"node", stand},
                                {"reason", "node_full"}
                            });
                            continue;
                        }
                        nodeOcc_[stand].insert("plane:" + flightId);
                        push_event_unsafe("plane.init_from_panel", {{"flightId", flightId}, {"node", stand}});
                        placed = true;
                    }

                    desiredInBoard_[flightId] = DesiredPlane{"grounded", stand};

                    // ВАЖНО: НЕ помечаем spawnedInBoard_ заранее. Только планируем action.
                    if (placed) {
                        const std::string curKind = spawnedInBoard_.count(flightId) ? spawnedInBoard_[flightId] : "";
                        if (curKind != "grounded") {
                            const std::string key = board_key_(flightId, "grounded");
                            if (can_try_board_now_unsafe_(key)) {
                                actions.push_back({"grounded", flightId, stand});
                            }
                        }
                    }
                    continue;
                }

                // ---- AIRBORNE ----
                if (phase == "airborne") {
                    if (!expectedNowOrPast) {
                        continue;
                    }

                    desiredInBoard_[flightId] = DesiredPlane{"airborne", ""};
                    const std::string curKind = spawnedInBoard_.count(flightId) ? spawnedInBoard_[flightId] : "";
                    if (curKind != "airborne") {
                        const std::string key = board_key_(flightId, "airborne");
                        if (can_try_board_now_unsafe_(key)) {
                            actions.push_back({"airborne", flightId, ""});
                        }
                    }
                    continue;
                }

                // unknown phase -> ignore
            }
        }

        // HTTP снаружи mutex + обновление состояния ТОЛЬКО ПОСЛЕ ACK
        for (const auto& a : actions) {
            bool ok = false;

            if (a.kind == "stop") {
                ok = stop_board_ack_(a.flightId);
                std::lock_guard<std::mutex> lk(mtx_);
                const std::string key = board_key_(a.flightId, "stop");
                if (ok) {
                    spawnedInBoard_.erase(a.flightId);
                    board_ok_unsafe_(key, a.flightId, "stop");
                } else {
                    board_fail_unsafe_(key, a.flightId, "stop", "http_or_ok_false");
                }
                continue;
            }

            if (a.kind == "airborne") {
                ok = spawn_board_airborne_ack_(a.flightId);
            } else {
                ok = spawn_board_grounded_ack_(a.flightId, a.parkingNode);
            }

            std::lock_guard<std::mutex> lk(mtx_);
            const std::string key = board_key_(a.flightId, a.kind);
            if (ok) {
                spawnedInBoard_[a.flightId] = a.kind; // <-- вот тут ставим “создан”
                board_ok_unsafe_(key, a.flightId, a.kind);
            } else {
                board_fail_unsafe_(key, a.flightId, a.kind, "http_or_ok_false");
            }
        }

        return true;
    }

    bool retry_board_init_once_()
    {
        struct Action {
            std::string kind;      // "airborne" | "grounded"
            std::string flightId;
            std::string parkingNode;
        };
        std::vector<Action> actions;

        {
            std::lock_guard<std::mutex> lk(mtx_);

            for (const auto& [flightId, d] : desiredInBoard_) {
                const std::string curKind = spawnedInBoard_.count(flightId) ? spawnedInBoard_[flightId] : "";
                if (curKind == d.kind) continue;

                const std::string key = board_key_(flightId, d.kind);
                if (!can_try_board_now_unsafe_(key)) continue;

                actions.push_back({d.kind, flightId, d.parkingNode});
            }
        }

        // HTTP снаружи mutex + update по ACK
        for (const auto& a : actions) {
            bool ok = false;

            if (a.kind == "airborne") ok = spawn_board_airborne_ack_(a.flightId);
            else                      ok = spawn_board_grounded_ack_(a.flightId, a.parkingNode);

            std::lock_guard<std::mutex> lk(mtx_);
            const std::string key = board_key_(a.flightId, a.kind);

            if (ok) {
                spawnedInBoard_[a.flightId] = a.kind;
                board_ok_unsafe_(key, a.flightId, a.kind);
            } else {
                board_fail_unsafe_(key, a.flightId, a.kind, "http_or_ok_false");
            }
        }

        // Проверяем: всё ли подтверждено
        {
            std::lock_guard<std::mutex> lk(mtx_);
            for (const auto& [flightId, d] : desiredInBoard_) {
                auto it = spawnedInBoard_.find(flightId);
                if (it == spawnedInBoard_.end() || it->second != d.kind) return false;
            }
        }
        return true;
    }

    static constexpr const char* kLandingNode = "RE-1";
    static constexpr const char* kTakeoffNode = "RW-1";

    static bool has_prefix_(const std::string& s, const char* p) {
        return s.rfind(p, 0) == 0;
    }

    // Для capacity на planeParking не считаем plane:* (чтобы vehicle мог стоять вместе с самолётом)
    int node_occ_for_capacity_unsafe_(const std::string& node) {
        auto nIt = nodes_.find(node);
        if (nIt == nodes_.end()) return 0;

        const auto& occ = nodeOcc_[node];

        if (nIt->second.type == "planeParking") {
            int cnt = 0;
            for (const auto& t : occ) {
                if (!has_prefix_(t, "plane:")) cnt++;
            }
            return cnt;
        }

        return static_cast<int>(occ.size());
    }

    // “Самолётный слот”: по умолчанию 1 plane:* на узел
    bool node_has_plane_slot_unsafe_(const std::string& node) {
        int planes = 0;
        for (const auto& t : nodeOcc_[node]) {
            if (has_prefix_(t, "plane:")) planes++;
        }
        return planes < 1;
    }

    // Надёжно перемещаем plane:* (не зависит от того, где он сейчас)
    void place_plane_unsafe_(const std::string& flightId, const std::string& targetNode) {
        const std::string token = "plane:" + flightId;

        // убираем “plane:flightId” отовсюду (на случай рассинхрона)
        for (auto& [node, occ] : nodeOcc_) {
            occ.erase(token);
        }

        // ставим в нужный узел
        nodeOcc_[targetNode].insert(token);
    }


private:
    std::unordered_map<std::string, Node> nodes_;
    std::unordered_map<std::string, Edge> edges_;
    std::unordered_map<std::string, std::vector<std::string>> adjacency_;

    std::unordered_map<std::string, std::unordered_set<std::string>> nodeOcc_;
    std::unordered_map<std::string, std::unordered_set<std::string>> edgeOcc_;
    std::unordered_map<std::string, VehiclePos> vehiclePos_;

    std::unordered_map<std::string, FlightSession> sessions_;
    std::unordered_set<std::string> reservedParking_;

    std::unordered_map<std::string, int> boardAttempt_;
    std::unordered_map<std::string, int64_t> boardRetryAfter_;

    uint64_t seq_ = 0;
    std::vector<json> events_;

    std::mutex mtx_;

    std::string panelHost_;
    int panelPort_;
    std::string fmHost_;
    int fmPort_;

    std::string selfHost_ = "localhost";
    int selfPort_ = 8081;

    std::string boardHost_ = "localhost";
    int boardPort_ = 8084;

    // Вариант 2: синхронизация с InformationPanel
    std::atomic<bool> panelSyncStop_{false};
    std::thread panelSyncThread_;
    int panelSyncPeriodSec_ = 5;
    std::string panelFlightsListPath_ = "/v1/flights"; // должен возвращать список

    std::atomic<bool> panelInitDone_{false};

    struct DesiredPlane {
        std::string kind;        // "airborne" | "grounded"
        std::string parkingNode; // только для grounded
    };

    std::unordered_map<std::string, DesiredPlane> desiredInBoard_; // то, что ДОЛЖНО быть создано в Board
    std::atomic<bool> boardInitDone_{false};

    // чтобы не спавнить самолеты повторно
    // value: "airborne" или "grounded"
    std::unordered_map<std::string, std::string> spawnedInBoard_;
};

int main(int argc, char** argv)
{
    // На каком порту слушает сам GroundControl
    int port = env_or_int("GC_PORT", 8081);
    if (argc > 1) {
        port = std::stoi(argv[1]);
    }

    // Путь к карте (в Docker обычно будет что-то вроде /app/data/airport_map.json)
    std::string mapPath = env_or("GC_MAP_PATH", "./data/airport_map.json");

    // InformationPanel
    std::string panelHost = env_or("INFO_HOST", "localhost");
    int panelPort = env_or_int("INFO_PORT", 8082);

    // FollowMe
    std::string fmHost = env_or("FOLLOWME_HOST", "localhost");
    int fmPort = env_or_int("FOLLOWME_PORT", 8083);

    // Board
    std::string boardHost = env_or("BOARD_HOST", "localhost");
    int boardPort = env_or_int("BOARD_PORT", 8084);

    // Как Board должен обращаться обратно к GroundControl
    // В Docker это должно быть "ground-control"
    std::string selfHost = env_or("GC_ADVERTISED_HOST", "localhost");
    int selfPort = env_or_int("GC_ADVERTISED_PORT", port);

    GroundControl gc(panelHost, panelPort, fmHost, fmPort, boardHost, boardPort, selfHost, selfPort);

    if (!gc.load_map(mapPath))
    {
        return 1;
    }

    gc.run(port);
    return 0;
}
