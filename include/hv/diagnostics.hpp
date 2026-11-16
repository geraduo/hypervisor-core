#pragma once
#include <mutex>
#include <string>
#include <vector>

namespace hv {

enum class EventLevel { Info, Warning, Error };
struct DiagnosticEvent { EventLevel level{EventLevel::Info}; std::string component; std::string message; };

class Diagnostics final {
public:
    void Record(EventLevel level, std::string component, std::string message);
    [[nodiscard]] std::vector<DiagnosticEvent> Snapshot() const;
    void Clear();
private:
    mutable std::mutex mutex_;
    std::vector<DiagnosticEvent> events_;
};

} // namespace hv
