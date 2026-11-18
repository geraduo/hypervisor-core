#include "hv/memory/guest_memory_manager.hpp"
#include "test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <vector>

using test::Check;
using namespace hv::memory;

namespace {

GuestRegionRequest MakeRequest(std::string name, std::size_t pages,
                               GuestPhysicalAddress base = GuestPhysicalAddress{}) {
    GuestRegionRequest request;
    request.name = std::move(name);
    request.guest_base = base;
    request.size = pages * static_cast<std::size_t>(kSmallPageSize);
    request.permissions = Permissions{true, true, false};
    request.alignment = static_cast<std::size_t>(kSmallPageSize);
    return request;
}

void TestRegionAllocationAndBacking() {
    FakeMemoryBackingProvider backing;
    Check(backing.Initialize().Succeeded(), "guest: backing provider initializes");
    GuestMemoryManager memory(backing);

    const GuestRegionResult region = memory.AddRegion(MakeRequest("ram", 3));
    Check(region.Succeeded(), "guest: add a three-page RAM region");
    Check(region.region.size == 3 * kSmallPageSize, "guest: region size is exact");
    Check(region.region.guest_base.IsAlignedTo(kSmallPageSize),
          "guest: region base is page aligned");
    Check(region.region.host_base.Value() >= kFirstSyntheticHostAddress,
          "guest: region receives a synthetic host-physical base");
    Check(backing.AllocationCount() == 1, "guest: one real backing allocation exists");
    Check(backing.AllocatedBytes() == 3 * kSmallPageSize,
          "guest: backing allocation has the region size");
    Check(memory.RegionCount() == 1 && memory.PageCount() == 3,
          "guest: region and page counters are correct");
    Check(memory.AllocatedBytes() == 3 * kSmallPageSize,
          "guest: allocated-byte gauge matches the region");
}

void TestWriteReadAcrossPages() {
    FakeMemoryBackingProvider backing;
    GuestMemoryManager memory(backing);
    const GuestRegionResult region = memory.AddRegion(MakeRequest("rw", 3));
    Check(region.Succeeded(), "guest: add region for read/write test");
    const GuestPhysicalAddress base = region.region.guest_base;

    const std::array<std::byte, 4> payload{
        std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44}};
    const MemoryResult written =
        memory.Write(GuestPhysicalAddress{base.Value() + kSmallPageSize - 2}, payload);
    Check(written.Succeeded(), "guest: write crosses a page boundary");

    std::array<std::byte, 4> readback{};
    const MemoryResult read =
        memory.Read(GuestPhysicalAddress{base.Value() + kSmallPageSize - 2}, readback);
    Check(read.Succeeded(), "guest: read crosses a page boundary");
    Check(readback == payload, "guest: page-crossing data round-trips");

    GuestPageInfo page{};
    Check(memory.QueryPage(GuestPhysicalAddress{base.Value() + kSmallPageSize}, page)
              .Succeeded(),
          "guest: query a backed guest page");
    Check(page.backed && page.region_name == "rw", "guest: page reports its region");
    Check(page.host_physical.Value() ==
              region.region.host_base.Value() + kSmallPageSize,
          "guest: page host address includes the in-region offset");
}

void TestRegionPermissions() {
    FakeMemoryBackingProvider backing;
    GuestMemoryManager memory(backing);
    const GuestRegionResult region = memory.AddRegion(MakeRequest("ro", 1));
    Check(region.Succeeded(), "guest: add region for permission test");
    const GuestPhysicalAddress base = region.region.guest_base;

    Check(memory.SetRegionPermissions("ro", Permissions{true, false, false}).Succeeded(),
          "guest: switch region to read-only");
    const std::array<std::byte, 1> byte{std::byte{0x7F}};
    Check(memory.Write(base, byte).status == MemoryStatus::AccessDenied,
          "guest: write denied on a read-only region");
    Check(memory.Translate(base, Access::Execute).status == MemoryStatus::AccessDenied,
          "guest: execute denied on a read-only region");
    Check(memory.Translate(base, Access::Read).Succeeded(),
          "guest: read allowed on a read-only region");

    Check(memory.WriteDirect(base, byte).Succeeded(),
          "guest: direct snapshot access bypasses region permissions");
    std::array<std::byte, 1> direct{};
    Check(memory.ReadDirect(base, direct).Succeeded() && direct == byte,
          "guest: direct snapshot read bypasses region permissions");

    Check(memory.SetRegionPermissions("missing", Permissions{true, true, true}).status ==
              MemoryStatus::InvalidContext,
          "guest: permission change on a missing region is rejected");
}

void TestRegionValidation() {
    FakeMemoryBackingProvider backing;
    GuestMemoryManager memory(backing);
    GuestPageInfo probe{};

    GuestRegionRequest zero_size = MakeRequest("zero", 1);
    zero_size.size = 0;
    Check(memory.AddRegion(zero_size).status == MemoryStatus::InvalidAddress,
          "guest: zero-size region is rejected");

    GuestRegionRequest partial_page = MakeRequest("partial", 1);
    partial_page.size = kSmallPageSize + 1;
    Check(memory.AddRegion(partial_page).status == MemoryStatus::AlignmentError,
          "guest: non-page-multiple region size is rejected");

    GuestRegionRequest bad_alignment = MakeRequest("bad-align", 1);
    bad_alignment.alignment = 3072;
    Check(memory.AddRegion(bad_alignment).status == MemoryStatus::AlignmentError,
          "guest: non-power-of-two region alignment is rejected");

    GuestRegionRequest unaligned_base =
        MakeRequest("unaligned", 1, GuestPhysicalAddress{0x5000'0001ULL});
    Check(memory.AddRegion(unaligned_base).status == MemoryStatus::AlignmentError,
          "guest: misaligned explicit region base is rejected");

    GuestRegionRequest oversized = MakeRequest("oversized", 1);
    oversized.size = static_cast<std::size_t>(1ULL << 48U);
    Check(memory.AddRegion(oversized).status == MemoryStatus::InvalidAddress,
          "guest: region larger than the 48-bit model is rejected");

    const GuestRegionResult first =
        memory.AddRegion(MakeRequest("first", 1, GuestPhysicalAddress{0x5000'0000ULL}));
    Check(first.Succeeded(), "guest: add explicit-base region");
    Check(memory.AddRegion(MakeRequest("first", 1)).status == MemoryStatus::InvalidMapping,
          "guest: duplicate region name is rejected");
    Check(memory.AddRegion(MakeRequest("overlap", 1, GuestPhysicalAddress{0x5000'0000ULL}))
              .status == MemoryStatus::InvalidMapping,
          "guest: overlapping explicit region is rejected");

    Check(memory.QueryPage(GuestPhysicalAddress{0x5000'0001ULL}, probe).status ==
              MemoryStatus::AlignmentError,
          "guest: unaligned page query is rejected");
    Check(memory.QueryPage(GuestPhysicalAddress{0x6000'0000ULL}, probe).status ==
              MemoryStatus::Unmapped,
          "guest: unregistered page query reports unmapped");
    Check(memory.Translate(GuestPhysicalAddress{0x6000'0000ULL}, Access::Read).status ==
              MemoryStatus::Unmapped,
          "guest: unregistered GPA translation reports unmapped");
    Check(memory.Translate(GuestPhysicalAddress{kMaxPhysicalAddress + 1ULL}, Access::Read)
              .status == MemoryStatus::InvalidAddress,
          "guest: GPA outside the 48-bit model is rejected");

    std::array<std::byte, 16> buffer{};
    Check(memory.Read(GuestPhysicalAddress{kMaxPhysicalAddress - 3ULL}, buffer).status ==
              MemoryStatus::InvalidAddress,
          "guest: read span crossing the 48-bit limit is rejected");
}

void TestRemoveRegionReleasesBacking() {
    FakeMemoryBackingProvider backing;
    GuestMemoryManager memory(backing);
    const GuestRegionResult region = memory.AddRegion(MakeRequest("temp", 2));
    Check(region.Succeeded(), "guest: add region for removal test");
    Check(backing.AllocationCount() == 1, "guest: removal test has one allocation");

    Check(memory.RemoveRegion("temp").Succeeded(), "guest: remove region by name");
    Check(memory.RegionCount() == 0, "guest: removal clears the region table");
    Check(backing.AllocationCount() == 0 && backing.AllocatedBytes() == 0,
          "guest: removal releases the real backing allocation");
    Check(memory.Translate(region.region.guest_base, Access::Read).status ==
              MemoryStatus::Unmapped,
          "guest: removed region no longer translates");
    Check(memory.RemoveRegion("temp").status == MemoryStatus::InvalidContext,
          "guest: removing a missing region is rejected");
    Check(memory.RemoveRegion(0xFFFFULL).status == MemoryStatus::InvalidContext,
          "guest: removing a missing region id is rejected");

    const GuestRegionResult one = memory.AddRegion(MakeRequest("one", 1));
    const GuestRegionResult two = memory.AddRegion(MakeRequest("two", 1));
    Check(one.Succeeded() && two.Succeeded(), "guest: add two regions for ReleaseAll");
    Check(backing.AllocationCount() == 2, "guest: two backing allocations exist");
    Check(memory.ReleaseAll().Succeeded(), "guest: ReleaseAll succeeds");
    Check(memory.RegionCount() == 0 && backing.AllocationCount() == 0,
          "guest: ReleaseAll releases every allocation");
}

void TestOutOfMemory() {
    FakeMemoryBackingProvider backing(4096);
    Check(backing.Initialize().Succeeded(), "guest: limited backing initializes");
    GuestMemoryManager memory(backing);

    Check(memory.AddRegion(MakeRequest("too-big", 2)).status == MemoryStatus::OutOfMemory,
          "guest: region larger than the backing limit reports OutOfMemory");
    Check(memory.RegionCount() == 0 && backing.AllocationCount() == 0,
          "guest: failed allocation leaves no region behind");

    backing.SetMaxBytes(FakeMemoryBackingProvider::kDefaultMaxBytes);
    backing.SetFailNextAllocation(true);
    Check(memory.AddRegion(MakeRequest("injected", 1)).status == MemoryStatus::OutOfMemory,
          "guest: injected allocation failure reports OutOfMemory");
    backing.SetFailNextAllocation(false);

    Check(memory.AddRegion(MakeRequest("injected", 1)).Succeeded(),
          "guest: a later allocation succeeds after the injected failure clears");
    backing.SetFailAllocations(true);
    Check(memory.AddRegion(MakeRequest("blocked", 1)).status == MemoryStatus::OutOfMemory,
          "guest: persistent allocation failure reports OutOfMemory");
    backing.SetFailAllocations(false);
}

void TestBackingProviderLifecycle() {
    FakeMemoryBackingProvider backing;
    Check(backing.IsInitialized(), "guest: fake provider starts initialized");
    const MemoryBlockResult block = backing.Allocate(4096, 4096);
    Check(block.Succeeded(), "guest: raw backing allocation succeeds");
    Check(block.block.address.IsAlignedTo(4096), "guest: backing address is page aligned");
    Check(backing.Owns(block.block.address), "guest: provider owns its allocation");
    Check(backing.Allocate(4096, 3000).status == MemoryStatus::AlignmentError,
          "guest: invalid backing alignment is rejected");
    Check(backing.Allocate(0, 4096).status == MemoryStatus::InvalidAddress,
          "guest: zero-size backing allocation is rejected");

    const std::array<std::byte, 4> data{std::byte{1}, std::byte{2}, std::byte{3},
                                        std::byte{4}};
    Check(backing.Write(block.block.address, data).Succeeded(),
          "guest: raw backing write succeeds");
    std::array<std::byte, 4> readback{};
    Check(backing.Read(block.block.address, readback).Succeeded() && readback == data,
          "guest: raw backing read succeeds");
    Check(backing.Read(HostPhysicalAddress{block.block.address.Value() + 4094ULL}, readback)
              .status == MemoryStatus::InvalidAddress,
          "guest: raw backing read cannot cross an allocation end");

    Check(backing.Shutdown().Succeeded(), "guest: backing shutdown succeeds");
    Check(!backing.IsInitialized(), "guest: provider reports shutdown");
    Check(backing.AllocationCount() == 0, "guest: shutdown releases every allocation");
    Check(backing.Allocate(4096, 4096).status == MemoryStatus::NotInitialized,
          "guest: allocation after shutdown is rejected");
    Check(backing.Initialize().Succeeded(), "guest: provider can be initialized again");
    Check(backing.Allocate(4096, 4096).Succeeded(),
          "guest: allocation works after re-initialization");
    Check(backing.ReleaseAll().Succeeded(), "guest: explicit ReleaseAll succeeds");
}

void TestRegionEnumeration() {
    FakeMemoryBackingProvider backing;
    GuestMemoryManager memory(backing);
    Check(memory.AddRegion(MakeRequest("alpha", 1)).Succeeded(),
          "guest: add alpha region");
    Check(memory.AddRegion(MakeRequest("beta", 2)).Succeeded(),
          "guest: add beta region");
    const std::vector<GuestRegionInfo> regions = memory.EnumerateRegions();
    Check(regions.size() == 2, "guest: enumeration returns two regions");

    std::vector<GuestPageInfo> pages;
    Check(memory.EnumeratePages("beta", pages).Succeeded(), "guest: enumerate beta pages");
    Check(pages.size() == 2, "guest: beta has two pages");
    Check(pages[1].host_physical.Value() ==
              pages[0].host_physical.Value() + kSmallPageSize,
          "guest: consecutive pages have consecutive host addresses");
    Check(memory.EnumeratePages("missing", pages).status == MemoryStatus::InvalidContext,
          "guest: enumerating a missing region is rejected");
}

} // namespace

int main() {
    TestRegionAllocationAndBacking();
    TestWriteReadAcrossPages();
    TestRegionPermissions();
    TestRegionValidation();
    TestRemoveRegionReleasesBacking();
    TestOutOfMemory();
    TestBackingProviderLifecycle();
    TestRegionEnumeration();

    if (test::failures != 0) {
        std::cerr << test::failures << " guest-memory test(s) failed\n";
        return 1;
    }
    std::cout << "All guest-memory tests passed\n";
    return 0;
}
