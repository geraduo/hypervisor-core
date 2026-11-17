#pragma once

// Minimal JSON string escaping shared by the diagnostic report formatters.
// This header deliberately depends only on the standard library so that it can
// be used by the platform and virtualization libraries alike.

#include <cstdio>
#include <string>

namespace hv::detail {

inline std::string JsonEscape(const std::string& text) {
    std::string result;
    for (const char ch : text) {
        const auto value = static_cast<unsigned char>(ch);
        switch (ch) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\n': result += "\\n"; break;
        case '\f': result += "\\f"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (value < 0x20) {
                char buffer[8]{};
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", value);
                result += buffer;
            } else {
                result.push_back(ch);
            }
            break;
        }
    }
    return result;
}

inline const char* JsonBool(bool value) {
    return value ? "true" : "false";
}

} // namespace hv::detail
