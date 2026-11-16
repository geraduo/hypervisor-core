#pragma once

#include "hv/platform/cpu_capability_manager.hpp"
#include "hv/platform/cpuid.hpp"
#include "hv/virtualization/vmx_status.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace hv {

// Purpose: CPUID access addressed to a specific logical processor.
// The existing ICpuidProvider answers for whichever processor the calling thread runs on, which
// cannot describe a multiprocessor system. This interface adds the processor index.
class ILogicalProcessorCpuidProvider {
public:
    virtual ~ILogicalProcessorCpuidProvider() = default;
    [[nodiscard]] virtual Result GetLogicalProcessorCount(std::uint32_t& out) = 0;
    [[nodiscard]] virtual Result Query(std::uint32_t cpu_index, std::uint32_t leaf,
                                       std::uint32_t subleaf, CpuidRegisters& out) = 0;
};

// Purpose: native implementation for Windows x64. It temporarily pins the calling thread to the
// requested logical processor (processor group aware), verifies that the thread really runs there,
// executes the unprivileged CPUID instruction through NativeCpuidProvider and restores the
// previous affinity. On other hosts every call returns Status::Unsupported.
// CPUID is unprivileged; no RDMSR/WRMSR/VMX instruction is executed.
class NativeLogicalProcessorCpuidProvider final : public ILogicalProcessorCpuidProvider {
public:
    [[nodiscard]] Result GetLogicalProcessorCount(std::uint32_t& out) override;
    [[nodiscard]] Result Query(std::uint32_t cpu_index, std::uint32_t leaf, std::uint32_t subleaf,
                               CpuidRegisters& out) override;

private:
    std::mutex mutex_;
    NativeCpuidProvider cpuid_;
};

struct VmxCapabilityReport {
    CpuCapabilityReport cpu;   // result for the calling processor (existing CpuCapabilityManager)
    bool cpu_collected{false};
    std::vector<ProcessorDiagnostic> processors;  // one entry per enumerated logical processor
    std::uint32_t processors_expected{0};
    std::uint32_t processors_checked{0};          // entries whose check_status != NOT_CHECKED
    std::uint32_t processors_vmx_reported{0};
    VmxCapabilityVerdict verdict{VmxCapabilityVerdict::Inconclusive};
    std::vector<VmxFailureDetail> reasons;
    std::string summary;
    bool kernel_verification_required{true};  // always true; see VmxLifecycleState
};

// Purpose: determines Intel VMX support as reported by CPUID, per logical processor.
// Responsibility: reuse CpuCapabilityManager / RunPlatformPreflight for the calling processor and
// run the same preflight against every enumerated logical processor.
// Result semantics: returns an error only when the evidence could not be collected at all (CPUID
// probe failure or processor enumeration failure). An unsupported CPU is a successful Result with
// verdict == NotSupported. Thread-safe: calls are serialized.
class VmxCapabilityManager final {
public:
    VmxCapabilityManager(CpuCapabilityManager& cpu, ILogicalProcessorCpuidProvider& processors)
        : cpu_(cpu), processors_(processors) {}

    [[nodiscard]] Result Query(VmxCapabilityReport& out);

private:
    CpuCapabilityManager& cpu_;
    ILogicalProcessorCpuidProvider& processors_;
    std::mutex mutex_;
};

} // namespace hv
