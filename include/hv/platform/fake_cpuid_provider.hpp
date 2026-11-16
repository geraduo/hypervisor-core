#pragma once
#include "hv/platform/cpuid.hpp"
#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace hv {

// Purpose: deterministic ICpuidProvider for unit tests. Not thread-safe.
// Leaves that were never configured return Status::NotFound, which lets tests prove that code
// does not read leaves it should not read (see Calls()).
class FakeCpuidProvider final : public ICpuidProvider {
public:
    void Set(std::uint32_t leaf, std::uint32_t subleaf, CpuidRegisters regs) {
        leaves_[{leaf, subleaf}] = regs;
    }
    void FailOn(std::uint32_t leaf, std::uint32_t subleaf, Status status, std::string message) {
        failures_[{leaf, subleaf}] = {status, std::move(message)};
    }
    void FailAllWith(Status status, std::string message) {
        fail_all_ = true; fail_status_ = status; fail_message_ = std::move(message);
    }

    [[nodiscard]] Result Query(std::uint32_t leaf, std::uint32_t subleaf,
                               CpuidRegisters& out) override {
        calls_.emplace_back(leaf, subleaf);
        if (fail_all_) return Result::Error(fail_status_, fail_message_);
        const auto key = std::make_pair(leaf, subleaf);
        const auto failure = failures_.find(key);
        if (failure != failures_.end())
            return Result::Error(failure->second.first, failure->second.second);
        const auto it = leaves_.find(key);
        if (it == leaves_.end()) return Result::Error(Status::NotFound, "fake leaf not configured");
        out = it->second;
        return Result::Ok();
    }

    [[nodiscard]] const std::vector<std::pair<std::uint32_t, std::uint32_t>>& Calls() const {
        return calls_;
    }
    [[nodiscard]] bool WasQueried(std::uint32_t leaf) const {
        for (const auto& call : calls_) if (call.first == leaf) return true;
        return false;
    }

    // CPU vendor leaf: the 12 characters are stored in EBX, EDX, ECX order.
    static CpuidRegisters CpuVendorLeaf(std::uint32_t max_leaf, const std::string& vendor) {
        return {max_leaf, Pack(vendor, 0), Pack(vendor, 8), Pack(vendor, 4)};
    }
    // Hypervisor vendor leaf: the 12 characters are stored in EBX, ECX, EDX order.
    static CpuidRegisters HypervisorVendorLeaf(std::uint32_t max_leaf, const std::string& vendor) {
        return {max_leaf, Pack(vendor, 0), Pack(vendor, 4), Pack(vendor, 8)};
    }

private:
    static std::uint32_t Pack(const std::string& text, std::size_t offset) {
        std::uint32_t value = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            const std::size_t index = offset + i;
            const auto ch = index < text.size() ? static_cast<unsigned char>(text[index]) : 0u;
            value |= static_cast<std::uint32_t>(ch) << (8 * i);
        }
        return value;
    }

    std::map<std::pair<std::uint32_t, std::uint32_t>, CpuidRegisters> leaves_;
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::pair<Status, std::string>> failures_;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> calls_;
    bool fail_all_{false};
    Status fail_status_{Status::InternalError};
    std::string fail_message_;
};

} // namespace hv
