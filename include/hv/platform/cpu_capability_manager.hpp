#pragma once

#include "hv/platform/cpuid.hpp"
#include "hv/platform/preflight.hpp"
#include <string>

namespace hv {

struct CpuCapabilityReport {
    PreflightReport preflight;
    std::string manufacturer;
    std::string model;
    std::string model_detail;
    bool model_string_read{false};
};

class CpuCapabilityManager final {
public:
    explicit CpuCapabilityManager(ICpuidProvider& cpuid) : cpuid_(cpuid) {}

    [[nodiscard]] Result Query(CpuCapabilityReport& out);

private:
    ICpuidProvider& cpuid_;
};

} // namespace hv