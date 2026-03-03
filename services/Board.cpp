#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
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
    try {
        return std::stoi(v);
    } catch (...) {
        return defVal;
    }
}

namespace {

std::string url_encode(const std::string& s) {
    std::ostringstream out;
    out << std::hex << std::uppercase;
    for (unsigned char c : s) {
        if (std::isalnum(static_cast<int>(c)) || c == '-' || c == '_' || c == '.' || c == '~') {
            out << c;
        } else {
            out << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(c);
        }
    }
    return out.str();
}

struct Agent {
    std::string flightId;
    std::string kind;   // "airborne" | "grounded"
    std::string state;  // human-readable
    int64_t startedAt = 0;

    std::atomic<bool> stop{false};
    std::thread th;

    // config
    std::string gcHost = "localhost";
    int gcPort = 8081;
    int pollSec = 30;
    int actionDelaySec = 2;      // touchdown or takeoff delay
    int handlingSec = 15;        // for grounded
    std::string parkingNode;     // for grounded

    // handling supervisor (optional stub)
    std::string handlingHost = "localhost";
    int handlingPort = 8085;

    std::string lastError;
};

class BoardService {
public:
    BoardService() {
        defaults_.gcHost = env_or("GC_HOST", "localhost");
        defaults_.gcPort = env_or_int("GC_PORT", 8081);

        defaults_.handlingHost = env_or("HANDLING_HOST", "localhost");
        defaults_.handlingPort = env_or_int("HANDLING_PORT", 8085);

        defaults_.airbornePollSec = env_or_int("BOARD_AIRBORNE_POLL_SEC", 30);
        defaults_.groundedPollSec = env_or_int("BOARD_GROUNDED_POLL_SEC", 10);
        defaults_.touchdownDelaySec = env_or_int("BOARD_TOUCHDOWN_DELAY_SEC", 2);
        defaults_.takeoffDelaySec = env_or_int("BOARD_TAKEOFF_DELAY_SEC", 2);
        defaults_.handlingSec = env_or_int("BOARD_HANDLING_SEC", 15);
    }

    void run(int port) {
        httplib::Server svr;

        svr.Get("/health", [&](const httplib::Request&, httplib::Response& res) {
            app::reply_json(res, 200, {
                {"service", "Board"},
                {"status", "ok"},
                {"time", app::now_sec()}
            });
        });

        // Create airborne plane agent
        // body: { "flightId":"SU100", "gcHost":"ground-control", "gcPort":8081, "pollSec":30, "touchdownDelaySec":2 }
        svr.Post("/v1/planes/airborne", [&](const httplib::Request& req, httplib::Response& res) {
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

            auto a = std::make_unique<Agent>();
            a->flightId = flightId;
            a->kind = "airborne";
            a->state = "created";
            a->startedAt = app::now_sec();

            // По умолчанию — из env; можно переопределить в запросе
            a->gcHost = body.value("gcHost", defaults_.gcHost);
            a->gcPort = body.value("gcPort", defaults_.gcPort);
            a->pollSec = body.value("pollSec", defaults_.airbornePollSec);
            a->actionDelaySec = body.value("touchdownDelaySec", defaults_.touchdownDelaySec);

            const bool ok = start_or_replace_agent(std::move(a));
            app::reply_json(res, ok ? 200 : 500, {
                {"ok", ok},
                {"flightId", flightId},
                {"kind", "airborne"}
            });
        });

        // Create grounded plane agent
        // body: {
        //   "flightId":"SU100",
        //   "parkingNode":"P-5",
        //   "gcHost":"ground-control",
        //   "gcPort":8081,
        //   "pollSec":10,
        //   "handlingSec":15,
        //   "takeoffDelaySec":2,
        //   "handlingHost":"handling-supervisor",   // optional
        //   "handlingPort":8085                     // optional
        // }
        svr.Post("/v1/planes/grounded", [&](const httplib::Request& req, httplib::Response& res) {
            auto bodyOpt = app::parse_json_body(req);
            if (!bodyOpt) {
                app::reply_json(res, 400, {{"error", "invalid json"}});
                return;
            }
            const auto& body = *bodyOpt;

            const std::string flightId = app::s_or(body, "flightId");
            const std::string parkingNode = app::s_or(body, "parkingNode");

            if (flightId.empty() || parkingNode.empty()) {
                app::reply_json(res, 400, {{"error", "flightId and parkingNode required"}});
                return;
            }

            auto a = std::make_unique<Agent>();
            a->flightId = flightId;
            a->kind = "grounded";
            a->state = "created";
            a->startedAt = app::now_sec();

            a->gcHost = body.value("gcHost", defaults_.gcHost);
            a->gcPort = body.value("gcPort", defaults_.gcPort);
            a->pollSec = body.value("pollSec", defaults_.groundedPollSec);
            a->handlingSec = body.value("handlingSec", defaults_.handlingSec);
            a->actionDelaySec = body.value("takeoffDelaySec", defaults_.takeoffDelaySec);
            a->parkingNode = parkingNode;

            a->handlingHost = body.value("handlingHost", defaults_.handlingHost);
            a->handlingPort = body.value("handlingPort", defaults_.handlingPort);

            const bool ok = start_or_replace_agent(std::move(a));
            app::reply_json(res, ok ? 200 : 500, {
                {"ok", ok},
                {"flightId", flightId},
                {"kind", "grounded"}
            });
        });

        // List agents
        svr.Get("/v1/planes", [&](const httplib::Request&, httplib::Response& res) {
            json arr = json::array();
            std::lock_guard<std::mutex> lk(mtx_);
            for (const auto& [id, a] : agents_) {
                (void)id;
                arr.push_back({
                    {"flightId", a->flightId},
                    {"kind", a->kind},
                    {"state", a->state},
                    {"parkingNode", a->parkingNode},
                    {"gcHost", a->gcHost},
                    {"gcPort", a->gcPort},
                    {"handlingHost", a->handlingHost},
                    {"handlingPort", a->handlingPort},
                    {"pollSec", a->pollSec},
                    {"startedAt", a->startedAt},
                    {"lastError", a->lastError}
                });
            }
            app::reply_json(res, 200, {{"planes", arr}});
        });

        // Stop agent
        // POST /v1/planes/stop { "flightId":"SU100" }
        svr.Post("/v1/planes/stop", [&](const httplib::Request& req, httplib::Response& res) {
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
            const bool ok = stop_agent(flightId);
            app::reply_json(res, 200, {{"ok", ok}, {"flightId", flightId}});
        });

        std::cout << "[Board] listening on 0.0.0.0:" << port << "\n";
        std::cout << "[Board] defaults: GC=" << defaults_.gcHost << ":" << defaults_.gcPort
                  << ", Handling=" << defaults_.handlingHost << ":" << defaults_.handlingPort << "\n";

        svr.listen("0.0.0.0", port);

        stop_all();
    }

private:
    struct Defaults {
        std::string gcHost;
        int gcPort = 8081;

        std::string handlingHost;
        int handlingPort = 8085;

        int airbornePollSec = 30;
        int groundedPollSec = 10;
        int touchdownDelaySec = 2;
        int takeoffDelaySec = 2;
        int handlingSec = 15;
    };

    bool start_or_replace_agent(std::unique_ptr<Agent> ptr) {
        if (!ptr || ptr->flightId.empty()) return false;

        const std::string flightId = ptr->flightId;

        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = agents_.find(flightId);
            if (it != agents_.end()) {
                it->second->stop.store(true);
            }
        }
        join_and_erase(flightId);

        Agent* ap = ptr.get();
        {
            std::lock_guard<std::mutex> lk(mtx_);
            agents_[flightId] = std::move(ptr);
        }

        ap->th = std::thread([this, ap]() {
            if (ap->kind == "airborne") {
                run_airborne(*ap);
            } else {
                run_grounded(*ap);
            }
        });

        return true;
    }

    void run_airborne(Agent& a) {
        a.state = "airborne.polling_land_permission";

        while (!a.stop.load()) {
            const std::string path = "/v1/land_permission?flightId=" + url_encode(a.flightId);
            auto r = app::http_get_json(a.gcHost, a.gcPort, path);

            if (!r.ok()) {
                a.lastError = "land_permission http error";
                std::this_thread::sleep_for(std::chrono::seconds(a.pollSec));
                continue;
            }

            const bool allowed = r.body.value("allowed", false);
            if (!allowed) {
                a.state = std::string("airborne.denied_or_delayed:") + r.body.value("reason", "");
                std::this_thread::sleep_for(std::chrono::seconds(a.pollSec));
                continue;
            }

            a.state = "airborne.landing_approved";
            std::this_thread::sleep_for(std::chrono::seconds(a.actionDelaySec));

            const std::string landedPath = "/v1/flights/" + url_encode(a.flightId) + "/landed";
            auto rr = app::http_post_json(a.gcHost, a.gcPort, landedPath, json::object());
            if (!rr.ok()) {
                a.lastError = "failed to POST landed";
                std::this_thread::sleep_for(std::chrono::seconds(2));
                continue;
            }

            a.state = "airborne.landed_notified";
            a.lastError.clear();
            return;
        }

        a.state = "stopped";
    }

    void run_grounded(Agent& a) {
        a.state = "grounded.request_handling_stub";

        // HandlingSupervisor stub (если сервиса нет — это не ошибка)
        (void)app::http_post_json(a.handlingHost, a.handlingPort, "/v1/handling/request", {
            {"flightId", a.flightId},
            {"parkingNode", a.parkingNode},
            {"services", json::array({"fuel", "catering", "boarding"})}
        });

        a.state = "grounded.handling_in_progress";
        for (int i = 0; i < a.handlingSec && !a.stop.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (a.stop.load()) {
            a.state = "stopped";
            return;
        }

        a.state = "grounded.handling_done_poll_takeoff_permission";

        while (!a.stop.load()) {
            const std::string path = "/v1/takeoff_permission?flightId=" + url_encode(a.flightId);
            auto r = app::http_get_json(a.gcHost, a.gcPort, path);

            if (!r.ok()) {
                a.lastError = "takeoff_permission http error";
                std::this_thread::sleep_for(std::chrono::seconds(a.pollSec));
                continue;
            }

            const bool allowed = r.body.value("allowed", false);
            if (!allowed) {
                a.state = std::string("grounded.takeoff_denied:") + r.body.value("reason", "");
                std::this_thread::sleep_for(std::chrono::seconds(a.pollSec));
                continue;
            }

            a.state = "grounded.takeoff_approved_noop";
            a.lastError.clear();

            while (!a.stop.load()) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            a.state = "stopped";
            return;
        }

        a.state = "stopped";
    }

    bool stop_agent(const std::string& flightId) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = agents_.find(flightId);
            if (it == agents_.end()) {
                return false;
            }
            it->second->stop.store(true);
        }
        join_and_erase(flightId);
        return true;
    }

    void join_and_erase(const std::string& flightId) {
        std::unique_ptr<Agent> victim;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = agents_.find(flightId);
            if (it == agents_.end()) return;
            victim = std::move(it->second);
            agents_.erase(it);
        }

        if (victim && victim->th.joinable()) {
            victim->th.join();
        }
    }

    void stop_all() {
        std::unordered_map<std::string, std::unique_ptr<Agent>> tmp;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            for (auto& [id, a] : agents_) {
                (void)id;
                a->stop.store(true);
            }
            tmp = std::move(agents_);
        }
        for (auto& [id, a] : tmp) {
            (void)id;
            if (a->th.joinable()) a->th.join();
        }
    }

private:
    Defaults defaults_;
    std::mutex mtx_;
    std::unordered_map<std::string, std::unique_ptr<Agent>> agents_;
};

} // namespace

int main(int argc, char** argv) {
    int port = env_or_int("BOARD_PORT", 8084);
    if (argc > 1) port = std::stoi(argv[1]);

    BoardService s;
    s.run(port);
    return 0;
}