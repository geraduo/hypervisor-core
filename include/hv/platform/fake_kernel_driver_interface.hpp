#pragma once
#include "hv/platform/kernel_driver_interface.hpp"

#include <cstdint>
#include <deque>
#include <string>
#include <utility>

namespace hv {

// Selective response overrides used by protocol and readiness tests. Every
// field is opt-in so existing tests keep the previous behaviour.
struct FakeDriverResponseOverride {
    bool override_magic{false};
    std::uint32_t magic{};
    bool override_protocol_version{false};
    std::uint32_t protocol_version{};
    bool override_structure_size{false};
    std::uint32_t structure_size{};
    bool override_status{false};
    std::int32_t status{};
    bool override_architecture{false};
    std::uint32_t architecture{};
    bool override_driver_version{false};
    std::uint32_t driver_version_major{};
    std::uint32_t driver_version_minor{};
    std::uint32_t driver_version_patch{};
    bool override_processor_count{false};
    std::uint32_t active_processor_count{};
    bool override_reserved{false};
    std::uint64_t reserved{};
};

// Scriptable IKernelDriverInterface for tests and examples that need a driver
// double without Windows handles. Not thread-safe.
class FakeKernelDriverInterface final : public IKernelDriverInterface {
public:
    void SetConnectResult(Result result) { connect_result_ = std::move(result); }
    void SetQueryResult(Result result) { query_result_ = std::move(result); }
    void SetPlatformInfoTemplate(DriverPlatformInfoResponse response) {
        platform_info_ = response;
    }
    void SetResponseOverride(FakeDriverResponseOverride override) {
        override_ = override;
    }
    void ClearResponseOverride() { override_ = FakeDriverResponseOverride{}; }
    void SetConnected(bool connected) { connected_ = connected; }
    void QueueConnectResult(Result result) { connect_script_.push_back(std::move(result)); }
    void QueueQueryResult(Result result) { query_script_.push_back(std::move(result)); }

    [[nodiscard]] Result Connect() override {
        ++connect_calls_;
        const Result result = TakeNext(connect_script_, connect_result_);
        if (result.Succeeded()) {
            connected_ = true;
            last_error_.clear();
        } else {
            connected_ = false;
            last_error_ = result.message;
        }
        return result;
    }

    void Disconnect() override {
        ++disconnect_calls_;
        connected_ = false;
    }

    [[nodiscard]] bool IsConnected() const override { return connected_; }

    [[nodiscard]] Result QueryPlatformInfo(DriverPlatformInfoResponse& out) override {
        ++query_calls_;
        out = DriverPlatformInfoResponse{};
        if (!connected_) {
            last_error_ = "Fake driver interface is not connected.";
            return Result::Error(Status::InvalidState, last_error_);
        }

        const Result result = TakeNext(query_script_, query_result_);
        if (!result.Succeeded()) {
            last_error_ = result.message;
            return result;
        }

        out = platform_info_;
        out.magic = kGeraduoResponseMagic;
        out.protocol_version = kGeraduoProtocolVersion;
        out.structure_size = sizeof(DriverPlatformInfoResponse);
        out.request_id = next_request_id_++;
        if (next_request_id_ == 0) next_request_id_ = 1;
        out.status = static_cast<std::int32_t>(DriverProtocolStatus::Success);
        out.reserved = 0;

        if (override_.override_magic) out.magic = override_.magic;
        if (override_.override_protocol_version)
            out.protocol_version = override_.protocol_version;
        if (override_.override_structure_size)
            out.structure_size = override_.structure_size;
        if (override_.override_status) out.status = override_.status;
        if (override_.override_architecture)
            out.architecture = override_.architecture;
        if (override_.override_driver_version) {
            out.driver_version_major = override_.driver_version_major;
            out.driver_version_minor = override_.driver_version_minor;
            out.driver_version_patch = override_.driver_version_patch;
        }
        if (override_.override_processor_count)
            out.active_processor_count = override_.active_processor_count;
        if (override_.override_reserved) out.reserved = override_.reserved;

        last_error_.clear();
        return Result::Ok();
    }

    [[nodiscard]] std::string GetLastError() const override { return last_error_; }

    [[nodiscard]] int ConnectCalls() const { return connect_calls_; }
    [[nodiscard]] int DisconnectCalls() const { return disconnect_calls_; }
    [[nodiscard]] int QueryCalls() const { return query_calls_; }

private:
    static Result TakeNext(std::deque<Result>& script, const Result& fallback) {
        if (script.empty()) return fallback;
        Result result = std::move(script.front());
        script.pop_front();
        return result;
    }

    Result connect_result_{};
    Result query_result_{};
    DriverPlatformInfoResponse platform_info_{};
    FakeDriverResponseOverride override_{};
    bool connected_{false};
    std::string last_error_;
    std::uint32_t next_request_id_{1};
    std::deque<Result> connect_script_;
    std::deque<Result> query_script_;
    int connect_calls_{0};
    int disconnect_calls_{0};
    int query_calls_{0};
};

} // namespace hv
