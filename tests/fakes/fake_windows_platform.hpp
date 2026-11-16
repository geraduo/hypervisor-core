#pragma once

#include "hv/platform/kernel_driver_interface.hpp"
#include "hv/platform/platform_provider.hpp"
#include <string>

class FakePlatformProvider final : public hv::IPlatformProvider {
public:
    hv::WindowsSystemInformation system{
        true, 10, 0, 22631, hv::ProcessorArchitecture::X64, 16, 4096};
    hv::WindowsMitigationInformation mitigations{false, false, false,
                                                  "Fake configuration values; runtime is not asserted."};
    hv::Result system_result{hv::Result::Ok()};
    hv::Result mitigation_result{hv::Result::Ok()};
    std::uint32_t system_queries{};
    std::uint32_t mitigation_queries{};

    hv::Result QuerySystemInformation(hv::WindowsSystemInformation& out) override {
        ++system_queries;
        if (system_result.Succeeded()) out = system;
        return system_result;
    }

    hv::Result QueryMitigationInformation(hv::WindowsMitigationInformation& out) override {
        ++mitigation_queries;
        if (mitigation_result.Succeeded()) out = mitigations;
        return mitigation_result;
    }
};

class FakeKernelDriverInterface final : public hv::IKernelDriverInterface {
public:
    bool connected{false};
    bool connect_fails{false};
    bool query_fails{false};
    hv::DriverPlatformInfo platform_info{
        10, 0, 22631, GERADUO_ARCHITECTURE_X64, 16, 4096, false, true};
    std::string last_error;
    std::uint32_t connect_calls{};
    std::uint32_t disconnect_calls{};
    std::uint32_t query_calls{};

    hv::Result Connect() override {
        ++connect_calls;
        if (connect_fails) {
            connected = false;
            last_error = "fake driver connection failed";
            return hv::Result::Error(hv::Status::DriverNotFound, last_error);
        }
        connected = true;
        last_error.clear();
        return hv::Result::Ok();
    }

    void Disconnect() noexcept override {
        ++disconnect_calls;
        connected = false;
    }

    bool IsConnected() const noexcept override { return connected; }

    hv::Result QueryPlatformInfo(hv::DriverPlatformInfo& out) override {
        ++query_calls;
        if (!connected) {
            last_error = "fake driver is not connected";
            return hv::Result::Error(hv::Status::DeviceNotConnected, last_error);
        }
        if (query_fails) {
            last_error = "fake platform query failed";
            return hv::Result::Error(hv::Status::TransportError, last_error);
        }
        out = platform_info;
        last_error.clear();
        return hv::Result::Ok();
    }

    std::string GetLastError() const override { return last_error; }
};