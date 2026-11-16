#include "hv/platform/cpuid.hpp"

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#define HV_CPUID_AVAILABLE 1
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#else
#define HV_CPUID_AVAILABLE 0
#endif

namespace hv {

bool NativeCpuidProvider::IsAvailableOnThisBuild() noexcept { return HV_CPUID_AVAILABLE != 0; }

Result NativeCpuidProvider::Query(std::uint32_t leaf, std::uint32_t subleaf, CpuidRegisters& out) {
#if HV_CPUID_AVAILABLE
#if defined(_MSC_VER)
    int regs[4]{};
    __cpuidex(regs, static_cast<int>(leaf), static_cast<int>(subleaf));
    out.eax = static_cast<std::uint32_t>(regs[0]);
    out.ebx = static_cast<std::uint32_t>(regs[1]);
    out.ecx = static_cast<std::uint32_t>(regs[2]);
    out.edx = static_cast<std::uint32_t>(regs[3]);
#else
    unsigned int a = 0, b = 0, c = 0, d = 0;
    __cpuid_count(leaf, subleaf, a, b, c, d);
    out = {a, b, c, d};
#endif
    return Result::Ok();
#else
    (void)leaf; (void)subleaf; (void)out;
    return Result::Error(Status::Unsupported, "CPUID is only available on x86/x64 builds");
#endif
}

} // namespace hv
