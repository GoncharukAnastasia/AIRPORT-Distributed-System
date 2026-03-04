#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <vector>
#include <iostream>
#include <algorithm>
#include <string>
#include <cstdlib>
#include <stdexcept>
#include <chrono>
#include <thread>
#include <atomic>
#include <random>
#include <sstream>
#include <iomanip>
#include <ctime>

#include <pqxx/pqxx>

#include "common.h"

using app::json;

namespace {

struct FlightRecord {
    std::string flightId;
    std::string planeId;
    std::string type = "depart";             // arrive | depart
    std::string fromCity;
    std::string toCity;
    int64_t scheduledAt = 0;                   // epoch seconds
    std::string scheduledTime;                 // ISO8601 (UTC)
    std::string arrivalTime;                   // ISO8601 (UTC)
    std::string status = "Scheduled";
    std::string gate;
    std::string planeParking;                  // PR1..PR5
    std::string runway;

    // Backward-compat fields consumed by GroundControl/Board
    std::string phase = "airborne";           // airborne | grounded | terminal
    std::string parkingNode;                   // P-1..P-5

    bool autoManage = false;                   // scheduler-controlled lifecycle
    int64_t updatedAt = 0;
};

static constexpr int kSeatsPerFlight = 100;

static std::string env_or(const char* key, const std::string& defVal) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : defVal;
}

static int env_or_int(const char* key, int defVal) {
    const char* v = std::getenv(key);
    if (!v || !*v) return defVal;
    try { return std::stoi(v); } catch (...) { return defVal; }
}

static bool is_terminal_status(const std::string& s) {
    return s == "Departed" || s == "Cancelled" || s == "Denied";
}

static bool is_ground_status(const std::string& s) {
    return s == "ArrivedParking" ||
           s == "Parked" ||
           s == "OnStand" ||
           s == "HandlingDone" ||
           s == "TaxiOutRequested" ||
           s == "ReadyForTakeoff" ||
           s == "TakeoffApproved" ||
           s == "RegistrationOpen" ||
           s == "RegistrationClosed" ||
           s == "Boarding" ||
           s == "Arrived";
}

static bool is_air_status(const std::string& s) {
    return s == "Scheduled" ||
           s == "Delayed" ||
           s == "LandingApproved" ||
           s == "Airborne" ||
           s == "LandedAtRE1";
}

static std::string infer_phase(const std::string& status,
                               const std::string& explicitPhase,
                               const std::string& type) {
    if (!explicitPhase.empty()) return explicitPhase;
    if (is_terminal_status(status)) return "terminal";
    if (is_ground_status(status)) return "grounded";
    if (is_air_status(status)) return "airborne";
    if (type == "arrive" && status == "Arrived") return "grounded";
    return "airborne";
}

static bool compute_takeoff_allowed(const FlightRecord& rec) {
    if (rec.phase != "grounded") return false;
    if (is_terminal_status(rec.status)) return false;
    if (rec.status == "Cancelled" || rec.status == "Delayed") return false;
    return true;
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

static int64_t parse_iso_utc(const std::string& s) {
    if (s.empty()) return 0;

    std::tm tm{};
    std::istringstream iss1(s);
    iss1 >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    if (!iss1.fail()) {
#if defined(_WIN32)
        return static_cast<int64_t>(_mkgmtime(&tm));
#else
        return static_cast<int64_t>(timegm(&tm));
#endif
    }

    std::istringstream iss2(s);
    tm = {};
    iss2 >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (!iss2.fail()) {
#if defined(_WIN32)
        return static_cast<int64_t>(_mkgmtime(&tm));
#else
        return static_cast<int64_t>(timegm(&tm));
#endif
    }

    return 0;
}

static std::string normalize_type(const std::string& typeRaw) {
    if (typeRaw == "arrive" || typeRaw == "depart") return typeRaw;
    return "depart";
}

static std::string default_gate_for_flight(const std::string& flightId) {
    const std::size_t idx = std::hash<std::string>{}(flightId) % 5 + 1;
    return "G" + std::to_string(idx);
}

static std::string default_parking_for_flight(const std::string& flightId) {
    const std::size_t idx = std::hash<std::string>{}(flightId + "parking") % 5 + 1;
    return "PR" + std::to_string(idx);
}

static std::string parking_node_from_plane_parking(const std::string& planeParking) {
    if (planeParking.size() == 3 && planeParking.rfind("PR", 0) == 0) {
        const char n = planeParking[2];
        if (n >= '1' && n <= '5') {
            return std::string("P-") + n;
        }
    }
    return "";
}

static std::string plane_parking_from_node(const std::string& parkingNode) {
    if (parkingNode.size() == 3 && parkingNode.rfind("P-", 0) == 0) {
        const char n = parkingNode[2];
        if (n >= '1' && n <= '5') {
            return std::string("PR") + n;
        }
    }
    return "";
}

static bool auto_manage_status(const std::string& status) {
    static const std::unordered_set<std::string> managed = {
        "Scheduled", "RegistrationOpen", "RegistrationClosed", "Boarding", "Arrived"
    };
    return managed.count(status) > 0;
}

static json to_public_json(const FlightRecord& rec, int64_t tsNow) {
    const bool expectedNowOrPast = rec.scheduledAt <= tsNow;

    return {
        {"flightId", rec.flightId},
        {"planeId", rec.planeId},
        {"type", rec.type},
        {"fromCity", rec.fromCity},
        {"toCity", rec.toCity},
        {"scheduledTime", rec.scheduledTime.empty() ? to_iso_utc(rec.scheduledAt) : rec.scheduledTime},
        {"scheduledAt", rec.scheduledAt},
        {"arrivalTime", rec.arrivalTime},
        {"status", rec.status},
        {"gate", rec.gate},
        {"planeParking", rec.planeParking},
        {"runway", rec.runway},
        {"phase", rec.phase},
        {"parkingNode", rec.parkingNode},
        {"expectedNowOrPast", expectedNowOrPast},
        {"takeoffAllowed", compute_takeoff_allowed(rec)},
        {"autoManage", rec.autoManage},
        {"capacity", kSeatsPerFlight},
        {"updatedAt", rec.updatedAt}
    };
}

static bool should_auto_manage(const FlightRecord& rec) {
    if (!rec.autoManage) return false;
    if (is_terminal_status(rec.status)) return false;
    if (rec.status == "Cancelled" || rec.status == "Denied") return false;
    if (!auto_manage_status(rec.status)) return false;
    return true;
}

static bool apply_scheduler_step(FlightRecord& rec, int64_t now) {
    if (!should_auto_manage(rec)) return false;

    std::string newStatus = rec.status;

    if (rec.type == "arrive") {
        newStatus = (now >= rec.scheduledAt) ? "Arrived" : "Scheduled";
    } else {
        const int64_t secToDeparture = rec.scheduledAt - now;

        if (secToDeparture > 40 * 60) {
            newStatus = "Scheduled";
        } else if (secToDeparture > 10 * 60) {
            newStatus = "RegistrationOpen";
        } else if (secToDeparture > 0) {
            newStatus = "RegistrationClosed";
        } else if (secToDeparture > -20 * 60) {
            newStatus = "Boarding";
        } else {
            newStatus = "Departed";
        }
    }

    bool changed = false;
    if (newStatus != rec.status) {
        rec.status = newStatus;
        changed = true;
    }

    if ((rec.status == "Boarding" || rec.status == "RegistrationClosed") && rec.gate.empty()) {
        rec.gate = default_gate_for_flight(rec.flightId);
        changed = true;
    }

    if (rec.phase.empty()) {
        rec.phase = infer_phase(rec.status, "", rec.type);
        changed = true;
    } else {
        const std::string inferred = infer_phase(rec.status, "", rec.type);
        if (inferred != rec.phase && auto_manage_status(rec.status)) {
            rec.phase = inferred;
            changed = true;
        }
    }

    if (rec.status == "Arrived" && rec.arrivalTime.empty()) {
        rec.arrivalTime = to_iso_utc(now);
        changed = true;
    }

    if (rec.phase == "grounded" && rec.planeParking.empty()) {
        rec.planeParking = default_parking_for_flight(rec.flightId);
        changed = true;
    }

    if (rec.parkingNode.empty() && !rec.planeParking.empty()) {
        rec.parkingNode = parking_node_from_plane_parking(rec.planeParking);
        changed = true;
    }

    if (changed) {
        rec.updatedAt = now;
    }

    return changed;
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
                flight_id      TEXT PRIMARY KEY,
                plane_id       TEXT NOT NULL DEFAULT '',
                flight_type    TEXT NOT NULL DEFAULT 'depart',
                from_city      TEXT NOT NULL DEFAULT '',
                to_city        TEXT NOT NULL DEFAULT '',
                scheduled_at   BIGINT NOT NULL,
                scheduled_time TEXT NOT NULL DEFAULT '',
                arrival_time   TEXT NOT NULL DEFAULT '',
                status         TEXT NOT NULL,
                gate           TEXT NOT NULL DEFAULT '',
                plane_parking  TEXT NOT NULL DEFAULT '',
                runway         TEXT NOT NULL DEFAULT '',
                phase          TEXT NOT NULL,
                parking_node   TEXT NULL,
                auto_manage    BOOLEAN NOT NULL DEFAULT FALSE,
                updated_at     BIGINT NOT NULL,
                CONSTRAINT chk_info_flights_phase CHECK (phase IN ('airborne', 'grounded', 'terminal')),
                CONSTRAINT chk_info_flights_type CHECK (flight_type IN ('arrive', 'depart'))
            );
        )sql");

        // Backward-compatible migrations from older schema.
        tx.exec(R"sql(ALTER TABLE info_flights ADD COLUMN IF NOT EXISTS plane_id TEXT NOT NULL DEFAULT '';)sql");
        tx.exec(R"sql(ALTER TABLE info_flights ADD COLUMN IF NOT EXISTS flight_type TEXT NOT NULL DEFAULT 'depart';)sql");
        tx.exec(R"sql(ALTER TABLE info_flights ADD COLUMN IF NOT EXISTS from_city TEXT NOT NULL DEFAULT '';)sql");
        tx.exec(R"sql(ALTER TABLE info_flights ADD COLUMN IF NOT EXISTS to_city TEXT NOT NULL DEFAULT '';)sql");
        tx.exec(R"sql(ALTER TABLE info_flights ADD COLUMN IF NOT EXISTS scheduled_time TEXT NOT NULL DEFAULT '';)sql");
        tx.exec(R"sql(ALTER TABLE info_flights ADD COLUMN IF NOT EXISTS arrival_time TEXT NOT NULL DEFAULT '';)sql");
        tx.exec(R"sql(ALTER TABLE info_flights ADD COLUMN IF NOT EXISTS gate TEXT NOT NULL DEFAULT '';)sql");
        tx.exec(R"sql(ALTER TABLE info_flights ADD COLUMN IF NOT EXISTS plane_parking TEXT NOT NULL DEFAULT '';)sql");
        tx.exec(R"sql(ALTER TABLE info_flights ADD COLUMN IF NOT EXISTS runway TEXT NOT NULL DEFAULT '';)sql");
        tx.exec(R"sql(ALTER TABLE info_flights ADD COLUMN IF NOT EXISTS auto_manage BOOLEAN NOT NULL DEFAULT FALSE;)sql");

        tx.exec(R"sql(CREATE INDEX IF NOT EXISTS idx_info_flights_status ON info_flights(status);)sql");
        tx.exec(R"sql(CREATE INDEX IF NOT EXISTS idx_info_flights_phase ON info_flights(phase);)sql");
        tx.exec(R"sql(CREATE INDEX IF NOT EXISTS idx_info_flights_type ON info_flights(flight_type);)sql");
        tx.exec(R"sql(CREATE INDEX IF NOT EXISTS idx_info_flights_scheduled_at ON info_flights(scheduled_at);)sql");

        tx.commit();
    }

    std::vector<FlightRecord> load_all() {
        pqxx::connection cx(connStr_);
        pqxx::work tx(cx);

        pqxx::result r = tx.exec(R"sql(
            SELECT
                flight_id,
                COALESCE(plane_id, '') AS plane_id,
                COALESCE(flight_type, 'depart') AS flight_type,
                COALESCE(from_city, '') AS from_city,
                COALESCE(to_city, '') AS to_city,
                scheduled_at,
                COALESCE(scheduled_time, '') AS scheduled_time,
                COALESCE(arrival_time, '') AS arrival_time,
                status,
                COALESCE(gate, '') AS gate,
                COALESCE(plane_parking, '') AS plane_parking,
                COALESCE(runway, '') AS runway,
                phase,
                COALESCE(parking_node, '') AS parking_node,
                CASE WHEN COALESCE(auto_manage, FALSE) THEN 1 ELSE 0 END AS auto_manage_i,
                updated_at
            FROM info_flights
            ORDER BY flight_id
        )sql");

        std::vector<FlightRecord> out;
        out.reserve(r.size());

        for (const auto& row : r) {
            FlightRecord rec;
            rec.flightId = row["flight_id"].as<std::string>();
            rec.planeId = row["plane_id"].as<std::string>();
            rec.type = normalize_type(row["flight_type"].as<std::string>());
            rec.fromCity = row["from_city"].as<std::string>();
            rec.toCity = row["to_city"].as<std::string>();
            rec.scheduledAt = row["scheduled_at"].as<int64_t>();
            rec.scheduledTime = row["scheduled_time"].as<std::string>();
            rec.arrivalTime = row["arrival_time"].as<std::string>();
            rec.status = row["status"].as<std::string>();
            rec.gate = row["gate"].as<std::string>();
            rec.planeParking = row["plane_parking"].as<std::string>();
            rec.runway = row["runway"].as<std::string>();
            rec.phase = row["phase"].as<std::string>();
            rec.parkingNode = row["parking_node"].as<std::string>();
            rec.autoManage = row["auto_manage_i"].as<int>() != 0;
            rec.updatedAt = row["updated_at"].as<int64_t>();

            if (rec.scheduledTime.empty()) {
                rec.scheduledTime = to_iso_utc(rec.scheduledAt);
            }
            if (rec.planeParking.empty() && !rec.parkingNode.empty()) {
                rec.planeParking = plane_parking_from_node(rec.parkingNode);
            }
            if (rec.parkingNode.empty() && !rec.planeParking.empty()) {
                rec.parkingNode = parking_node_from_plane_parking(rec.planeParking);
            }

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
            const std::string autoManageStr = rec.autoManage ? "true" : "false";
            tx.exec_params(R"sql(
                INSERT INTO info_flights (
                    flight_id,
                    plane_id,
                    flight_type,
                    from_city,
                    to_city,
                    scheduled_at,
                    scheduled_time,
                    arrival_time,
                    status,
                    gate,
                    plane_parking,
                    runway,
                    phase,
                    parking_node,
                    auto_manage,
                    updated_at
                )
                VALUES (
                    $1, $2, $3, $4, $5,
                    $6, $7, $8, $9, $10,
                    $11, $12, $13, NULLIF($14, ''), $15::boolean, $16
                )
                ON CONFLICT (flight_id) DO UPDATE SET
                    plane_id = EXCLUDED.plane_id,
                    flight_type = EXCLUDED.flight_type,
                    from_city = EXCLUDED.from_city,
                    to_city = EXCLUDED.to_city,
                    scheduled_at = EXCLUDED.scheduled_at,
                    scheduled_time = EXCLUDED.scheduled_time,
                    arrival_time = EXCLUDED.arrival_time,
                    status = EXCLUDED.status,
                    gate = EXCLUDED.gate,
                    plane_parking = EXCLUDED.plane_parking,
                    runway = EXCLUDED.runway,
                    phase = EXCLUDED.phase,
                    parking_node = EXCLUDED.parking_node,
                    auto_manage = EXCLUDED.auto_manage,
                    updated_at = EXCLUDED.updated_at
            )sql",
                rec.flightId,
                rec.planeId,
                rec.type,
                rec.fromCity,
                rec.toCity,
                rec.scheduledAt,
                rec.scheduledTime,
                rec.arrivalTime,
                rec.status,
                rec.gate,
                rec.planeParking,
                rec.runway,
                rec.phase,
                rec.parkingNode,
                autoManageStr,
                rec.updatedAt
            );
        }

        tx.commit();
    }

    void upsert_one(const FlightRecord& rec) {
        std::vector<FlightRecord> v{rec};
        upsert_many(v);
    }

private:
    std::string connStr_;
};

} // namespace

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

        const int schedulerPeriodSec = std::max(1, env_or_int("INFO_SCHEDULER_PERIOD_SEC", 5));

        FlightsRepository repo(pgConn);
        repo.ensure_schema();

        std::unordered_map<std::string, FlightRecord> flights;
        std::mutex mtx;

        // Load from DB at startup
        {
            auto loaded = repo.load_all();
            std::lock_guard<std::mutex> lk(mtx);
            for (const auto& rec : loaded) {
                flights[rec.flightId] = rec;
            }
            std::cout << "[InformationPanel][DB] loaded " << loaded.size() << " flights\n";
        }

        std::atomic<bool> stopScheduler{false};
        std::thread scheduler([&]() {
            while (!stopScheduler.load()) {
                std::this_thread::sleep_for(std::chrono::seconds(schedulerPeriodSec));

                std::vector<FlightRecord> changed;
                const int64_t now = app::now_sec();

                {
                    std::lock_guard<std::mutex> lk(mtx);
                    for (auto& [id, rec] : flights) {
                        (void)id;
                        if (apply_scheduler_step(rec, now)) {
                            changed.push_back(rec);
                        }
                    }
                }

                if (!changed.empty()) {
                    try {
                        repo.upsert_many(changed);
                    } catch (const std::exception& e) {
                        std::cerr << "[InformationPanel] scheduler persist error: " << e.what() << "\n";
                    }
                }
            }
        });

        httplib::Server svr;

        svr.Get("/health", [&](const httplib::Request&, httplib::Response& res) {
            app::reply_json(res, 200, {
                {"service", "InformationPanel"},
                {"status", "ok"},
                {"time", app::now_sec()}
            });
        });

        // INIT flights (backward-compatible + extended)
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
                rec.planeId = f.value("planeId", std::string(""));
                rec.type = normalize_type(f.value("type", std::string("depart")));

                rec.fromCity = f.value("fromCity", std::string(""));
                rec.toCity = f.value("toCity", std::string(""));

                rec.scheduledAt = f.value("scheduledAt", int64_t(0));
                rec.scheduledTime = f.value("scheduledTime", std::string(""));
                if (rec.scheduledAt == 0 && !rec.scheduledTime.empty()) {
                    rec.scheduledAt = parse_iso_utc(rec.scheduledTime);
                }
                if (rec.scheduledAt == 0) {
                    rec.scheduledAt = app::now_sec();
                }
                if (rec.scheduledTime.empty()) {
                    rec.scheduledTime = to_iso_utc(rec.scheduledAt);
                }

                rec.arrivalTime = f.value("arrivalTime", std::string(""));
                rec.status = f.value("status", std::string("Scheduled"));
                rec.gate = f.value("gate", std::string(""));
                rec.planeParking = f.value("planeParking", std::string(""));
                rec.runway = f.value("runway", std::string(""));

                const std::string explicitPhase = f.value("phase", std::string(""));
                rec.phase = infer_phase(rec.status, explicitPhase, rec.type);

                rec.parkingNode = f.value("parkingNode", std::string(""));
                if (rec.parkingNode.empty() && !rec.planeParking.empty()) {
                    rec.parkingNode = parking_node_from_plane_parking(rec.planeParking);
                }
                if (rec.planeParking.empty() && !rec.parkingNode.empty()) {
                    rec.planeParking = plane_parking_from_node(rec.parkingNode);
                }

                if (rec.phase == "grounded" && rec.parkingNode.empty()) {
                    warnings.push_back({{"flightId", flightId}, {"reason", "grounded_without_parkingNode"}});
                }

                if (f.contains("autoManage") && f["autoManage"].is_boolean()) {
                    rec.autoManage = f["autoManage"].get<bool>();
                } else {
                    // If caller explicitly uses extended fields (type/from/to), assume lifecycle management.
                    rec.autoManage = f.contains("type") || f.contains("fromCity") || f.contains("toCity");
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

            auto payload = to_public_json(it->second, ts);
            payload["known"] = true;
            app::reply_json(res, 200, payload);
        });

        // Manual override PATCH /v1/flights/{flightId}
        svr.Patch(R"(/v1/flights/([A-Za-z0-9\-_]+))", [&](const httplib::Request& req, httplib::Response& res) {
            auto bodyOpt = app::parse_json_body(req);
            if (!bodyOpt) {
                app::reply_json(res, 400, {{"error", "invalid json"}});
                return;
            }
            const auto& body = *bodyOpt;
            const std::string flightId = req.matches[1];

            FlightRecord persisted;
            try {
                std::lock_guard<std::mutex> lk(mtx);
                auto it = flights.find(flightId);
                if (it == flights.end()) {
                    app::reply_json(res, 404, {{"error", "flight_not_found"}, {"flightId", flightId}});
                    return;
                }

                auto& rec = it->second;

                if (body.contains("planeId") && body["planeId"].is_string()) rec.planeId = body["planeId"].get<std::string>();
                if (body.contains("type") && body["type"].is_string()) rec.type = normalize_type(body["type"].get<std::string>());
                if (body.contains("fromCity") && body["fromCity"].is_string()) rec.fromCity = body["fromCity"].get<std::string>();
                if (body.contains("toCity") && body["toCity"].is_string()) rec.toCity = body["toCity"].get<std::string>();

                if (body.contains("scheduledAt") && body["scheduledAt"].is_number_integer()) {
                    rec.scheduledAt = body["scheduledAt"].get<int64_t>();
                    rec.scheduledTime = to_iso_utc(rec.scheduledAt);
                }
                if (body.contains("scheduledTime") && body["scheduledTime"].is_string()) {
                    const std::string st = body["scheduledTime"].get<std::string>();
                    const int64_t parsed = parse_iso_utc(st);
                    if (parsed == 0) {
                        app::reply_json(res, 400, {{"error", "invalid_scheduledTime"}});
                        return;
                    }
                    rec.scheduledTime = st;
                    rec.scheduledAt = parsed;
                }

                if (body.contains("arrivalTime")) {
                    if (body["arrivalTime"].is_null()) rec.arrivalTime.clear();
                    else if (body["arrivalTime"].is_string()) rec.arrivalTime = body["arrivalTime"].get<std::string>();
                }

                if (body.contains("status") && body["status"].is_string()) rec.status = body["status"].get<std::string>();
                if (body.contains("gate")) {
                    if (body["gate"].is_null()) rec.gate.clear();
                    else if (body["gate"].is_string()) rec.gate = body["gate"].get<std::string>();
                }
                if (body.contains("planeParking")) {
                    if (body["planeParking"].is_null()) rec.planeParking.clear();
                    else if (body["planeParking"].is_string()) rec.planeParking = body["planeParking"].get<std::string>();
                }
                if (body.contains("runway")) {
                    if (body["runway"].is_null()) rec.runway.clear();
                    else if (body["runway"].is_string()) rec.runway = body["runway"].get<std::string>();
                }

                if (body.contains("parkingNode")) {
                    if (body["parkingNode"].is_null()) rec.parkingNode.clear();
                    else if (body["parkingNode"].is_string()) rec.parkingNode = body["parkingNode"].get<std::string>();
                }

                if (body.contains("phase") && body["phase"].is_string()) {
                    rec.phase = body["phase"].get<std::string>();
                } else {
                    rec.phase = infer_phase(rec.status, "", rec.type);
                }

                if (rec.parkingNode.empty() && !rec.planeParking.empty()) {
                    rec.parkingNode = parking_node_from_plane_parking(rec.planeParking);
                }
                if (rec.planeParking.empty() && !rec.parkingNode.empty()) {
                    rec.planeParking = plane_parking_from_node(rec.parkingNode);
                }

                if (body.contains("autoManage") && body["autoManage"].is_boolean()) {
                    rec.autoManage = body["autoManage"].get<bool>();
                } else {
                    // PATCH is explicit override by operator.
                    rec.autoManage = false;
                }

                rec.updatedAt = app::now_sec();
                persisted = rec;

                repo.upsert_one(persisted);
            } catch (const std::exception& e) {
                app::reply_json(res, 500, {{"error", "db_error"}, {"message", e.what()}});
                return;
            }

            app::reply_json(res, 200, to_public_json(persisted, app::now_sec()));
        });

        // Update status (backward-compatible endpoint used by GroundControl)
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
                    rec.scheduledTime = to_iso_utc(rec.scheduledAt);
                    rec.status = status;
                    rec.type = normalize_type(body.value("type", std::string("depart")));
                    rec.fromCity = body.value("fromCity", std::string(""));
                    rec.toCity = body.value("toCity", std::string(""));
                    rec.gate = body.value("gate", std::string(""));
                    rec.planeParking = body.value("planeParking", std::string(""));
                    rec.runway = body.value("runway", std::string(""));
                    rec.parkingNode = body.value("parkingNode", std::string(""));

                    if (rec.parkingNode.empty() && !rec.planeParking.empty()) {
                        rec.parkingNode = parking_node_from_plane_parking(rec.planeParking);
                    }
                    if (rec.planeParking.empty() && !rec.parkingNode.empty()) {
                        rec.planeParking = plane_parking_from_node(rec.parkingNode);
                    }

                    const std::string explicitPhase = body.value("phase", std::string(""));
                    rec.phase = infer_phase(status, explicitPhase, rec.type);
                    rec.updatedAt = app::now_sec();
                    rec.autoManage = false;
                    flights[flightId] = rec;
                } else {
                    auto& rec = it->second;
                    rec.status = status;
                    if (body.contains("type") && body["type"].is_string()) rec.type = normalize_type(body["type"].get<std::string>());
                    if (body.contains("fromCity") && body["fromCity"].is_string()) rec.fromCity = body["fromCity"].get<std::string>();
                    if (body.contains("toCity") && body["toCity"].is_string()) rec.toCity = body["toCity"].get<std::string>();
                    if (body.contains("gate") && body["gate"].is_string()) rec.gate = body["gate"].get<std::string>();
                    if (body.contains("runway") && body["runway"].is_string()) rec.runway = body["runway"].get<std::string>();
                    if (body.contains("planeParking") && body["planeParking"].is_string()) rec.planeParking = body["planeParking"].get<std::string>();
                    if (body.contains("parkingNode") && body["parkingNode"].is_string()) rec.parkingNode = body["parkingNode"].get<std::string>();

                    if (rec.parkingNode.empty() && !rec.planeParking.empty()) {
                        rec.parkingNode = parking_node_from_plane_parking(rec.planeParking);
                    }
                    if (rec.planeParking.empty() && !rec.parkingNode.empty()) {
                        rec.planeParking = plane_parking_from_node(rec.parkingNode);
                    }

                    const std::string explicitPhase = body.value("phase", std::string(""));
                    rec.phase = infer_phase(status, explicitPhase, rec.type);
                    rec.updatedAt = app::now_sec();

                    if (body.contains("autoManage") && body["autoManage"].is_boolean()) {
                        rec.autoManage = body["autoManage"].get<bool>();
                    } else if (!auto_manage_status(status)) {
                        rec.autoManage = false;
                    }
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

        // List flights (backward-compatible wrapper { flights: [...] })
        svr.Get("/v1/flights", [&](const httplib::Request& req, httplib::Response& res) {
            int64_t ts = app::now_sec();
            if (req.has_param("ts")) {
                try { ts = std::stoll(req.get_param_value("ts")); } catch (...) { ts = app::now_sec(); }
            }

            std::string typeFilter;
            if (req.has_param("type")) {
                const std::string rawType = req.get_param_value("type");
                if (rawType != "arrive" && rawType != "depart") {
                    app::reply_json(res, 400, {{"error", "type must be 'arrive' or 'depart'"}});
                    return;
                }
                typeFilter = rawType;
            }
            const std::string statusFilter = req.has_param("status") ? req.get_param_value("status") : "";

            json arr = json::array();
            std::lock_guard<std::mutex> lk(mtx);
            for (const auto& [id, rec] : flights) {
                (void)id;
                if (!typeFilter.empty() && rec.type != typeFilter) continue;
                if (!statusFilter.empty() && rec.status != statusFilter) continue;
                arr.push_back(to_public_json(rec, ts));
            }
            app::reply_json(res, 200, {{"flights", arr}});
        });

        std::cout << "[InformationPanel] listening on 0.0.0.0:" << port << '\n';
        svr.listen("0.0.0.0", port);

        stopScheduler.store(true);
        if (scheduler.joinable()) scheduler.join();
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "[InformationPanel] fatal: " << e.what() << '\n';
        return 1;
    }
}
