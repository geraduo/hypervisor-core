#pragma once

#include "hv/platform/preflight.hpp"
#include "hv/status.hpp"

#include <cstdint>
#include <string>

namespace hv {

// Purpose: lifecycle states of the Intel VT-x readiness state machine.
// There is intentionally no "Running" or "Ready" state. v0.5.0 never executes VMXON, creates a
// VMCS or enters a guest. ReadyForReview means only: every condition that can be checked from
// user mode plus the read-only diagnostic driver is consistent with Intel VT-x, and a human or a
// later kernel-mode stage must still verify the rest.
enum class VmxLifecycleState {
    Uninitialized,
    CheckingCapabilities,
    NotSupported,
    Blocked,
    ReadyForReview,
    Failed,
    Stopped
};

// The VirtualizationBackend state is the lifecycle state; there is a single state machine.
using VirtualizationBackendState = VmxLifecycleState;

// Per logical processor VMX capability as seen by CPUID on that processor.
enum class VmxCapabilityState {
    NotChecked,        // the processor could not be probed; nothing is concluded about it
    CpuidReported,     // Intel CPU and CPUID.01H:ECX[5] is set (does not prove VMXON is allowed)
    NotReported,       // Intel CPU and CPUID.01H:ECX[5] is clear
    UnsupportedVendor  // not an Intel CPU; the VT-x backend does not apply
};

enum class VmxCapabilityVerdict {
    CpuidReportedOnAllProcessors,  // every enumerated logical processor was checked individually
    NotSupported,                  // at least one confirmed negative result
    Inconclusive                   // some processors are NOT_CHECKED and no negative was found
};

// Verdict of the readiness evaluation. There is no "Ready" value on purpose.
enum class VmxReadinessVerdict { NotSupported, Blocked, ReadyForReview, Failed };

enum class VmxFailureReason {
    None,
    // NotSupported category
    UnsupportedPlatform,
    UnsupportedArchitecture,
    UnsupportedVendor,
    VmxNotReported,
    PlatformUnsupported,
    // Blocked category
    ProcessorNotChecked,
    DriverNotFound,
    DriverAccessDenied,
    DriverConnectionFailed,
    DriverProtocolMismatch,
    DriverQueryFailed,
    DriverDataInconsistent,
    // Failed category (an error in the diagnostic machinery itself)
    CpuProbeFailed,
    ProcessorEnumerationFailed,
    PlatformBackendFailed,
    PlatformCompatibilityFailed,
    DiagnosticsFailed,
    InternalError
};

struct VmxFailureDetail {
    VmxFailureReason reason{VmxFailureReason::None};
    Status status{Status::Success};
    std::string message;
    std::string component;
};

// Diagnostic state of ONE logical processor. A result for one processor is never copied to
// another one; a processor that could not be probed stays NOT_CHECKED with its error code.
struct ProcessorDiagnostic {
    std::uint32_t cpu_index{};
    std::string vendor;
    VmxCapabilityState vmx_capability{VmxCapabilityState::NotChecked};
    CheckStatus check_status{CheckStatus::NotChecked};
    Status error_code{Status::Success};
    std::string diagnostic_result;
};

struct VmxCondition {
    std::string id;
    std::string title;
    CheckStatus status{CheckStatus::NotChecked};
    std::string detail;
};

struct VmxLifecycleTransition {
    std::uint64_t sequence{};
    VmxLifecycleState from{VmxLifecycleState::Uninitialized};
    VmxLifecycleState to{VmxLifecycleState::Uninitialized};
    std::string reason;
};

[[nodiscard]] const char* ToString(VmxLifecycleState value) noexcept;
[[nodiscard]] const char* ToString(VmxCapabilityState value) noexcept;
[[nodiscard]] const char* ToString(VmxCapabilityVerdict value) noexcept;
[[nodiscard]] const char* ToString(VmxReadinessVerdict value) noexcept;
[[nodiscard]] const char* ToString(VmxFailureReason value) noexcept;

// Stable machine-readable token: PASS, FAIL, WARN, INFO, NOT_CHECKED.
[[nodiscard]] const char* VmxCheckToken(CheckStatus value) noexcept;

// Maps a failure reason to the readiness verdict it forces. None maps to ReadyForReview.
[[nodiscard]] VmxReadinessVerdict VerdictFor(VmxFailureReason reason) noexcept;

} // namespace hv
