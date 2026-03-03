#include <unordered_map>
#include <mutex>
#include <vector>
#include <iostream>
#include <algorithm>
#include <string>
#include <cstdlib>
#include <stdexcept>

#include <pqxx/pqxx>

#include "common.h"

using app::json;

struct FlightRecord {
    std::string flightId;
    int64_t scheduledAt = 0;             // epoch seconds
    std::string status = "Scheduled";    // Scheduled, Delayed, LandingApproved, Landed, ArrivedParking, Parked, TakeoffApproved, Departed, Cancelled, Denied...
    std::string phase = "airborne";      // airborne | grounded | terminal
    std::string parkingNode;             // e.g. P-5 when grounded (optional)
    int64_t updatedAt = 0;
};

static bool is_terminal_status(const std::string& s) {
    return s == "Departed" || s == "Cancelled" || s == "Denied";
}

static bool is_ground_status(const std::string& s) {
    return s == "ArrivedParking" || s == "Parked" || s == "OnStand";
}

static bool is_air_status(const std::string& s) {
    return s == "Scheduled" || s == "Delayed" || s == "LandingApproved" || s == "Airborne" || s == "LandedAtRE1";
}

static std::string infer_phase(const std::string& status, const std::string& explicitPhase) {
    if (!explicitPhase.empty()) return explicitPhase;
    if (is_terminal_status(status)) return "terminal";
    if (is_ground_status(status)) return "grounded";
    if (is_air_status(status)) return "airborne";
    return "airborne";
}

static bool compute_takeoff_allowed(const FlightRecord& rec) {
    if (rec.phase != "grounded") return false;
    if (is_terminal_status(rec.status)) return false;
    return true;
}

class FlightsRepository {
public:
    explicit FlightsRepository(std::string connStr)
        : connStr_(std::move(connStr)) {}

    void ensure_schema() {
        pqxx::connection cx(connStr_);
        pqxx::work tx(cx);

        tx.exec(R"sql(
            CREATE TABLE IF NOT EXISTS info_flights (
                flight_id     TEXT PRIMARY KEY,
                scheduled_at  BIGINT NOT NULL,
                status        TEXT NOT NULL,
                phase         TEXT NOT NULL,
                parking_node  TEXT NULL,
                updated_at    BIGINT NOT NULL,
                CONSTRAINT chk_info_flights_phase CHECK (phase IN ('airborne', 'grounded', 'terminal'))
            );
        )sql");

        tx.exec(R"sql(
            CREATE INDEX IF NOT EXISTS idx_info_flights_status ON info_flights(status);
        )sql");
        tx.exec(R"sql(
            CREATE INDEX IF NOT EXISTS idx_info_flights_phase ON info_flights(phase);
        )sql");
        tx.exec(R"sql(
            CREATE INDEX IF NOT EXISTS idx_info_flights_scheduled_at ON info_flights(scheduled_at);
        )sql");

        tx.commit();
    }

    std::vector<FlightRecord> load_all() {
        pqxx::connection cx(connStr_);
        pqxx::work tx(cx);

        pqxx::result r = tx.exec(R"sql(
            SELECT
                flight_id,
                scheduled_at,
                status,
                phase,
                COALESCE(parking_node, '') AS parking_node,
                updated_at
            FROM info_flights
            ORDER BY flight_id
        )sql");

        std::vector<FlightRecord> out;
        out.reserve(r.size());

        for (const auto& row : r) {
            FlightRecord rec;
            rec.flightId = row["flight_id"].as<std::string>();
            rec.scheduledAt = row["scheduled_at"].as<int64_t>();
            rec.status = row["status"].as<std::string>();
            rec.phase = row["phase"].as<std::string>();
            rec.parkingNode = row["parking_node"].as<std::string>();
            rec.updatedAt = row["updated_at"].as<int64_t>();
            out.push_back(std::move(rec));
        }

        tx.commit();
        return out;
    }

    void upsert_many(const std::vector<FlightRecord>& flights) {
        if (flights.empty()) return;

        pqxx::connection cx(connStr_);
        pqxx::work tx(cx);

        for (const auto& rec : flights) {
            tx.exec_params(R"sql(
                INSERT INTO info_flights (flight_id, scheduled_at, status, phase, parking_node, updated_at)
                VALUES ($1, $2, $3, $4, NULLIF($5, ''), $6)
                ON CONFLICT (flight_id) DO UPDATE SET
                    scheduled_at = EXCLUDED.scheduled_at,
                    status = EXCLUDED.status,
                    phase = EXCLUDED.phase,
                    parking_node = EXCLUDED.parking_node,
                    updated_at = EXCLUDED.updated_at
            )sql",
                rec.flightId,
                rec.scheduledAt,
                rec.status,
                rec.phase,
                rec.parkingNode,
                rec.updatedAt
            );
        }

        tx.commit();
    }

    void upsert_one(const FlightRecord& rec) {
        pqxx::connection cx(connStr_);
        pqxx::work tx(cx);

        tx.exec_params(R"sql(
            INSERT INTO info_flights (flight_id, scheduled_at, status, phase, parking_node, updated_at)
            VALUES ($1, $2, $3, $4, NULLIF($5, ''), $6)
            ON CONFLICT (flight_id) DO UPDATE SET
                scheduled_at = EXCLUDED.scheduled_at,
                status = EXCLUDED.status,
                phase = EXCLUDED.phase,
                parking_node = EXCLUDED.parking_node,
                updated_at = EXCLUDED.updated_at
        )sql",
            rec.flightId,
            rec.scheduledAt,
            rec.status,
            rec.phase,
            rec.parkingNode,
            rec.updatedAt
        );

        tx.commit();
    }

private:
    std::string connStr_;
};

static std::string env_or(const char* key, const std::string& defVal) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : defVal;
}

int main(int argc, char** argv) {
    try {
        int port = 8082;
        if (argc > 1) {
            port = std::stoi(argv[1]);
        }

        std::string pgConn = env_or("FM_PG_DSN", "");
        if (pgConn.empty()) {
            const std::string host = env_or("FM_PG_HOST", "127.0.0.1");
            const std::string pgPort = env_or("FM_PG_PORT", "5432");
            const std::string db = env_or("FM_PG_DB", "airport");
            const std::string user = env_or("FM_PG_USER", "airport_user");
            const std::string pass = env_or("FM_PG_PASSWORD", "airport_pass");

            pgConn =
                "host=" + host +
                " port=" + pgPort +
                " dbname=" + db +
                " user=" + user +
                " password=" + pass;
        }

        FlightsRepository repo(pgConn);
        repo.ensure_schema();

        std::unordered_map<std::string, FlightRecord> flights;
        std::mutex mtx;

        // загрузка из БД при старте
        {
            auto loaded = repo.load_all();
            std::lock_guard<std::mutex> lk(mtx);
            for (const auto& rec : loaded) {
                flights[rec.flightId] = rec;
            }
            std::cout << "[InformationPanel][DB] loaded " << loaded.size() << " flights\n";
        }

        httplib::Server svr;

        svr.Get("/health", [&](const httplib::Request&, httplib::Response& res) {
            app::reply_json(res, 200, {
                {"service", "InformationPanel"},
                {"status", "ok"},
                {"time", app::now_sec()}
            });
        });

        // INIT flights
        svr.Post("/v1/flights/init", [&](const httplib::Request& req, httplib::Response& res) {
            auto bodyOpt = app::parse_json_body(req);
            if (!bodyOpt) {
                app::reply_json(res, 400, {{"error", "invalid json"}});
                return;
            }
            const auto& body = *bodyOpt;

            if (!body.contains("flights") || !body["flights"].is_array()) {
                app::reply_json(res, 400, {{"error", "field 'flights' array is required"}});
                return;
            }

            int count = 0;
            json warnings = json::array();
            std::vector<FlightRecord> batch;
            batch.reserve(body["flights"].size());

            for (const auto& f : body["flights"]) {
                const std::string flightId = app::s_or(f, "flightId");
                if (flightId.empty()) {
                    warnings.push_back({{"reason", "empty_flightId"}});
                    continue;
                }

                FlightRecord rec;
                rec.flightId = flightId;
                rec.scheduledAt = f.value("scheduledAt", app::now_sec());
                rec.status = f.value("status", std::string("Scheduled"));

                const std::string explicitPhase = f.value("phase", std::string(""));
                rec.phase = infer_phase(rec.status, explicitPhase);

                rec.parkingNode = f.value("parkingNode", std::string(""));

                if (rec.phase == "grounded" && rec.parkingNode.empty()) {
                    warnings.push_back({{"flightId", flightId}, {"reason", "grounded_without_parkingNode"}});
                }

                rec.updatedAt = app::now_sec();
                batch.push_back(std::move(rec));
                ++count;
            }

            try {
                repo.upsert_many(batch);

                std::lock_guard<std::mutex> lk(mtx);
                for (const auto& rec : batch) {
                    flights[rec.flightId] = rec;
                }
            } catch (const std::exception& e) {
                app::reply_json(res, 500, {
                    {"error", "db_error"},
                    {"message", e.what()}
                });
                return;
            }

            app::reply_json(res, 200, {
                {"ok", true},
                {"initialized", count},
                {"warnings", warnings}
            });
        });

        // GET single flight
        svr.Get(R"(/v1/flights/([A-Za-z0-9\-_]+))", [&](const httplib::Request& req, httplib::Response& res) {
            const std::string flightId = req.matches[1];

            int64_t ts = app::now_sec();
            if (req.has_param("ts")) {
                try { ts = std::stoll(req.get_param_value("ts")); } catch (...) { ts = app::now_sec(); }
            }

            std::lock_guard<std::mutex> lk(mtx);
            auto it = flights.find(flightId);
            if (it == flights.end()) {
                app::reply_json(res, 404, {
                    {"flightId", flightId},
                    {"known", false},
                    {"expectedNowOrPast", false},
                    {"reason", "unknown_flight"}
                });
                return;
            }

            const auto& rec = it->second;
            const bool expectedNowOrPast = rec.scheduledAt <= ts;

            app::reply_json(res, 200, {
                {"flightId", rec.flightId},
                {"known", true},
                {"scheduledAt", rec.scheduledAt},
                {"status", rec.status},
                {"phase", rec.phase},
                {"parkingNode", rec.parkingNode},
                {"expectedNowOrPast", expectedNowOrPast},
                {"takeoffAllowed", compute_takeoff_allowed(rec)},
                {"updatedAt", rec.updatedAt}
            });
        });

        // Update status
        svr.Post("/v1/flights/status", [&](const httplib::Request& req, httplib::Response& res) {
            auto bodyOpt = app::parse_json_body(req);
            if (!bodyOpt) {
                app::reply_json(res, 400, {{"error", "invalid json"}});
                return;
            }

            const auto& body = *bodyOpt;
            const std::string flightId = app::s_or(body, "flightId");
            const std::string status = app::s_or(body, "status");
            const std::string reason = app::s_or(body, "reason");

            const std::string explicitPhase = body.value("phase", std::string(""));
            const std::string parkingNode = body.value("parkingNode", std::string(""));

            if (flightId.empty() || status.empty()) {
                app::reply_json(res, 400, {{"error", "flightId and status required"}});
                return;
            }

            FlightRecord persisted;
            try {
                std::lock_guard<std::mutex> lk(mtx);
                auto it = flights.find(flightId);
                if (it == flights.end()) {
                    FlightRecord rec;
                    rec.flightId = flightId;
                    rec.scheduledAt = app::now_sec();
                    rec.status = status;
                    rec.phase = infer_phase(status, explicitPhase);
                    rec.parkingNode = parkingNode;
                    rec.updatedAt = app::now_sec();
                    flights[flightId] = rec;
                } else {
                    it->second.status = status;
                    it->second.phase = infer_phase(status, explicitPhase);
                    if (!parkingNode.empty()) {
                        it->second.parkingNode = parkingNode;
                    }
                    it->second.updatedAt = app::now_sec();
                }

                persisted = flights[flightId];
                repo.upsert_one(persisted);
            } catch (const std::exception& e) {
                app::reply_json(res, 500, {
                    {"error", "db_error"},
                    {"message", e.what()}
                });
                return;
            }

            app::reply_json(res, 200, {
                {"ok", true},
                {"flightId", flightId},
                {"status", persisted.status},
                {"phase", persisted.phase},
                {"parkingNode", persisted.parkingNode},
                {"reason", reason}
            });
        });

        // List flights
        svr.Get("/v1/flights", [&](const httplib::Request& req, httplib::Response& res) {
            int64_t ts = app::now_sec();
            if (req.has_param("ts")) {
                try { ts = std::stoll(req.get_param_value("ts")); } catch (...) { ts = app::now_sec(); }
            }

            json arr = json::array();
            std::lock_guard<std::mutex> lk(mtx);
            for (const auto& [id, rec] : flights) {
                (void)id;
                const bool expectedNowOrPast = rec.scheduledAt <= ts;
                arr.push_back({
                    {"flightId", rec.flightId},
                    {"scheduledAt", rec.scheduledAt},
                    {"status", rec.status},
                    {"phase", rec.phase},
                    {"parkingNode", rec.parkingNode},
                    {"expectedNowOrPast", expectedNowOrPast},
                    {"takeoffAllowed", compute_takeoff_allowed(rec)},
                    {"updatedAt", rec.updatedAt}
                });
            }
            app::reply_json(res, 200, {{"flights", arr}});
        });

        std::cout << "[InformationPanel] listening on 0.0.0.0:" << port << '\n';
        svr.listen("0.0.0.0", port);
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "[InformationPanel] fatal: " << e.what() << '\n';
        return 1;
    }
}