#pragma once
#include "hv/status.hpp"
#include <cstdint>

namespace hv {

struct CpuidRegisters {
    std::uint32_t eax{};
    std::uint32_t ebx{};
    std::uint32_t ecx{};
    std::uint32_t edx{};
};

// Purpose: abstraction over the unprivileged CPUID instruction so that preflight logic can be
// tested with fake data and later reused from other layers.
// Responsibility: return raw register values for one (leaf, subleaf) pair, or an error.
// It performs no interpretation.
class ICpuidProvider {
public:
    virtual ~ICpuidProvider() = default;
    [[nodiscard]] virtual Result Query(std::uint32_t leaf, std::uint32_t subleaf,
                                       CpuidRegisters& out) = 0;
};

// Purpose: executes the real CPUID instruction (x86/x64 builds only).
// Limitation: CPUID reports the logical processor the calling thread currently runs on.
// On non-x86 builds Query() returns Status::Unsupported.
// CPUID is unprivileged; this class never executes RDMSR/WRMSR/VMX instructions.
class NativeCpuidProvider final : public ICpuidProvider {
public:
    [[nodiscard]] static bool IsAvailableOnThisBuild() noexcept;
    [[nodiscard]] Result Query(std::uint32_t leaf, std::uint32_t subleaf,
                               CpuidRegisters& out) override;
};

} // namespace hv
