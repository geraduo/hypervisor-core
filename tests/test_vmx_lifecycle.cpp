#include "hv/virtualization/vmx_diagnostics.hpp"
#include "hv/virtualization/vmx_lifecycle_controller.hpp"
#include "test_support.hpp"
#include "vmx_test_fixture.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using test::Check;

namespace {

bool HasEvent(const hv::VmxDiagnosticsReport& report, hv::VmxDiagnosticCode code) {
    for (const auto& event : report.events)
        if (event.code == code) return true;
    return false;
}

void TestHappyPath() {
    vmx_test::LifecycleFixture fixture;
    hv::VmxLifecycleController& controller = fixture.controller;

    Check(controller.GetState() == hv::VirtualizationBackendState::Uninitialized,
          "lifecycle: initial state is Uninitialized");
    const hv::VirtualizationBackendStatus initial = controller.GetStatus();
    Check(!initial.initialized && !initial.stopped,
          "lifecycle: initial status flags are false");
    Check(initial.generation == 0 && initial.successful_evaluations == 0,
          "lifecycle: initial counters are zero");
    Check(!initial.hardware_vmx_started && !initial.hardware_start_supported,
          "lifecycle: hardware VMX is never claimed");
    Check(!controller.HardwareVmxStarted(), "lifecycle: HardwareVmxStarted() is false");
    Check(std::string(controller.Name()).find("Intel VT-x") != std::string::npos,
          "lifecycle: backend name identifies the Intel VT-x foundation");

    const hv::Result result = controller.Initialize();
    Check(result.Succeeded(), "lifecycle: Initialize() succeeds on a clean fixture");
    Check(controller.GetState() == hv::VirtualizationBackendState::ReadyForReview,
          "lifecycle: clean fixture reaches ReadyForReview");
    const hv::VirtualizationBackendStatus status = controller.GetStatus();
    Check(status.initialized && !status.stopped,
          "lifecycle: status reports an initialized, running backend");
    Check(status.generation == 1, "lifecycle: first transition increments generation");
    Check(status.successful_evaluations == 1,
          "lifecycle: one evaluation was committed");
    Check(status.reason == hv::VmxBlockReason::None,
          "lifecycle: ready state has no blocking reason");
    Check(status.error_class == hv::VmxErrorClass::None,
          "lifecycle: ready state has no internal error");
    Check(!status.last_transition_failed,
          "lifecycle: no transition failure is recorded");
    Check(!status.hardware_vmx_started && !status.hardware_start_supported,
          "lifecycle: status keeps the hardware boundary false");
    Check(fixture.driver.ConnectCalls() == 1 && fixture.driver.QueryCalls() == 1,
          "lifecycle: the diagnostic driver was queried once");
    Check(!controller.HardwareVmxStarted(),
          "lifecycle: successful review does not start hardware VMX");

    const hv::VmxDiagnosticsReport report = controller.CaptureReport();
    Check(report.backend.state == hv::VirtualizationBackendState::ReadyForReview,
          "lifecycle: captured report contains the committed backend state");
    Check(report.capability.support == hv::VmxSupportLevel::Supported,
          "lifecycle: captured report retains the capability verdict");
    Check(report.readiness.state == hv::VmxReadinessState::ReadyForReview,
          "lifecycle: captured report retains the readiness verdict");
    Check(!report.events.empty(), "lifecycle: the diagnostic journal is populated");
    Check(HasEvent(report, hv::VmxDiagnosticCode::LifecycleInitialized),
          "lifecycle: initialization is journaled");
    Check(HasEvent(report, hv::VmxDiagnosticCode::CapabilityEvaluated),
          "lifecycle: capability evaluation is journaled");
    Check(HasEvent(report, hv::VmxDiagnosticCode::ReadinessEvaluated),
          "lifecycle: readiness evaluation is journaled");
}

void TestDoubleInitializeRejected() {
    vmx_test::LifecycleFixture fixture;
    hv::VmxLifecycleController& controller = fixture.controller;
    Check(controller.Initialize().Succeeded(), "double-init: first Initialize() succeeds");

    const hv::Result second = controller.Initialize();
    Check(!second.Succeeded() && second.status == hv::Status::InvalidState,
          "double-init: second Initialize() is rejected");
    Check(controller.GetState() == hv::VirtualizationBackendState::ReadyForReview,
          "double-init: the committed state is unchanged");
    const hv::VirtualizationBackendStatus status = controller.GetStatus();
    Check(status.last_transition_failed,
          "double-init: the rejected transition is recorded");
    Check(status.last_transition_error.find("already initialized") != std::string::npos,
          "double-init: the rejection explains the state");
    Check(status.generation == 1 && status.successful_evaluations == 1,
          "double-init: the rejection does not advance counters");
    Check(HasEvent(controller.CaptureReport(), hv::VmxDiagnosticCode::LifecycleRejected),
          "double-init: the rejection is journaled");
}

void TestStopIsTerminal() {
    vmx_test::LifecycleFixture fixture;
    hv::VmxLifecycleController& controller = fixture.controller;
    Check(controller.Initialize().Succeeded(), "stop: Initialize() succeeds");

    const hv::Result stop = controller.Stop();
    Check(stop.Succeeded(), "stop: Stop() succeeds");
    Check(controller.GetState() == hv::VirtualizationBackendState::Stopped,
          "stop: state is Stopped");
    const hv::VirtualizationBackendStatus status = controller.GetStatus();
    Check(status.stopped && status.initialized,
          "stop: status reports the terminal stop");
    Check(status.generation == 2, "stop: the stop transition increments generation");
    Check(status.reason == hv::VmxBlockReason::None,
          "stop: stopping is not a failure reason");
    Check(fixture.driver.DisconnectCalls() == 1,
          "stop: the driver connection is released exactly once");
    Check(HasEvent(controller.CaptureReport(), hv::VmxDiagnosticCode::LifecycleStopped),
          "stop: stopping is journaled");

    Check(!controller.Stop().Succeeded(), "stop: a second Stop() is rejected");
    Check(!controller.Refresh().Succeeded(), "stop: Refresh() after Stop() is rejected");
    Check(!controller.Initialize().Succeeded(),
          "stop: Initialize() after Stop() is rejected");
    Check(controller.GetState() == hv::VirtualizationBackendState::Stopped,
          "stop: rejected calls keep the terminal state");
    Check(fixture.driver.DisconnectCalls() == 1,
          "stop: rejected calls do not disconnect again");
    Check(!controller.HardwareVmxStarted(),
          "stop: the hardware boundary remains false");
}

void TestRejectedBeforeInitialize() {
    vmx_test::LifecycleFixture fixture;
    hv::VmxLifecycleController& controller = fixture.controller;

    Check(!controller.Stop().Succeeded(), "early: Stop() before Initialize() is rejected");
    Check(!controller.Refresh().Succeeded(),
          "early: Refresh() before Initialize() is rejected");
    Check(controller.GetState() == hv::VirtualizationBackendState::Uninitialized,
          "early: the state remains Uninitialized");
    const hv::VirtualizationBackendStatus status = controller.GetStatus();
    Check(status.last_transition_failed,
          "early: rejected transitions are recorded");
    Check(status.generation == 0 && status.successful_evaluations == 0,
          "early: rejected calls do not advance counters");
    Check(fixture.driver.ConnectCalls() == 0,
          "early: no driver connection was attempted");
}

void TestRefreshCommitsNewVerdict() {
    vmx_test::LifecycleFixture fixture;
    hv::VmxLifecycleController& controller = fixture.controller;
    Check(controller.Initialize().Succeeded(), "refresh: Initialize() succeeds");

    Check(controller.Refresh().Succeeded(), "refresh: a clean Refresh() succeeds");
    const hv::VirtualizationBackendStatus status = controller.GetStatus();
    Check(status.generation == 2 && status.successful_evaluations == 2,
          "refresh: counters record the second evaluation");
    Check(status.state == hv::VirtualizationBackendState::ReadyForReview,
          "refresh: the committed state is unchanged when inputs are unchanged");
    Check(fixture.driver.ConnectCalls() == 2 && fixture.driver.QueryCalls() == 2,
          "refresh: the driver is queried again");

    fixture.cpuid = vmx_test::MakeAmd();
    fixture.processor_probe.SetObservations({});
    Check(controller.Refresh().Succeeded(), "refresh: an unsupported CPU is a verdict");
    Check(controller.GetState() == hv::VirtualizationBackendState::NotSupported,
          "refresh: the new NotSupported verdict is committed");
    const hv::VirtualizationBackendStatus changed = controller.GetStatus();
    Check(changed.reason == hv::VmxBlockReason::UnsupportedCpuVendor,
          "refresh: the committed reason is updated");
    Check(changed.successful_evaluations == 3,
          "refresh: the successful evaluation counter advances");
    Check(!changed.last_transition_failed,
          "refresh: a successful refresh clears the transition failure flag");
    Check(controller.CaptureReport().capability.support == hv::VmxSupportLevel::NotSupported,
          "refresh: the committed capability report matches the new verdict");
}

void TestRefreshRollbackOnPlatformFailure() {
    vmx_test::LifecycleFixture fixture;
    hv::VmxLifecycleController& controller = fixture.controller;
    Check(controller.Initialize().Succeeded(), "rollback: Initialize() succeeds");

    fixture.platform_provider.FailWith(hv::Status::InitializationFailed,
                                       "test: platform probe failed");
    const hv::Result refresh = controller.Refresh();
    Check(!refresh.Succeeded(), "rollback: an internal failure returns an error");
    Check(controller.GetState() == hv::VirtualizationBackendState::ReadyForReview,
          "rollback: the previous ReadyForReview state is restored");
    const hv::VirtualizationBackendStatus status = controller.GetStatus();
    Check(status.last_transition_failed,
          "rollback: the failed transition is recorded");
    Check(status.last_transition_error.find("platform") != std::string::npos,
          "rollback: the failure detail is retained");
    Check(status.reason == hv::VmxBlockReason::None,
          "rollback: the committed reason is restored");
    Check(status.generation == 2,
          "rollback: the attempted transition still increments generation");
    Check(status.successful_evaluations == 1,
          "rollback: a failed refresh does not count as successful");
    const hv::VmxDiagnosticsReport captured = controller.CaptureReport();
    Check(captured.readiness.state == hv::VmxReadinessState::ReadyForReview,
          "rollback: the previous readiness report is retained");
    Check(captured.capability.support == hv::VmxSupportLevel::Supported,
          "rollback: the previous capability report is retained");
    Check(HasEvent(captured, hv::VmxDiagnosticCode::EvaluationFailed),
          "rollback: the failure is journaled");

    fixture.platform_provider.ClearFailure();
    Check(controller.Refresh().Succeeded(), "rollback: Refresh() recovers after clearing");
    const hv::VirtualizationBackendStatus recovered = controller.GetStatus();
    Check(recovered.state == hv::VirtualizationBackendState::ReadyForReview,
          "rollback: recovery returns to the ready state");
    Check(!recovered.last_transition_failed,
          "rollback: recovery clears the failure flag");
    Check(recovered.successful_evaluations == 2,
          "rollback: recovery counts a successful evaluation");
    Check(recovered.generation == 3,
          "rollback: recovery advances generation");
}

void TestRefreshRollbackOnCapabilityFailure() {
    vmx_test::LifecycleFixture fixture;
    hv::VmxLifecycleController& controller = fixture.controller;
    Check(controller.Initialize().Succeeded(), "rollback-cpu: Initialize() succeeds");

    fixture.cpuid.FailAllWith(hv::Status::AccessDenied, "test: CPUID failed");
    const hv::Result refresh = controller.Refresh();
    Check(!refresh.Succeeded(), "rollback-cpu: CPUID failure returns an error");
    Check(controller.GetState() == hv::VirtualizationBackendState::ReadyForReview,
          "rollback-cpu: the previous state is restored");
    const hv::VmxDiagnosticsReport captured = controller.CaptureReport();
    Check(captured.capability.support == hv::VmxSupportLevel::Supported,
          "rollback-cpu: the previous capability report survives");
    Check(captured.capability.error_class == hv::VmxErrorClass::None,
          "rollback-cpu: the rolled-back report has no error class");
    Check(controller.GetStatus().last_transition_failed,
          "rollback-cpu: the failed attempt is recorded");
}

void TestInitializeFailureCommitsFailedState() {
    vmx_test::LifecycleFixture fixture;
    fixture.cpuid.FailAllWith(hv::Status::AccessDenied, "test: CPUID failed");
    hv::VmxLifecycleController& controller = fixture.controller;

    const hv::Result result = controller.Initialize();
    Check(!result.Succeeded(), "init-fail: Initialize() returns an error");
    Check(controller.GetState() == hv::VirtualizationBackendState::Failed,
          "init-fail: the failed initialization is committed");
    const hv::VirtualizationBackendStatus status = controller.GetStatus();
    Check(status.reason == hv::VmxBlockReason::CpuidProbeFailed,
          "init-fail: reason is CpuidProbeFailed");
    Check(status.error_class == hv::VmxErrorClass::CpuProbe,
          "init-fail: error class is CpuProbe");
    Check(status.successful_evaluations == 0,
          "init-fail: no evaluation is counted as successful");
    Check(status.last_transition_failed,
          "init-fail: the failure is recorded");
    Check(!controller.HardwareVmxStarted(),
          "init-fail: hardware VMX is still not started");
    const hv::VmxDiagnosticsReport captured = controller.CaptureReport();
    Check(captured.capability.support == hv::VmxSupportLevel::NotChecked,
          "init-fail: the failed capability report is available");
    Check(captured.readiness.state == hv::VmxReadinessState::NotEvaluated,
          "init-fail: readiness was not evaluated after a CPU failure");

    fixture.cpuid = vmx_test::MakeIntel(true);
    Check(controller.Refresh().Succeeded(), "init-fail: Failed is retryable with Refresh()");
    Check(controller.GetState() == hv::VirtualizationBackendState::ReadyForReview,
          "init-fail: a successful retry commits the ready state");
    Check(controller.GetStatus().successful_evaluations == 1,
          "init-fail: the successful retry is counted");
}

void TestConcurrentLifecycle() {
    vmx_test::LifecycleFixture fixture;
    hv::VmxLifecycleController& controller = fixture.controller;

    std::atomic<int> initialize_successes{0};
    std::vector<std::thread> initializers;
    initializers.reserve(8);
    for (int index = 0; index < 8; ++index) {
        initializers.emplace_back([&controller, &initialize_successes]() {
            if (controller.Initialize().Succeeded()) ++initialize_successes;
        });
    }
    for (auto& thread : initializers) thread.join();

    Check(initialize_successes.load() == 1,
          "concurrency: exactly one Initialize() succeeds");
    Check(controller.GetState() == hv::VirtualizationBackendState::ReadyForReview,
          "concurrency: the backend is fully initialized");
    Check(controller.GetStatus().successful_evaluations == 1,
          "concurrency: only one evaluation was committed");

    std::atomic<int> refresh_failures{0};
    std::vector<std::thread> refreshers;
    refreshers.reserve(4);
    for (int index = 0; index < 4; ++index) {
        refreshers.emplace_back([&controller, &refresh_failures]() {
            for (int iteration = 0; iteration < 5; ++iteration)
                if (!controller.Refresh().Succeeded()) ++refresh_failures;
        });
    }
    for (auto& thread : refreshers) thread.join();
    Check(refresh_failures.load() == 0,
          "concurrency: serialized Refresh() calls all succeed");
    const hv::VirtualizationBackendStatus refreshed = controller.GetStatus();
    Check(refreshed.successful_evaluations == 21,
          "concurrency: all refreshes are counted exactly once");
    Check(refreshed.generation == 21,
          "concurrency: generation tracks every transition");

    std::atomic<int> reader_failures{0};
    std::vector<std::thread> readers;
    readers.reserve(4);
    for (int index = 0; index < 4; ++index) {
        readers.emplace_back([&controller, &reader_failures]() {
            for (int iteration = 0; iteration < 25; ++iteration) {
                if (controller.GetStatus().state ==
                    hv::VirtualizationBackendState::Uninitialized)
                    ++reader_failures;
                const hv::VmxDiagnosticsReport report = controller.CaptureReport();
                if (report.backend.state ==
                    hv::VirtualizationBackendState::Uninitialized)
                    ++reader_failures;
                if (report.events.empty()) ++reader_failures;
            }
        });
    }
    for (auto& thread : readers) thread.join();
    Check(reader_failures.load() == 0,
          "concurrency: concurrent readers always observe a committed snapshot");

    Check(controller.Stop().Succeeded(), "concurrency: Stop() succeeds after contention");
    Check(controller.GetState() == hv::VirtualizationBackendState::Stopped,
          "concurrency: the backend reaches the terminal state");
    Check(controller.GetStatus().successful_evaluations == 21,
          "concurrency: Stop() does not change the evaluation counter");
}

void TestDiagnosticsJournal() {
    hv::VmxDiagnostics journal(3);
    Check(journal.Capacity() == 3, "journal: capacity is retained");
    journal.Record(hv::EventLevel::Info, hv::VmxDiagnosticCode::LifecycleInitialized, "e0");
    journal.Record(hv::EventLevel::Warning, hv::VmxDiagnosticCode::NotChecked, "e1");
    journal.Record(hv::EventLevel::Error, hv::VmxDiagnosticCode::EvaluationFailed, "e2");
    journal.Record(hv::EventLevel::Info, hv::VmxDiagnosticCode::LifecycleRefreshed, "e3");
    journal.Record(hv::EventLevel::Info, hv::VmxDiagnosticCode::LifecycleStopped, "e4");
    Check(journal.Size() == 3, "journal: bounded capacity is enforced");
    const auto snapshot = journal.Snapshot();
    Check(snapshot.size() == 3, "journal: snapshot is bounded");
    Check(snapshot.front().message == "e2" && snapshot.back().message == "e4",
          "journal: the newest events are retained");
    Check(snapshot.front().code == hv::VmxDiagnosticCode::EvaluationFailed,
          "journal: event codes are retained");
    Check(std::string(hv::ToString(snapshot.front().code)) == "EvaluationFailed",
          "journal: diagnostic code text is stable");

    journal.Clear();
    Check(journal.Size() == 0, "journal: Clear() empties the journal");

    hv::VmxDiagnostics zero_capacity(0);
    Check(zero_capacity.Capacity() == 1,
          "journal: zero capacity is clamped to one event");
    zero_capacity.Record(hv::EventLevel::Info, hv::VmxDiagnosticCode::None, "a");
    zero_capacity.Record(hv::EventLevel::Info, hv::VmxDiagnosticCode::None, "b");
    Check(zero_capacity.Size() == 1 &&
              zero_capacity.Snapshot().front().message == "b",
          "journal: clamped capacity keeps the newest event");
}

void TestDiagnosticsJournalThreadSafety() {
    hv::VmxDiagnostics journal(1000);
    std::vector<std::thread> writers;
    writers.reserve(4);
    for (int index = 0; index < 4; ++index) {
        writers.emplace_back([&journal, index]() {
            for (int event = 0; event < 100; ++event)
                journal.Record(hv::EventLevel::Info, hv::VmxDiagnosticCode::ReportGenerated,
                               "thread " + std::to_string(index) + " event " +
                                   std::to_string(event));
        });
    }
    for (auto& thread : writers) thread.join();
    Check(journal.Size() == 400, "journal: concurrent writers do not lose events");
    Check(journal.Snapshot().size() == 400,
          "journal: concurrent snapshot returns every event");
}

void TestDiagnosticsFormatting() {
    hv::VmxDiagnosticsReport report;
    report.backend.state = hv::VirtualizationBackendState::Blocked;
    report.backend.reason = hv::VmxBlockReason::DriverNotConnected;
    report.backend.error_class = hv::VmxErrorClass::None;
    report.backend.detail = "backend detail: \"quoted\" \\ slash\n";
    report.backend.generation = 7;
    report.backend.successful_evaluations = 3;
    report.backend.initialized = true;
    report.backend.stopped = false;
    report.backend.last_transition_failed = true;
    report.backend.last_transition_error = "transition: \"failed\"\n";
    report.backend.hardware_vmx_started = false;
    report.backend.hardware_start_supported = false;

    report.capability.summary = "capability: \"quoted\"\n";
    report.readiness.summary = "readiness: \"quoted\"\n";

    hv::VmxDiagnosticEvent event;
    event.level = hv::EventLevel::Error;
    event.code = hv::VmxDiagnosticCode::EvaluationFailed;
    event.message = "event: \"quoted\" \\ slash\n tab:\t control:\x01";
    event.timestamp = std::chrono::system_clock::now();
    report.events.push_back(event);

    const std::string json = hv::FormatVmxDiagnosticsJson(report);
    Check(json.find("\"schema\": \"geraduo.vmx.diagnostics.v1\"") != std::string::npos,
          "diagnostics JSON: schema is serialized");
    Check(json.find("\"tool_version\": \"0.6.0\"") != std::string::npos,
          "diagnostics JSON: tool version is serialized");
    Check(json.find("backend detail: \\\"quoted\\\" \\\\ slash\\n") != std::string::npos,
          "diagnostics JSON: backend detail is escaped");
    Check(json.find("transition: \\\"failed\\\"\\n") != std::string::npos,
          "diagnostics JSON: transition error is escaped");
    Check(json.find("event: \\\"quoted\\\" \\\\ slash\\n tab:\\t control:\\u0001") !=
              std::string::npos,
          "diagnostics JSON: event message is escaped");
    Check(json.find("\"vmxon_executed\": false") != std::string::npos,
          "diagnostics JSON: VMXON boundary is false");
    Check(json.find("\"vmcs_accessed\": false") != std::string::npos,
          "diagnostics JSON: VMCS boundary is false");
    Check(json.find("\"vm_entry_exit_executed\": false") != std::string::npos,
          "diagnostics JSON: VM-entry/exit boundary is false");
    Check(json.find("\"hardware_ept_configured\": false") != std::string::npos,
          "diagnostics JSON: EPT boundary is false");
    Check(json.find("\"privileged_instructions_executed\": false") != std::string::npos,
          "diagnostics JSON: privileged instruction boundary is false");

    const std::string text = hv::FormatVmxDiagnosticsText(report);
    Check(text.find("Geraduo Hypervisor Core v0.6.0 VT-x diagnostics") != std::string::npos,
          "diagnostics text: release header is present");
    Check(text.find("Diagnostic journal (1 events)") != std::string::npos,
          "diagnostics text: journal is present");
    Check(text.find("no VMXON") != std::string::npos,
          "diagnostics text: boundary statement is present");

    const std::string backend_json =
        hv::FormatVirtualizationBackendStatusJson(report.backend);
    Check(backend_json.find("\"state\": \"Blocked\"") != std::string::npos,
          "backend JSON: state is serialized");
    Check(backend_json.find("\"last_transition_failed\": true") != std::string::npos,
          "backend JSON: transition failure is serialized");
    Check(hv::IsTerminalBackendState(hv::VirtualizationBackendState::Stopped),
          "backend: Stopped is terminal");
    Check(!hv::IsTerminalBackendState(hv::VirtualizationBackendState::Failed),
          "backend: Failed is not terminal");
    Check(std::string(hv::ToString(hv::VirtualizationBackendState::CheckingCapabilities)) ==
              "CheckingCapabilities",
          "backend: state enum text is stable");
}

} // namespace

int main() {
    TestHappyPath();
    TestDoubleInitializeRejected();
    TestStopIsTerminal();
    TestRejectedBeforeInitialize();
    TestRefreshCommitsNewVerdict();
    TestRefreshRollbackOnPlatformFailure();
    TestRefreshRollbackOnCapabilityFailure();
    TestInitializeFailureCommitsFailedState();
    TestConcurrentLifecycle();
    TestDiagnosticsJournal();
    TestDiagnosticsJournalThreadSafety();
    TestDiagnosticsFormatting();

    if (test::failures != 0) {
        std::cerr << test::failures << " VT-x lifecycle test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All VT-x lifecycle tests passed\n";
    return EXIT_SUCCESS;
}
