#include <chrono>
#include <cctype>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#include "common.h"

using app::json;

static std::string url_encode(const std::string& s) {
    std::ostringstream out;
    out << std::hex << std::uppercase;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out << c;
        } else {
            out << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(c);
        }
    }
    return out.str();
}

int main(int argc, char** argv) {
    // Usage:
    // board_mock <flightId> [gcHost] [gcPort] [etaEpochSec] [retrySec]
    // Example:
    // board_mock SU100 localhost 8081 0 30
    if (argc < 1) {
        std::cerr << "Usage: board_mock <flightId> [gcHost] [gcPort] [etaEpochSec] [retrySec]\n";
        return 1;
    }

    const std::string flightId = "SU100";   // argv[1];
    const std::string gcHost = (argc > 2 ? argv[2] : "localhost");
    const int gcPort = (argc > 3 ? std::stoi(argv[3]) : 8081);
    int64_t etaEpoch = (argc > 4 ? std::stoll(argv[4]) : 0);
    const int retrySec = (argc > 5 ? std::stoi(argv[5]) : 30);

    if (etaEpoch <= 0) {
        etaEpoch = app::now_sec();
    }

    std::cout << "[Board " << flightId << "] target GC: "
              << gcHost << ":" << gcPort << "\n";

    // Wait until ETA
    while (app::now_sec() < etaEpoch) {
        auto left = etaEpoch - app::now_sec();
        std::cout << "[Board " << flightId << "] waiting ETA, " << left << " sec left...\n";
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    bool allowed = false;
    std::string reason;

    // Poll land permission every retrySec
    while (!allowed) {
        const std::string path = "/v1/land_permission?flightId=" + url_encode(flightId);
        auto r = app::http_get_json(gcHost, gcPort, path);

        if (!r.ok()) {
            std::cout << "[Board " << flightId << "] land_permission failed status="
                      << r.status << " err=" << r.error
                      << " -> retry in " << retrySec << " sec\n";
            std::this_thread::sleep_for(std::chrono::seconds(retrySec));
            continue;
        }

        allowed = r.body.value("allowed", false);
        reason = r.body.value("reason", "");

        if (!allowed) {
            std::cout << "[Board " << flightId << "] denied/delayed reason=" << reason
                      << " -> retry in " << retrySec << " sec\n";
            std::this_thread::sleep_for(std::chrono::seconds(retrySec));
            continue;
        }

        std::cout << "[Board " << flightId << "] landing approved. vehicleId="
                  << r.body.value("vehicleId", "") << " parking="
                  << r.body.value("parking", "") << "\n";
    }

    // Simulate touchdown after short delay
    std::this_thread::sleep_for(std::chrono::seconds(2));

    const std::string landedPath = "/v1/flights/" + url_encode(flightId) + "/landed";

    // Retry landed notify few times
    bool landedAck = false;
    for (int attempt = 1; attempt <= 5; ++attempt) {
        auto rr = app::http_post_json(gcHost, gcPort, landedPath, json::object());
        if (rr.ok()) {
            landedAck = true;
            std::cout << "[Board " << flightId << "] landed ack success.\n";
            break;
        }
        std::cout << "[Board " << flightId << "] landed ack failed attempt=" << attempt
                  << " status=" << rr.status << " err=" << rr.error << "\n";
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    if (!landedAck) {
        std::cerr << "[Board " << flightId << "] failed to notify landed.\n";
        return 2;
    }

    std::cout << "[Board " << flightId << "] done.\n";
    return 0;
}


