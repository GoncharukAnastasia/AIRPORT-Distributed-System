#ifndef AIRPORT_COMMON_H
#define AIRPORT_COMMON_H

#pragma once

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <iostream>

namespace app {

using json = nlohmann::json;

inline int64_t now_sec() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

inline void reply_json(httplib::Response& res, int status, const json& body) {
    res.status = status;
    res.set_content(body.dump(2), "application/json; charset=utf-8");
}

inline std::optional<json> parse_json_body(const httplib::Request& req) {
    if (req.body.empty()) {
        return json::object();
    }
    auto parsed = json::parse(req.body, nullptr, false);
    if (parsed.is_discarded()) {
        return std::nullopt;
    }
    return parsed;
}

struct HttpResult {
    int status = -1;
    json body = json::object();
    std::string error;

    bool ok() const {
        return status >= 200 && status < 300;
    }
};

inline HttpResult http_get_json(const std::string& host, int port, const std::string& path) {
    HttpResult out;
    try {
        httplib::Client cli(host, port);
        cli.set_connection_timeout(2, 0);
        cli.set_read_timeout(5, 0);

        auto res = cli.Get(path.c_str());
        if (!res) {
            out.error = "connection failed";
            return out;
        }

        out.status = res->status;
        if (!res->body.empty()) {
            auto parsed = json::parse(res->body, nullptr, false);
            if (!parsed.is_discarded()) {
                out.body = std::move(parsed);
            }
        }
        return out;
    } catch (const std::exception& e) {
        out.error = e.what();
        return out;
    }
}

inline HttpResult http_post_json(const std::string& host, int port, const std::string& path, const json& payload) {
    HttpResult out;
    try {
        httplib::Client cli(host, port);
        cli.set_connection_timeout(2, 0);
        cli.set_read_timeout(5, 0);

        auto res = cli.Post(path.c_str(), payload.dump(), "application/json");
        if (!res) {
            out.error = "connection failed";
            return out;
        }

        out.status = res->status;
        if (!res->body.empty()) {
            auto parsed = json::parse(res->body, nullptr, false);
            if (!parsed.is_discarded()) {
                out.body = std::move(parsed);
            }
        }
        return out;
    } catch (const std::exception& e) {
        out.error = e.what();
        return out;
    }
}

inline std::string s_or(const json& j, const std::string& key, const std::string& def = "") {
    if (!j.contains(key) || !j.at(key).is_string()) {
        return def;
    }
    return j.at(key).get<std::string>();
}

} // namespace app

#endif //AIRPORT_COMMON_H