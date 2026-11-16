#include "hv/virtualization/vmx_lifecycle_controller.hpp"

#include <string>

namespace hv {
namespace {
constexpr const char* kComponent = "VmxLifecycleController";
}

VmxLifecycleController::VmxLifecycleController(VmxDiagnostics* diagnostics)
    : diagnostics_(diagnostics) {}

bool VmxLifecycleController::IsTransitionAllowed(VmxLifecycleState from,
                                                 VmxLifecycleState to) noexcept {
    switch (from) {
    case VmxLifecycleState::Uninitialized:
        return to == VmxLifecycleState::CheckingCapabilities || to == VmxLifecycleState::Stopped;
    case VmxLifecycleState::CheckingCapabilities:
        return to == VmxLifecycleState::NotSupported || to == VmxLifecycleState::Blocked ||
               to == VmxLifecycleState::ReadyForReview || to == VmxLifecycleState::Failed ||
               to == VmxLifecycleState::Stopped;
    case VmxLifecycleState::NotSupported:
    case VmxLifecycleState::Blocked:
    case VmxLifecycleState::ReadyForReview:
    case VmxLifecycleState::Failed:
        return to == VmxLifecycleState::CheckingCapabilities || to == VmxLifecycleState::Stopped;
    case VmxLifecycleState::Stopped:
        return to == VmxLifecycleState::Uninitialized ||
               to == VmxLifecycleState::CheckingCapabilities;
    }
    return false;
}

VmxLifecycleState VmxLifecycleController::State() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

Result VmxLifecycleController::BeginCheck(std::uint64_t& token) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == VmxLifecycleState::CheckingCapabilities) {
        return RejectLocked("a capability check is already in progress");
    }
    const Result result = ApplyLocked(VmxLifecycleState::CheckingCapabilities,
                                      "capability check requested");
    if (result.Succeeded()) token = generation_;
    return result;
}

Result VmxLifecycleController::CompleteCheck(std::uint64_t token, VmxLifecycleState outcome,
                                             const std::string& reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool valid_outcome = outcome == VmxLifecycleState::NotSupported ||
                               outcome == VmxLifecycleState::Blocked ||
                               outcome == VmxLifecycleState::ReadyForReview ||
                               outcome == VmxLifecycleState::Failed;
    if (!valid_outcome) {
        last_error_ = std::string("invalid check outcome: ") + ToString(outcome);
        return Result::Error(Status::InvalidArgument, last_error_);
    }
    if (state_ != VmxLifecycleState::CheckingCapabilities || token != generation_) {
        return RejectLocked(std::string("the check result was discarded: the check was superseded "
                                        "or the controller is in state ") +
                            ToString(state_));
    }
    return ApplyLocked(outcome, reason);
}

Result VmxLifecycleController::Stop(const std::string& reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == VmxLifecycleState::Stopped) return Result::Ok();  // idempotent
    return ApplyLocked(VmxLifecycleState::Stopped, reason);
}

Result VmxLifecycleController::Transition(VmxLifecycleState to, const std::string& reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    return ApplyLocked(to, reason);
}

std::vector<VmxLifecycleTransition> VmxLifecycleController::History() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return history_;
}

std::string VmxLifecycleController::GetLastError() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_error_;
}

Result VmxLifecycleController::ApplyLocked(VmxLifecycleState to, const std::string& reason) {
    if (!IsTransitionAllowed(state_, to)) {
        return RejectLocked(std::string("invalid transition ") + ToString(state_) + " -> " +
                            ToString(to));
    }
    VmxLifecycleTransition record;
    record.sequence = next_sequence_++;
    record.from = state_;
    record.to = to;
    record.reason = reason;
    history_.push_back(record);
    if (history_.size() > kMaxHistory) history_.erase(history_.begin());

    state_ = to;
    if (to == VmxLifecycleState::CheckingCapabilities || to == VmxLifecycleState::Stopped) {
        ++generation_;  // entering a check or stopping invalidates any running check
    }
    last_error_.clear();
    if (diagnostics_ != nullptr) {
        diagnostics_->Record(VmxEventLevel::Info, VmxEventCode::StateTransition, kComponent,
                             std::string(ToString(record.from)) + " -> " + ToString(record.to) +
                                 ": " + reason);
    }
    return Result::Ok();
}

Result VmxLifecycleController::RejectLocked(const std::string& message) {
    last_error_ = message;
    if (diagnostics_ != nullptr) {
        diagnostics_->Record(VmxEventLevel::Warning, VmxEventCode::TransitionRejected, kComponent,
                             message, Status::InvalidState);
    }
    return Result::Error(Status::InvalidState, message);
}

} // namespace hv
