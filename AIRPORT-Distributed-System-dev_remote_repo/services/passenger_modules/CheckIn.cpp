#include <algorithm>
#include <cctype>
#include <ctime>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
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

static std::string random_counter() {
    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(1, 6);
    return "C" + std::to_string(dist(rng));
}

static std::string random_seat_number() {
    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> rowDist(1, 35);
    std::uniform_int_distribution<int> colDist(0, 5);
    static const char* cols = "ABCDEF";

    return std::to_string(rowDist(rng)) + std::string(1, cols[colDist(rng)]);
}

static bool is_valid_status(const std::string& s) {
    static const std::unordered_set<std::string> allowed = {
        "Pending", "Validated", "Rejected", "Completed", "Confirmed"
    };
    return allowed.count(s) > 0;
}

static bool is_flight_status_open_for_checkin(const std::string& status) {
    static const std::unordered_set<std::string> allowed = {
        "Scheduled", "RegistrationOpen", "RegistrationClosed", "Boarding", "Parked", "ArrivedParking"
    };
    return allowed.count(status) > 0;
}

class CheckInService {
public:
    CheckInService(std::string infoHost,
                   int infoPort,
                   std::string ticketsHost,
                   int ticketsPort,
                   std::string passengerHost,
                   int passengerPort,
                   std::string cateringHost,
                   int cateringPort,
                   std::string baggageHost,
                   int baggagePort,
                   std::string reportingHost,
                   int reportingPort)
        : infoHost_(std::move(infoHost)),
          infoPort_(infoPort),
          ticketsHost_(std::move(ticketsHost)),
          ticketsPort_(ticketsPort),
          passengerHost_(std::move(passengerHost)),
          passengerPort_(passengerPort),
          cateringHost_(std::move(cateringHost)),
          cateringPort_(cateringPort),
          baggageHost_(std::move(baggageHost)),
          baggagePort_(baggagePort),
          reportingHost_(std::move(reportingHost)),
          reportingPort_(reportingPort) {}

    void run(int port) {
        httplib::Server svr;

        svr.Get("/health", [&](const httplib::Request&, httplib::Response& res) {
            app::reply_json(res, 200, {
                {"service", "CheckIn"},
                {"status", "ok"},
                {"time", app::now_sec()}
            });
        });

        // Ticket Sales can push known tickets per flight.
        svr.Post("/v1/checkin/tickets", [&](const httplib::Request& req, httplib::Response& res) {
            auto bodyOpt = app::parse_json_body(req);
            if (!bodyOpt) {
                app::reply_json(res, 400, {{"error", "invalid json"}});
                return;
            }
            const auto& body = *bodyOpt;
            const std::string flightId = app::s_or(body, "flightId");
            if (flightId.empty() || !body.contains("tickets") || !body["tickets"].is_array()) {
                app::reply_json(res, 400, {{"error", "flightId and tickets[] required"}});
                return;
            }

            int accepted = 0;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                for (const auto& t : body["tickets"]) {
                    if (!t.is_object()) continue;
                    const std::string tid = t.value("ticketId", std::string(""));
                    if (tid.empty()) continue;
                    knownTickets_[flightId][tid] = t;
                    ++accepted;
                }
            }

            app::reply_json(res, 200, {
                {"ok", true},
                {"flightId", flightId},
                {"accepted", accepted}
            });
        });

        // Start check-in for one passenger.
        svr.Post("/v1/checkin/start", [&](const httplib::Request& req, httplib::Response& res) {
            auto bodyOpt = app::parse_json_body(req);
            if (!bodyOpt) {
                app::reply_json(res, 400, {{"error", "invalid json"}});
                return;
            }
            const auto& body = *bodyOpt;

            const std::string flightId = app::s_or(body, "flightId");
            const std::string passengerId = app::s_or(body, "passengerId");
            const std::string ticketId = app::s_or(body, "ticketId");

            if (flightId.empty() || passengerId.empty() || ticketId.empty()) {
                app::reply_json(res, 400, {{"error", "flightId, passengerId, ticketId required"}});
                return;
            }

            // 1) Verify passenger + name.
            auto passengerRes = app::http_get_json(
                passengerHost_,
                passengerPort_,
                "/v1/passengers/" + url_encode(passengerId)
            );
            if (!passengerRes.ok()) {
                app::reply_json(res, 404, {{"error", "passenger_not_found"}, {"passengerId", passengerId}});
                return;
            }
            const std::string passengerName = passengerRes.body.value("name", std::string(""));

            // 2) Verify ticket against TicketSales.
            json ticket;
            {
                auto verifyRes = app::http_post_json(ticketsHost_, ticketsPort_, "/v1/tickets/verify", {
                    {"ticketId", ticketId},
                    {"passengerId", passengerId},
                    {"flightId", flightId},
                    {"passengerName", passengerName}
                });

                if (verifyRes.ok() && verifyRes.body.value("valid", false)) {
                    ticket = verifyRes.body.value("ticket", json::object());
                } else {
                    std::lock_guard<std::mutex> lk(mtx_);
                    auto fit = knownTickets_.find(flightId);
                    if (fit != knownTickets_.end()) {
                        auto tit = fit->second.find(ticketId);
                        if (tit != fit->second.end()) {
                            const auto& localTicket = tit->second;
                            if (localTicket.value("status", std::string("")) == "active" &&
                                localTicket.value("passengerId", std::string("")) == passengerId &&
                                localTicket.value("passengerName", std::string("")) == passengerName) {
                                ticket = localTicket;
                            }
                        }
                    }
                }
            }

            if (ticket.is_null() || ticket.empty()) {
                app::reply_json(res, 409, {{"error", "ticket_validation_failed"}, {"status", "Rejected"}});
                return;
            }

            // 3) Check flight status in information panel.
            auto flightRes = app::http_get_json(infoHost_, infoPort_, "/v1/flights/" + url_encode(flightId) + "?ts=" + std::to_string(app::now_sec()));
            if (!flightRes.ok()) {
                app::reply_json(res, 404, {{"error", "flight_not_found"}, {"flightId", flightId}});
                return;
            }
            const std::string flightStatus = flightRes.body.value("status", std::string(""));
            if (!is_flight_status_open_for_checkin(flightStatus)) {
                app::reply_json(res, 409, {
                    {"error", "checkin_closed_for_flight"},
                    {"flightStatus", flightStatus}
                });
                return;
            }

            // 4) Create record and mark completed immediately in this simulation.
            json rec = {
                {"checkInId", "CH-" + random_uuid_like()},
                {"flightId", flightId},
                {"passengerId", passengerId},
                {"passengerName", passengerName},
                {"ticketId", ticketId},
                {"taskType", "registration"},
                {"counter", random_counter()},
                {"status", "Validated"},
                {"state", "completed"},
                {"checkedInAt", to_iso_utc(app::now_sec())},
                {"details", {
                    {"seatNumber", random_seat_number()},
                    {"mealPreference", ticket.value("menuType", std::string("chicken"))},
                    {"frequentFlyer", ticket.value("isVIP", false)}
                }}
            };

            {
                std::lock_guard<std::mutex> lk(mtx_);
                checkins_[rec["checkInId"].get<std::string>()] = rec;
                checkedTicketsByFlight_[flightId].insert(ticketId);

                // Aggregate baggage for warehouse.
                const double baggageWeight = ticket.value("baggageWeight", 0.0);
                if (baggageWeight > 0.0) {
                    const std::string baggageId = "BAG-" + random_uuid_like();
                    baggageByFlight_[flightId]["flightId"] = flightId;
                    baggageByFlight_[flightId]["baggageList"][baggageId] = {
                        {"owner", passengerName},
                        {"weight", baggageWeight}
                    };
                }

                // Aggregate menu summary.
                const std::string menu = ticket.value("menuType", std::string("chicken"));
                menuByFlight_[flightId]["flightId"] = flightId;
                if (!menuByFlight_[flightId].contains("menu")) menuByFlight_[flightId]["menu"] = json::object();
                menuByFlight_[flightId]["menu"][menu] = menuByFlight_[flightId]["menu"].value(menu, 0) + 1;
            }

            // Best effort: move passenger state forward.
            (void)app::http_patch_json(
                passengerHost_,
                passengerPort_,
                "/v1/passengers/" + url_encode(passengerId) + "/state",
                {{"state", "ReadyForBus"}}
            );

            // Best effort: persist check-in timestamp back to ticket.
            (void)app::http_post_json(
                ticketsHost_,
                ticketsPort_,
                "/v1/tickets/checkin",
                {
                    {"ticketId", ticketId},
                    {"checkInTime", rec["checkedInAt"]}
                }
            );

            (void)app::http_post_json(reportingHost_, reportingPort_, "/v1/events", {
                {"eventId", random_uuid_like()},
                {"eventType", "passenger_checked_in"},
                {"flightId", flightId},
                {"passengerId", passengerId}
            });

            app::reply_json(res, 201, {
                {"checkInId", rec["checkInId"]},
                {"status", rec["status"]}
            });
        });

        svr.Get(R"(/v1/checkin/([A-Za-z0-9\-_]+)/menu)", [&](const httplib::Request& req, httplib::Response& res) {
            const std::string flightId = req.matches[1];
            std::lock_guard<std::mutex> lk(mtx_);

            auto it = menuByFlight_.find(flightId);
            if (it == menuByFlight_.end()) {
                app::reply_json(res, 200, {
                    {"flightId", flightId},
                    {"menu", json::object()}
                });
                return;
            }

            app::reply_json(res, 200, it->second);
        });

        svr.Post(R"(/v1/checkin/([A-Za-z0-9\-_]+)/menu)", [&](const httplib::Request& req, httplib::Response& res) {
            const std::string checkInId = req.matches[1];

            json rec;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                auto it = checkins_.find(checkInId);
                if (it == checkins_.end()) {
                    app::reply_json(res, 404, {{"error", "checkin_not_found"}});
                    return;
                }
                rec = it->second;
            }

            const std::string flightId = rec.value("flightId", std::string(""));
            json menu;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                auto it = menuByFlight_.find(flightId);
                menu = (it == menuByFlight_.end())
                    ? json{{"flightId", flightId}, {"menu", json::object()}}
                    : it->second;
            }

            // Best effort forward to Catering service endpoint from spec.
            (void)app::http_post_json(cateringHost_, cateringPort_, "/checkin/menu", menu);

            app::reply_json(res, 200, {
                {"status", "success"},
                {"flightId", flightId},
                {"menuSummary", menu.value("menu", json::object())}
            });
        });

        svr.Post(R"(/v1/checkin/([A-Za-z0-9\-_]+)/baggage)", [&](const httplib::Request& req, httplib::Response& res) {
            const std::string checkInId = req.matches[1];

            json rec;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                auto it = checkins_.find(checkInId);
                if (it == checkins_.end()) {
                    app::reply_json(res, 404, {{"error", "checkin_not_found"}});
                    return;
                }
                rec = it->second;
            }

            const std::string flightId = rec.value("flightId", std::string(""));
            json baggage;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                auto it = baggageByFlight_.find(flightId);
                baggage = (it == baggageByFlight_.end())
                    ? json{{"flightId", flightId}, {"baggageList", json::object()}}
                    : it->second;
            }

            (void)app::http_post_json(baggageHost_, baggagePort_, "/checkin/baggage", baggage);

            int processed = 0;
            if (baggage.contains("baggageList") && baggage["baggageList"].is_object()) {
                processed = static_cast<int>(baggage["baggageList"].size());
            }

            app::reply_json(res, 200, {
                {"status", "success"},
                {"processedBags", processed},
                {"flightId", flightId}
            });
        });

        svr.Post(R"(/v1/checkin/([A-Za-z0-9\-_]+)/baggage-track)", [&](const httplib::Request& req, httplib::Response& res) {
            const std::string checkInId = req.matches[1];
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = checkins_.find(checkInId);
            if (it == checkins_.end()) {
                app::reply_json(res, 404, {{"error", "checkin_not_found"}});
                return;
            }

            app::reply_json(res, 200, {
                {"status", "success"},
                {"checkInId", checkInId},
                {"message", "Baggage tracking request accepted"}
            });
        });

        svr.Get(R"(/v1/checkin/([A-Za-z0-9\-_]+))", [&](const httplib::Request& req, httplib::Response& res) {
            const std::string checkInId = req.matches[1];
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = checkins_.find(checkInId);
            if (it == checkins_.end()) {
                app::reply_json(res, 404, {{"detail", "Check-in record not found"}});
                return;
            }
            app::reply_json(res, 200, it->second);
        });

        svr.Patch(R"(/v1/checkin/([A-Za-z0-9\-_]+))", [&](const httplib::Request& req, httplib::Response& res) {
            auto bodyOpt = app::parse_json_body(req);
            if (!bodyOpt) {
                app::reply_json(res, 400, {{"error", "invalid json"}});
                return;
            }
            const std::string checkInId = req.matches[1];
            const auto& body = *bodyOpt;
            const std::string status = app::s_or(body, "status");
            if (!is_valid_status(status)) {
                app::reply_json(res, 400, {{"error", "invalid_status"}});
                return;
            }

            std::lock_guard<std::mutex> lk(mtx_);
            auto it = checkins_.find(checkInId);
            if (it == checkins_.end()) {
                app::reply_json(res, 404, {{"error", "checkin_not_found"}});
                return;
            }

            it->second["status"] = status;
            it->second["checkedInAt"] = to_iso_utc(app::now_sec());
            app::reply_json(res, 200, it->second);
        });

        // Public compatibility endpoints from docs.
        svr.Post("/checkin/baggage", [&](const httplib::Request& req, httplib::Response& res) {
            auto bodyOpt = app::parse_json_body(req);
            if (!bodyOpt) {
                app::reply_json(res, 400, {{"status", "error"}, {"message", "Invalid JSON"}, {"errorCode", 4000}});
                return;
            }
            const auto& body = *bodyOpt;
            const std::string flightId = app::s_or(body, "flightId");
            if (flightId.empty() || !body.contains("baggageList") || !body["baggageList"].is_object()) {
                app::reply_json(res, 400, {
                    {"status", "error"},
                    {"message", "Flight ID is missing or invalid."},
                    {"errorCode", 4001}
                });
                return;
            }

            {
                std::lock_guard<std::mutex> lk(mtx_);
                baggageByFlight_[flightId] = body;
            }

            app::reply_json(res, 200, {
                {"status", "success"},
                {"message", "Baggage data received and recorded successfully."},
                {"processedBags", static_cast<int>(body["baggageList"].size())},
                {"flightId", flightId}
            });
        });

        svr.Post("/checkin/menu", [&](const httplib::Request& req, httplib::Response& res) {
            auto bodyOpt = app::parse_json_body(req);
            if (!bodyOpt) {
                app::reply_json(res, 400, {{"status", "error"}, {"message", "Invalid JSON"}, {"errorCode", 4000}});
                return;
            }
            const auto& body = *bodyOpt;
            const std::string flightId = app::s_or(body, "flightId");
            if (flightId.empty() || !body.contains("menu") || !body["menu"].is_object()) {
                app::reply_json(res, 400, {
                    {"status", "error"},
                    {"message", "Invalid menu format or missing data."},
                    {"errorCode", 4002}
                });
                return;
            }

            {
                std::lock_guard<std::mutex> lk(mtx_);
                menuByFlight_[flightId] = body;
            }

            app::reply_json(res, 200, {
                {"status", "success"},
                {"message", "Food order received and planned successfully."},
                {"flightId", flightId},
                {"menuSummary", body["menu"]}
            });
        });

        std::cout << "[CheckIn] listening on 0.0.0.0:" << port << '\n';
        svr.listen("0.0.0.0", port);
    }

private:
    std::mutex mtx_;

    // checkInId -> record
    std::unordered_map<std::string, json> checkins_;

    // flightId -> ticketId -> ticket JSON
    std::unordered_map<std::string, std::unordered_map<std::string, json>> knownTickets_;

    // flightId -> checked ticket IDs
    std::unordered_map<std::string, std::unordered_set<std::string>> checkedTicketsByFlight_;

    // flightId -> aggregated payload
    std::unordered_map<std::string, json> baggageByFlight_;
    std::unordered_map<std::string, json> menuByFlight_;

    std::string infoHost_;
    int infoPort_ = 8082;
    std::string ticketsHost_;
    int ticketsPort_ = 8003;
    std::string passengerHost_;
    int passengerPort_ = 8004;

    std::string cateringHost_;
    int cateringPort_ = 8089;
    std::string baggageHost_;
    int baggagePort_ = 8091;
    std::string reportingHost_;
    int reportingPort_ = 8092;
};

} // namespace

int main(int argc, char** argv) {
    int port = env_or_int("CHECKIN_PORT", 8005);
    if (argc > 1) {
        try { port = std::stoi(argv[1]); } catch (...) {}
    }

    CheckInService svc(
        env_or("CHECKIN_INFO_HOST", "localhost"),
        env_or_int("CHECKIN_INFO_PORT", 8082),
        env_or("CHECKIN_TICKETS_HOST", "localhost"),
        env_or_int("CHECKIN_TICKETS_PORT", 8003),
        env_or("CHECKIN_PASSENGER_HOST", "localhost"),
        env_or_int("CHECKIN_PASSENGER_PORT", 8004),
        env_or("CHECKIN_CATERING_HOST", "localhost"),
        env_or_int("CHECKIN_CATERING_PORT", 8089),
        env_or("CHECKIN_BAGGAGE_HOST", "localhost"),
        env_or_int("CHECKIN_BAGGAGE_PORT", 8091),
        env_or("CHECKIN_REPORTING_HOST", env_or("REPORTING_HOST", "localhost")),
        env_or_int("CHECKIN_REPORTING_PORT", env_or_int("REPORTING_PORT", 8092))
    );

    svc.run(port);
    return 0;
}
