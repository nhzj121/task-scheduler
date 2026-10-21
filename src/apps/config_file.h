#pragma once

#include <fstream>
#include <set>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace ts::apps {
inline nlohmann::json config_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot read config file");
    std::string text; char buffer[4096];
    while (input.read(buffer, sizeof(buffer)) || input.gcount()) {
        if (text.size() + static_cast<std::size_t>(input.gcount()) > 1048576) throw std::runtime_error("config exceeds 1 MiB");
        text.append(buffer, static_cast<std::size_t>(input.gcount()));
    }
    if (!input.eof()) throw std::runtime_error("config read failed");
    std::vector<std::set<std::string>> keys;
    return nlohmann::json::parse(text, [&](int, auto event, auto& value) {
        if (event == nlohmann::json::parse_event_t::object_start) keys.emplace_back();
        if (event == nlohmann::json::parse_event_t::key && !keys.back().insert(value.template get<std::string>()).second)
            throw std::runtime_error("duplicate config key");
        if (event == nlohmann::json::parse_event_t::object_end) keys.pop_back();
        return true;
    });
}
inline std::uint64_t unsigned_value(const std::string& text, std::uint64_t maximum = UINT64_MAX) {
    if (text.empty()) throw std::invalid_argument("empty integer");
    std::uint64_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9' || static_cast<std::uint64_t>(c - '0') > maximum ||
            value > (maximum - static_cast<std::uint64_t>(c - '0')) / 10) throw std::invalid_argument("invalid/overflowing unsigned integer");
        value = value * 10 + static_cast<std::uint64_t>(c - '0');
    }
    return value;
}
} // namespace ts::apps
