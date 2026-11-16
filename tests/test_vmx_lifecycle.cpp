#include "fakes/fake_vmx.hpp"
#include "test_support.hpp"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

using test::Check;
using hv::VmxLifecycleState;

namespace {

void TestTransitionTable() {
    using C = hv::VmxLifecycleController;
    Check(C::IsTransitionAllowed(VmxLifecycleState::Uninitialized, VmxLifecycleState::CheckingCapabilities),
          "table: Uninitialized -> CheckingCapabilities");
    Check(C::IsTransitionAllowed(VmxLifecycleState::CheckingCapabilities, VmxLifecycleState::ReadyForReview) &&
              C::IsTransitionAllowed(VmxLifecycleState::CheckingCapabilities, VmxLifecycleState::NotSupported) &&
              C::IsTransitionAllowed(VmxLifecycleState::CheckingCapabilities, VmxLifecycleState::Blocked) &&
              C::IsTransitionAllowed(VmxLifecycleState::CheckingCapabilities, VmxLifecycleState::Failed),
          "table: a check can end in every terminal verdict state");
    Check(!C::IsTransitionAllowed(VmxLifecycleState::Uninitialized, VmxLifecycleState::ReadyForReview),
          "table: Uninitialized -> ReadyForReview is rejected");
    Check(!C::IsTransitionAllowed(VmxLifecycleState::NotSupported, VmxLifecycleState::ReadyForReview) &&
              !C::IsTransitionAllowed(VmxLifecycleState::Blocked, VmxLifecycleState::ReadyForReview) &&
              !C::IsTransitionAllowed(VmxLifecycleState::Failed, VmxLifecycleState::ReadyForReview),
          "table: no verdict state jumps to ReadyForReview without a new check");
    Check(!C::IsTransitionAllowed(VmxLifecycleState::Stopped, VmxLifecycleState::ReadyForReview) &&
              !C::IsTransitionAllowed(VmxLifecycleState::CheckingCapabilities,
                                      VmxLifecycleState::CheckingCapabilities) &&
              !C::IsTransitionAllowed(VmxLifecycleState::ReadyForReview, VmxLifecycleState::Uninitialized),
          "table: other invalid transitions are rejected");
    Check(C::IsTransitionAllowed(VmxLifecycleState::Stopped, VmxLifecycleState::CheckingCapabilities),
          "table: a stopped controller can be restarted");
}

void TestInvalidTransitionsDoNotChangeState() {
    hv::VmxDiagnostics diagnostics;
    hv::VmxLifecycleController controller(&diagnostics);
    const hv::Result bad = controller.Transition(VmxLifecycleState::ReadyForReview, "illegal");
    Check(!bad.Succeeded() && bad.status == hv::Status::InvalidState &&
              controller.State() == VmxLifecycleState::Uninitialized &&
              controller.History().empty() && !controller.GetLastError().empty(),
          "invalid transition: InvalidState, state and history unchanged, error recorded");

    std::uint64_t token = 0;
    Check(controller.BeginCheck(token).Succeeded() &&
              controller.State() == VmxLifecycleState::CheckingCapabilities,
          "BeginCheck moves to CheckingCapabilities");
    std::uint64_t second = 0;
    Check(controller.BeginCheck(second).status == hv::Status::InvalidState,
          "repeated BeginCheck while checking is rejected");
    Check(controller.CompleteCheck(token, VmxLifecycleState::Stopped, "x").status ==
              hv::Status::InvalidArgument,
          "CompleteCheck rejects a non-verdict outcome");
    Check(controller.CompleteCheck(token + 1, VmxLifecycleState::ReadyForReview, "x").status ==
              hv::Status::InvalidState,
          "CompleteCheck rejects a wrong token");
    Check(controller.State() == VmxLifecycleState::CheckingCapabilities,
          "rejected completions leave the state unchanged");
    Check(controller.CompleteCheck(token, VmxLifecycleState::Blocked, "driver missing").Succeeded() &&
              controller.State() == VmxLifecycleState::Blocked,
          "valid completion applies the outcome");
    Check(!controller.CompleteCheck(token, VmxLifecycleState::ReadyForReview, "replay").Succeeded(),
          "a completed check cannot be completed twice");

    bool rejection_logged = false;
    for (const auto& event : diagnostics.Snapshot()) {
        if (event.code == hv::VmxEventCode::TransitionRejected) rejection_logged = true;
    }
    Check(rejection_logged, "rejected transitions are written to the diagnostics log");
}

void TestStopSupersedesCheck() {
    hv::VmxLifecycleController controller;
    std::uint64_t token = 0;
    Check(controller.BeginCheck(token).Succeeded(), "supersede: check started");
    Check(controller.Stop("shutdown").Succeeded() && controller.State() == VmxLifecycleState::Stopped,
          "supersede: Stop wins");
    Check(controller.CompleteCheck(token, VmxLifecycleState::ReadyForReview, "late").status ==
                  hv::Status::InvalidState &&
              controller.State() == VmxLifecycleState::Stopped,
          "supersede: a stale check result cannot overwrite Stopped");
    Check(controller.Stop("again").Succeeded(), "repeated Stop is an idempotent success");
    Check(controller.Transition(VmxLifecycleState::Uninitialized, "reset").Succeeded(),
          "Stopped -> Uninitialized is allowed");
}

void TestConcurrentBeginCheck() {
    hv::VmxLifecycleController controller;
    std::atomic<int> winners{0};
    std::atomic<int> rejected{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 16; ++i) {
        threads.emplace_back([&] {
            std::uint64_t token = 0;
            const hv::Result result = controller.BeginCheck(token);
            if (result.Succeeded()) ++winners;
            else if (result.status == hv::Status::InvalidState) ++rejected;
        });
    }
    for (auto& thread : threads) thread.join();
    Check(winners == 1 && rejected == 15,
          "concurrent BeginCheck: exactly one caller wins, the others get InvalidState");
    Check(controller.State() == VmxLifecycleState::CheckingCapabilities &&
              controller.History().size() == 1,
          "concurrent BeginCheck: a single transition was recorded");
}

void TestBackendHappyPath() {
    fakes::VmxRig rig(4, true);
    hv::IntelVirtualizationBackend backend(rig.readiness, rig.diagnostics);

    hv::VmxDiagnosticReport early;
    Check(!backend.RunDiagnostics(early).Succeeded() && early.state == VmxLifecycleState::Uninitialized &&
              !early.readiness_available,
          "backend: diagnostics before Initialize report InvalidState and no data");

    Check(backend.Initialize().Succeeded() && backend.GetState() == VmxLifecycleState::ReadyForReview,
          "backend: Initialize reaches ReadyForReview");
    Check(!backend.IsHypervisorRunning(), "backend: no hypervisor is running");
    Check(backend.Initialize().Succeeded() && rig.driver.connect_calls == 1,
          "backend: a repeated Initialize re-checks without reconnecting the driver");

    const hv::Result start = backend.StartVirtualization();
    Check(start.status == hv::Status::Unsupported && !backend.IsHypervisorRunning() &&
              backend.GetState() == VmxLifecycleState::ReadyForReview,
          "backend: StartVirtualization is refused and nothing is started");

    hv::VmxDiagnosticReport report;
    Check(backend.RunDiagnostics(report).Succeeded() && report.readiness_available &&
              !report.hypervisor_running && !report.hardware_virtualization_started &&
              !report.transitions.empty() && !report.events.empty(),
          "backend: diagnostics contain readiness, transitions and events");

    Check(backend.Shutdown().Succeeded() && backend.GetState() == VmxLifecycleState::Stopped &&
              !rig.driver.connected && rig.driver.disconnect_calls == 1,
          "backend: Shutdown stops and closes the driver connection");
    Check(backend.Shutdown().Succeeded() && rig.driver.disconnect_calls == 1,
          "backend: a repeated Shutdown is idempotent");
    Check(backend.Initialize().Succeeded() && backend.GetState() == VmxLifecycleState::ReadyForReview,
          "backend: can be restarted after Stop");
    static_cast<void>(backend.Shutdown());
}

void TestBackendNotSupportedAndBlocked() {
    {
        fakes::VmxRig rig(2, false);
        hv::IntelVirtualizationBackend backend(rig.readiness, rig.diagnostics);
        const hv::Result result = backend.Initialize();
        Check(result.status == hv::Status::Unsupported &&
                  backend.GetState() == VmxLifecycleState::NotSupported && !rig.driver.connected,
              "backend: no VMX -> NotSupported, driver released");
        Check(!backend.GetLastError().empty(), "backend: last error is retained");
    }
    {
        fakes::VmxRig rig(2, true);
        rig.driver.connect_status = hv::Status::DriverNotFound;
        hv::IntelVirtualizationBackend backend(rig.readiness, rig.diagnostics);
        Check(!backend.Initialize().Succeeded() && backend.GetState() == VmxLifecycleState::Blocked,
              "backend: missing driver -> Blocked");
    }
}

void TestBackendFailureAndRecovery() {
    fakes::VmxRig rig(2, true);
    rig.platform.system_result = hv::Result::Error(hv::Status::InitializationFailed, "fault");
    hv::IntelVirtualizationBackend backend(rig.readiness, rig.diagnostics);
    const hv::Result failed = backend.Initialize();
    Check(failed.status == hv::Status::InitializationFailed &&
              backend.GetState() == VmxLifecycleState::Failed,
          "recovery: Platform Backend error -> Failed with the original status");

    hv::VmxDiagnosticReport report;
    Check(backend.RunDiagnostics(report).Succeeded() && report.readiness_available &&
              !report.readiness.reasons.empty(),
          "recovery: a Failed backend still produces a diagnostic report with reasons");

    rig.platform.system_result = hv::Result::Ok();
    Check(backend.Initialize().Succeeded() && backend.GetState() == VmxLifecycleState::ReadyForReview,
          "recovery: Failed -> re-check -> ReadyForReview after the fault is gone");
    static_cast<void>(backend.Shutdown());
}

void TestConcurrentInitializeAndDiagnostics() {
    fakes::VmxRig rig(4, true);
    hv::IntelVirtualizationBackend backend(rig.readiness, rig.diagnostics);
    std::atomic<int> ok{0};
    std::atomic<int> other{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 5; ++i) {
                if (backend.Initialize().Succeeded()) ++ok;
                else ++other;
                hv::VmxDiagnosticReport report;
                static_cast<void>(backend.RunDiagnostics(report));
            }
        });
    }
    for (auto& thread : threads) thread.join();
    Check(ok == 40 && other == 0 && backend.GetState() == VmxLifecycleState::ReadyForReview,
          "concurrent Initialize: all requests complete and the final state is consistent");
    Check(rig.driver.connect_calls == 1, "concurrent Initialize: the driver is connected only once");
    static_cast<void>(backend.Shutdown());
    Check(!rig.driver.connected, "concurrent Initialize: Shutdown releases the driver");
}

void TestConcurrentInitializeAndShutdown() {
    fakes::VmxRig rig(4, true);
    hv::IntelVirtualizationBackend backend(rig.readiness, rig.diagnostics);
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < 10; ++i) {
                if ((t % 2) == 0) static_cast<void>(backend.Initialize());
                else static_cast<void>(backend.Shutdown());
            }
        });
    }
    for (auto& thread : threads) thread.join();
    const VmxLifecycleState state = backend.GetState();
    Check(state == VmxLifecycleState::Stopped || state == VmxLifecycleState::ReadyForReview,
          "concurrent Initialize/Shutdown: the state is one of the two valid end states");
    Check(rig.driver.connected == (state == VmxLifecycleState::ReadyForReview),
          "concurrent Initialize/Shutdown: the driver connection matches the state");
    static_cast<void>(backend.Shutdown());
    Check(backend.GetState() == VmxLifecycleState::Stopped && !rig.driver.connected,
          "concurrent Initialize/Shutdown: final Shutdown leaves nothing connected");
}

} // namespace

int main() {
    TestTransitionTable();
    TestInvalidTransitionsDoNotChangeState();
    TestStopSupersedesCheck();
    TestConcurrentBeginCheck();
    TestBackendHappyPath();
    TestBackendNotSupportedAndBlocked();
    TestBackendFailureAndRecovery();
    TestConcurrentInitializeAndDiagnostics();
    TestConcurrentInitializeAndShutdown();
    if (test::failures != 0) return EXIT_FAILURE;
    std::cout << "All VMX lifecycle tests passed\n";
    return EXIT_SUCCESS;
}
