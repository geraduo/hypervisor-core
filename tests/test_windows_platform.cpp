#include "hv/platform/cpu_capability_manager.hpp"
#include "hv/platform/driver_diagnostics.hpp"
#include "hv/platform/fake_cpuid_provider.hpp"
#include "hv/platform/fake_kernel_driver_interface.hpp"
#include "hv/platform/fake_platform_provider.hpp"
#include "hv/platform/kernel_driver_interface.hpp"
#include "hv/platform/windows_platform_backend.hpp"
#include "test_support.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <winioctl.h>
#endif

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

using test::Check;

namespace {

constexpr std::uint32_t kVmxBit = 1u << 5;
constexpr std::uint32_t kHypervisorBit = 1u << 31;
constexpr std::uint32_t kSvmBit = 1u << 2;
constexpr std::uint32_t kExtendedLeaf0 = 0x80000000u;
constexpr std::uint32_t kExtendedLeaf1 = 0x80000001u;

hv::DriverPlatformInfoResponse MakeValidResponse() {
    hv::DriverPlatformInfoResponse response{};
    response.magic = hv::kGeraduoResponseMagic;
    response.protocol_version = hv::kGeraduoProtocolVersion;
    response.structure_size = static_cast<std::uint32_t>(sizeof(response));
    response.request_id = 0;
    response.status = static_cast<std::int32_t>(hv::DriverProtocolStatus::Success);
    response.capability_flags = hv::kGeraduoCapabilityDiagnosticQuery |
                                hv::kGeraduoCapabilityOsVersion |
                                hv::kGeraduoCapabilityProcessorInfo;
    response.driver_version_major = hv::kGeraduoDriverVersionMajor;
    response.driver_version_minor = hv::kGeraduoDriverVersionMinor;
    response.driver_version_patch = hv::kGeraduoDriverVersionPatch;
    response.architecture = static_cast<std::uint32_t>(hv::DriverArchitecture::X64);
    response.os_major = 10;
    response.os_minor = 0;
    response.os_build = 22631;
    response.os_revision = 1;
    response.active_processor_count = 8;
    response.page_size = 4096;
    response.physical_memory_bytes = 16ull * 1024ull * 1024ull * 1024ull;
    response.reserved = 0;
    return response;
}

class ScriptedTransport final : public hv::IDriverTransport {
public:
    [[nodiscard]] hv::Result Open(std::string& error_detail) override {
        ++open_calls;
        hv::Result result = open_fallback;
        if (!open_script.empty()) {
            result = open_script.front();
            open_script.pop_front();
        }
        if (result.Succeeded()) {
            open_state = true;
            return hv::Result::Ok();
        }
        error_detail = result.message;
        return result;
    }

    [[nodiscard]] hv::Result Transact(const void* input, std::size_t input_size, void* output,
                                      std::size_t output_size, std::size_t& returned,
                                      std::string& error_detail) override {
        ++transact_calls;
        returned = 0;
        if (!open_state) {
            error_detail = "scripted transport is not open";
            return hv::Result::Error(hv::Status::InvalidState, error_detail);
        }
        if (input == nullptr || output == nullptr ||
            input_size != sizeof(hv::DriverQueryPlatformInfoRequest) ||
            output_size < sizeof(hv::DriverPlatformInfoResponse)) {
            error_detail = "scripted transport received invalid buffers";
            return hv::Result::Error(hv::Status::InvalidArgument, error_detail);
        }

        std::memcpy(&last_request, input, sizeof(last_request));
        saw_request = true;
        if (echo_request_id)
            response.request_id = last_request.request_id;

        const std::size_t requested = returned_bytes;
        const std::size_t destination = std::min(requested, output_size);
        const std::size_t source = std::min(destination, sizeof(response));
        if (source != 0) std::memcpy(output, &response, source);

        if (!transact_fallback.Succeeded()) {
            error_detail = transact_fallback.message;
            return transact_fallback;
        }
        returned = requested;
        return hv::Result::Ok();
    }

    void Close() noexcept override {
        ++close_calls;
        open_state = false;
    }

    [[nodiscard]] bool IsOpen() const noexcept override { return open_state; }

    std::deque<hv::Result> open_script;
    hv::Result open_fallback{};
    hv::DriverPlatformInfoResponse response{MakeValidResponse()};
    hv::Result transact_fallback{};
    std::size_t returned_bytes{sizeof(hv::DriverPlatformInfoResponse)};
    bool echo_request_id{true};
    bool open_state{false};
    int open_calls{0};
    int close_calls{0};
    int transact_calls{0};
    hv::DriverQueryPlatformInfoRequest last_request{};
    bool saw_request{false};
};

struct DriverFixture {
    ScriptedTransport* transport{};
    std::unique_ptr<hv::KernelDriverInterface> interface;
};

DriverFixture MakeDriverFixture() {
    auto transport = std::make_unique<ScriptedTransport>();
    auto* raw = transport.get();
    auto interface = std::make_unique<hv::KernelDriverInterface>(std::move(transport));
    return {raw, std::move(interface)};
}

bool HasDiagnostic(const hv::KernelDriverInterface& interface, hv::DriverDiagnosticCode code) {
    for (const auto& event : interface.Diagnostics().Snapshot())
        if (event.code == code) return true;
    return false;
}

void CheckRejected(const std::string& name, hv::Status expected_status,
                   const std::function<void(ScriptedTransport&)>& setup,
                   hv::DriverDiagnosticCode expected_code) {
    auto fixture = MakeDriverFixture();
    setup(*fixture.transport);
    Check(fixture.interface->Connect().Succeeded(), name + ": fixture connects");
    hv::DriverPlatformInfoResponse output{};
    const hv::Result result = fixture.interface->QueryPlatformInfo(output);
    Check(!result.Succeeded(), name + ": query is rejected");
    Check(result.status == expected_status, name + ": mapped status");
    Check(HasDiagnostic(*fixture.interface, expected_code), name + ": diagnostic code");
    Check(!fixture.interface->GetLastError().empty(), name + ": error detail is retained");
}

void TestProtocolLayout() {
    Check(sizeof(hv::DriverQueryPlatformInfoRequest) == 24,
          "request structure is 24 bytes");
    Check(sizeof(hv::DriverPlatformInfoResponse) == 80,
          "response structure is 80 bytes");
    Check(hv::kGeraduoRequestMagic != hv::kGeraduoResponseMagic,
          "request and response magic values differ");
    Check(hv::IsKnownDriverArchitecture(
              static_cast<std::uint32_t>(hv::DriverArchitecture::Arm64)),
          "ARM64 is a known driver architecture");
    Check(!hv::IsKnownDriverArchitecture(99), "unknown driver architecture is rejected");
    Check(std::string(hv::ToString(hv::DriverProtocolStatus::UnsupportedProtocolVersion)) ==
              "UnsupportedProtocolVersion",
          "protocol status names are stable");
    Check(std::string(hv::ToString(hv::DriverArchitecture::X64)) == "x64",
          "architecture names are stable");
#if defined(_WIN32)
    const std::uint32_t sdk_ioctl = static_cast<std::uint32_t>(
        CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_READ_DATA));
    Check(hv::IOCTL_GERADUO_QUERY_PLATFORM_INFO == sdk_ioctl,
          "numeric IOCTL matches the Windows SDK CTL_CODE definition");
    Check((hv::IOCTL_GERADUO_QUERY_PLATFORM_INFO & 0x3u) == METHOD_BUFFERED,
          "IOCTL uses METHOD_BUFFERED");
    Check((hv::IOCTL_GERADUO_QUERY_PLATFORM_INFO & 0x3u) != METHOD_NEITHER,
          "IOCTL never uses METHOD_NEITHER");
#endif
}

void TestMissingDriver() {
    auto fixture = MakeDriverFixture();
    fixture.transport->open_fallback =
        hv::Result::Error(hv::Status::NotFound, "the device interface is not present");

    const hv::Result connect = fixture.interface->Connect();
    Check(!connect.Succeeded(), "missing driver: Connect fails");
    Check(connect.status == hv::Status::NotFound, "missing driver: NotFound is preserved");
    Check(!fixture.interface->IsConnected(), "missing driver: interface is not connected");
    Check(fixture.interface->GetLastError().find("not present") != std::string::npos,
          "missing driver: error explains the missing interface");
    Check(HasDiagnostic(*fixture.interface, hv::DriverDiagnosticCode::DeviceInterfaceMissing),
          "missing driver: DeviceInterfaceMissing diagnostic is recorded");

    hv::DriverPlatformInfoResponse output{};
    const hv::Result query = fixture.interface->QueryPlatformInfo(output);
    Check(query.status == hv::Status::InvalidState,
          "missing driver: query without connection is rejected");
    Check(HasDiagnostic(*fixture.interface, hv::DriverDiagnosticCode::NotConnected),
          "missing driver: NotConnected diagnostic is recorded");
}

void TestReconnectAndQuery() {
    auto fixture = MakeDriverFixture();
    fixture.transport->open_script.push_back(
        hv::Result::Error(hv::Status::NotFound, "driver not started yet"));

    Check(!fixture.interface->Connect().Succeeded(), "reconnect: first attempt fails");
    Check(fixture.interface->Connect().Succeeded(), "reconnect: second attempt succeeds");
    Check(fixture.interface->IsConnected(), "reconnect: interface reports connected");
    Check(fixture.transport->open_calls == 2, "reconnect: transport was opened twice");

    hv::DriverPlatformInfoResponse first{};
    Check(fixture.interface->QueryPlatformInfo(first).Succeeded(), "reconnect: first query succeeds");
    Check(first.request_id == 1, "reconnect: first request id is 1");
    Check(first.driver_version_minor == hv::kGeraduoDriverVersionMinor &&
              first.os_build == 22631,
          "reconnect: driver and OS fields survive validation");
    Check(fixture.transport->saw_request, "reconnect: transport saw the request");
    Check(fixture.transport->last_request.magic == hv::kGeraduoRequestMagic,
          "reconnect: request magic is written");
    Check(fixture.transport->last_request.protocol_version == hv::kGeraduoProtocolVersion,
          "reconnect: request protocol version is written");
    Check(fixture.transport->last_request.structure_size ==
              sizeof(hv::DriverQueryPlatformInfoRequest),
          "reconnect: request structure size is written");
    Check(fixture.transport->last_request.request_id != 0,
          "reconnect: request id is nonzero");
    Check(fixture.transport->last_request.status ==
              static_cast<std::int32_t>(hv::DriverProtocolStatus::Success),
          "reconnect: request status is Success");
    Check(fixture.transport->last_request.reserved == 0,
          "reconnect: request reserved field is zero");

    fixture.interface->Disconnect();
    Check(!fixture.interface->IsConnected(), "reconnect: disconnect clears connection");
    Check(fixture.transport->close_calls == 1, "reconnect: transport handle is closed once");

    hv::DriverPlatformInfoResponse disconnected{};
    Check(fixture.interface->QueryPlatformInfo(disconnected).status ==
              hv::Status::InvalidState,
          "reconnect: query after disconnect is rejected");
    Check(HasDiagnostic(*fixture.interface, hv::DriverDiagnosticCode::NotConnected),
          "reconnect: disconnect gap is diagnosed");

    Check(fixture.interface->Connect().Succeeded(), "reconnect: third attempt succeeds");
    hv::DriverPlatformInfoResponse second{};
    Check(fixture.interface->QueryPlatformInfo(second).Succeeded(),
          "reconnect: query after reconnect succeeds");
    Check(second.request_id == 2, "reconnect: request id increments across reconnect");
    Check(fixture.interface->GetLastError().empty(),
          "reconnect: successful query clears the last error");
}

void TestResponseValidation() {
    hv::DriverPlatformInfoResponse good = MakeValidResponse();
    good.request_id = 7;
    std::string detail;
    hv::Status status = hv::Status::InternalError;
    Check(hv::KernelDriverInterface::ValidateResponse(good, 7, detail, status) &&
              status == hv::Status::Success,
          "validation: a correct response is accepted");
    Check(!hv::KernelDriverInterface::ValidateResponse(good, 8, detail, status) &&
              detail.find("request id") != std::string::npos,
          "validation: a stale request id is rejected");

    CheckRejected("short response", hv::Status::InternalError,
                  [](ScriptedTransport& transport) {
                      transport.returned_bytes = sizeof(hv::DriverPlatformInfoResponse) - 1;
                  },
                  hv::DriverDiagnosticCode::ResponseMalformed);

    CheckRejected("zero-byte response", hv::Status::InternalError,
                  [](ScriptedTransport& transport) { transport.returned_bytes = 0; },
                  hv::DriverDiagnosticCode::ResponseMalformed);

    CheckRejected("bad magic", hv::Status::InternalError,
                  [](ScriptedTransport& transport) {
                      transport.response.magic = 0xDEADBEEFu;
                  },
                  hv::DriverDiagnosticCode::ResponseMalformed);

    CheckRejected("protocol version mismatch", hv::Status::Unsupported,
                  [](ScriptedTransport& transport) {
                      transport.response.protocol_version = 2;
                  },
                  hv::DriverDiagnosticCode::ProtocolVersionMismatch);

    CheckRejected("structure size mismatch", hv::Status::InternalError,
                  [](ScriptedTransport& transport) {
                      transport.response.structure_size = 79;
                  },
                  hv::DriverDiagnosticCode::ResponseMalformed);

    CheckRejected("stale request id", hv::Status::InternalError,
                  [](ScriptedTransport& transport) {
                      transport.echo_request_id = false;
                      transport.response.request_id = 999;
                  },
                  hv::DriverDiagnosticCode::ResponseMalformed);

    CheckRejected("reserved field not zero", hv::Status::InternalError,
                  [](ScriptedTransport& transport) { transport.response.reserved = 1; },
                  hv::DriverDiagnosticCode::ResponseMalformed);

    CheckRejected("unknown architecture", hv::Status::InternalError,
                  [](ScriptedTransport& transport) { transport.response.architecture = 99; },
                  hv::DriverDiagnosticCode::ResponseMalformed);

    CheckRejected("zero processors", hv::Status::InternalError,
                  [](ScriptedTransport& transport) {
                      transport.response.active_processor_count = 0;
                  },
                  hv::DriverDiagnosticCode::ResponseMalformed);

    CheckRejected("non-power-of-two page size", hv::Status::InternalError,
                  [](ScriptedTransport& transport) { transport.response.page_size = 3000; },
                  hv::DriverDiagnosticCode::ResponseMalformed);

    CheckRejected("unsupported request status", hv::Status::Unsupported,
                  [](ScriptedTransport& transport) {
                      transport.response.status = static_cast<std::int32_t>(
                          hv::DriverProtocolStatus::UnsupportedRequest);
                  },
                  hv::DriverDiagnosticCode::QueryFailed);

    CheckRejected("invalid request status", hv::Status::InvalidArgument,
                  [](ScriptedTransport& transport) {
                      transport.response.status = static_cast<std::int32_t>(
                          hv::DriverProtocolStatus::InvalidRequest);
                  },
                  hv::DriverDiagnosticCode::QueryFailed);

    CheckRejected("driver internal error status", hv::Status::InternalError,
                  [](ScriptedTransport& transport) {
                      transport.response.status = static_cast<std::int32_t>(
                          hv::DriverProtocolStatus::InternalError);
                  },
                  hv::DriverDiagnosticCode::QueryFailed);
}

void TestTransportFailureAndJournal() {
    auto fixture = MakeDriverFixture();
    Check(fixture.interface->Connect().Succeeded(), "transport failure: connects");
    fixture.transport->transact_fallback =
        hv::Result::Error(hv::Status::AccessDenied, "IOCTL access denied");
    hv::DriverPlatformInfoResponse output{};
    const hv::Result result = fixture.interface->QueryPlatformInfo(output);
    Check(result.status == hv::Status::AccessDenied, "transport failure: status is mapped");
    Check(HasDiagnostic(*fixture.interface, hv::DriverDiagnosticCode::IoctlFailed),
          "transport failure: IoctlFailed diagnostic is recorded");

    hv::DriverDiagnostics journal(2);
    journal.Record(hv::EventLevel::Info, hv::DriverDiagnosticCode::Connected, "one");
    journal.Record(hv::EventLevel::Warning, hv::DriverDiagnosticCode::NotConnected, "two");
    journal.Record(hv::EventLevel::Error, hv::DriverDiagnosticCode::IoctlFailed, "three");
    Check(journal.Size() == 2 && journal.Capacity() == 2,
          "journal: capacity bounds retained events");
    const auto events = journal.Snapshot();
    Check(events.size() == 2 && events.front().message == "two" &&
              events.back().message == "three",
          "journal: oldest event is trimmed first");
    const auto text = hv::FormatDriverDiagnosticsText(events);
    const auto json = hv::FormatDriverDiagnosticsJson(events);
    Check(text.find("IoctlFailed") != std::string::npos,
          "journal: text contains diagnostic codes");
    Check(json.find("\"code\": \"IoctlFailed\"") != std::string::npos,
          "journal: JSON contains diagnostic codes");
    journal.Clear();
    Check(journal.Size() == 0, "journal: Clear releases all events");
}

void TestFakeKernelDriverInterface() {
    hv::FakeKernelDriverInterface fake;
    hv::DriverPlatformInfoResponse output{};
    Check(fake.QueryPlatformInfo(output).status == hv::Status::InvalidState,
          "fake driver: query before connect is rejected");
    fake.QueueConnectResult(hv::Result::Error(hv::Status::NotFound, "no device"));
    Check(!fake.Connect().Succeeded(), "fake driver: scripted connect failure");
    Check(fake.Connect().Succeeded(), "fake driver: later connect succeeds");
    Check(fake.IsConnected(), "fake driver: connected state");
    fake.SetPlatformInfoTemplate(MakeValidResponse());
    Check(fake.QueryPlatformInfo(output).Succeeded(), "fake driver: query succeeds");
    Check(output.request_id == 1, "fake driver: request id starts at 1");
    Check(fake.QueryPlatformInfo(output).Succeeded() && output.request_id == 2,
          "fake driver: request id increments");
    Check(fake.ConnectCalls() == 2 && fake.QueryCalls() == 3,
          "fake driver: call counters are accurate");
    fake.Disconnect();
    Check(!fake.IsConnected() && fake.DisconnectCalls() == 1,
          "fake driver: disconnect is observable");
    hv::DriverPlatformInfoResponse after_disconnect{};
    Check(fake.QueryPlatformInfo(after_disconnect).status == hv::Status::InvalidState,
          "fake driver: disconnected query is rejected");
    Check(!fake.GetLastError().empty(),
          "fake driver: failed query retains an error detail");
}

hv::PlatformProviderSnapshot MakePlatformSnapshot() {
    hv::PlatformProviderSnapshot snapshot{};
    snapshot.os_version = {10, 0, 22631, 1};
    snapshot.os_name = "Windows 11";
    snapshot.is_windows_server = false;
    snapshot.native_architecture = hv::PlatformArchitecture::X64;
    snapshot.process_architecture = hv::PlatformArchitecture::X64;
    snapshot.is_wow64_process = false;
    snapshot.logical_processor_count = 16;
    snapshot.page_size = 4096;
    snapshot.total_physical_memory_bytes = 32ull * 1024ull * 1024ull * 1024ull;
    snapshot.windows_apis = hv::PlatformCapabilityState::Available;
    snapshot.cpuid = hv::PlatformCapabilityState::Available;
    snapshot.windows_hypervisor_platform_api = hv::PlatformCapabilityState::Available;
    snapshot.kernel_driver_interface = hv::PlatformCapabilityState::RequiresKernelVerification;
    snapshot.virtualization_firmware = hv::PlatformCapabilityState::RequiresKernelVerification;
    return snapshot;
}

void TestPlatformBackendLifecycle() {
    hv::FakePlatformProvider provider;
    provider.SetSnapshot(MakePlatformSnapshot());
    hv::WindowsPlatformBackend backend(provider);

    hv::SystemInfo before{};
    Check(backend.GetSystemInfo(before).status == hv::Status::InvalidState,
          "platform: reads before Initialize are rejected");
    Check(backend.GetState() == hv::PlatformState::Uninitialized,
          "platform: starts uninitialized");

    Check(backend.Initialize().Succeeded(), "platform: Initialize succeeds");
    Check(backend.GetState() == hv::PlatformState::Initialized,
          "platform: state becomes Initialized");
    Check(provider.QueryCount() == 1, "platform: provider is queried once");
    Check(backend.Initialize().Succeeded() && provider.QueryCount() == 1,
          "platform: repeated Initialize uses the cached snapshot");

    hv::OsVersion version{};
    Check(backend.GetOsVersion(version).Succeeded() && version.build == 22631,
          "platform: OS version is readable");
    hv::PlatformArchitecture architecture{};
    Check(backend.GetArchitecture(architecture).Succeeded() &&
              architecture == hv::PlatformArchitecture::X64,
          "platform: native architecture is readable");

    hv::SystemInfo info{};
    Check(backend.GetSystemInfo(info).Succeeded() && info.logical_processor_count == 16 &&
              info.page_size == 4096,
          "platform: system information is readable");

    hv::PlatformCapabilityReport capabilities{};
    Check(backend.GetCapabilities(capabilities).Succeeded(),
          "platform: capability report is readable");
    Check(capabilities.support == hv::PlatformSupport::Supported,
          "platform: a clean x64 client snapshot is Supported");
    bool saw_architecture = false;
    bool saw_firmware = false;
    for (const auto& capability : capabilities.capabilities) {
        if (capability.id == "os.architecture" &&
            capability.status == hv::CheckStatus::Pass)
            saw_architecture = true;
        if (capability.id == "platform.virtualization_firmware" &&
            capability.status == hv::CheckStatus::NotChecked)
            saw_firmware = true;
    }
    Check(saw_architecture, "platform: architecture check passes");
    Check(saw_firmware, "platform: kernel-only firmware check remains NOT CHECKED");
    Check(capabilities.summary.find("no hardware virtualization") != std::string::npos,
          "platform: summary disclaims hardware virtualization");

    const auto text = hv::FormatPlatformReportText(info, capabilities, backend.GetState());
    const auto json = hv::FormatPlatformReportJson(info, capabilities, backend.GetState());
    Check(text.find("State: Initialized") != std::string::npos &&
              text.find("Support: Supported") != std::string::npos,
          "platform: text report has state and support");
    Check(json.find("\"support\": \"Supported\"") != std::string::npos &&
              json.find("\"os_version\": \"10.0.22631.1\"") != std::string::npos,
          "platform: JSON report has structured support and version");

    Check(backend.Refresh().Succeeded() && provider.QueryCount() == 2,
          "platform: Refresh re-probes the provider");
    Check(backend.Shutdown().Succeeded() && !backend.IsInitialized(),
          "platform: Shutdown clears the initialized state");
    Check(backend.GetCapabilities(capabilities).status == hv::Status::InvalidState,
          "platform: Shutdown releases cached data");
}

void TestPlatformBackendWarningsAndFailures() {
    hv::FakePlatformProvider provider;
    hv::PlatformProviderSnapshot server_snapshot = MakePlatformSnapshot();
    server_snapshot.is_windows_server = true;
    server_snapshot.process_architecture = hv::PlatformArchitecture::X86;
    server_snapshot.is_wow64_process = true;
    provider.SetSnapshot(server_snapshot);
    hv::WindowsPlatformBackend server_backend(provider);
    Check(server_backend.Initialize().Succeeded(), "platform warnings: server initializes");
    hv::PlatformCapabilityReport server_report{};
    Check(server_backend.GetCapabilities(server_report).Succeeded() &&
              server_report.support == hv::PlatformSupport::SupportedWithWarnings,
          "platform warnings: Server/WOW64 is SupportedWithWarnings");

    hv::PlatformProviderSnapshot arm_snapshot = MakePlatformSnapshot();
    arm_snapshot.native_architecture = hv::PlatformArchitecture::Arm64;
    provider.SetSnapshot(arm_snapshot);
    Check(server_backend.Refresh().Succeeded(), "platform unsupported: refresh succeeds");
    hv::PlatformCapabilityReport arm_report{};
    Check(server_backend.GetCapabilities(arm_report).Succeeded() &&
              arm_report.support == hv::PlatformSupport::Unsupported,
          "platform unsupported: non-x64 is Unsupported");
    bool architecture_failed = false;
    for (const auto& capability : arm_report.capabilities)
        if (capability.id == "os.architecture" &&
            capability.status == hv::CheckStatus::Fail)
            architecture_failed = true;
    Check(architecture_failed, "platform unsupported: architecture check fails");

    hv::FakePlatformProvider failing_provider;
    failing_provider.FailWith(hv::Status::InitializationFailed, "probe unavailable");
    hv::WindowsPlatformBackend failing_backend(failing_provider);
    Check(!failing_backend.Initialize().Succeeded(), "platform failure: Initialize fails");
    Check(failing_backend.GetState() == hv::PlatformState::Failed,
          "platform failure: state becomes Failed");
    Check(failing_backend.GetLastError().find("probe unavailable") != std::string::npos,
          "platform failure: provider detail is retained");

    failing_provider.ClearFailure();
    failing_provider.SetSnapshot(MakePlatformSnapshot());
    Check(failing_backend.Initialize().Succeeded(),
          "platform failure: a later Initialize can recover");
    Check(failing_backend.GetState() == hv::PlatformState::Initialized,
          "platform failure: recovery reaches Initialized");
}

void TestPlatformSnapshotValidation() {
    const auto expect_rejected = [](hv::PlatformProviderSnapshot snapshot,
                                    const std::string& name) {
        hv::FakePlatformProvider provider;
        provider.SetSnapshot(std::move(snapshot));
        hv::WindowsPlatformBackend backend(provider);
        Check(!backend.Initialize().Succeeded(), name + ": initialize fails");
        Check(backend.GetState() == hv::PlatformState::Failed, name + ": state is Failed");
        Check(!backend.GetLastError().empty(), name + ": error detail is present");
    };

    auto no_os = MakePlatformSnapshot();
    no_os.os_version.major = 0;
    expect_rejected(no_os, "platform validation: zero OS version");

    auto no_processors = MakePlatformSnapshot();
    no_processors.logical_processor_count = 0;
    expect_rejected(no_processors, "platform validation: zero processors");

    auto bad_page = MakePlatformSnapshot();
    bad_page.page_size = 0;
    expect_rejected(bad_page, "platform validation: zero page size");

    auto odd_page = MakePlatformSnapshot();
    odd_page.page_size = 3000;
    expect_rejected(odd_page, "platform validation: non-power-of-two page size");

    auto zero_memory = MakePlatformSnapshot();
    zero_memory.total_physical_memory_bytes = 0;
    hv::FakePlatformProvider zero_memory_provider;
    zero_memory_provider.SetSnapshot(zero_memory);
    hv::WindowsPlatformBackend zero_memory_backend(zero_memory_provider);
    Check(zero_memory_backend.Initialize().Succeeded(),
          "platform validation: zero memory is reported, not invented");
}

void SetBrand(hv::FakeCpuidProvider& fake, const std::string& brand) {
    std::string padded = brand;
    padded.resize(48, ' ');
    constexpr std::uint32_t kBrandLeafFirst = 0x80000002u;
    for (std::uint32_t leaf = 0; leaf < 3; ++leaf) {
        std::uint32_t registers[4]{};
        for (std::uint32_t reg = 0; reg < 4; ++reg) {
            for (std::uint32_t byte = 0; byte < 4; ++byte) {
                const std::size_t offset = leaf * 16 + reg * 4 + byte;
                registers[reg] |= static_cast<std::uint32_t>(
                                      static_cast<unsigned char>(padded[offset]))
                                  << (8u * byte);
            }
        }
        fake.Set(kBrandLeafFirst + leaf, 0, {registers[0], registers[1], registers[2],
                                            registers[3]});
    }
}

void TestCpuCapabilityManager() {
    hv::FakeCpuidProvider intel;
    intel.Set(0, 0, hv::FakeCpuidProvider::CpuVendorLeaf(0x16, "GenuineIntel"));
    intel.Set(1, 0, {0x000906EA, 0, kVmxBit, 0});
    intel.Set(kExtendedLeaf0, 0, {0x80000004u, 0, 0, 0});
    SetBrand(intel, "Geraduo Test CPU 9000");

    hv::CpuCapabilityManager manager(intel);
    hv::CpuCapabilityReport report;
    Check(manager.Probe(report).Succeeded(), "cpu: Intel probe succeeds");
    Check(report.vendor == hv::CpuVendor::Intel && report.vendor_string == "GenuineIntel",
          "cpu: Intel vendor is decoded");
    Check(report.brand_string == "Geraduo Test CPU 9000" && report.brand_string_read,
          "cpu: brand string is decoded and trimmed");
    Check(report.vmx_cpuid_bit && !report.hypervisor_present_bit,
          "cpu: VMX bit is reported without a hypervisor bit");
    Check(report.preflight_verdict == hv::PreflightVerdict::PendingKernelVerification,
          "cpu: preflight remains pending kernel verification");
    Check(report.eligibility == hv::CpuEligibility::KernelVerificationRequired,
          "cpu: eligibility never claims readiness");
    Check(report.kernel_verification_required,
          "cpu: kernel verification is always required");
    const auto text = hv::FormatCpuCapabilityReportText(report);
    const auto json = hv::FormatCpuCapabilityReportJson(report);
    Check(text.find("KernelVerificationRequired") != std::string::npos,
          "cpu: text report exposes eligibility");
    Check(json.find("\"brand\": \"Geraduo Test CPU 9000\"") != std::string::npos,
          "cpu: JSON report contains the brand string");

    hv::FakeCpuidProvider amd;
    amd.Set(0, 0, hv::FakeCpuidProvider::CpuVendorLeaf(0x0D, "AuthenticAMD"));
    amd.Set(1, 0, {0x00800F12, 0, kSvmBit, 0});
    amd.Set(kExtendedLeaf0, 0, {0x80000008u, 0, 0, 0});
    amd.Set(kExtendedLeaf1, 0, {0, 0, kSvmBit, 0});
    hv::CpuCapabilityManager amd_manager(amd);
    hv::CpuCapabilityReport amd_report;
    Check(amd_manager.Probe(amd_report).Succeeded(), "cpu: AMD probe succeeds");
    Check(amd_report.vendor == hv::CpuVendor::Amd && amd_report.svm_cpuid_bit,
          "cpu: AMD SVM is recorded as information");
    Check(amd_report.eligibility == hv::CpuEligibility::Unsupported,
          "cpu: AMD remains unsupported without an SVM backend");
    Check(amd_report.summary.find("no implemented virtualization backend") !=
              std::string::npos,
          "cpu: AMD summary explains the missing backend");

    hv::FakeCpuidProvider broken;
    broken.FailAllWith(hv::Status::InternalError, "CPUID source fault");
    hv::CpuCapabilityManager broken_manager(broken);
    hv::CpuCapabilityReport broken_report;
    Check(!broken_manager.Probe(broken_report).Succeeded(),
          "cpu: probe failure is returned");
    Check(broken_report.eligibility == hv::CpuEligibility::Unsupported &&
              broken_report.preflight_verdict == hv::PreflightVerdict::ProbeFailed,
          "cpu: probe failure is never interpreted as support");
    Check(!broken_manager.GetLastError().empty(), "cpu: probe error detail is retained");
}

void TestNativePlatformProvider() {
    hv::NativePlatformProvider provider;
    hv::PlatformProviderSnapshot snapshot;
    const hv::Result result = provider.Query(snapshot);
#if defined(_WIN32)
    Check(result.Succeeded(), "native platform: query succeeds on Windows");
    if (result.Succeeded()) {
        Check(snapshot.os_version.major >= 6 && snapshot.page_size != 0,
              "native platform: OS version and page size are plausible");
        Check(snapshot.logical_processor_count > 0,
              "native platform: processor count is plausible");
        Check(snapshot.windows_apis == hv::PlatformCapabilityState::Available,
              "native platform: Win32 APIs are reported available");
    }
#else
    Check(!result.Succeeded() && result.status == hv::Status::Unsupported,
          "native platform: non-Windows builds report Unsupported");
#endif
}

} // namespace

int main() {
    TestProtocolLayout();
    TestMissingDriver();
    TestReconnectAndQuery();
    TestResponseValidation();
    TestTransportFailureAndJournal();
    TestFakeKernelDriverInterface();
    TestPlatformBackendLifecycle();
    TestPlatformBackendWarningsAndFailures();
    TestPlatformSnapshotValidation();
    TestCpuCapabilityManager();
    TestNativePlatformProvider();

    if (test::failures != 0) {
        std::cerr << test::failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All Windows platform and driver-interface tests passed\n";
    return EXIT_SUCCESS;
}
