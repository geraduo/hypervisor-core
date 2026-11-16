#include "fakes/fake_windows_platform.hpp"
#include "hv/platform/cpu_capability_manager.hpp"
#include "hv/platform/driver_diagnostics.hpp"
#include "hv/platform/fake_cpuid_provider.hpp"
#include "hv/platform/kernel_driver_interface.hpp"
#include "hv/platform/driver_protocol_validation.hpp"
#include "hv/platform/windows_platform_backend.hpp"
#include "test_support.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <string>

using test::Check;

namespace {

constexpr std::uint32_t kVmxBit = 1u << 5;
constexpr std::uint32_t kHypervisorBit = 1u << 31;

hv::FakeCpuidProvider MakeIntelCpu(std::uint32_t leaf1_ecx = kVmxBit) {
    hv::FakeCpuidProvider fake;
    fake.Set(0, 0, hv::FakeCpuidProvider::CpuVendorLeaf(0x16, "GenuineIntel"));
    fake.Set(1, 0, {0x000906EA, 0, leaf1_ecx, 0});
    if ((leaf1_ecx & kHypervisorBit) != 0) {
        fake.Set(0x40000000u, 0,
                 hv::FakeCpuidProvider::HypervisorVendorLeaf(0x4000000Au, "Microsoft Hv"));
    }
    fake.Set(0x80000000u, 0, {0x80000004u, 0, 0, 0});
    return fake;
}

void SetBrandLeaf(hv::FakeCpuidProvider& fake, std::uint32_t leaf, const std::string& text) {
    std::array<std::uint32_t, 4> registers{};
    for (std::size_t i = 0; i < registers.size(); ++i) {
        for (std::size_t byte = 0; byte < 4; ++byte) {
            const std::size_t index = i * 4 + byte;
            const auto value = index < text.size() ? static_cast<unsigned char>(text[index]) : 0u;
            registers[i] |= static_cast<std::uint32_t>(value) << (byte * 8);
        }
    }
    fake.Set(leaf, 0, {registers[0], registers[1], registers[2], registers[3]});
}

class FakeDriverDevice final : public hv::IDriverDevice {
public:
    enum class ResponseMode { Valid, Short, WrongVersion, WrongRequestId, VersionRejected };

    hv::Result open_result{hv::Result::Ok()};
    bool is_open{false};
    ResponseMode response_mode{ResponseMode::Valid};
    std::uint32_t open_calls{};
    std::uint32_t close_calls{};
    std::uint32_t control_calls{};
    std::uint32_t last_control_code{};
    GERADUO_QUERY_REQUEST last_request{};

    hv::Result Open() override {
        ++open_calls;
        if (open_result.Succeeded()) is_open = true;
        return open_result;
    }

    void Close() noexcept override {
        ++close_calls;
        is_open = false;
    }

    bool IsOpen() const noexcept override { return is_open; }

    hv::Result Control(std::uint32_t control_code, const void* input, std::size_t input_size,
                       void* output, std::size_t output_size,
                       std::size_t& bytes_returned) override {
        ++control_calls;
        last_control_code = control_code;
        bytes_returned = 0;
        if (!is_open) return hv::Result::Error(hv::Status::DeviceNotConnected, "fake device is closed");
        if (control_code != GERADUO_IOCTL_QUERY_PLATFORM_INFO || input_size != sizeof(GERADUO_QUERY_REQUEST) ||
            output_size < sizeof(GERADUO_QUERY_RESPONSE)) {
            return hv::Result::Error(hv::Status::InvalidArgument, "fake IOCTL buffers are invalid");
        }

        std::memcpy(&last_request, input, sizeof(last_request));
        GERADUO_QUERY_RESPONSE response{};
        response.protocol_version = response_mode == ResponseMode::WrongVersion
                                        ? GERADUO_DRIVER_PROTOCOL_VERSION + 1
                                        : GERADUO_DRIVER_PROTOCOL_VERSION;
        response.structure_size = sizeof(response);
        response.request_id = response_mode == ResponseMode::WrongRequestId
                                  ? last_request.request_id + 1
                                  : last_request.request_id;
        response.status = response_mode == ResponseMode::VersionRejected
                              ? GERADUO_QUERY_STATUS_VERSION_MISMATCH
                              : GERADUO_QUERY_STATUS_SUCCESS;
        response.platform = {10, 0, 22631, GERADUO_ARCHITECTURE_X64, 16, 4096,
                             GERADUO_PLATFORM_FLAG_VMX_REPORTED};
        std::memcpy(output, &response, sizeof(response));
        bytes_returned = response_mode == ResponseMode::Short ? sizeof(response) - 1 : sizeof(response);
        return hv::Result::Ok();
    }
};

void TestKernelConnectionAndReconnect() {
    hv::DriverDiagnostics diagnostics(8);
    auto device = std::make_unique<FakeDriverDevice>();
    auto* device_view = device.get();
    hv::KernelDriverInterface driver(std::move(device), &diagnostics);

    Check(!driver.IsConnected(), "driver interface begins disconnected");
    Check(driver.Connect().Succeeded() && driver.IsConnected(), "driver connection succeeds");
    Check(driver.Connect().Succeeded() && device_view->open_calls == 1,
          "repeated Connect is idempotent");

    hv::DriverPlatformInfo info;
    Check(driver.QueryPlatformInfo(info).Succeeded(), "platform info query succeeds");
    Check(info.os_build_number == 22631 && info.vmx_reported &&
              device_view->last_control_code == GERADUO_IOCTL_QUERY_PLATFORM_INFO,
          "query maps the structured response and restricted IOCTL");
    Check(device_view->last_request.protocol_version == GERADUO_DRIVER_PROTOCOL_VERSION &&
              device_view->last_request.structure_size == sizeof(GERADUO_QUERY_REQUEST) &&
              device_view->last_request.request_id != 0,
          "query sends versioned size-checked request with request ID");

    driver.Disconnect();
    driver.Disconnect();
    Check(!driver.IsConnected() && device_view->close_calls == 1,
          "Disconnect is idempotent and releases the device handle once");
    Check(driver.QueryPlatformInfo(info).status == hv::Status::DeviceNotConnected,
          "query after disconnect reports missing connection");
    Check(driver.Connect().Succeeded() && device_view->open_calls == 2,
          "driver can reconnect after disconnect");
    driver.Disconnect();
    Check(diagnostics.Snapshot().size() >= 4, "driver lifecycle events are retained in diagnostics");
}

void TestMissingDriverAndRetry() {
    auto device = std::make_unique<FakeDriverDevice>();
    auto* device_view = device.get();
    device_view->open_result = hv::Result::Error(hv::Status::DriverNotFound, "no device interface");
    hv::KernelDriverInterface driver(std::move(device));

    Check(driver.Connect().status == hv::Status::DriverNotFound,
          "missing driver is reported distinctly");
    Check(!driver.IsConnected() && driver.GetLastError() == "no device interface",
          "missing driver leaves no open connection and preserves error detail");
    device_view->open_result = hv::Result::Ok();
    Check(driver.Connect().Succeeded(), "connection can be retried after driver becomes available");
    driver.Disconnect();
}

void TestProtocolValidation() {
    GERADUO_QUERY_REQUEST request{GERADUO_DRIVER_PROTOCOL_VERSION,
                                  static_cast<std::uint32_t>(sizeof(GERADUO_QUERY_REQUEST)), 7};
    Check(hv::platform::protocol::ValidateQueryRequest(request, sizeof(request)) ==
              hv::platform::protocol::QueryRequestValidation::Valid,
          "well-formed driver query structure passes validation");
    request.structure_size = static_cast<std::uint32_t>(sizeof(request) - 1);
    Check(hv::platform::protocol::ValidateQueryRequest(request, sizeof(request)) ==
              hv::platform::protocol::QueryRequestValidation::InvalidStructure,
          "incorrect structure size is rejected before platform access");
    request.structure_size = static_cast<std::uint32_t>(sizeof(request));
    request.request_id = 0;
    Check(hv::platform::protocol::ValidateQueryRequest(request, sizeof(request)) ==
              hv::platform::protocol::QueryRequestValidation::InvalidStructure,
          "zero request identifiers are rejected");
    request.request_id = 7;
    request.protocol_version += 1;
    Check(hv::platform::protocol::ValidateQueryRequest(request, sizeof(request)) ==
              hv::platform::protocol::QueryRequestValidation::VersionMismatch,
          "unsupported protocol version is rejected");

    const std::array<FakeDriverDevice::ResponseMode, 3> invalid_modes{
        FakeDriverDevice::ResponseMode::Short,
        FakeDriverDevice::ResponseMode::WrongVersion,
        FakeDriverDevice::ResponseMode::WrongRequestId};
    for (const auto mode : invalid_modes) {
        auto device = std::make_unique<FakeDriverDevice>();
        device->response_mode = mode;
        hv::KernelDriverInterface driver(std::move(device));
        Check(driver.Connect().Succeeded(), "protocol fixture connects");
        hv::DriverPlatformInfo output;
        const auto result = driver.QueryPlatformInfo(output);
        if (mode == FakeDriverDevice::ResponseMode::Short)
            Check(result.status == hv::Status::InvalidResponse, "short response is rejected");
        else if (mode == FakeDriverDevice::ResponseMode::WrongVersion)
            Check(result.status == hv::Status::ProtocolMismatch, "response version mismatch is rejected");
        else
            Check(result.status == hv::Status::InvalidResponse, "response request ID mismatch is rejected");
        driver.Disconnect();
    }

    auto device = std::make_unique<FakeDriverDevice>();
    device->response_mode = FakeDriverDevice::ResponseMode::VersionRejected;
    hv::KernelDriverInterface driver(std::move(device));
    Check(driver.Connect().Succeeded(), "version rejection fixture connects");
    hv::DriverPlatformInfo output;
    Check(driver.QueryPlatformInfo(output).status == hv::Status::ProtocolMismatch,
          "driver-side protocol version rejection is surfaced");
}

void TestCpuBrandAndBackendLifecycle() {
    auto cpuid = MakeIntelCpu();
    SetBrandLeaf(cpuid, 0x80000002u, "Intel(R) Core(TM");
    SetBrandLeaf(cpuid, 0x80000003u, ") i7 test CPU   ");
    SetBrandLeaf(cpuid, 0x80000004u, "                ");
    hv::CpuCapabilityManager cpu(cpuid);
    hv::CpuCapabilityReport cpu_report;
    const auto cpu_query = cpu.Query(cpu_report);
    Check(cpu_query.Succeeded() && cpu_report.manufacturer == "GenuineIntel" &&
              cpu_report.model_string_read && cpu_report.model.find("Intel(R) Core(TM) i7 test CPU") != std::string::npos,
          "CPU manager decodes manufacturer, model name, and preflight report");

    FakePlatformProvider platform;
    hv::WindowsPlatformBackend backend(platform, cpu);
    hv::WindowsPlatformSnapshot snapshot;
    Check(backend.GetSnapshot(snapshot).status == hv::Status::InvalidState,
          "backend rejects snapshot access before initialization");
    Check(backend.Initialize().Succeeded() && backend.GetState() == hv::WindowsBackendState::Ready,
          "backend initializes from system and CPUID providers");
    Check(backend.GetSnapshot(snapshot).Succeeded() && snapshot.system.build_number == 22631,
          "initialized backend returns an immutable platform snapshot");
    Check(backend.Initialize().Succeeded() && platform.system_queries == 1,
          "repeated initialization is idempotent while ready");
    Check(backend.Shutdown().Succeeded() && backend.GetState() == hv::WindowsBackendState::Stopped,
          "backend shutdown clears its snapshot");

    FakePlatformProvider failing_platform;
    failing_platform.system_result = hv::Result::Error(hv::Status::InitializationFailed,
                                                        "system provider failure");
    hv::WindowsPlatformBackend failing_backend(failing_platform, cpu);
    Check(failing_backend.Initialize().status == hv::Status::InitializationFailed &&
              failing_backend.GetState() == hv::WindowsBackendState::Failed &&
              failing_backend.GetLastError() == "system provider failure",
          "initialization errors transition the backend into a diagnosable failed state");
}

void TestDiagnosticLogCapacity() {
    hv::DriverDiagnostics diagnostics(2);
    diagnostics.Record(hv::DriverDiagnosticLevel::Info, hv::DriverDiagnosticCode::Connected, "one");
    diagnostics.Record(hv::DriverDiagnosticLevel::Warning, hv::DriverDiagnosticCode::DriverNotFound, "two");
    diagnostics.Record(hv::DriverDiagnosticLevel::Error, hv::DriverDiagnosticCode::QueryFailed, "three");
    const auto events = diagnostics.Snapshot();
    Check(events.size() == 2 && events.front().message == "two" && events.back().sequence == 3,
          "diagnostic event history is bounded and sequence-numbered");
    diagnostics.Clear();
    Check(diagnostics.Snapshot().empty(), "diagnostic history can be cleared");
}

} // namespace

int main() {
    TestKernelConnectionAndReconnect();
    TestMissingDriverAndRetry();
    TestProtocolValidation();
    TestCpuBrandAndBackendLifecycle();
    TestDiagnosticLogCapacity();
    if (test::failures != 0) return EXIT_FAILURE;
    std::cout << "All Windows platform backend tests passed\n";
    return EXIT_SUCCESS;
}