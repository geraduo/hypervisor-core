#include "hv/diagnostics.hpp"
#include <utility>

namespace hv {
void Diagnostics::Record(EventLevel level, std::string component, std::string message) {
    std::lock_guard lock(mutex_);
    events_.push_back({level, std::move(component), std::move(message)});
}
std::vector<DiagnosticEvent> Diagnostics::Snapshot() const {
    std::lock_guard lock(mutex_);
    return events_;
}
void Diagnostics::Clear() {
    std::lock_guard lock(mutex_);
    events_.clear();
}
} // namespace hv
