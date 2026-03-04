#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

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
    std::string parkingNode;

    std::string callbackHost = "localhost";
    int callbackPort = 8084;

    int completeAfterSec = 5; // можно оставить как "оставшиеся наземные операции" после заправки
    int retryEverySec = 2;

    std::atomic<bool> refuelRequested{false};
    std::atomic<bool> refuelDone{false};
    std::atomic<bool> refuelFailed{false};

    std::atomic<bool> acked{false};
    std::atomic<bool> stop{false};

    std::string refuelReason;
    int fuelAdded = 0;

    std::string state = "created"; // created | request_refueler | waiting_refueler | post_refuel_handling | notifying | acked | stopped
    int64_t createdAt = 0;
    int64_t updatedAt = 0;

    std::thread th;
};

class HandlingSupervisorService {
public:
    HandlingSupervisorService(std::string boardHost,
                              int boardPort,
                              std::string refuelerHost,
                              int refuelerPort)
        : boardHost_(std::move(boardHost))
        , boardPort_(boardPort)
        , refuelerHost_(std::move(refuelerHost))
        , refuelerPort_(refuelerPort) {}

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
        svr.Post("/v1/handling/request", [&](const httplib::Request& req, httplib::Response& res) {
            auto bodyOpt = app::parse_json_body(req);
            if (!bodyOpt) {
                app::reply_json(res, 400, {{"error", "invalid json"}});
                return;
            }

            const auto& body = *bodyOpt;
            const std::string flightId = app::s_or(body, "flightId");
            const std::string parkingNode = app::s_or(body, "parkingNode");

            if (flightId.empty()) {
                app::reply_json(res, 400, {{"error", "flightId required"}});
                return;
            }

            std::lock_guard<std::mutex> lk(mtx_);

            auto it = tasks_.find(flightId);
            if (it != tasks_.end()) {
                it->second->parkingNode = parkingNode.empty() ? it->second->parkingNode : parkingNode;
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
            t->parkingNode = parkingNode;
            t->callbackHost = body.value("callbackHost", boardHost_);
            t->callbackPort = body.value("callbackPort", boardPort_);
            t->completeAfterSec = body.value("completeAfterSec", 5);
            t->retryEverySec = body.value("retryEverySec", 2);
            t->createdAt = app::now_sec();
            t->updatedAt = t->createdAt;
            t->state = "request_refueler";

            HandlingTask* tp = t.get();
            tp->th = std::thread([this, tp]() { notify_loop(*tp); });

            tasks_[flightId] = std::move(t);

            app::reply_json(res, 200, {
                {"ok", true},
                {"flightId", flightId},
                {"state", "request_refueler"}
            });
        });

        // Refueler -> HandlingSupervisor
        svr.Post("/v1/handling/refueling-status", [&](const httplib::Request& req, httplib::Response& res) {
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

            const bool ok = body.value("ok", false);
            const std::string reason = body.value("reason", std::string(""));
            const int fuelAdded = body.value("fuelAdded", 0);

            std::lock_guard<std::mutex> lk(mtx_);
            auto it = tasks_.find(flightId);
            if (it == tasks_.end()) {
                app::reply_json(res, 404, {{"error", "task not found"}, {"acked", false}});
                return;
            }

            if (ok) {
                it->second->refuelDone.store(true);
                it->second->refuelFailed.store(false);
                it->second->fuelAdded = fuelAdded;
                it->second->state = "post_refuel_handling";
            } else {
                it->second->refuelFailed.store(true);
                it->second->refuelRequested.store(false);
                it->second->refuelReason = reason;
                it->second->state = "request_refueler";
            }

            it->second->updatedAt = app::now_sec();

            app::reply_json(res, 200, {
                {"ok", true},
                {"acked", true},
                {"flightId", flightId}
            });
        });

        svr.Get("/v1/handling/tasks", [&](const httplib::Request&, httplib::Response& res) {
            json arr = json::array();
            std::lock_guard<std::mutex> lk(mtx_);
            for (const auto& [id, t] : tasks_) {
                (void)id;
                arr.push_back({
                    {"flightId", t->flightId},
                    {"parkingNode", t->parkingNode},
                    {"callbackHost", t->callbackHost},
                    {"callbackPort", t->callbackPort},
                    {"refuelRequested", t->refuelRequested.load()},
                    {"refuelDone", t->refuelDone.load()},
                    {"refuelFailed", t->refuelFailed.load()},
                    {"fuelAdded", t->fuelAdded},
                    {"acked", t->acked.load()},
                    {"state", t->state},
                    {"createdAt", t->createdAt},
                    {"updatedAt", t->updatedAt}
                });
            }
            app::reply_json(res, 200, {{"tasks", arr}});
        });

        std::cout << "[HandlingSupervisor] listening on 0.0.0.0:" << port << "\n";
        std::cout << "[HandlingSupervisor] Refueler: " << refuelerHost_ << ":" << refuelerPort_ << "\n";

        svr.listen("0.0.0.0", port);
        stop_all();
    }

private:
    void notify_loop(HandlingTask& t) {
        // 1) Добиваемся принятия заявки Refueler-ом и ждём завершения заправки
        while (!t.stop.load() && !t.refuelDone.load()) {
            if (!t.refuelRequested.load()) {
                t.state = "request_refueler";
                t.updatedAt = app::now_sec();

                auto rr = app::http_post_json(refuelerHost_, refuelerPort_, "/v1/refueling/request", {
                    {"flightId", t.flightId},
                    {"parkingNode", t.parkingNode},
                    {"callbackHost", "handling-superviser"},
                    {"callbackPort", 8085}
                });

                if (rr.ok() && rr.body.value("accepted", false)) {
                    t.refuelRequested.store(true);
                    t.state = "waiting_refueler";
                    t.updatedAt = app::now_sec();
                } else {
                    std::this_thread::sleep_for(std::chrono::seconds(t.retryEverySec));
                    continue;
                }
            }

            if (t.refuelFailed.load()) {
                t.refuelFailed.store(false);
                t.refuelRequested.store(false);
                t.state = "request_refueler";
                t.updatedAt = app::now_sec();
                std::this_thread::sleep_for(std::chrono::seconds(t.retryEverySec));
                continue;
            }

            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        if (t.stop.load()) {
            t.state = "stopped";
            t.updatedAt = app::now_sec();
            return;
        }

        // 2) Дополнительное наземное обслуживание после заправки
        t.state = "post_refuel_handling";
        t.updatedAt = app::now_sec();

        for (int i = 0; i < t.completeAfterSec && !t.stop.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        if (t.stop.load()) {
            t.state = "stopped";
            t.updatedAt = app::now_sec();
            return;
        }

        // 3) Push в Board до ACK
        while (!t.stop.load() && !t.acked.load()) {
            t.state = "notifying";
            t.updatedAt = app::now_sec();

            auto r = app::http_post_json(t.callbackHost, t.callbackPort, "/v1/planes/handling-complete", {
                {"flightId", t.flightId},
                {"status", "completed"},
                {"fuelAdded", t.fuelAdded}
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

    std::string refuelerHost_ = "localhost";
    int refuelerPort_ = 8086;

    std::mutex mtx_;
    std::unordered_map<std::string, std::unique_ptr<HandlingTask>> tasks_;
};

int main(int argc, char** argv) {
    int port = env_or_int("HANDLING_PORT", 8085);
    if (argc > 1) port = std::stoi(argv[1]);

    const std::string boardHost = env_or("BOARD_HOST", "localhost");
    const int boardPort = env_or_int("BOARD_PORT", 8084);

    const std::string refuelerHost = env_or("REFUELER_HOST", "localhost");
    const int refuelerPort = env_or_int("REFUELER_PORT", 8086);

    HandlingSupervisorService s(boardHost, boardPort, refuelerHost, refuelerPort);
    s.run(port);
    return 0;
}