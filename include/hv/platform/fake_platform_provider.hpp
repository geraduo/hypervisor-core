#pragma once
#include "hv/platform/windows_platform_backend.hpp"

#include <string>
#include <utility>

namespace hv {

// Deterministic IPlatformProvider for unit tests. Not thread-safe.
class FakePlatformProvider final : public IPlatformProvider {
public:
    void SetSnapshot(PlatformProviderSnapshot snapshot) { snapshot_ = std::move(snapshot); }
    void FailWith(Status status, std::string message) {
        fail_ = true;
        fail_status_ = status;
        fail_message_ = std::move(message);
    }
    void ClearFailure() { fail_ = false; fail_message_.clear(); }

    [[nodiscard]] Result Query(PlatformProviderSnapshot& out) override {
        ++query_count_;
        out = PlatformProviderSnapshot{};
        if (fail_) return Result::Error(fail_status_, fail_message_);
        out = snapshot_;
        return Result::Ok();
    }

    [[nodiscard]] int QueryCount() const { return query_count_; }

private:
    PlatformProviderSnapshot snapshot_{};
    bool fail_{false};
    Status fail_status_{Status::InitializationFailed};
    std::string fail_message_;
    int query_count_{0};
};

} // namespace hv
