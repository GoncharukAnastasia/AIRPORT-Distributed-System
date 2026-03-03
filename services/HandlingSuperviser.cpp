#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "common.h"

using app::json;

static std::string env_or(const char* key, const std::string& defVal) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : defVal;
}

static int env_or_int(const char* key, int defVal) {
    const char* v = std::getenv(key);
    if (!v || !*v) return defVal;
    try { return std::stoi(v); } catch (...) { return defVal; }
}

struct HandlingTask {
    std::string flightId;

    std::string callbackHost = "localhost";
    int callbackPort = 8084;

    int completeAfterSec = 5;   // через сколько считаем обслуживание завершённым
    int retryEverySec = 2;      // как часто повторяем push до ACK

    std::atomic<bool> acked{false};
    std::atomic<bool> stop{false};

    std::string state = "created"; // created | waiting | notifying | acked | stopped
    int64_t createdAt = 0;
    int64_t updatedAt = 0;

    // Дополнительно: тип миссии и парковка (для наземных служб)
    std::string missionType = "landing";  // landing | takeoff
    std::string parkingNode;              // узел парковки самолёта

    std::thread th;
};

class HandlingSupervisorService {
public:
    HandlingSupervisorService(std::string boardHost, int boardPort,
                              std::string cateringHost, int cateringPort)
        : boardHost_(std::move(boardHost)), boardPort_(boardPort)
        , cateringHost_(std::move(cateringHost)), cateringPort_(cateringPort) {}

    void run(int port) {
        httplib::Server svr;

        svr.Get("/health", [&](const httplib::Request&, httplib::Response& res) {
            app::reply_json(res, 200, {
                {"service", "HandlingSupervisor"},
                {"status", "ok"},
                {"time", app::now_sec()}
            });
        });

        // Board -> HandlingSupervisor
        // {
        //   "flightId":"SU200",
        //   "callbackHost":"board",
        //   "callbackPort":8084,
        //   "completeAfterSec":5
        // }
        svr.Post("/v1/handling/request", [&](const httplib::Request& req, httplib::Response& res) {
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

            std::lock_guard<std::mutex> lk(mtx_);

            auto it = tasks_.find(flightId);
            if (it != tasks_.end()) {
                // Повторный запрос: просто обновим callback/параметры и вернем ok
                it->second->callbackHost = body.value("callbackHost", it->second->callbackHost.empty() ? boardHost_ : it->second->callbackHost);
                it->second->callbackPort = body.value("callbackPort", it->second->callbackPort > 0 ? it->second->callbackPort : boardPort_);
                it->second->completeAfterSec = body.value("completeAfterSec", it->second->completeAfterSec);
                it->second->updatedAt = app::now_sec();

                app::reply_json(res, 200, {
                    {"ok", true},
                    {"flightId", flightId},
                    {"reused", true},
                    {"state", it->second->state}
                });
                return;
            }

            auto t = std::make_unique<HandlingTask>();
            t->flightId = flightId;
            t->callbackHost = body.value("callbackHost", boardHost_);
            t->callbackPort = body.value("callbackPort", boardPort_);
            t->completeAfterSec = body.value("completeAfterSec", 5);
            t->retryEverySec = body.value("retryEverySec", 2);
            t->missionType = body.value("missionType", std::string("landing"));
            t->parkingNode = body.value("parkingNode", std::string(""));
            t->createdAt = app::now_sec();
            t->updatedAt = t->createdAt;
            t->state = "waiting";

            HandlingTask* tp = t.get();
            tp->th = std::thread([this, tp]() { notify_loop(*tp); });

            tasks_[flightId] = std::move(t);

            // Запускаем CateringTruck (fire-and-forget, не блокируем ответ Board)
            if (!cateringHost_.empty() && !tasks_[flightId]->parkingNode.empty()) {
                trigger_catering_truck(flightId, tasks_[flightId]->missionType, tasks_[flightId]->parkingNode);
            }

            app::reply_json(res, 200, {
                {"ok", true},
                {"flightId", flightId},
                {"state", "waiting"}
            });
        });

        // Для отладки
        svr.Get("/v1/handling/tasks", [&](const httplib::Request&, httplib::Response& res) {
            json arr = json::array();
            std::lock_guard<std::mutex> lk(mtx_);
            for (const auto& [id, t] : tasks_) {
                (void)id;
                arr.push_back({
                    {"flightId", t->flightId},
                    {"callbackHost", t->callbackHost},
                    {"callbackPort", t->callbackPort},
                    {"acked", t->acked.load()},
                    {"state", t->state},
                    {"missionType", t->missionType},
                    {"parkingNode", t->parkingNode},
                    {"createdAt", t->createdAt},
                    {"updatedAt", t->updatedAt}
                });
            }
            app::reply_json(res, 200, {{"tasks", arr}});
        });

        // CateringTruck -> HandlingSuperviser: этап обслуживания завершён
        svr.Post("/v1/handling/catering/complete", [&](const httplib::Request& req, httplib::Response& res) {
            auto bodyOpt = app::parse_json_body(req);
            if (!bodyOpt) {
                app::reply_json(res, 400, {{"error", "invalid json"}});
                return;
            }
            const auto& body = *bodyOpt;
            const std::string flightId    = app::s_or(body, "flightId");
            const std::string vehicleId   = app::s_or(body, "vehicleId");
            const std::string missionType = body.value("missionType", std::string("landing"));
            const std::string status      = body.value("status", std::string("completed"));

            if (flightId.empty()) {
                app::reply_json(res, 400, {{"error", "flightId required"}});
                return;
            }

            std::cerr << "[HandlingSupervisor] CateringTruck stage done: flight=" << flightId
                      << " vehicle=" << vehicleId
                      << " missionType=" << missionType
                      << " status=" << status << "\n";

            app::reply_json(res, 200, {{"ok", true}, {"flightId", flightId}});
        });

        std::cout << "[HandlingSupervisor] listening on 0.0.0.0:" << port << "\n";
        svr.listen("0.0.0.0", port);

        stop_all();
    }

private:
    void notify_loop(HandlingTask& t) {
        // 1) "Выполняем обслуживание"
        for (int i = 0; i < t.completeAfterSec && !t.stop.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (t.stop.load()) {
            t.state = "stopped";
            t.updatedAt = app::now_sec();
            return;
        }

        // 2) Push в Board до подтверждения
        while (!t.stop.load() && !t.acked.load()) {
            t.state = "notifying";
            t.updatedAt = app::now_sec();

            auto r = app::http_post_json(t.callbackHost, t.callbackPort, "/v1/planes/handling-complete", {
                {"flightId", t.flightId},
                {"status", "completed"}
            });

            if (r.ok() && r.body.value("acked", false)) {
                t.acked.store(true);
                t.state = "acked";
                t.updatedAt = app::now_sec();
                return;
            }

            std::this_thread::sleep_for(std::chrono::seconds(t.retryEverySec));
        }

        if (t.stop.load()) {
            t.state = "stopped";
            t.updatedAt = app::now_sec();
        }
    }

    void trigger_catering_truck(const std::string& flightId,
                                 const std::string& missionType,
                                 const std::string& parkingNode) {
        // fire-and-forget в отдельном потоке
        std::thread([this, flightId, missionType, parkingNode]() {
            auto r = app::http_post_json(
                cateringHost_, cateringPort_,
                "/v1/catering/handling/start",
                {
                    {"flightId",    flightId},
                    {"missionType", missionType},
                    {"parkingNode", parkingNode}
                }
            );
            if (!r.ok()) {
                std::cerr << "[HandlingSupervisor] CateringTruck start failed: "
                          << r.error << " status=" << r.status << "\n";
            } else {
                std::cerr << "[HandlingSupervisor] CateringTruck started for flight="
                          << flightId << "\n";
            }
        }).detach();
    }

    void stop_all() {
        std::unordered_map<std::string, std::unique_ptr<HandlingTask>> tmp;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            for (auto& [id, t] : tasks_) {
                (void)id;
                t->stop.store(true);
            }
            tmp = std::move(tasks_);
        }

        for (auto& [id, t] : tmp) {
            (void)id;
            if (t->th.joinable()) t->th.join();
        }
    }

private:
    std::string boardHost_ = "localhost";
    int boardPort_ = 8084;

    std::string cateringHost_ = "localhost";
    int cateringPort_ = 8086;

    std::mutex mtx_;
    std::unordered_map<std::string, std::unique_ptr<HandlingTask>> tasks_;
};

int main(int argc, char** argv) {
    int port = env_or_int("HANDLING_PORT", 8085);
    if (argc > 1) port = std::stoi(argv[1]);

    std::string boardHost    = env_or("BOARD_HOST",    "localhost");
    int boardPort            = env_or_int("BOARD_PORT", 8084);
    std::string cateringHost = env_or("CATERING_HOST", "localhost");
    int cateringPort         = env_or_int("CATERING_PORT", 8086);

    HandlingSupervisorService s(boardHost, boardPort, cateringHost, cateringPort);
    s.run(port);
    return 0;
}