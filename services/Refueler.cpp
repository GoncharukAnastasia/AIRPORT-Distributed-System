#include <unordered_map>
#include <mutex>
#include <thread>
#include <chrono>
#include <iostream>
#include <vector>
#include <string>
#include <cstdlib>
#include <stdexcept>

#include <pqxx/pqxx>

#include "common.h"

using app::json;

struct RefuelVehicle {
    std::string vehicleId;
    std::string status = "empty"; // empty, reserved, moveToRefuelPosition, refueling, returning
    std::string currentNode = "RS-1";
    std::string flightId;
    std::string parkingNode;
};

class RefuelerRepository {
public:
    explicit RefuelerRepository(std::string connStr)
        : connStr_(std::move(connStr)) {}

    void ensure_schema() {
        pqxx::connection cx(connStr_);
        pqxx::work tx(cx);

        tx.exec(R"sql(
            CREATE TABLE IF NOT EXISTS refueler_vehicles (
                vehicle_id    TEXT PRIMARY KEY,
                current_node  TEXT NOT NULL DEFAULT 'RS-1',
                status        TEXT NOT NULL DEFAULT 'empty',
                flight_id     TEXT NULL,
                parking_node  TEXT NULL,
                updated_at    TIMESTAMPTZ NOT NULL DEFAULT NOW(),
                CONSTRAINT chk_refueler_status CHECK (
                    status IN ('empty', 'reserved', 'moveToRefuelPosition', 'refueling', 'returning')
                )
            );
        )sql");

        tx.exec(R"sql(
            CREATE INDEX IF NOT EXISTS idx_refueler_status ON refueler_vehicles(status);
        )sql");

        tx.commit();
    }

    std::vector<RefuelVehicle> load_all() {
        pqxx::connection cx(connStr_);
        pqxx::work tx(cx);

        pqxx::result r = tx.exec(R"sql(
            SELECT
                vehicle_id,
                current_node,
                status,
                COALESCE(flight_id, '') AS flight_id,
                COALESCE(parking_node, '') AS parking_node
            FROM refueler_vehicles
            ORDER BY vehicle_id
        )sql");

        std::vector<RefuelVehicle> out;
        out.reserve(r.size());

        for (const auto& row : r) {
            RefuelVehicle v;
            v.vehicleId = row["vehicle_id"].as<std::string>();
            v.currentNode = row["current_node"].as<std::string>();
            v.status = row["status"].as<std::string>();
            v.flightId = row["flight_id"].as<std::string>();
            v.parkingNode = row["parking_node"].as<std::string>();
            out.push_back(std::move(v));
        }

        tx.commit();
        return out;
    }

    void upsert_many(const std::vector<RefuelVehicle>& vehicles) {
        if (vehicles.empty()) return;

        pqxx::connection cx(connStr_);
        pqxx::work tx(cx);

        for (const auto& v : vehicles) {
            tx.exec_params(R"sql(
                INSERT INTO refueler_vehicles (vehicle_id, current_node, status, flight_id, parking_node, updated_at)
                VALUES ($1, $2, $3, NULLIF($4, ''), NULLIF($5, ''), NOW())
                ON CONFLICT (vehicle_id) DO UPDATE SET
                    current_node = EXCLUDED.current_node,
                    status = EXCLUDED.status,
                    flight_id = EXCLUDED.flight_id,
                    parking_node = EXCLUDED.parking_node,
                    updated_at = NOW()
            )sql", v.vehicleId, v.currentNode, v.status, v.flightId, v.parkingNode);
        }

        tx.commit();
    }

    void upsert_one(const RefuelVehicle& v) {
        pqxx::connection cx(connStr_);
        pqxx::work tx(cx);

        tx.exec_params(R"sql(
            INSERT INTO refueler_vehicles (vehicle_id, current_node, status, flight_id, parking_node, updated_at)
            VALUES ($1, $2, $3, NULLIF($4, ''), NULLIF($5, ''), NOW())
            ON CONFLICT (vehicle_id) DO UPDATE SET
                current_node = EXCLUDED.current_node,
                status = EXCLUDED.status,
                flight_id = EXCLUDED.flight_id,
                parking_node = EXCLUDED.parking_node,
                updated_at = NOW()
        )sql", v.vehicleId, v.currentNode, v.status, v.flightId, v.parkingNode);

        tx.commit();
    }

private:
    std::string connStr_;
};

class RefuelerService {
public:
    RefuelerService(std::string gcHost,
                    int gcPort,
                    std::string boardHost,
                    int boardPort,
                    std::string pgConnStr)
        : gcHost_(std::move(gcHost))
        , gcPort_(gcPort)
        , boardHost_(std::move(boardHost))
        , boardPort_(boardPort)
        , repo_(std::move(pgConnStr)) {}

    void run(int port) {
        try {
            repo_.ensure_schema();
            load_vehicles_from_db();
            sync_loaded_vehicles_to_ground_control();
        } catch (const std::exception& e) {
            std::cerr << "[Refueler][DB] startup error: " << e.what() << '\n';
            throw;
        }

        httplib::Server svr;

        svr.Get("/health", [&](const httplib::Request&, httplib::Response& res) {
            app::reply_json(res, 200, {
                {"service", "Refueler"},
                {"status", "ok"},
                {"time", app::now_sec()}
            });
        });

        svr.Post("/v1/refuelers/init", [&](const httplib::Request& req, httplib::Response& res) {
            auto bodyOpt = app::parse_json_body(req);
            if (!bodyOpt) {
                app::reply_json(res, 400, {{"error", "invalid json"}});
                return;
            }

            const auto& body = *bodyOpt;
            if (!body.contains("vehicles") || !body["vehicles"].is_array()) {
                app::reply_json(res, 400, {{"error", "'vehicles' array required"}});
                return;
            }

            std::vector<RefuelVehicle> batch;
            batch.reserve(body["vehicles"].size());

            for (const auto& v : body["vehicles"]) {
                RefuelVehicle rv;
                rv.vehicleId = app::s_or(v, "vehicleId");
                rv.currentNode = v.value("currentNode", std::string("RS-1"));
                rv.status = v.value("status", std::string("empty"));
                rv.flightId.clear();
                rv.parkingNode.clear();

                if (rv.vehicleId.empty()) continue;
                batch.push_back(std::move(rv));
            }

            try {
                repo_.upsert_many(batch);

                {
                    std::lock_guard<std::mutex> lk(mtx_);
                    for (const auto& v : batch) {
                        vehicles_[v.vehicleId] = v;
                    }
                }
            } catch (const std::exception& e) {
                app::reply_json(res, 500, {
                    {"error", "db_error"},
                    {"message", e.what()}
                });
                return;
            }

            sync_loaded_vehicles_to_ground_control();

            app::reply_json(res, 200, {
                {"ok", true},
                {"initialized", static_cast<int>(batch.size())}
            });
        });

        svr.Get("/v1/refuelers/hasEmpty", [&](const httplib::Request&, httplib::Response& res) {
            int cnt = 0;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                for (const auto& [id, v] : vehicles_) {
                    (void)id;
                    if (v.status == "empty") ++cnt;
                }
            }

            app::reply_json(res, 200, {
                {"hasEmpty", cnt > 0},
                {"emptyCount", cnt}
            });
        });

        // HandlingSupervisor -> Refueler
        // {
        //   "flightId":"SU200",
        //   "parkingNode":"P-3",
        //   "callbackHost":"handling-superviser",
        //   "callbackPort":8085
        // }
        svr.Post("/v1/refueling/request", [&](const httplib::Request& req, httplib::Response& res) {
            auto bodyOpt = app::parse_json_body(req);
            if (!bodyOpt) {
                app::reply_json(res, 400, {{"error", "invalid json"}});
                return;
            }

            const auto& body = *bodyOpt;
            const std::string flightId = app::s_or(body, "flightId");
            const std::string parkingNode = app::s_or(body, "parkingNode");
            const std::string callbackHost = body.value("callbackHost", std::string("handling-superviser"));
            const int callbackPort = body.value("callbackPort", 8085);

            if (flightId.empty() || parkingNode.empty()) {
                app::reply_json(res, 400, {{"error", "flightId and parkingNode required"}});
                return;
            }

            std::string picked;

            try {
                std::lock_guard<std::mutex> lk(mtx_);

                // идемпотентность: если уже выделена машинка под этот рейс
                for (auto& [id, v] : vehicles_) {
                    if (v.flightId == flightId && v.status != "empty") {
                        app::reply_json(res, 200, {
                            {"ok", true},
                            {"accepted", true},
                            {"reused", true},
                            {"vehicleId", v.vehicleId},
                            {"flightId", flightId}
                        });
                        return;
                    }
                }

                for (auto& [id, v] : vehicles_) {
                    if (v.status == "empty") {
                        v.status = "reserved";
                        v.flightId = flightId;
                        v.parkingNode = parkingNode;
                        repo_.upsert_one(v);
                        picked = id;
                        break;
                    }
                }
            } catch (const std::exception& e) {
                app::reply_json(res, 500, {
                    {"error", "db_error"},
                    {"message", e.what()}
                });
                return;
            }

            if (picked.empty()) {
                app::reply_json(res, 409, {
                    {"ok", false},
                    {"accepted", false},
                    {"error", "no empty refueler"}
                });
                return;
            }

            std::thread([this, picked, flightId, parkingNode, callbackHost, callbackPort]() {
                this->run_mission_worker(picked, flightId, parkingNode, callbackHost, callbackPort);
            }).detach();

            app::reply_json(res, 200, {
                {"ok", true},
                {"accepted", true},
                {"vehicleId", picked},
                {"flightId", flightId},
                {"status", "reserved"}
            });
        });

        svr.Post("/v1/refuelers/release", [&](const httplib::Request& req, httplib::Response& res) {
            auto bodyOpt = app::parse_json_body(req);
            if (!bodyOpt) {
                app::reply_json(res, 400, {{"error", "invalid json"}});
                return;
            }

            const auto& body = *bodyOpt;
            const std::string vehicleId = app::s_or(body, "vehicleId");
            if (vehicleId.empty()) {
                app::reply_json(res, 400, {{"error", "vehicleId required"}});
                return;
            }

            try {
                std::lock_guard<std::mutex> lk(mtx_);
                auto it = vehicles_.find(vehicleId);
                if (it == vehicles_.end()) {
                    app::reply_json(res, 404, {{"error", "vehicle not found"}});
                    return;
                }

                it->second.status = "empty";
                it->second.flightId.clear();
                it->second.parkingNode.clear();
                repo_.upsert_one(it->second);

                app::reply_json(res, 200, {
                    {"ok", true},
                    {"vehicleId", vehicleId},
                    {"status", "empty"}
                });
            } catch (const std::exception& e) {
                app::reply_json(res, 500, {
                    {"error", "db_error"},
                    {"message", e.what()}
                });
            }
        });

        svr.Get("/v1/refuelers", [&](const httplib::Request&, httplib::Response& res) {
            json arr = json::array();
            std::lock_guard<std::mutex> lk(mtx_);
            for (const auto& [id, v] : vehicles_) {
                (void)id;
                arr.push_back({
                    {"vehicleId", v.vehicleId},
                    {"status", v.status},
                    {"currentNode", v.currentNode},
                    {"flightId", v.flightId},
                    {"parkingNode", v.parkingNode}
                });
            }

            app::reply_json(res, 200, {{"vehicles", arr}});
        });

        std::cout << "[Refueler] listening on 0.0.0.0:" << port << '\n';
        std::cout << "[Refueler] GroundControl: " << gcHost_ << ":" << gcPort_
                  << ", Board: " << boardHost_ << ":" << boardPort_ << '\n';

        svr.listen("0.0.0.0", port);
    }

private:
    void load_vehicles_from_db() {
        auto loaded = repo_.load_all();

        {
            std::lock_guard<std::mutex> lk(mtx_);
            vehicles_.clear();
            for (const auto& v : loaded) {
                vehicles_[v.vehicleId] = v;
            }
        }

        std::cout << "[Refueler][DB] loaded " << loaded.size() << " vehicles\n";
    }

    void sync_loaded_vehicles_to_ground_control() {
        json arr = json::array();
        {
            std::lock_guard<std::mutex> lk(mtx_);
            for (const auto& [id, v] : vehicles_) {
                (void)id;
                arr.push_back({
                    {"vehicleId", v.vehicleId},
                    {"currentNode", v.currentNode},
                    {"status", v.status}
                });
            }
        }

        if (arr.empty()) return;

        auto res = app::http_post_json(gcHost_, gcPort_, "/v1/vehicles/init", {
            {"vehicles", arr}
        });

        if (!res.ok()) {
            std::cerr << "[Refueler] warning: failed to sync loaded vehicles to GroundControl\n";
        }
    }

    void set_vehicle_status(const std::string& vehicleId, const std::string& status) {
        try {
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = vehicles_.find(vehicleId);
            if (it != vehicles_.end()) {
                it->second.status = status;
                repo_.upsert_one(it->second);
            }
        } catch (const std::exception& e) {
            std::cerr << "[Refueler][DB] set_vehicle_status failed: " << e.what() << '\n';
        }
    }

    void set_vehicle_node(const std::string& vehicleId, const std::string& node) {
        try {
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = vehicles_.find(vehicleId);
            if (it != vehicles_.end()) {
                it->second.currentNode = node;
                repo_.upsert_one(it->second);
            }
        } catch (const std::exception& e) {
            std::cerr << "[Refueler][DB] set_vehicle_node failed: " << e.what() << '\n';
        }
    }

    void rollback_vehicle_to_empty(const std::string& vehicleId) {
        try {
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = vehicles_.find(vehicleId);
            if (it != vehicles_.end()) {
                it->second.status = "empty";
                it->second.flightId.clear();
                it->second.parkingNode.clear();
                repo_.upsert_one(it->second);
            }
        } catch (const std::exception& e) {
            std::cerr << "[Refueler][DB] rollback failed: " << e.what() << '\n';
        }
    }

    std::vector<std::string> json_to_nodes(const json& j, const std::string& key) {
        std::vector<std::string> out;
        if (!j.contains(key) || !j.at(key).is_array()) return out;
        for (const auto& x : j.at(key)) {
            if (x.is_string()) out.push_back(x.get<std::string>());
        }
        return out;
    }

    bool drive_route(const std::string& vehicleId,
                     const std::string& flightId,
                     const std::vector<std::string>& routeNodes,
                     int maxWaitPerStepSec = 90) {
        if (routeNodes.size() < 2) return true;

        for (size_t i = 0; i + 1 < routeNodes.size(); ++i) {
            const std::string from = routeNodes[i];
            const std::string to = routeNodes[i + 1];

            int waited = 0;
            while (true) {
                auto enterRes = app::http_post_json(gcHost_, gcPort_, "/v1/map/traffic/enter-edge", {
                    {"vehicleId", vehicleId},
                    {"flightId", flightId},
                    {"from", from},
                    {"to", to}
                });

                if (enterRes.ok() && enterRes.body.value("granted", false)) {
                    break;
                }

                std::this_thread::sleep_for(std::chrono::seconds(1));
                if (++waited >= maxWaitPerStepSec) {
                    std::cerr << "[Refueler] enter-edge timeout " << from << " -> " << to << "\n";
                    return false;
                }
            }

            std::this_thread::sleep_for(std::chrono::seconds(1));

            waited = 0;
            while (true) {
                auto leaveRes = app::http_post_json(gcHost_, gcPort_, "/v1/map/traffic/leave-edge", {
                    {"vehicleId", vehicleId},
                    {"flightId", flightId},
                    {"to", to}
                });

                if (leaveRes.ok() && leaveRes.body.value("granted", false)) {
                    set_vehicle_node(vehicleId, to);
                    break;
                }

                std::this_thread::sleep_for(std::chrono::seconds(1));
                if (++waited >= maxWaitPerStepSec) {
                    std::cerr << "[Refueler] leave-edge timeout -> " << to << "\n";
                    return false;
                }
            }
        }

        return true;
    }

    void notify_handling_status(const std::string& host,
                               int port,
                               const std::string& flightId,
                               bool ok,
                               const std::string& vehicleId,
                               const std::string& reason,
                               int fuelAdded) {
        (void)app::http_post_json(host, port, "/v1/handling/refueling-status", {
            {"flightId", flightId},
            {"ok", ok},
            {"vehicleId", vehicleId},
            {"reason", reason},
            {"fuelAdded", fuelAdded}
        });
    }

    void notify_gc_refueled(const std::string& vehicleId,
                            const std::string& flightId,
                            const std::string& parkingNode,
                            const std::string& serviceNode,
                            int fuelAdded) {
        (void)app::http_post_json(gcHost_, gcPort_, "/v1/refuelers/mission/refueled", {
            {"vehicleId", vehicleId},
            {"flightId", flightId},
            {"parkingNode", parkingNode},
            {"serviceNode", serviceNode},
            {"fuelAdded", fuelAdded}
        });
    }

    void notify_gc_completed(const std::string& vehicleId,
                             const std::string& flightId,
                             const std::string& parkingNode) {
        (void)app::http_post_json(gcHost_, gcPort_, "/v1/refuelers/mission/completed", {
            {"vehicleId", vehicleId},
            {"flightId", flightId},
            {"parkingNode", parkingNode}
        });
    }

    void notify_gc_failed(const std::string& vehicleId,
                          const std::string& flightId,
                          const std::string& parkingNode,
                          const std::string& reason) {
        (void)app::http_post_json(gcHost_, gcPort_, "/v1/refuelers/mission/failed", {
            {"vehicleId", vehicleId},
            {"flightId", flightId},
            {"parkingNode", parkingNode},
            {"reason", reason}
        });
    }

    void fail_and_release(const std::string& vehicleId,
                          const std::string& flightId,
                          const std::string& parkingNode,
                          const std::string& callbackHost,
                          int callbackPort,
                          const std::string& reason) {
        notify_gc_failed(vehicleId, flightId, parkingNode, reason);
        rollback_vehicle_to_empty(vehicleId);
        notify_handling_status(callbackHost, callbackPort, flightId, false, vehicleId, reason, 0);
    }

    void run_mission_worker(const std::string& vehicleId,
                            const std::string& flightId,
                            const std::string& parkingNode,
                            const std::string& callbackHost,
                            int callbackPort) {
        auto pathRes = app::http_get_json(
            gcHost_, gcPort_,
            "/v1/map/refueler/path?vehicleId=" + vehicleId + "&parkingNode=" + parkingNode
        );

        if (!pathRes.ok()) {
            fail_and_release(vehicleId, flightId, parkingNode, callbackHost, callbackPort, "path_unreachable");
            return;
        }

        const std::string serviceNode = pathRes.body.value("serviceNode", std::string());
        auto routeToService = json_to_nodes(pathRes.body, "routeToService");
        auto routeReturn = json_to_nodes(pathRes.body, "routeReturn");

        if (serviceNode.empty() || routeToService.empty() || routeReturn.empty()) {
            fail_and_release(vehicleId, flightId, parkingNode, callbackHost, callbackPort, "bad_route");
            return;
        }

        set_vehicle_status(vehicleId, "moveToRefuelPosition");
        if (!drive_route(vehicleId, flightId, routeToService)) {
            fail_and_release(vehicleId, flightId, parkingNode, callbackHost, callbackPort, "routeToService_timeout");
            return;
        }

        set_vehicle_status(vehicleId, "refueling");

        auto refuelRes = app::http_post_json(boardHost_, boardPort_, "/v1/planes/refuel", {
            {"flightId", flightId},
            {"fillToFull", true},
            {"source", "Refueler"},
            {"vehicleId", vehicleId}
        });

        if (!refuelRes.ok() || !refuelRes.body.value("ok", false)) {
            fail_and_release(vehicleId, flightId, parkingNode, callbackHost, callbackPort, "board_refuel_failed");
            return;
        }

        const int fuelAdded = refuelRes.body.value("fuelAdded", 0);

        notify_gc_refueled(vehicleId, flightId, parkingNode, serviceNode, fuelAdded);

        set_vehicle_status(vehicleId, "returning");
        if (!drive_route(vehicleId, flightId, routeReturn)) {
            fail_and_release(vehicleId, flightId, parkingNode, callbackHost, callbackPort, "routeReturn_timeout");
            return;
        }

        rollback_vehicle_to_empty(vehicleId);

        notify_gc_completed(vehicleId, flightId, parkingNode);
        notify_handling_status(callbackHost, callbackPort, flightId, true, vehicleId, "", fuelAdded);
    }

private:
    std::unordered_map<std::string, RefuelVehicle> vehicles_;
    std::mutex mtx_;

    std::string gcHost_;
    int gcPort_;

    std::string boardHost_;
    int boardPort_;

    RefuelerRepository repo_;
};

static std::string env_or(const char* key, const std::string& defVal) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : defVal;
}

int main(int argc, char** argv) {
    try {
        int port = 8086;
        if (argc > 1) port = std::stoi(argv[1]);

        const std::string gcHost = env_or("GC_HOST", "localhost");
        const int gcPort = std::stoi(env_or("GC_PORT", "8081"));

        const std::string boardHost = env_or("BOARD_HOST", "localhost");
        const int boardPort = std::stoi(env_or("BOARD_PORT", "8084"));

        std::string pgConn = env_or("RF_PG_DSN", "");
        if (pgConn.empty()) {
            const std::string host = env_or("RF_PG_HOST", "127.0.0.1");
            const std::string pgPort = env_or("RF_PG_PORT", "5432");
            const std::string db = env_or("RF_PG_DB", "airport");
            const std::string user = env_or("RF_PG_USER", "airport_user");
            const std::string pass = env_or("RF_PG_PASSWORD", "airport_pass");

            pgConn =
                "host=" + host +
                " port=" + pgPort +
                " dbname=" + db +
                " user=" + user +
                " password=" + pass;
        }

        RefuelerService s(gcHost, gcPort, boardHost, boardPort, pgConn);
        s.run(port);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[Refueler] fatal: " << e.what() << '\n';
        return 1;
    }
}