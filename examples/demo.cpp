#include "hv/hypervisor.hpp"
#include "hv/memory_manager.hpp"
#include <iostream>

namespace {
class DemoHost final : public hv::IHostBackend {
public:
    hv::Result Initialize() override { std::cout << "Host backend initialized (simulation)\n"; return hv::Result::Ok(); }
    hv::Result Shutdown() override { std::cout << "Host backend stopped (simulation)\n"; return hv::Result::Ok(); }
};
class DemoGuest final : public hv::IGuestBackend {
public:
    hv::Result StartTestSession() override { std::cout << "Synthetic guest test session started\n"; return hv::Result::Ok(); }
    hv::Result StopTestSession() override { std::cout << "Synthetic guest test session stopped\n"; return hv::Result::Ok(); }
};
}

int main() {
    DemoHost host;
    DemoGuest guest;
    hv::Hypervisor core(host, guest);
    auto result = core.Start();
    if (!result.Succeeded()) { std::cerr << "Start failed: " << result.message << '\n'; return 1; }

    hv::SyntheticPageTable guest_tables;
    hv::EptManager ept;
    (void)guest_tables.Map(0x1000, 0x5000, {true, true, false});
    (void)ept.Map(0x5000, 0x9000, {true, true, false});
    hv::MemoryManager memory(guest_tables, ept);
    const auto translated = memory.Translate({1, true}, 0x1234, hv::Access::Read);
    if (translated.Succeeded())
        std::cout << "Synthetic GVA 0x1234 -> HPA 0x" << std::hex
                  << translated.host_physical_address << std::dec << '\n';

    result = core.Stop();
    if (!result.Succeeded()) { std::cerr << "Stop failed: " << result.message << '\n'; return 2; }
    return 0;
}
