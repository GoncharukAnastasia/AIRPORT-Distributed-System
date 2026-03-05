#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
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

static std::string random_seat_number() {
    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> rowDist(1, 35);
    std::uniform_int_distribution<int> colDist(0, 5);
    static const char* cols = "ABCDEF";

    return std::to_string(rowDist(rng)) + std::string(1, cols[colDist(rng)]);
}

static bool is_purchase_allowed_status(const std::string& status) {
    static const std::unordered_set<std::string> denied = {
        "Cancelled", "Departed", "Denied"
    };
    return denied.count(status) == 0;
}

class TicketSalesService {
public:
    TicketSalesService(std::string infoHost,
                       int infoPort,
                       std::string passengerHost,
                       int passengerPort,
                       std::string checkinHost,
                       int checkinPort,
                       std::string reportingHost,
                       int reportingPort,
                       bool requirePassengerValidation,
                       int maxTicketsPerFlight)
        : infoHost_(std::move(infoHost)),
          infoPort_(infoPort),
          passengerHost_(std::move(passengerHost)),
          passengerPort_(passengerPort),
          checkinHost_(std::move(checkinHost)),
          checkinPort_(checkinPort),
          reportingHost_(std::move(reportingHost)),
          reportingPort_(reportingPort),
          requirePassengerValidation_(requirePassengerValidation),
          maxTicketsPerFlight_(maxTicketsPerFlight) {}

    void run(int port) {
        httplib::Server svr;

        svr.Get("/health", [&](const httplib::Request&, httplib::Response& res) {
            app::reply_json(res, 200, {
                {"service", "TicketSales"},
                {"status", "ok"},
                {"time", app::now_sec()}
            });
        });

        svr.Get("/v1/tickets", [&](const httplib::Request&, httplib::Response& res) {
            json arr = json::array();
            std::lock_guard<std::mutex> lk(mtx_);
            for (const auto& [id, t] : tickets_) {
                (void)id;
                arr.push_back(t);
            }
            app::reply_json(res, 200, arr);
        });

        svr.Get(R"(/v1/tickets/([A-Za-z0-9\-]+))", [&](const httplib::Request& req, httplib::Response& res) {
            const std::string ticketId = req.matches[1];
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = tickets_.find(ticketId);
            if (it == tickets_.end()) {
                app::reply_json(res, 404, {{"detail", "Ticket not found"}});
                return;
            }
            app::reply_json(res, 200, it->second);
        });

        svr.Get(R"(/v1/tickets/passenger/([A-Za-z0-9\-]+))", [&](const httplib::Request& req, httplib::Response& res) {
            const std::string passengerId = req.matches[1];
            json arr = json::array();

            std::lock_guard<std::mutex> lk(mtx_);
            auto pit = passengerToTickets_.find(passengerId);
            if (pit != passengerToTickets_.end()) {
                for (const auto& tid : pit->second) {
                    auto it = tickets_.find(tid);
                    if (it != tickets_.end()) arr.push_back(it->second);
                }
            }

            app::reply_json(res, 200, arr);
        });

        svr.Get(R"(/v1/tickets/flight/([A-Za-z0-9\-_]+))", [&](const httplib::Request& req, httplib::Response& res) {
            const std::string flightId = req.matches[1];
            const bool activeOnly = req.has_param("activeOnly")
                ? (req.get_param_value("activeOnly") == "true" || req.get_param_value("activeOnly") == "1")
                : false;

            json arr = json::array();
            std::lock_guard<std::mutex> lk(mtx_);
            for (const auto& [id, t] : tickets_) {
                (void)id;
                if (t.value("flightId", std::string()) != flightId) continue;
                if (activeOnly && t.value("status", std::string()) != "active") continue;
                arr.push_back(t);
            }
            app::reply_json(res, 200, arr);
        });

        svr.Post("/v1/tickets/verify", [&](const httplib::Request& req, httplib::Response& res) {
            auto bodyOpt = app::parse_json_body(req);
            if (!bodyOpt) {
                app::reply_json(res, 400, {{"error", "invalid json"}});
                return;
            }
            const auto& body = *bodyOpt;

            const std::string ticketId = app::s_or(body, "ticketId");
            const std::string passengerId = app::s_or(body, "passengerId");
            const std::string flightId = app::s_or(body, "flightId");
            const std::string passengerName = app::s_or(body, "passengerName");

            if (ticketId.empty()) {
                app::reply_json(res, 400, {{"error", "ticketId required"}});
                return;
            }

            json ticket;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                auto it = tickets_.find(ticketId);
                if (it == tickets_.end()) {
                    app::reply_json(res, 200, {{"valid", false}, {"reason", "ticket_not_found"}});
                    return;
                }
                ticket = it->second;
            }

            if (ticket.value("status", std::string()) != "active") {
                app::reply_json(res, 200, {{"valid", false}, {"reason", "ticket_not_active"}});
                return;
            }
            if (!passengerId.empty() && ticket.value("passengerId", std::string()) != passengerId) {
                app::reply_json(res, 200, {{"valid", false}, {"reason", "passenger_mismatch"}});
                return;
            }
            if (!flightId.empty() && ticket.value("flightId", std::string()) != flightId) {
                app::reply_json(res, 200, {{"valid", false}, {"reason", "flight_mismatch"}});
                return;
            }
            if (!passengerName.empty() && ticket.value("passengerName", std::string()) != passengerName) {
                app::reply_json(res, 200, {{"valid", false}, {"reason", "passenger_name_mismatch"}});
                return;
            }

            app::reply_json(res, 200, {{"valid", true}, {"ticket", ticket}});
        });

        // Update ticket registration timestamp from CheckIn service.
        svr.Post("/v1/tickets/checkin", [&](const httplib::Request& req, httplib::Response& res) {
            auto bodyOpt = app::parse_json_body(req);
            if (!bodyOpt) {
                app::reply_json(res, 400, {{"error", "invalid json"}});
                return;
            }
            const auto& body = *bodyOpt;
            const std::string ticketId = app::s_or(body, "ticketId");
            if (ticketId.empty()) {
                app::reply_json(res, 400, {{"error", "ticketId required"}});
                return;
            }

            std::lock_guard<std::mutex> lk(mtx_);
            auto it = tickets_.find(ticketId);
            if (it == tickets_.end()) {
                app::reply_json(res, 404, {{"error", "ticket_not_found"}});
                return;
            }

            if (it->second.value("status", std::string()) != "active") {
                app::reply_json(res, 409, {{"error", "ticket_not_active"}});
                return;
            }

            const std::string checkInTime = body.value("checkInTime", to_iso_utc(app::now_sec()));
            it->second["checkInTime"] = checkInTime;

            app::reply_json(res, 200, {
                {"ok", true},
                {"ticketId", ticketId},
                {"checkInTime", checkInTime}
            });
        });

        svr.Post("/v1/tickets/buy", [&](const httplib::Request& req, httplib::Response& res) {
            auto bodyOpt = app::parse_json_body(req);
            if (!bodyOpt) {
                app::reply_json(res, 400, {{"error", "invalid json"}});
                return;
            }
            const auto& body = *bodyOpt;

            const std::string passengerId = app::s_or(body, "passengerId");
            const std::string passengerName = app::s_or(body, "passengerName");
            const std::string flightId = app::s_or(body, "flightId");

            if (passengerId.empty() || passengerName.empty() || flightId.empty()) {
                app::reply_json(res, 400, {{"error", "passengerId, passengerName and flightId are required"}});
                return;
            }

            auto flightRes = app::http_get_json(infoHost_, infoPort_, "/v1/flights/" + url_encode(flightId) + "?ts=" + std::to_string(app::now_sec()));
            if (!flightRes.ok()) {
                app::reply_json(res, 404, {{"error", "flight_not_found"}, {"flightId", flightId}});
                return;
            }
            const json flight = flightRes.body;

            const std::string flightStatus = flight.value("status", std::string(""));
            if (!is_purchase_allowed_status(flightStatus)) {
                app::reply_json(res, 409, {{"error", "flight_not_available_for_sale"}, {"status", flightStatus}});
                return;
            }

            if (requirePassengerValidation_) {
                auto passengerRes = app::http_get_json(passengerHost_, passengerPort_, "/v1/passengers/" + url_encode(passengerId));
                if (!passengerRes.ok()) {
                    app::reply_json(res, 404, {{"error", "passenger_not_found"}, {"passengerId", passengerId}});
                    return;
                }
                const std::string actualName = passengerRes.body.value("name", std::string(""));
                if (!actualName.empty() && actualName != passengerName) {
                    app::reply_json(res, 409, {
                        {"error", "passenger_name_mismatch"},
                        {"expected", actualName},
                        {"received", passengerName}
                    });
                    return;
                }
            }

            json ticket;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                if (activeByFlight_[flightId] >= maxTicketsPerFlight_) {
                    app::reply_json(res, 409, {
                        {"error", "flight_capacity_reached"},
                        {"flightId", flightId},
                        {"capacity", maxTicketsPerFlight_}
                    });
                    return;
                }

                ticket = json::object();
                ticket["ticketId"] = "TCK-" + random_uuid_like();
                ticket["flightId"] = flightId;
                ticket["passengerId"] = passengerId;
                ticket["passengerName"] = passengerName;
                ticket["isVIP"] = body.value("isVIP", false);
                ticket["menuType"] = body.value("menuType", std::string("chicken"));
                ticket["baggageWeight"] = body.value("baggageWeight", 0.0);
                ticket["hasBaggage"] = ticket["baggageWeight"].get<double>() > 0.0;
                ticket["status"] = "active";
                ticket["isFake"] = false;
                ticket["createdAt"] = to_iso_utc(app::now_sec());
                ticket["gate"] = flight.value("gate", json(nullptr));
                ticket["seatNumber"] = random_seat_number();
                ticket["flightDepartureTime"] = flight.value("scheduledTime", json(nullptr));
                ticket["boardingStartTime"] = to_iso_utc(flight.value("scheduledAt", app::now_sec()) - 40 * 60);
                ticket["checkInTime"] = nullptr;
                ticket["fromCity"] = flight.value("fromCity", json(nullptr));
                ticket["toCity"] = flight.value("toCity", json(nullptr));

                tickets_[ticket["ticketId"].get<std::string>()] = ticket;
                passengerToTickets_[passengerId].push_back(ticket["ticketId"].get<std::string>());
                activeByFlight_[flightId] += 1;
            }

            // Best-effort sync to check-in module
            (void)app::http_post_json(checkinHost_, checkinPort_, "/v1/checkin/tickets", {
                {"flightId", flightId},
                {"tickets", json::array({ticket})}
            });

            (void)app::http_post_json(reportingHost_, reportingPort_, "/v1/events", {
                {"eventId", random_uuid_like()},
                {"eventType", "ticket_sold"},
                {"flightId", flightId},
                {"passengerId", passengerId},
                {"count", 1}
            });

            app::reply_json(res, 200, ticket);
        });

        svr.Post("/v1/tickets/refund", [&](const httplib::Request& req, httplib::Response& res) {
            auto bodyOpt = app::parse_json_body(req);
            if (!bodyOpt) {
                app::reply_json(res, 400, {{"error", "invalid json"}});
                return;
            }

            const auto& body = *bodyOpt;
            const std::string ticketId = app::s_or(body, "ticketId");
            const std::string passengerId = app::s_or(body, "passengerId");
            if (ticketId.empty() || passengerId.empty()) {
                app::reply_json(res, 400, {{"error", "ticketId and passengerId required"}});
                return;
            }

            std::string flightId;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                auto it = tickets_.find(ticketId);
                if (it == tickets_.end()) {
                    app::reply_json(res, 404, {{"error", "ticket_not_found"}});
                    return;
                }

                if (it->second.value("passengerId", std::string()) != passengerId) {
                    app::reply_json(res, 409, {{"error", "passenger_mismatch"}});
                    return;
                }

                if (it->second.value("status", std::string()) == "returned") {
                    app::reply_json(res, 409, {{"error", "already_returned"}});
                    return;
                }

                it->second["status"] = "returned";
                it->second["returnedAt"] = to_iso_utc(app::now_sec());

                flightId = it->second.value("flightId", std::string(""));
                if (!flightId.empty() && activeByFlight_[flightId] > 0) {
                    activeByFlight_[flightId] -= 1;
                }
            }

            (void)app::http_post_json(reportingHost_, reportingPort_, "/v1/events", {
                {"eventId", random_uuid_like()},
                {"eventType", "ticket_refunded"},
                {"flightId", flightId},
                {"passengerId", passengerId},
                {"count", 1}
            });

            app::reply_json(res, 200, {
                {"ticketId", ticketId},
                {"status", "returned"}
            });
        });

        std::cout << "[TicketSales] listening on 0.0.0.0:" << port << '\n';
        svr.listen("0.0.0.0", port);
    }

private:
    std::mutex mtx_;
    std::unordered_map<std::string, json> tickets_;
    std::unordered_map<std::string, std::vector<std::string>> passengerToTickets_;
    std::unordered_map<std::string, int> activeByFlight_;

    std::string infoHost_;
    int infoPort_ = 8082;
    std::string passengerHost_;
    int passengerPort_ = 8004;
    std::string checkinHost_;
    int checkinPort_ = 8005;
    std::string reportingHost_;
    int reportingPort_ = 8092;
    bool requirePassengerValidation_ = true;
    int maxTicketsPerFlight_ = 100;
};

} // namespace

int main(int argc, char** argv) {
    int port = env_or_int("TICKETS_PORT", 8003);
    if (argc > 1) {
        try { port = std::stoi(argv[1]); } catch (...) {}
    }

    const std::string infoHost = env_or("TICKETS_INFO_HOST", "localhost");
    const int infoPort = env_or_int("TICKETS_INFO_PORT", 8082);

    const std::string passengerHost = env_or("TICKETS_PASSENGER_HOST", "localhost");
    const int passengerPort = env_or_int("TICKETS_PASSENGER_PORT", 8004);

    const std::string checkinHost = env_or("TICKETS_CHECKIN_HOST", "localhost");
    const int checkinPort = env_or_int("TICKETS_CHECKIN_PORT", 8005);

    const std::string reportingHost = env_or("TICKETS_REPORTING_HOST", env_or("REPORTING_HOST", "localhost"));
    const int reportingPort = env_or_int("TICKETS_REPORTING_PORT", env_or_int("REPORTING_PORT", 8092));

    const bool requirePassengerValidation = env_or_int("TICKETS_REQUIRE_PASSENGER_VALIDATION", 1) != 0;
    const int maxTicketsPerFlight = std::max(1, env_or_int("TICKETS_MAX_PER_FLIGHT", 100));

    TicketSalesService svc(
        infoHost,
        infoPort,
        passengerHost,
        passengerPort,
        checkinHost,
        checkinPort,
        reportingHost,
        reportingPort,
        requirePassengerValidation,
        maxTicketsPerFlight
    );

    svc.run(port);
    return 0;
}
