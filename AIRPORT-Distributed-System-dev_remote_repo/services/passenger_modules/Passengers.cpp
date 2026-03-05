#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <ctime>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common.h"

using app::json;

namespace {

static std::string env_or(const char* key, const std::string& defVal) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : defVal;
}

static int env_or_int(const char* key, int defVal) {
    const char* v = std::getenv(key);
    if (!v || !*v) return defVal;
    try { return std::stoi(v); } catch (...) { return defVal; }
}

static std::string to_iso_utc(int64_t epochSec) {
    std::time_t t = static_cast<std::time_t>(epochSec);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

static std::string url_encode(const std::string& s) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;

    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        } else {
            escaped << '%' << std::uppercase << std::setw(2) << int(c) << std::nouppercase;
        }
    }
    return escaped.str();
}

static std::string random_uuid_like() {
    static thread_local std::mt19937_64 rng(std::random_device{}());
    std::uniform_int_distribution<uint32_t> dist(0, 0xFFFFFFFFu);

    auto h4 = [&]() {
        std::ostringstream oss;
        oss << std::hex << std::setw(8) << std::setfill('0') << dist(rng);
        return oss.str();
    };

    const std::string p1 = h4();
    const std::string p2 = h4().substr(0, 4);
    const std::string p3 = h4().substr(0, 4);
    const std::string p4 = h4().substr(0, 4);
    const std::string p5 = h4() + h4().substr(0, 4);

    return p1 + "-" + p2 + "-" + p3 + "-" + p4 + "-" + p5;
}

static std::string random_name() {
    static const std::vector<std::string> names = {
        "Alice", "Bob", "Eve", "Ivan", "Maria", "Dmitry", "Olga", "Pavel", "Elena", "Ruslan"
    };
    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<std::size_t> dist(0, names.size() - 1);
    return names[dist(rng)] + "_" + std::to_string(dist(rng) + 1);
}

static std::string random_menu() {
    static const std::vector<std::string> menus = {
        "meat", "chicken", "fish"
    };
    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<std::size_t> dist(0, menus.size() - 1);
    return menus[dist(rng)];
}

static bool is_checkin_open_status(const std::string& status) {
    static const std::unordered_set<std::string> allowed = {
        "RegistrationOpen", "RegistrationClosed", "Boarding", "Scheduled", "Parked", "ArrivedParking"
    };
    return allowed.count(status) > 0;
}

static bool is_allowed_passenger_state(const std::string& state) {
    static const std::unordered_set<std::string> allowed = {
        "CameToAirport", "GotTicket", "CheckedIn", "ReadyForBus", "OnBus", "Boarded"
    };
    return allowed.count(state) > 0;
}

class PassengersService {
public:
    PassengersService(std::string infoHost,
                      int infoPort,
                      std::string ticketsHost,
                      int ticketsPort,
                      std::string checkinHost,
                      int checkinPort,
                      int autogenIntervalSec,
                      std::string reportingHost,
                      int reportingPort)
        : infoHost_(std::move(infoHost)),
          infoPort_(infoPort),
          ticketsHost_(std::move(ticketsHost)),
          ticketsPort_(ticketsPort),
          checkinHost_(std::move(checkinHost)),
          checkinPort_(checkinPort),
          autogenIntervalSec_(autogenIntervalSec),
          reportingHost_(std::move(reportingHost)),
          reportingPort_(reportingPort) {}

    void run(int port) {
        if (autogenIntervalSec_ > 0) {
            start_autogen_thread();
        }

        httplib::Server svr;

        svr.Get("/health", [&](const httplib::Request&, httplib::Response& res) {
            app::reply_json(res, 200, {
                {"service", "Passengers"},
                {"status", "ok"},
                {"time", app::now_sec()}
            });
        });

        // Create passenger
        svr.Post("/v1/passengers", [&](const httplib::Request& req, httplib::Response& res) {
            auto bodyOpt = app::parse_json_body(req);
            if (!bodyOpt) {
                app::reply_json(res, 400, {{"error", "invalid json"}});
                return;
            }

            json created;
            std::string err;
            int status = 201;
            if (!create_passenger_internal(*bodyOpt, true, created, status, err)) {
                app::reply_json(res, status, {{"error", err}});
                return;
            }

            app::reply_json(res, 201, created);
        });

        // List passengers
        svr.Get("/v1/passengers", [&](const httplib::Request&, httplib::Response& res) {
            json arr = json::array();
            std::lock_guard<std::mutex> lk(mtx_);
            for (const auto& [id, p] : passengers_) {
                (void)id;
                arr.push_back(p);
            }
            app::reply_json(res, 200, arr);
        });

        // Passengers by flight
        svr.Get(R"(/v1/passengers/flight/([A-Za-z0-9\-_]+))", [&](const httplib::Request& req, httplib::Response& res) {
            const std::string flightId = req.matches[1];
            json arr = json::array();
            std::lock_guard<std::mutex> lk(mtx_);
            for (const auto& [id, p] : passengers_) {
                (void)id;
                if (p.value("flightId", std::string()) == flightId) {
                    arr.push_back(p);
                }
            }
            app::reply_json(res, 200, arr);
        });

        // IDs of checked-in passengers, then transition to OnBus.
        svr.Get(R"(/v1/passengersId/flight/([A-Za-z0-9\-_]+))", [&](const httplib::Request& req, httplib::Response& res) {
            const std::string flightId = req.matches[1];
            json ids = json::array();

            std::lock_guard<std::mutex> lk(mtx_);
            for (auto& [id, p] : passengers_) {
                if (p.value("flightId", std::string()) != flightId) continue;
                const std::string state = p.value("state", std::string(""));
                if (state == "CheckedIn") {
                    ids.push_back(id);
                    p["state"] = "OnBus";
                    p["updatedAt"] = to_iso_utc(app::now_sec());
                }
            }

            app::reply_json(res, 200, {{"passengers", ids}});
        });

        // Bulk boarding update.
        svr.Post("/v1/passengers/board", [&](const httplib::Request& req, httplib::Response& res) {
            auto bodyOpt = app::parse_json_body(req);
            if (!bodyOpt || !bodyOpt->contains("passenger_ids") || !(*bodyOpt)["passenger_ids"].is_array()) {
                app::reply_json(res, 400, {{"error", "passenger_ids array required"}});
                return;
            }

            int updated = 0;
            std::string flightIdForEvent;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                for (const auto& idj : (*bodyOpt)["passenger_ids"]) {
                    if (!idj.is_string()) continue;
                    const std::string id = idj.get<std::string>();
                    auto it = passengers_.find(id);
                    if (it == passengers_.end()) continue;
                    const std::string state = it->second.value("state", std::string(""));
                    if (state == "CheckedIn" || state == "OnBus") {
                        it->second["state"] = "Boarded";
                        it->second["updatedAt"] = to_iso_utc(app::now_sec());
                        ++updated;

                        if (flightIdForEvent.empty()) {
                            flightIdForEvent = it->second.value("flightId", std::string(""));
                        }
                    }
                }
            }

            if (!flightIdForEvent.empty()) {
                (void)app::http_post_json(reportingHost_, reportingPort_, "/v1/events", {
                    {"eventId", random_uuid_like()},
                    {"eventType", "passengers_boarded"},
                    {"flightId", flightIdForEvent},
                    {"count", updated}
                });
            }

            app::reply_json(res, 200, {{"status", "success"}, {"updated", updated}});
        });

        // Manual state update
        auto patch_state = [&](const std::string& passengerId, const json& body, httplib::Response& res) {
            const std::string state = app::s_or(body, "state");
            if (!is_allowed_passenger_state(state)) {
                app::reply_json(res, 400, {{"error", "invalid_state"}});
                return;
            }

            std::lock_guard<std::mutex> lk(mtx_);
            auto it = passengers_.find(passengerId);
            if (it == passengers_.end()) {
                app::reply_json(res, 404, {{"detail", "Passenger not found"}});
                return;
            }
            it->second["state"] = state;
            it->second["updatedAt"] = to_iso_utc(app::now_sec());
            app::reply_json(res, 200, it->second);
        };

        svr.Patch(R"(/v1/passengers/([A-Za-z0-9\-]+)/state)", [&](const httplib::Request& req, httplib::Response& res) {
            auto bodyOpt = app::parse_json_body(req);
            if (!bodyOpt) {
                app::reply_json(res, 400, {{"error", "invalid json"}});
                return;
            }
            patch_state(req.matches[1], *bodyOpt, res);
        });

        // Compatibility alias for services that only support POST.
        svr.Post(R"(/v1/passengers/([A-Za-z0-9\-]+)/state)", [&](const httplib::Request& req, httplib::Response& res) {
            auto bodyOpt = app::parse_json_body(req);
            if (!bodyOpt) {
                app::reply_json(res, 400, {{"error", "invalid json"}});
                return;
            }
            patch_state(req.matches[1], *bodyOpt, res);
        });

        // Explicit check-in call.
        svr.Post(R"(/v1/passengers/([A-Za-z0-9\-]+)/checkin)", [&](const httplib::Request& req, httplib::Response& res) {
            const std::string passengerId = req.matches[1];
            json passenger;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                auto it = passengers_.find(passengerId);
                if (it == passengers_.end()) {
                    app::reply_json(res, 404, {{"detail", "Passenger not found"}});
                    return;
                }
                passenger = it->second;
            }

            if (!passenger.contains("ticket") || passenger["ticket"].is_null() || !passenger["ticket"].is_object()) {
                app::reply_json(res, 409, {{"error", "ticket_not_found"}, {"state", passenger.value("state", std::string())}});
                return;
            }

            const bool hasForged = passenger.contains("forgedTicket") && passenger["forgedTicket"].is_object();
            const std::string ticketId = hasForged
                ? passenger["forgedTicket"].value("ticketId", std::string(""))
                : passenger["ticket"].value("ticketId", std::string(""));

            auto ck = app::http_post_json(checkinHost_, checkinPort_, "/v1/checkin/start", {
                {"flightId", passenger.value("flightId", std::string(""))},
                {"passengerId", passengerId},
                {"ticketId", ticketId}
            });

            {
                std::lock_guard<std::mutex> lk(mtx_);
                auto it = passengers_.find(passengerId);
                if (it == passengers_.end()) {
                    app::reply_json(res, 404, {{"detail", "Passenger not found"}});
                    return;
                }

                if (ck.ok()) {
                    it->second["state"] = "CheckedIn";
                    it->second["updatedAt"] = to_iso_utc(app::now_sec());
                } else {
                    // forged/invalid ticket branch
                    it->second["state"] = "CameToAirport";
                    it->second["updatedAt"] = to_iso_utc(app::now_sec());
                }

                app::reply_json(res, 200, it->second);
            }
        });

        // Get one passenger
        svr.Get(R"(/v1/passengers/([A-Za-z0-9\-]+))", [&](const httplib::Request& req, httplib::Response& res) {
            const std::string passengerId = req.matches[1];
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = passengers_.find(passengerId);
            if (it == passengers_.end()) {
                app::reply_json(res, 404, {{"detail", "Passenger not found"}});
                return;
            }
            app::reply_json(res, 200, it->second);
        });

        // --- UI endpoints ---
        svr.Get("/ui", [&](const httplib::Request&, httplib::Response& res) {
            std::ostringstream html;
            html << "<html><head><title>Passengers</title></head><body>";
            html << "<h1>Passengers</h1><table border='1'><tr><th>ID</th><th>Name</th><th>Flight</th><th>State</th><th>VIP</th></tr>";
            {
                std::lock_guard<std::mutex> lk(mtx_);
                for (const auto& [id, p] : passengers_) {
                    html << "<tr><td>" << id << "</td><td>" << p.value("name", std::string()) << "</td><td>" << p.value("flightId", std::string())
                         << "</td><td>" << p.value("state", std::string()) << "</td><td>" << (p.value("isVIP", false) ? "yes" : "no") << "</td></tr>";
                }
            }
            html << "</table></body></html>";

            res.status = 200;
            res.set_content(html.str(), "text/html; charset=utf-8");
        });

        svr.Post("/ui/create_passenger", [&](const httplib::Request& req, httplib::Response& res) {
            auto bodyOpt = app::parse_json_body(req);
            if (!bodyOpt) {
                app::reply_json(res, 400, {{"error", "invalid json"}});
                return;
            }

            json created;
            std::string err;
            int status = 201;
            if (!create_passenger_internal(*bodyOpt, true, created, status, err)) {
                app::reply_json(res, status, {{"error", err}});
                return;
            }
            app::reply_json(res, 200, {{"ok", true}, {"passenger", created}});
        });

        svr.Post("/ui/create_bulk_passengers", [&](const httplib::Request& req, httplib::Response& res) {
            auto bodyOpt = app::parse_json_body(req);
            if (!bodyOpt) {
                app::reply_json(res, 400, {{"error", "invalid json"}});
                return;
            }
            const int count = std::clamp(bodyOpt->value("count", 1), 1, 200);
            const std::string flightId = bodyOpt->value("flightId", std::string(""));

            int createdCnt = 0;
            json createdList = json::array();
            for (int i = 0; i < count; ++i) {
                json reqBody = {
                    {"name", random_name()},
                    {"baggageWeight", bodyOpt->value("baggageWeight", 0.0)},
                    {"menuType", bodyOpt->value("menuType", random_menu())},
                    {"isVIP", bodyOpt->value("isVIP", false)}
                };
                if (!flightId.empty()) reqBody["flightId"] = flightId;

                json created;
                std::string err;
                int status = 201;
                if (create_passenger_internal(reqBody, true, created, status, err)) {
                    ++createdCnt;
                    createdList.push_back(created);
                }
            }

            app::reply_json(res, 200, {{"ok", true}, {"created", createdCnt}, {"passengers", createdList}});
        });

        svr.Post("/ui/register_all", [&](const httplib::Request& req, httplib::Response& res) {
            auto bodyOpt = app::parse_json_body(req);
            if (!bodyOpt) {
                app::reply_json(res, 400, {{"error", "invalid json"}});
                return;
            }
            const std::string flightId = app::s_or(*bodyOpt, "flightId");
            if (flightId.empty()) {
                app::reply_json(res, 400, {{"error", "flightId required"}});
                return;
            }

            int registered = 0;
            std::vector<std::string> ids;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                for (const auto& [id, p] : passengers_) {
                    if (p.value("flightId", std::string("")) == flightId) {
                        ids.push_back(id);
                    }
                }
            }

            for (const auto& id : ids) {
                auto call = app::http_post_json(
                    env_or("PASSENGER_SELF_HOST", "localhost"),
                    env_or_int("PASSENGER_SELF_PORT", 8004),
                    "/v1/passengers/" + url_encode(id) + "/checkin",
                    json::object()
                );
                if (call.ok()) ++registered;
            }

            app::reply_json(res, 200, {{"ok", true}, {"registered", registered}});
        });

        svr.Post("/ui/toggle_vip", [&](const httplib::Request& req, httplib::Response& res) {
            auto bodyOpt = app::parse_json_body(req);
            if (!bodyOpt) {
                app::reply_json(res, 400, {{"error", "invalid json"}});
                return;
            }
            const std::string passengerId = app::s_or(*bodyOpt, "passengerId");
            if (passengerId.empty()) {
                app::reply_json(res, 400, {{"error", "passengerId required"}});
                return;
            }

            std::lock_guard<std::mutex> lk(mtx_);
            auto it = passengers_.find(passengerId);
            if (it == passengers_.end()) {
                app::reply_json(res, 404, {{"error", "passenger_not_found"}});
                return;
            }

            auto flightRes = app::http_get_json(infoHost_, infoPort_, "/v1/flights/" + url_encode(it->second.value("flightId", std::string(""))));
            if (!flightRes.ok() || flightRes.body.value("status", std::string("")) != "Scheduled") {
                app::reply_json(res, 409, {{"error", "vip_toggle_allowed_only_in_scheduled"}});
                return;
            }

            bool vip = it->second.value("isVIP", false);
            it->second["isVIP"] = !vip;
            it->second["updatedAt"] = to_iso_utc(app::now_sec());
            app::reply_json(res, 200, {{"ok", true}, {"isVIP", !vip}, {"passenger", it->second}});
        });

        svr.Post("/ui/fake_ticket", [&](const httplib::Request& req, httplib::Response& res) {
            auto bodyOpt = app::parse_json_body(req);
            if (!bodyOpt) {
                app::reply_json(res, 400, {{"error", "invalid json"}});
                return;
            }
            const std::string passengerId = app::s_or(*bodyOpt, "passengerId");
            if (passengerId.empty()) {
                app::reply_json(res, 400, {{"error", "passengerId required"}});
                return;
            }

            std::lock_guard<std::mutex> lk(mtx_);
            auto it = passengers_.find(passengerId);
            if (it == passengers_.end()) {
                app::reply_json(res, 404, {{"error", "passenger_not_found"}});
                return;
            }
            if (!it->second.contains("ticket") || !it->second["ticket"].is_object()) {
                app::reply_json(res, 409, {{"error", "no_real_ticket"}});
                return;
            }

            json forged = it->second["ticket"];
            forged["ticketId"] = "FAKE-" + forged.value("ticketId", std::string(""));
            forged["status"] = "fake";
            forged["isFake"] = true;
            it->second["forgedTicket"] = forged;
            it->second["updatedAt"] = to_iso_utc(app::now_sec());

            app::reply_json(res, 200, {{"ok", true}, {"forgedTicket", forged}, {"passenger", it->second}});
        });

        std::cout << "[Passengers] listening on 0.0.0.0:" << port << '\n';
        svr.listen("0.0.0.0", port);

        stopAutogen_.store(true);
        if (autogenThread_.joinable()) autogenThread_.join();
    }

private:
    bool create_passenger_internal(const json& body,
                                   bool allowAutoCheckin,
                                   json& created,
                                   int& statusCode,
                                   std::string& error) {
        json passenger;
        passenger["id"] = random_uuid_like();
        passenger["name"] = body.value("name", random_name());
        passenger["baggageWeight"] = body.value("baggageWeight", 0.0);
        passenger["menuType"] = body.value("menuType", random_menu());
        passenger["isVIP"] = body.value("isVIP", false);
        passenger["ticket"] = nullptr;
        passenger["forgedTicket"] = nullptr;
        passenger["state"] = "CameToAirport";
        passenger["createdAt"] = to_iso_utc(app::now_sec());
        passenger["updatedAt"] = passenger["createdAt"];

        std::string flightId = body.value("flightId", std::string(""));
        if (flightId.empty()) {
            auto chosen = choose_flight_for_passenger();
            if (!chosen.has_value()) {
                statusCode = 409;
                error = "no_available_flights";
                return false;
            }
            flightId = *chosen;
        }
        passenger["flightId"] = flightId;

        // Save passenger first: TicketSales validates passengerId + name.
        {
            std::lock_guard<std::mutex> lk(mtx_);
            passengers_[passenger["id"].get<std::string>()] = passenger;
        }

        const auto ticketRes = app::http_post_json(ticketsHost_, ticketsPort_, "/v1/tickets/buy", {
            {"passengerId", passenger["id"]},
            {"passengerName", passenger["name"]},
            {"flightId", passenger["flightId"]},
            {"isVIP", passenger["isVIP"]},
            {"menuType", passenger["menuType"]},
            {"baggageWeight", passenger["baggageWeight"]}
        });

        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto& p = passengers_[passenger["id"].get<std::string>()];

            if (ticketRes.ok()) {
                p["ticket"] = ticketRes.body;
                p["state"] = "GotTicket";
                p["updatedAt"] = to_iso_utc(app::now_sec());

                if (allowAutoCheckin && should_auto_checkin(flightId)) {
                    auto ck = app::http_post_json(checkinHost_, checkinPort_, "/v1/checkin/start", {
                        {"flightId", flightId},
                        {"passengerId", p["id"]},
                        {"ticketId", ticketRes.body.value("ticketId", std::string(""))}
                    });

                    if (ck.ok()) {
                        p["state"] = "CheckedIn";
                        p["updatedAt"] = to_iso_utc(app::now_sec());
                    }
                }
            } else {
                p["ticket"] = nullptr;
                p["state"] = "CameToAirport";
                p["ticketError"] = {
                    {"status", ticketRes.status},
                    {"error", ticketRes.error}
                };
                p["updatedAt"] = to_iso_utc(app::now_sec());
            }

            created = p;
        }

        statusCode = 201;
        return true;
    }

    std::optional<std::string> choose_flight_for_passenger() {
        auto r = app::http_get_json(infoHost_, infoPort_, "/v1/flights?type=depart&ts=" + std::to_string(app::now_sec()));
        if (!r.ok()) return std::nullopt;

        json flights = json::array();
        if (r.body.is_object() && r.body.contains("flights") && r.body["flights"].is_array()) {
            flights = r.body["flights"];
        } else if (r.body.is_array()) {
            flights = r.body;
        }

        std::vector<std::string> candidates;
        for (const auto& f : flights) {
            if (!f.is_object()) continue;
            const std::string id = f.value("flightId", std::string(""));
            const std::string status = f.value("status", std::string(""));
            if (id.empty()) continue;
            if (status == "Scheduled" || status == "RegistrationOpen" || status == "RegistrationClosed") {
                candidates.push_back(id);
            }
        }

        if (candidates.empty()) return std::nullopt;

        static thread_local std::mt19937 rng(std::random_device{}());
        std::uniform_int_distribution<std::size_t> dist(0, candidates.size() - 1);
        return candidates[dist(rng)];
    }

    bool should_auto_checkin(const std::string& flightId) {
        auto f = app::http_get_json(infoHost_, infoPort_, "/v1/flights/" + url_encode(flightId));
        if (!f.ok()) return false;
        return is_checkin_open_status(f.body.value("status", std::string("")));
    }

    void start_autogen_thread() {
        autogenThread_ = std::thread([this]() {
            while (!stopAutogen_.load()) {
                std::this_thread::sleep_for(std::chrono::seconds(autogenIntervalSec_));
                if (stopAutogen_.load()) break;

                json req = {
                    {"name", random_name()},
                    {"baggageWeight", 0.0},
                    {"menuType", random_menu()},
                    {"isVIP", false}
                };
                json created;
                std::string err;
                int code = 201;
                (void)create_passenger_internal(req, true, created, code, err);
            }
        });
    }

private:
    std::mutex mtx_;
    std::unordered_map<std::string, json> passengers_;

    std::string infoHost_;
    int infoPort_ = 8082;
    std::string ticketsHost_;
    int ticketsPort_ = 8003;
    std::string checkinHost_;
    int checkinPort_ = 8005;

    int autogenIntervalSec_ = 0;
    std::string reportingHost_;
    int reportingPort_ = 8092;
    std::atomic<bool> stopAutogen_{false};
    std::thread autogenThread_;
};

} // namespace

int main(int argc, char** argv) {
    int port = env_or_int("PASSENGER_PORT", 8004);
    if (argc > 1) {
        try { port = std::stoi(argv[1]); } catch (...) {}
    }

    PassengersService svc(
        env_or("PASSENGER_INFO_HOST", "localhost"),
        env_or_int("PASSENGER_INFO_PORT", 8082),
        env_or("PASSENGER_TICKETS_HOST", "localhost"),
        env_or_int("PASSENGER_TICKETS_PORT", 8003),
        env_or("PASSENGER_CHECKIN_HOST", "localhost"),
        env_or_int("PASSENGER_CHECKIN_PORT", 8005),
        env_or_int("PASSENGER_AUTOGENERATE_INTERVAL_SEC", 0),
        env_or("PASSENGER_REPORTING_HOST", env_or("REPORTING_HOST", "localhost")),
        env_or_int("PASSENGER_REPORTING_PORT", env_or_int("REPORTING_PORT", 8092))
    );

    svc.run(port);
    return 0;
}
