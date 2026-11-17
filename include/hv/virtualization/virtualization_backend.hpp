#pragma once

// Platform-neutral interface of the future hardware virtualization backend.
//
// v0.5.0 is a foundation release. The only implementation is
// VmxLifecycleController, which manages diagnostic states. No implementation
// may execute VMXON in this release; HardwareVmxStarted() is therefore a
// constant false and is part of the contract, not a status flag.

#include "hv/virtualization/vmx_status.hpp"

#include <cstdint>
#include <string>

namespace hv {

enum class VirtualizationBackendState {
    Uninitialized,        // Constructed, no diagnostic run yet.
    CheckingCapabilities, // A diagnostic evaluation is in progress.
    NotSupported,         // CPU or platform cannot support the implemented backend.
    Blocked,              // A required component is missing or not verified.
    ReadyForReview,       // User-mode review passed; not a hardware VMXON state.
    Failed,               // The diagnostic evaluation failed internally.
    Stopped               // Terminal state; the backend will not be reinitialized.
};

struct VirtualizationBackendStatus {
    VirtualizationBackendState state{VirtualizationBackendState::Uninitialized};
    VmxBlockReason reason{VmxBlockReason::None};
    VmxErrorClass error_class{VmxErrorClass::None};
    std::string detail;
    std::uint64_t generation{0};
    std::uint64_t successful_evaluations{0};
    bool initialized{false};
    bool stopped{false};
    bool last_transition_failed{false};
    std::string last_transition_error;

    // Hard boundary of v0.5.0. Both fields stay false in every code path.
    bool hardware_vmx_started{false};
    bool hardware_start_supported{false};
};

class IVirtualizationBackend {
public:
    virtual ~IVirtualizationBackend() = default;

    [[nodiscard]] virtual const char* Name() const noexcept = 0;
    [[nodiscard]] virtual VirtualizationBackendState GetState() const = 0;
    [[nodiscard]] virtual VirtualizationBackendStatus GetStatus() const = 0;

    // Must remain false for the whole lifetime of v0.5.0. A future hardware
    // backend may override it only when VMXON has actually succeeded.
    [[nodiscard]] virtual bool HardwareVmxStarted() const noexcept = 0;
};

[[nodiscard]] const char* ToString(VirtualizationBackendState value) noexcept;
[[nodiscard]] bool IsTerminalBackendState(VirtualizationBackendState state) noexcept;

[[nodiscard]] std::string FormatVirtualizationBackendStatusText(
    const VirtualizationBackendStatus& status);
[[nodiscard]] std::string FormatVirtualizationBackendStatusJson(
    const VirtualizationBackendStatus& status);

} // namespace hv
