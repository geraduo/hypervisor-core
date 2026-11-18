#include "hv/memory/address_space_manager.hpp"
#include "hv/memory/ept_manager.hpp"
#include "hv/memory/guest_memory_manager.hpp"
#include "hv/memory/memory_diagnostics.hpp"
#include "hv/memory/memory_mapping_manager.hpp"
#include "test_support.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

using test::Check;
using namespace hv::memory;

namespace {

constexpr std::uint64_t kGvaBase = 0x0000'0000'0001'0000ULL;

void TestAddressSpaceLifecycle() {
    GuestAddressSpaceManager spaces;
    const AddressSpaceCreationResult created = spaces.CreateAddressSpace("kernel");
    Check(created.Succeeded() && created.id.IsValid(),
          "space: create a named address space");
    Check(spaces.CreateAddressSpace("kernel").status == MemoryStatus::InvalidMapping,
          "space: duplicate address-space name is rejected");
    Check(spaces.CreateAddressSpace("").status == MemoryStatus::InvalidContext,
          "space: empty address-space name is rejected");
    Check(spaces.AddressSpaceCount() == 1 && spaces.Contains(created.id),
          "space: created address space is present");

    AddressSpaceInfo info{};
    Check(spaces.GetAddressSpace(created.id, info).Succeeded() && info.valid &&
              info.name == "kernel",
          "space: address-space metadata is readable");
    Check(spaces.EnumerateAddressSpaces().size() == 1,
          "space: address-space enumeration returns one entry");

    Check(spaces.DestroyAddressSpace(created.id).Succeeded(), "space: destroy address space");
    Check(!spaces.Contains(created.id) && spaces.AddressSpaceCount() == 0,
          "space: destroyed address space is gone");
    Check(spaces.DestroyAddressSpace(created.id).status == MemoryStatus::InvalidContext,
          "space: destroying a missing address space is rejected");
    Check(spaces.GetAddressSpace(created.id, info).status == MemoryStatus::InvalidContext,
          "space: reading a missing address space is rejected");
}

void TestMapTranslateUnmap() {
    GuestAddressSpaceManager spaces;
    const AddressSpaceId id = spaces.CreateAddressSpace("user").id;

    Check(spaces.Map(id, GuestVirtualAddress{0x1000}, GuestPhysicalAddress{0x2000},
                     Permissions{true, true, true})
              .Succeeded(),
          "space: map GVA to GPA");
    Check(spaces.Map(id, GuestVirtualAddress{0x1000}, GuestPhysicalAddress{0x3000},
                     Permissions{true, true, false})
              .status == MemoryStatus::AlreadyMapped,
          "space: duplicate GVA mapping is rejected");

    const TranslationResult translation =
        spaces.Translate(id, GuestVirtualAddress{0x1234}, Access::Read);
    Check(translation.Succeeded(), "space: GVA translation succeeds");
    Check(translation.guest_physical.Value() == 0x2234,
          "space: GVA translation preserves the page offset");
    Check(translation.guest_translated && !translation.nested_translated,
          "space: only the first stage is reported by the address-space manager");
    Check(translation.page_size == kSmallPageSize, "space: guest page size is 4 KiB");
    Check(translation.permissions.execute, "space: mapping permissions are returned");

    GuestMappingInfo mapping{};
    Check(spaces.Query(id, GuestVirtualAddress{0x1234}, mapping).Succeeded(),
          "space: query a mapped GVA");
    Check(mapping.virtual_page.Value() == 0x1000 &&
              mapping.guest_physical_page.Value() == 0x2000,
          "space: query reports page-aligned mapping endpoints");
    Check(spaces.EnumerateMappings(id).size() == 1,
          "space: enumerate one guest mapping");

    Check(spaces.Translate(id, GuestVirtualAddress{0x5000}, Access::Read).status ==
              MemoryStatus::Unmapped,
          "space: unmapped GVA translation reports unmapped");
    Check(spaces.Query(id, GuestVirtualAddress{0x5000}, mapping).status ==
              MemoryStatus::Unmapped,
          "space: query of an unmapped GVA reports unmapped");
    Check(spaces.Unmap(id, GuestVirtualAddress{0x1000}).Succeeded(),
          "space: unmap a GVA");
    Check(spaces.Unmap(id, GuestVirtualAddress{0x1000}).status == MemoryStatus::Unmapped,
          "space: double unmap is rejected");
    Check(spaces.Translate(id, GuestVirtualAddress{0x1000}, Access::Read).status ==
              MemoryStatus::Unmapped,
          "space: translation after unmap reports unmapped");

    Check(spaces.Map(id, GuestVirtualAddress{0x2000}, GuestPhysicalAddress{0x4000})
              .Succeeded(),
          "space: map a second page for clear test");
    Check(spaces.ClearMappings(id).Succeeded(), "space: clear all guest mappings");
    Check(spaces.TotalMappingCount() == 0, "space: clear removes every mapping");
}

void TestAlignmentAndContextErrors() {
    GuestAddressSpaceManager spaces;
    const AddressSpaceId id = spaces.CreateAddressSpace("errors").id;
    const AddressSpaceId missing{9999};

    Check(spaces.Map(id, GuestVirtualAddress{0x1001}, GuestPhysicalAddress{0x2000})
              .status == MemoryStatus::AlignmentError,
          "space: misaligned GVA is rejected");
    Check(spaces.Map(id, GuestVirtualAddress{0x1000}, GuestPhysicalAddress{0x2001})
              .status == MemoryStatus::AlignmentError,
          "space: misaligned GPA is rejected");
    Check(spaces.Map(id, GuestVirtualAddress{0x1000},
                     GuestPhysicalAddress{1ULL << 48U})
              .status == MemoryStatus::InvalidAddress,
          "space: GPA outside the 48-bit model is rejected");
    Check(spaces.Map(missing, GuestVirtualAddress{0x1000}, GuestPhysicalAddress{0x2000})
              .status == MemoryStatus::InvalidContext,
          "space: mapping into a missing address space is rejected");
    Check(spaces.Translate(missing, GuestVirtualAddress{0x1000}, Access::Read).status ==
              MemoryStatus::InvalidContext,
          "space: translation in a missing address space is rejected");
    Check(spaces.Protect(id, GuestVirtualAddress{0x1001},
                         Permissions{true, false, false})
              .status == MemoryStatus::AlignmentError,
          "space: misaligned protect is rejected");
    Check(spaces.Protect(id, GuestVirtualAddress{0x1000},
                         Permissions{true, false, false})
              .status == MemoryStatus::Unmapped,
          "space: protecting an unmapped page is rejected");
    Check(spaces.Unmap(missing, GuestVirtualAddress{0x1000}).status ==
              MemoryStatus::InvalidContext,
          "space: unmapping in a missing address space is rejected");
    Check(spaces.ClearMappings(missing).status == MemoryStatus::InvalidContext,
          "space: clearing a missing address space is rejected");
}

void TestProtectPermissions() {
    GuestAddressSpaceManager spaces;
    const AddressSpaceId id = spaces.CreateAddressSpace("protect").id;
    Check(spaces.Map(id, GuestVirtualAddress{0x1000}, GuestPhysicalAddress{0x2000},
                     Permissions{true, true, false})
              .Succeeded(),
          "space: map writable page for protect test");
    Check(spaces.Protect(id, GuestVirtualAddress{0x1000},
                         Permissions{true, false, false})
              .Succeeded(),
          "space: change guest permissions to read-only");
    Check(spaces.Translate(id, GuestVirtualAddress{0x1000}, Access::Write).status ==
              MemoryStatus::AccessDenied,
          "space: write denied after protection change");
    Check(spaces.Translate(id, GuestVirtualAddress{0x1000}, Access::Read).Succeeded(),
          "space: read still allowed after protection change");
    Check(spaces.Protect(id, GuestVirtualAddress{0x1000},
                         Permissions{true, true, true})
              .Succeeded(),
          "space: restore execute permission");
    Check(spaces.Translate(id, GuestVirtualAddress{0x1000}, Access::Execute).Succeeded(),
          "space: execute allowed after permission restore");
}

void TestAddressSpaceIsolation() {
    GuestAddressSpaceManager spaces;
    const AddressSpaceId first = spaces.CreateAddressSpace("first").id;
    const AddressSpaceId second = spaces.CreateAddressSpace("second").id;
    Check(spaces.Map(first, GuestVirtualAddress{0x1000}, GuestPhysicalAddress{0x2000})
              .Succeeded(),
          "space: map the same GVA in the first space");
    Check(spaces.Map(second, GuestVirtualAddress{0x1000}, GuestPhysicalAddress{0x3000})
              .Succeeded(),
          "space: map the same GVA in the second space");

    const TranslationResult first_result =
        spaces.Translate(first, GuestVirtualAddress{0x1000}, Access::Read);
    const TranslationResult second_result =
        spaces.Translate(second, GuestVirtualAddress{0x1000}, Access::Read);
    Check(first_result.Succeeded() && first_result.guest_physical.Value() == 0x2000,
          "space: first address space resolves its own mapping");
    Check(second_result.Succeeded() && second_result.guest_physical.Value() == 0x3000,
          "space: second address space resolves its own mapping");
    Check(spaces.TotalMappingCount() == 2, "space: isolation keeps two independent mappings");
}

struct Stack {
    FakeMemoryBackingProvider backing;
    GuestMemoryManager memory;
    GuestAddressSpaceManager spaces;
    EptManager ept;
    MemoryDiagnostics diagnostics;
    MemoryMappingManager mappings;
    AddressSpaceId space{};
    GuestPhysicalAddress gpa{};
    HostPhysicalAddress hpa{};

    explicit Stack(std::size_t pages)
        : memory(backing), mappings(spaces, ept, memory, &diagnostics) {
        (void)backing.Initialize();
        GuestRegionRequest request;
        request.name = "ram";
        request.size = pages * static_cast<std::size_t>(kSmallPageSize);
        request.permissions = Permissions{true, true, false};
        request.alignment = static_cast<std::size_t>(kSmallPageSize);
        const GuestRegionResult region = memory.AddRegion(request);
        if (!region.Succeeded()) return;
        gpa = region.region.guest_base;
        hpa = region.region.host_base;
        const AddressSpaceCreationResult created = spaces.CreateAddressSpace("stack");
        if (!created.Succeeded()) return;
        space = created.id;
    }

    [[nodiscard]] bool Ready() const noexcept {
        return space.IsValid() && !gpa.IsZero() && !hpa.IsZero();
    }
};

void TestTwoStageTranslationAndAccess() {
    Stack stack(2);
    Check(stack.Ready(), "two-stage: stack fixture initializes");
    if (!stack.Ready()) return;

    for (std::uint64_t page = 0; page < 2; ++page) {
        Check(stack.mappings
                  .MapAll(stack.space,
                          GuestVirtualAddress{kGvaBase + page * kSmallPageSize},
                          GuestPhysicalAddress{stack.gpa.Value() + page * kSmallPageSize},
                          Permissions{true, true, true})
                  .Succeeded(),
              "two-stage: MapAll maps GVA/GPA/EPT page " + std::to_string(page));
    }

    const TranslationResult translation = stack.mappings.Translate(
        stack.space, GuestVirtualAddress{kGvaBase + 0x1234}, Access::Read);
    Check(translation.Succeeded(), "two-stage: combined translation succeeds");
    Check(translation.guest_translated && translation.nested_translated,
          "two-stage: both stages are reported");
    Check(translation.host_physical.Value() == stack.hpa.Value() + 0x1234,
          "two-stage: host address preserves the guest offset");
    Check(translation.page_size == kSmallPageSize,
          "two-stage: effective page size is 4 KiB");

    const std::array<std::byte, 5> payload{std::byte{'h'}, std::byte{'e'},
                                           std::byte{'l'}, std::byte{'l'},
                                           std::byte{'o'}};
    Check(stack.mappings
              .Write(stack.space, GuestVirtualAddress{kGvaBase + 0x100}, payload)
              .Succeeded(),
          "two-stage: write through both stages");
    std::array<std::byte, 5> readback{};
    Check(stack.mappings
                  .Read(stack.space, GuestVirtualAddress{kGvaBase + 0x100}, readback)
                  .Succeeded() &&
              readback == payload,
          "two-stage: read through both stages returns the same bytes");

    Check(stack.mappings.Execute(stack.space, GuestVirtualAddress{kGvaBase}).status ==
              MemoryStatus::AccessDenied,
          "two-stage: region permissions deny execute");
    Check(stack.memory.SetRegionPermissions("ram", Permissions{true, true, true}).Succeeded(),
          "two-stage: enable execute on the region");
    Check(stack.mappings.Execute(stack.space, GuestVirtualAddress{kGvaBase}).Succeeded(),
          "two-stage: execute succeeds when every layer allows it");

    MappingInfo info{};
    Check(stack.mappings.GetMappingInfo(stack.space, GuestVirtualAddress{kGvaBase + 0x10},
                                        info)
              .Succeeded(),
          "two-stage: get mapping info");
    Check(info.guest_mapped && info.nested_mapped && info.backed,
          "two-stage: mapping info reports both stages and backing");
    Check(info.host_physical_address.Value() == stack.hpa.Value(),
          "two-stage: mapping info reports the nested host base");
    Check(stack.mappings.ValidateAll().Succeeded(),
          "two-stage: full-stack validation succeeds");

    Check(stack.mappings
              .ProtectAll(stack.space, GuestVirtualAddress{kGvaBase},
                          Permissions{true, false, false})
              .Succeeded(),
          "two-stage: ProtectAll changes both stages");
    Check(stack.mappings.Translate(stack.space, GuestVirtualAddress{kGvaBase}, Access::Write)
              .status == MemoryStatus::AccessDenied,
          "two-stage: ProtectAll denial is visible through translation");
    Check(stack.mappings.UnmapAll(stack.space, GuestVirtualAddress{kGvaBase}).Succeeded(),
          "two-stage: UnmapAll removes both stages");
    Check(stack.mappings.Translate(stack.space, GuestVirtualAddress{kGvaBase}, Access::Read)
              .status == MemoryStatus::Unmapped,
          "two-stage: UnmapAll makes the page untranslatable");
    Check(stack.spaces.TotalMappingCount() == 1 && stack.ept.MappingCount() == 1,
          "two-stage: UnmapAll leaves the other mapped page intact");
}

void TestPermissionLayers() {
    Stack stack(1);
    Check(stack.Ready(), "permissions: stack fixture initializes");
    if (!stack.Ready()) return;

    Check(stack.mappings
              .MapAll(stack.space, GuestVirtualAddress{kGvaBase},
                      stack.gpa, Permissions{true, true, false})
              .Succeeded(),
          "permissions: map a writable page");
    Check(stack.mappings
              .ProtectNested(stack.gpa, Permissions{true, false, false})
              .Succeeded(),
          "permissions: make the nested page read-only");
    Check(stack.mappings.Translate(stack.space, GuestVirtualAddress{kGvaBase}, Access::Write)
              .status == MemoryStatus::AccessDenied,
          "permissions: nested-stage write denial is enforced");
    Check(stack.mappings.Translate(stack.space, GuestVirtualAddress{kGvaBase}, Access::Read)
              .Succeeded(),
          "permissions: nested-stage read is still allowed");

    Check(stack.mappings
              .ProtectNested(stack.gpa, Permissions{true, true, false})
              .Succeeded(),
          "permissions: restore nested write permission");
    Check(stack.mappings
              .Protect(stack.space, GuestVirtualAddress{kGvaBase},
                       Permissions{true, false, false})
              .Succeeded(),
          "permissions: make the guest page read-only");
    Check(stack.mappings.Translate(stack.space, GuestVirtualAddress{kGvaBase}, Access::Write)
              .status == MemoryStatus::AccessDenied,
          "permissions: guest-stage write denial is enforced");
}

void TestUnmappedNestedStage() {
    Stack stack(1);
    Check(stack.Ready(), "nested: stack fixture initializes");
    if (!stack.Ready()) return;

    Check(stack.spaces.Map(stack.space, GuestVirtualAddress{kGvaBase}, stack.gpa,
                           Permissions{true, true, false})
              .Succeeded(),
          "nested: map the guest stage only");
    const TranslationResult missing =
        stack.mappings.Translate(stack.space, GuestVirtualAddress{kGvaBase}, Access::Read);
    Check(missing.status == MemoryStatus::Unmapped && missing.guest_translated &&
              !missing.nested_translated,
          "nested: missing second stage reports unmapped");

    GuestRegionInfo region{};
    Check(stack.memory.GetRegion("ram", region).Succeeded(),
          "nested: read region metadata");
    MemoryBlock block{};
    Check(stack.backing.TryGetBlock(region.host_base, block),
          "nested: region backing block is resolvable");
    Check(stack.mappings.MapNested(stack.gpa, block.address, Permissions{true, true, false})
              .Succeeded(),
          "nested: map the second stage explicitly");
    Check(stack.mappings.Translate(stack.space, GuestVirtualAddress{kGvaBase}, Access::Read)
              .Succeeded(),
          "nested: translation succeeds after the second stage is mapped");
    Check(stack.mappings.UnmapNested(stack.gpa).Succeeded(),
          "nested: remove the second stage again");
}

void TestConcurrentMappings() {
    constexpr std::size_t kPages = 64;
    Stack stack(kPages);
    Check(stack.Ready(), "concurrent: stack fixture initializes");
    if (!stack.Ready()) return;

    constexpr int kThreads = 4;
    constexpr int kPagesPerThread = 16;
    std::atomic<bool> all_ok{true};
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int thread = 0; thread < kThreads; ++thread) {
        workers.emplace_back([&stack, &all_ok, thread]() {
            for (int page = 0; page < kPagesPerThread; ++page) {
                const std::uint64_t index =
                    static_cast<std::uint64_t>(thread) * kPagesPerThread +
                    static_cast<std::uint64_t>(page);
                const MemoryResult result = stack.mappings.MapAll(
                    stack.space,
                    GuestVirtualAddress{kGvaBase + index * kSmallPageSize},
                    GuestPhysicalAddress{stack.gpa.Value() + index * kSmallPageSize},
                    Permissions{true, true, false});
                if (!result.Succeeded()) all_ok = false;
            }
        });
    }
    for (std::thread& worker : workers) worker.join();

    Check(all_ok.load(), "concurrent: every MapAll call succeeds");
    Check(stack.spaces.TotalMappingCount() == kThreads * kPagesPerThread,
          "concurrent: guest mapping count is exact");
    Check(stack.ept.MappingCount() == kThreads * kPagesPerThread,
          "concurrent: nested mapping count is exact");
    Check(stack.mappings.ValidateAll().Succeeded(),
          "concurrent: full-stack validation after parallel mapping");
}

} // namespace

int main() {
    TestAddressSpaceLifecycle();
    TestMapTranslateUnmap();
    TestAlignmentAndContextErrors();
    TestProtectPermissions();
    TestAddressSpaceIsolation();
    TestTwoStageTranslationAndAccess();
    TestPermissionLayers();
    TestUnmappedNestedStage();
    TestConcurrentMappings();

    if (test::failures != 0) {
        std::cerr << test::failures << " address-space test(s) failed\n";
        return 1;
    }
    std::cout << "All address-space tests passed\n";
    return 0;
}
