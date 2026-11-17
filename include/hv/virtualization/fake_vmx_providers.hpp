#pragma once

// Fake providers for the VT-x capability and readiness tests. They never touch
// CPUID, Windows APIs or the kernel driver.

#include "hv/virtualization/vmx_capability_manager.hpp"

#include <string>
#include <utility>
#include <vector>

namespace hv {

// Scriptable ILogicalProcessorProbe. By default it fails with InvalidState so a
// test that forgets to configure it cannot accidentally look like a real CPU.
class FakeLogicalProcessorProbe final : public ILogicalProcessorProbe {
public:
    FakeLogicalProcessorProbe() = default;

    void SetObservations(std::vector<LogicalProcessorObservation> observations) {
        observations_ = std::move(observations);
        // Scripted observations imply a successful probe; FailWith() can still
        // override this before Probe() runs.
        failure_ = Result::Ok();
    }
    void FailWith(Result result) { failure_ = std::move(result); }
    void ClearFailure() { failure_ = Result::Ok(); }

    [[nodiscard]] Result Probe(std::vector<LogicalProcessorObservation>& out) override {
        ++probe_calls_;
        out.clear();
        if (!failure_.Succeeded()) return failure_;
        out = observations_;
        return Result::Ok();
    }

    [[nodiscard]] int ProbeCalls() const { return probe_calls_; }

private:
    std::vector<LogicalProcessorObservation> observations_;
    Result failure_{Status::InvalidState,
                    "FakeLogicalProcessorProbe has no scripted observations."};
    int probe_calls_{0};
};

// Convenience builders for tests.
[[nodiscard]] inline LogicalProcessorObservation MakeProcessorObservation(
    std::uint32_t index, bool vmx, bool hypervisor = false) {
    LogicalProcessorObservation observation;
    observation.processor_index = index;
    observation.cpuid_result = Result::Ok();
    observation.vmx_reported = vmx;
    observation.hypervisor_present = hypervisor;
    observation.detail = vmx ? "CPUID.01H:ECX[5] is set." : "CPUID.01H:ECX[5] is clear.";
    return observation;
}

[[nodiscard]] inline std::vector<LogicalProcessorObservation> MakeProcessorSet(
    std::uint32_t count, bool vmx, bool hypervisor = false) {
    std::vector<LogicalProcessorObservation> observations;
    observations.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index)
        observations.push_back(MakeProcessorObservation(index, vmx, hypervisor));
    return observations;
}

} // namespace hv
