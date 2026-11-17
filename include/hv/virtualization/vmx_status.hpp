#pragma once

// Shared vocabulary for the Intel VT-x virtualization foundation.
//
// These enums are diagnostic concepts only. VmxSupportLevel::Supported means
// that user-mode CPUID and the diagnostic components agree that the CPU
// advertises Intel VMX. It never means that VMXON was executed, that a VMCS
// exists, or that a guest can run.

#include "hv/platform/check_status.hpp"

#include <cstdint>

namespace hv {

// Result of the user-mode VT-x capability evaluation.
enum class VmxSupportLevel {
    NotChecked,    // The capability could not be established (for example a partial per-CPU probe).
    NotSupported,  // The CPU vendor, feature bits or platform rule out the implemented backend.
    ReviewRequired, // VMX is reported, but a hypervisor environment requires review.
    Supported      // CPUID reports Intel VMX on every observed logical processor.
};

// Result of the full readiness evaluation.
enum class VmxReadinessState {
    NotEvaluated,  // No diagnostic run has completed yet.
    NotSupported,  // Hardware or operating system cannot support the implemented backend.
    Blocked,       // A required component is missing, disabled or not verified.
    ReadyForReview, // User-mode prerequisites passed; kernel-stage review may begin.
    Failed         // A diagnostic component failed internally.
};

// Separate classification of errors from expected diagnostic verdicts.
enum class VmxErrorClass {
    None,          // No internal error; the verdict is a normal diagnostic result.
    CpuProbe,      // CPUID or CPU capability processing failed.
    PlatformProbe, // Windows platform backend or compatibility evaluation failed.
    Driver,        // Driver transport, connection or IOCTL failed unexpectedly.
    Protocol,      // Driver replied with an incompatible or malformed protocol payload.
    StateMachine,  // Lifecycle transition was rejected.
    Internal       // Defensive fallback; should not occur.
};

// Reasons reported by the capability, readiness and lifecycle layers. Only
// reasons other than None block ReadyForReview. Kernel-only checks use
// CheckStatus::NotChecked without a blocking reason, because ReadyForReview
// explicitly means "ready for kernel-stage review", not "ready for VMXON".
enum class VmxBlockReason {
    None,
    UnsupportedCpuVendor,
    VmxNotReported,
    CpuidProbeFailed,
    CpuidReportInvalid,
    PerProcessorCheckIncomplete,
    HypervisorPresentReviewRequired,
    PlatformNotChecked,
    PlatformBackendFailed,
    PlatformUnsupported,
    CompatibilityNotChecked,
    CompatibilityEvaluationFailed,
    CompatibilityUnsupported,
    CompatibilityReviewRequired,
    HypervisorConflict,
    DriverNotChecked,
    DriverNotConnected,
    DriverAccessDenied,
    DriverQueryFailed,
    DriverProtocolMismatch,
    DriverResponseInvalid,
    DriverArchitectureMismatch,
    DriverVersionIncompatible,
    LifecycleNotInitialized,
    LifecycleAlreadyInitialized,
    LifecycleAlreadyStopped,
    LifecycleBusy,
    LifecycleInvalidTransition,
    InternalError
};

[[nodiscard]] const char* ToString(VmxSupportLevel value) noexcept;
[[nodiscard]] const char* ToString(VmxReadinessState value) noexcept;
[[nodiscard]] const char* ToString(VmxErrorClass value) noexcept;
[[nodiscard]] const char* ToString(VmxBlockReason value) noexcept;

[[nodiscard]] inline constexpr bool IsBlockingReason(VmxBlockReason reason) noexcept {
    return reason != VmxBlockReason::None;
}

} // namespace hv
