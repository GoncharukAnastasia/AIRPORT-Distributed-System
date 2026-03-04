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

struct Vehicle {
    std::string vehicleId;
    std::string status = "empty"; // empty, reserved, moveToLandingPosition, movingWithPlane, returning
    std::string currentNode = "FS-1";
    std::string flightId;
    std::string missionType; // "", "landing", "takeoff"
};

class VehicleRepository {
public:
    explicit VehicleRepository(std::string connStr)
        : connStr_(std::move(connStr)) {}

    void ensure_schema() {
        pqxx::connection cx(connStr_);
        pqxx::work tx(cx);

        tx.exec(R"sql(
            CREATE TABLE IF NOT EXISTS followme_vehicles (
                vehicle_id   TEXT PRIMARY KEY,
                current_node TEXT NOT NULL DEFAULT 'FS-1',
                status       TEXT NOT NULL DEFAULT 'empty',
                flight_id    TEXT NULL,
                updated_at   TIMESTAMPTZ NOT NULL DEFAULT NOW(),
                CONSTRAINT chk_followme_status CHECK (
                    status IN ('empty', 'reserved', 'moveToLandingPosition', 'movingWithPlane', 'returning')
                )
            );
        )sql");

        tx.exec(R"sql(
            CREATE INDEX IF NOT EXISTS idx_followme_status ON followme_vehicles(status);
        )sql");

        tx.commit();
    }

    std::vector<Vehicle> load_all() {
        pqxx::connection cx(connStr_);
        pqxx::work tx(cx);

        pqxx::result r = tx.exec(R"sql(
            SELECT
                vehicle_id,
                current_node,
                status,
                COALESCE(flight_id, '') AS flight_id
            FROM followme_vehicles
            ORDER BY vehicle_id
        )sql");

        std::vector<Vehicle> out;
        out.reserve(r.size());

        for (const auto& row : r) {
            Vehicle v;
            v.vehicleId = row["vehicle_id"].as<std::string>();
            v.currentNode = row["current_node"].as<std::string>();
            v.status = row["status"].as<std::string>();
            v.flightId = row["flight_id"].as<std::string>();
            out.push_back(std::move(v));
        }

        tx.commit();
        return out;
    }

    void upsert_many(const std::vector<Vehicle>& vehicles) {
        if (vehicles.empty()) return;

        pqxx::connection cx(connStr_);
        pqxx::work tx(cx);

        for (const auto& v : vehicles) {
            tx.exec_params(R"sql(
                INSERT INTO followme_vehicles (vehicle_id, current_node, status, flight_id, updated_at)
                VALUES ($1, $2, $3, NULLIF($4, ''), NOW())
                ON CONFLICT (vehicle_id) DO UPDATE SET
                    current_node = EXCLUDED.current_node,
                    status = EXCLUDED.status,
                    flight_id = EXCLUDED.flight_id,
                    updated_at = NOW()
            )sql", v.vehicleId, v.currentNode, v.status, v.flightId);
        }

        tx.commit();
    }

    void upsert_one(const Vehicle& v) {
        pqxx::connection cx(connStr_);
        pqxx::work tx(cx);

        tx.exec_params(R"sql(
            INSERT INTO followme_vehicles (vehicle_id, current_node, status, flight_id, updated_at)
            VALUES ($1, $2, $3, NULLIF($4, ''), NOW())
            ON CONFLICT (vehicle_id) DO UPDATE SET
                current_node = EXCLUDED.current_node,
                status = EXCLUDED.status,
                flight_id = EXCLUDED.flight_id,
                updated_at = NOW()
        )sql", v.vehicleId, v.currentNode, v.status, v.flightId);

        tx.commit();
    }

private:
    std::string connStr_;
};

class FollowMeService {
public:
    FollowMeService(std::string gcHost, int gcPort, std::string pgConnStr)
        : gcHost_(std::move(gcHost))
        , gcPort_(gcPort)
        , repo_(std::move(pgConnStr)) {}

    void run(int port) {
        try {
            repo_.ensure_schema();
            load_vehicles_from_db();
            sync_loaded_vehicles_to_ground_control();
        } catch (const std::exception& e) {
            std::cerr << "[FollowMe][DB] startup error: " << e.what() << '\n';
            throw;
        }

        httplib::Server svr;

        svr.Get("/health", [&](const httplib::Request&, httplib::Response& res) {
            app::reply_json(res, 200, {
                {"service", "FollowMe"},
                {"status", "ok"},
                {"time", app::now_sec()}
            });
        });

        // API init: теперь не только в память, но и в БД
        svr.Post("/v1/vehicles/init", [&](const httplib::Request& req, httplib::Response& res) {
            auto bodyOpt = app::parse_json_body(req);
            if (!bodyOpt) {
                app::reply_json(res, 400, {{"error", "invalid json"}});
                return;
            }

            auto body = *bodyOpt;
            if (!body.contains("vehicles") || !body["vehicles"].is_array()) {
                app::reply_json(res, 400, {{"error", "'vehicles' array required"}});
                return;
            }

            std::vector<Vehicle> batch;
            batch.reserve(body["vehicles"].size());

            for (const auto& v : body["vehicles"]) {
                Vehicle vv;
                vv.vehicleId = app::s_or(v, "vehicleId");
                vv.currentNode = v.value("currentNode", std::string("FS-1"));
                vv.status = v.value("status", std::string("empty"));
                vv.flightId.clear();

                if (vv.vehicleId.empty()) continue;
                batch.push_back(std::move(vv));
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

            // sync to GroundControl
            (void)app::http_post_json(gcHost_, gcPort_, "/v1/vehicles/init", body);

            app::reply_json(res, 200, {
                {"ok", true},
                {"initialized", static_cast<int>(batch.size())}
            });
        });

        svr.Get("/v1/vehicles/hasEmpty", [&](const httplib::Request&, httplib::Response& res) {
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

        // Reserve one empty FollowMe for flight and start mission worker
        svr.Post("/v1/vehicles/reserve", [&](const httplib::Request& req, httplib::Response& res) {
            auto bodyOpt = app::parse_json_body(req);
            if (!bodyOpt) {
                app::reply_json(res, 400, {{"error", "invalid json"}});
                return;
            }

            const auto& body = *bodyOpt;
            const std::string flightId = app::s_or(body, "flightId");
            if (flightId.empty()) {
                app::reply_json(res, 400, {{"error", "flightId required"}});
                return;
            }

            const std::string missionType = body.value("missionType", std::string("landing"));
            if (missionType != "landing" && missionType != "takeoff") {
                app::reply_json(res, 400, {{"error", "invalid missionType"}});
                return;
            }

            std::string picked;
            try {
                std::lock_guard<std::mutex> lk(mtx_);
                for (auto& [id, v] : vehicles_) {
                    if (v.status == "empty") {
                        v.status = "reserved";
                        v.flightId = flightId;
                        v.missionType = missionType;
                        repo_.upsert_one(v); // persist
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
                    {"error", "no empty vehicle"}
                });
                return;
            }

            // start background mission worker
            std::thread([this, picked, flightId, missionType]() {
                this->run_mission_worker(picked, flightId, missionType);
            }).detach();

            app::reply_json(res, 200, {
                {"ok", true},
                {"vehicleId", picked},
                {"flightId", flightId},
                {"status", "reserved"}
            });
        });

        // Release vehicle (if GroundControl reservation rollback)
        svr.Post("/v1/vehicles/release", [&](const httplib::Request& req, httplib::Response& res) {
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

        svr.Get("/v1/vehicles", [&](const httplib::Request&, httplib::Response& res) {
            json arr = json::array();
            std::lock_guard<std::mutex> lk(mtx_);
            for (const auto& [id, v] : vehicles_) {
                (void)id;
                arr.push_back({
                    {"vehicleId", v.vehicleId},
                    {"status", v.status},
                    {"currentNode", v.currentNode},
                    {"flightId", v.flightId}
                });
            }
            app::reply_json(res, 200, {{"vehicles", arr}});
        });

        std::cout << "[FollowMe] listening on 0.0.0.0:" << port << '\n';
        std::cout << "[FollowMe] GroundControl: " << gcHost_ << ":" << gcPort_ << '\n';
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

        std::cout << "[FollowMe][DB] loaded " << loaded.size() << " vehicles\n";
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
            std::cerr << "[FollowMe] warning: failed to sync loaded vehicles to GroundControl\n";
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
            std::cerr << "[FollowMe][DB] set_vehicle_status failed: " << e.what() << '\n';
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
            std::cerr << "[FollowMe][DB] set_vehicle_node failed: " << e.what() << '\n';
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
                    std::cerr << "[FollowMe] enter-edge timeout " << from << " -> " << to << "\n";
                    return false;
                }
            }

            // движение по ребру
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
                    std::cerr << "[FollowMe] leave-edge timeout -> " << to << "\n";
                    return false;
                }
            }
        }

        return true;
    }

    void run_mission_worker(const std::string& vehicleId,
                            const std::string& flightId,
                            const std::string& missionType) {
        // 1) Получить маршрут у GroundControl
        auto missionRes = app::http_get_json(
            gcHost_, gcPort_,
            "/v1/map/followme/path?flightId=" + flightId + "&missionType=" + missionType
        );

        if (!missionRes.ok()) {
            rollback_vehicle_to_empty(vehicleId);
            return;
        }

        // Для совместимости:
        std::vector<std::string> routeA;
        if (missionType == "landing") {
            routeA = json_to_nodes(missionRes.body, "routeToRunway");
        } else {
            routeA = json_to_nodes(missionRes.body, "routeToPlane");
        }

        auto routeWithPlane = json_to_nodes(missionRes.body, "routeWithPlane");
        auto routeReturn = json_to_nodes(missionRes.body, "routeReturn");

        // 2) Ждем разрешение стартовать миссию
        while (true) {
            auto permRes = app::http_get_json(
                gcHost_, gcPort_,
                "/v1/map/followme/permission?flightId=" + flightId + "&missionType=" + missionType
            );
            if (permRes.ok() && permRes.body.value("allowed", false)) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }

        // 3) Движение машинки к точке встречи
        set_vehicle_status(vehicleId, missionType == "landing" ? "moveToLandingPosition" : "moveToPlane");
        if (!drive_route(vehicleId, flightId, routeA)) {
            notify_mission_failed(vehicleId, flightId, missionType, "routeA_timeout");
            rollback_vehicle_to_empty(vehicleId);
            return;
        }

        // 4) Ведем самолет
        set_vehicle_status(vehicleId, "movingWithPlane");
        if (!drive_route(vehicleId, flightId, routeWithPlane)) {
            notify_mission_failed(vehicleId, flightId, missionType, "routeWithPlane_timeout");
            rollback_vehicle_to_empty(vehicleId);
            return;
        }

        // 5) Уведомляем GC о ключевой точке
        if (missionType == "landing") {
            (void)app::http_post_json(gcHost_, gcPort_, "/v1/vehicles/followme", {
                {"vehicleId", vehicleId},
                {"flightId", flightId},
                {"missionType", missionType},
                {"status", "arrivedParking"}
            });
        } else {
            (void)app::http_post_json(gcHost_, gcPort_, "/v1/vehicles/followme", {
                {"vehicleId", vehicleId},
                {"flightId", flightId},
                {"missionType", missionType},
                {"status", "arrivedRunwayEntrance"}
            });
        }

        // 6) Возвращаемся на базу
        set_vehicle_status(vehicleId, "returning");
        (void)drive_route(vehicleId, flightId, routeReturn);

        // 7) Освобождаем машинку
        rollback_vehicle_to_empty(vehicleId);

        (void)app::http_post_json(gcHost_, gcPort_, "/v1/followme/mission/completed", {
            {"vehicleId", vehicleId},
            {"flightId", flightId},
            {"missionType", missionType}
        });
    }

    void rollback_vehicle_to_empty(const std::string& vehicleId) {
        try {
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = vehicles_.find(vehicleId);
            if (it != vehicles_.end()) {
                it->second.status = "empty";
                it->second.flightId.clear();
                it->second.missionType.clear();
                repo_.upsert_one(it->second);
            }
        } catch (const std::exception& e) {
            std::cerr << "[FollowMe][DB] rollback failed: " << e.what() << '\n';
        }
    }

    void notify_mission_failed(const std::string& vehicleId,
                              const std::string& flightId,
                              const std::string& missionType,
                              const std::string& reason) {
        (void)app::http_post_json(gcHost_, gcPort_, "/v1/followme/mission/failed", {
            {"vehicleId", vehicleId},
            {"flightId", flightId},
            {"missionType", missionType},
            {"reason", reason}
        });
    }

private:
    std::unordered_map<std::string, Vehicle> vehicles_;
    std::mutex mtx_;

    std::string gcHost_;
    int gcPort_;

    VehicleRepository repo_;
};

static std::string env_or(const char* key, const std::string& defVal) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : defVal;
}

int main(int argc, char** argv) {
    try {
        int port = 8083;
        std::string gcHost = env_or("GC_HOST", "localhost");
        int gcPort = std::stoi(env_or("GC_PORT", "8081"));

        if (argc > 1) port = std::stoi(argv[1]);

        // Можно передать одной строкой через FM_PG_DSN,
        // либо собрать из отдельных переменных.
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

        FollowMeService s(gcHost, gcPort, pgConn);
        s.run(port);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[FollowMe] fatal: " << e.what() << '\n';
        return 1;
    }
}