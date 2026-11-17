#pragma once
#include "hv/platform/platform_compatibility_manager.hpp"

#include <string>
#include <utility>

namespace hv {

// Deterministic ICompatibilityEnvironment for tests. Not thread-safe; the
// readiness manager serializes calls in production and in the lifecycle tests.
class FakeCompatibilityEnvironment final : public ICompatibilityEnvironment {
public:
    void SetSnapshot(CompatibilityEnvironmentSnapshot snapshot) {
        snapshot_ = std::move(snapshot);
    }
    void FailWith(Status status, std::string message) {
        fail_ = true;
        fail_status_ = status;
        fail_message_ = std::move(message);
    }
    void ClearFailure() { fail_ = false; }

    [[nodiscard]] Result Query(CompatibilityEnvironmentSnapshot& out) override {
        ++query_count_;
        out = CompatibilityEnvironmentSnapshot{};
        if (fail_) return Result::Error(fail_status_, fail_message_);
        out = snapshot_;
        return Result::Ok();
    }

    [[nodiscard]] int QueryCount() const { return query_count_; }

private:
    CompatibilityEnvironmentSnapshot snapshot_{};
    bool fail_{false};
    Status fail_status_{Status::InitializationFailed};
    std::string fail_message_;
    int query_count_{0};
};

} // namespace hv
