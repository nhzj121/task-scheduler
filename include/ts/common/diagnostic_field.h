#pragma once

#include <algorithm>
#include <string>
#include <string_view>

namespace ts::common {
// Bounded, terminal-safe ASCII scalar. High/control bytes use explicit hex
// escapes; never interpret workload strings as format strings or terminal input.
inline std::string diagnostic_field(std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    constexpr std::size_t limit = 256;
    std::string result{"\""};
    for (std::size_t i = 0; i < std::min(value.size(), limit); ++i) {
        const auto byte = static_cast<unsigned char>(value[i]);
        if (byte == '"' || byte == '\\') { result += '\\'; result += static_cast<char>(byte); }
        else if (byte < 32 || byte >= 127) {
            result += "\\x"; result += hex[byte >> 4U]; result += hex[byte & 15U];
        } else result += static_cast<char>(byte);
    }
    if (value.size() > limit) result += "...";
    result += '"';
    return result;
}
} // namespace ts::common
