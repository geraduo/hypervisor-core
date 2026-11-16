#include "hv/hypervisor.hpp"
#include "hv/memory_manager.hpp"
#include "hv/program_runner.hpp"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {
int failures = 0;
void Check(bool condition, const std::string& name) {
    if (condition) std::cout << "[PASS] " << name << '\n';
    else { std::cerr << "[FAIL] " << name << '\n'; ++failures; }
}
class FakeHost final : public hv::IHostBackend {
public:
    bool initialized{false};
    hv::Result Initialize() override { initialized = true; return hv::Result::Ok(); }
    hv::Result Shutdown() override { initialized = false; return hv::Result::Ok(); }
};
class FakeGuest final : public hv::IGuestBackend {
public:
    bool running{false};
    bool fail_start{false};
    hv::Result StartTestSession() override {
        if (fail_start) return hv::Result::Error(hv::Status::InitializationFailed, "synthetic guest failure");
        running = true; return hv::Result::Ok();
    }
    hv::Result StopTestSession() override { running = false; return hv::Result::Ok(); }
};

void TestTranslation() {
    hv::SyntheticPageTable guest;
    hv::EptManager nested;
    Check(guest.Map(0x1000, 0x5000, {true, true, false}).Succeeded(), "map GVA page");
    Check(nested.Map(0x5000, 0x9000, {true, true, false}).Succeeded(), "map GPA page");
    hv::MemoryManager manager(guest, nested);
    const auto result = manager.Translate({7, true}, 0x1234, hv::Access::Read);
    Check(result.Succeeded(), "two-stage translation succeeds");
    Check(result.host_physical_address == 0x9234, "page offset preserved");
    Check(manager.Translate({7, true}, 0x1234, hv::Access::Execute).status ==
          hv::TranslationStatus::AccessDenied, "permission denied for execute");
    Check(manager.Translate({0, false}, 0x1234, hv::Access::Read).status ==
          hv::TranslationStatus::InvalidContext, "invalid context rejected");
    Check(manager.Translate({7, true}, 0xA234, hv::Access::Read).status ==
          hv::TranslationStatus::UnmappedGuestVirtual, "unmapped GVA rejected");
}

void TestLifecycleAndOrchestration() {
    hv::Lifecycle lifecycle;
    Check(lifecycle.GetState() == hv::State::Stopped, "lifecycle starts stopped");
    Check(lifecycle.MarkRunning().status == hv::Status::InvalidState, "illegal state transition rejected");
    Check(lifecycle.BeginInitialization().Succeeded(), "begin initialization");
    Check(lifecycle.MarkRunning().Succeeded(), "mark running");
    Check(lifecycle.BeginShutdown().Succeeded(), "begin shutdown");
    Check(lifecycle.MarkStopped().Succeeded(), "mark stopped");

    FakeHost host; FakeGuest guest; hv::Hypervisor core(host, guest);
    Check(core.Start().Succeeded(), "orchestrator starts");
    Check(core.GetState() == hv::State::Running, "orchestrator reports running");
    Check(core.Stop().Succeeded(), "orchestrator stops");
    Check(!host.initialized && !guest.running, "backends cleaned up");

    FakeHost host2; FakeGuest guest2; guest2.fail_start = true;
    hv::Hypervisor failing(host2, guest2);
    Check(!failing.Start().Succeeded(), "guest startup error is returned");
    Check(!host2.initialized, "host rolled back after guest startup error");
    Check(failing.GetState() == hv::State::Stopped, "successful rollback returns to stopped");
}

void TestProgramRunner() {
    hv::ProgramRunner runner;
    hv::ProgramSpec spec;
#ifdef _WIN32
    spec.executable = "C:\\Windows\\System32\\cmd.exe";
    spec.arguments = {"/C", "exit", "7"};
#else
    spec.executable = "/bin/sh";
    spec.arguments = {"-c", "exit 7"};
#endif
    hv::ProcessInfo info;
    auto result = runner.Launch(spec, info);
    Check(result.Succeeded(), "host process launches");
    if (!result.Succeeded()) return;
    result = runner.Wait(info.handle, std::chrono::seconds(10), info);
    Check(result.Succeeded(), "host process wait completes");
    Check(info.has_exit_code && info.exit_code == 7, "host process exit code captured");
    Check(!info.running, "host process is no longer running");
    Check(runner.Forget(info.handle).Succeeded(), "exited process record can be released");
}
} // namespace

int main() {
    TestTranslation();
    TestLifecycleAndOrchestration();
    TestProgramRunner();
    if (failures != 0) { std::cerr << failures << " test(s) failed\n"; return EXIT_FAILURE; }
    std::cout << "All tests passed\n";
    return EXIT_SUCCESS;
}
