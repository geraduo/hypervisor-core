#include "hv/platform/driver_diagnostics.hpp"

#include <cstdio>
#include <utility>

namespace hv {
namespace {

std::string JsonEscape(const std::string& text) {
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

long long MillisecondsSinceEpoch(std::chrono::system_clock::time_point time) {
    return static_cast<long long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count());
}

} // namespace

DriverDiagnostics::DriverDiagnostics(std::size_t capacity)
    : capacity_(capacity == 0 ? 1 : capacity) {}

void DriverDiagnostics::Record(EventLevel level, DriverDiagnosticCode code, std::string message) {
    std::lock_guard lock(mutex_);
    events_.push_back({level, code, std::move(message), std::chrono::system_clock::now()});
    TrimLocked();
}

std::vector<DriverDiagnosticEvent> DriverDiagnostics::Snapshot() const {
    std::lock_guard lock(mutex_);
    return events_;
}

void DriverDiagnostics::Clear() {
    std::lock_guard lock(mutex_);
    events_.clear();
}

std::size_t DriverDiagnostics::Size() const {
    std::lock_guard lock(mutex_);
    return events_.size();
}

std::size_t DriverDiagnostics::Capacity() const {
    std::lock_guard lock(mutex_);
    return capacity_;
}

void DriverDiagnostics::TrimLocked() {
    if (events_.size() <= capacity_) return;
    const std::size_t excess = events_.size() - capacity_;
    events_.erase(events_.begin(), events_.begin() + static_cast<std::ptrdiff_t>(excess));
}

const char* ToString(EventLevel value) noexcept {
    switch (value) {
    case EventLevel::Info: return "INFO";
    case EventLevel::Warning: return "WARN";
    case EventLevel::Error: return "ERROR";
    }
    return "INFO";
}

const char* ToString(DriverDiagnosticCode value) noexcept {
    switch (value) {
    case DriverDiagnosticCode::None: return "None";
    case DriverDiagnosticCode::DeviceInterfaceMissing: return "DeviceInterfaceMissing";
    case DriverDiagnosticCode::DeviceOpenFailed: return "DeviceOpenFailed";
    case DriverDiagnosticCode::DeviceAccessDenied: return "DeviceAccessDenied";
    case DriverDiagnosticCode::IoctlFailed: return "IoctlFailed";
    case DriverDiagnosticCode::ProtocolVersionMismatch: return "ProtocolVersionMismatch";
    case DriverDiagnosticCode::ResponseMalformed: return "ResponseMalformed";
    case DriverDiagnosticCode::NotConnected: return "NotConnected";
    case DriverDiagnosticCode::Connected: return "Connected";
    case DriverDiagnosticCode::Disconnected: return "Disconnected";
    case DriverDiagnosticCode::QuerySucceeded: return "QuerySucceeded";
    case DriverDiagnosticCode::QueryFailed: return "QueryFailed";
    case DriverDiagnosticCode::InitializationFailed: return "InitializationFailed";
    }
    return "None";
}

std::string FormatDriverDiagnosticsText(const std::vector<DriverDiagnosticEvent>& events) {
    std::string output = "Geraduo driver diagnostics journal\n";
    if (events.empty()) {
        output += "  (no events)\n";
        return output;
    }
    for (const auto& event : events) {
        output += "  [";
        output += ToString(event.level);
        output += "] ";
        output += ToString(event.code);
        output += ": ";
        output += event.message;
        output += '\n';
    }
    return output;
}

std::string FormatDriverDiagnosticsJson(const std::vector<DriverDiagnosticEvent>& events) {
    std::string output = "{\n  \"events\": [\n";
    for (std::size_t i = 0; i < events.size(); ++i) {
        const auto& event = events[i];
        output += "    {\"level\": \"";
        output += ToString(event.level);
        output += "\", \"code\": \"";
        output += ToString(event.code);
        output += "\", \"message\": \"";
        output += JsonEscape(event.message);
        output += "\", \"time_ms\": ";
        output += std::to_string(MillisecondsSinceEpoch(event.timestamp));
        output += "}";
        output += (i + 1 < events.size()) ? ",\n" : "\n";
    }
    output += "  ]\n}\n";
    return output;
}

} // namespace hv
