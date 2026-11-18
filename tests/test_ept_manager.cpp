#include "hv/memory/ept_manager.hpp"
#include "hv/memory_manager.hpp"
#include "hv/nested_paging.hpp"
#include "hv/page_table.hpp"
#include "test_support.hpp"

#include <atomic>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

using test::Check;
using namespace hv::memory;

namespace {

constexpr std::uint64_t kGpaBase = 0x0000'0000'1000'0000ULL;
constexpr std::uint64_t kHpaBase = 0x0000'0000'9000'0000ULL;

void TestSmallPageMappingAndTranslation() {
    EptManager ept;
    Check(ept.Map(GuestPhysicalAddress{0x1000}, HostPhysicalAddress{0x9000},
                  Permissions{true, true, false})
              .Succeeded(),
          "EPT: map one 4 KiB page");

    const EptTranslationResult result =
        ept.Translate(GuestPhysicalAddress{0x1234}, Access::Read);
    Check(result.Succeeded(), "EPT: 4 KiB translation succeeds");
    Check(result.host_physical.Value() == 0x9234, "EPT: 4 KiB page offset is preserved");
    Check(result.page_size == kSmallPageSize, "EPT: reported page size is 4 KiB");
    Check(result.level == EptEntryLevel::Pt, "EPT: leaf is at the PT level");
    Check(ept.MappingCount() == 1, "EPT: mapping count is one");

    const EptTranslationResult denied =
        ept.Translate(GuestPhysicalAddress{0x1234}, Access::Execute);
    Check(denied.status == MemoryStatus::AccessDenied, "EPT: execute denied by permissions");
}

void TestLargePageMapping() {
    EptManager ept;
    Check(ept.Map(GuestPhysicalAddress{0x20'0000}, HostPhysicalAddress{0x40'0000},
                  Permissions{true, true, false}, kLargePageSize)
              .Succeeded(),
          "EPT: map one 2 MiB page");
    Check(ept.Map(GuestPhysicalAddress{0x4000'0000}, HostPhysicalAddress{0x8000'0000},
                  Permissions{true, false, false}, kHugePageSize)
              .Succeeded(),
          "EPT: map one 1 GiB page");

    const EptTranslationResult large =
        ept.Translate(GuestPhysicalAddress{0x20'1234}, Access::Read);
    Check(large.Succeeded() && large.host_physical.Value() == 0x40'1234,
          "EPT: 2 MiB translation preserves the offset");
    Check(large.page_size == kLargePageSize, "EPT: 2 MiB page size is reported");
    Check(large.level == EptEntryLevel::Pd, "EPT: 2 MiB leaf is at the PD level");

    const EptTranslationResult huge =
        ept.Translate(GuestPhysicalAddress{0x4000'1234}, Access::Read);
    Check(huge.Succeeded() && huge.host_physical.Value() == 0x8000'1234,
          "EPT: 1 GiB translation preserves the offset");
    Check(huge.page_size == kHugePageSize, "EPT: 1 GiB page size is reported");
    Check(huge.level == EptEntryLevel::Pdpt, "EPT: 1 GiB leaf is at the PDPT level");

    Check(ept.Translate(GuestPhysicalAddress{0x4000'1234}, Access::Write).status ==
              MemoryStatus::AccessDenied,
          "EPT: 1 GiB read-only page rejects writes");
    Check(ept.MappingCount() == 2, "EPT: large-page mapping count is two");
    Check(ept.Validate().Succeeded(), "EPT: table validates with large pages");
}

void TestDuplicateAndOverlappingMappings() {
    EptManager ept;
    Check(ept.Map(GuestPhysicalAddress{0x1000}, HostPhysicalAddress{0x9000}, {true, true, false})
              .Succeeded(),
          "EPT: initial mapping for duplicate test");
    Check(ept.Map(GuestPhysicalAddress{0x1000}, HostPhysicalAddress{0xA000}, {true, true, false})
              .status == MemoryStatus::AlreadyMapped,
          "EPT: duplicate GPA mapping is rejected");
    Check(ept.Map(GuestPhysicalAddress{0x2000}, HostPhysicalAddress{0x9000}, {true, true, false})
              .Succeeded(),
          "EPT: two GPAs may share one host page in the model");

    Check(ept.Map(GuestPhysicalAddress{0x20'0000}, HostPhysicalAddress{0x40'0000},
                  {true, true, false}, kLargePageSize)
              .Succeeded(),
          "EPT: map 2 MiB page");
    Check(ept.Map(GuestPhysicalAddress{0x20'1000}, HostPhysicalAddress{0x50'0000},
                  {true, true, false})
              .status == MemoryStatus::AlreadyMapped,
          "EPT: 4 KiB page inside an existing 2 MiB page is rejected");
    Check(ept.Map(GuestPhysicalAddress{0x20'0000}, HostPhysicalAddress{0x60'0000},
                  {true, true, false}, kLargePageSize)
              .status == MemoryStatus::AlreadyMapped,
          "EPT: duplicate 2 MiB page is rejected");
}

void TestUnmapAndProtect() {
    EptManager ept;
    Check(ept.Map(GuestPhysicalAddress{0x1000}, HostPhysicalAddress{0x9000}, {true, true, false})
              .Succeeded(),
          "EPT: map page before protection test");
    Check(ept.Protect(GuestPhysicalAddress{0x1000}, {true, false, false}).Succeeded(),
          "EPT: change page permissions");
    Check(ept.Translate(GuestPhysicalAddress{0x1000}, Access::Write).status ==
              MemoryStatus::AccessDenied,
          "EPT: write is denied after protection change");
    Check(ept.Translate(GuestPhysicalAddress{0x1000}, Access::Read).Succeeded(),
          "EPT: read is still allowed after protection change");

    Check(ept.Unmap(GuestPhysicalAddress{0x1000}).Succeeded(), "EPT: unmap page");
    Check(ept.Translate(GuestPhysicalAddress{0x1000}, Access::Read).status ==
              MemoryStatus::Unmapped,
          "EPT: translation after unmap reports unmapped");
    Check(ept.Unmap(GuestPhysicalAddress{0x1000}).status == MemoryStatus::Unmapped,
          "EPT: double unmap is rejected");
    Check(ept.Protect(GuestPhysicalAddress{0x1000}, {true, false, false}).status ==
              MemoryStatus::Unmapped,
          "EPT: protecting an unmapped page is rejected");
}

void TestAlignmentAndLimits() {
    EptManager ept;
    Check(ept.Map(GuestPhysicalAddress{0x1234}, HostPhysicalAddress{0x9000}, {true, true, false})
              .status == MemoryStatus::AlignmentError,
          "EPT: misaligned GPA is rejected");
    Check(ept.Map(GuestPhysicalAddress{0x1000}, HostPhysicalAddress{0x9234}, {true, true, false})
              .status == MemoryStatus::AlignmentError,
          "EPT: misaligned HPA is rejected");
    Check(ept.Map(GuestPhysicalAddress{0x1000}, HostPhysicalAddress{0x9000}, {true, true, false},
                  8192)
              .status == MemoryStatus::InvalidMapping,
          "EPT: unsupported page size is rejected");
    Check(ept.Map(GuestPhysicalAddress{0x1000},
                  HostPhysicalAddress{(1ULL << 48U)}, {true, true, false})
              .status == MemoryStatus::InvalidAddress,
          "EPT: HPA outside the 48-bit model is rejected");

    EptPageTable::Config config;
    config.address_limit = 0x10'0000;
    EptManager limited(config);
    Check(limited.Map(GuestPhysicalAddress{0x20'0000}, HostPhysicalAddress{0x9000},
                      {true, true, false})
              .status == MemoryStatus::InvalidAddress,
          "EPT: GPA beyond the configured limit is rejected");

    EptPageTable::Config no_large;
    no_large.allow_large_pages = false;
    EptManager small_only(no_large);
    Check(small_only.Map(GuestPhysicalAddress{0x20'0000}, HostPhysicalAddress{0x40'0000},
                         {true, true, false}, kLargePageSize)
              .status == MemoryStatus::Unsupported,
          "EPT: large pages can be disabled by configuration");
}

void TestRangeMappingAndRangeEnumeration() {
    EptManager ept;
    Check(ept.MapRange(GuestPhysicalAddress{kGpaBase}, HostPhysicalAddress{kHpaBase}, 4,
                       Permissions{true, true, false}, kSmallPageSize)
              .Succeeded(),
          "EPT: range mapping of four pages succeeds");
    Check(ept.MappingCount() == 4, "EPT: range mapping creates four leaves");

    const std::vector<EptRangeInfo> ranges = ept.GetMappingRanges();
    Check(ranges.size() == 1, "EPT: contiguous identical pages coalesce into one range");
    Check(!ranges.empty() && ranges[0].size == 4 * kSmallPageSize,
          "EPT: coalesced range spans four pages");

    Check(ept.MapRange(GuestPhysicalAddress{kGpaBase}, HostPhysicalAddress{kHpaBase}, 0,
                       Permissions{true, true, false}, kSmallPageSize)
              .status == MemoryStatus::InvalidMapping,
          "EPT: zero-page range mapping is rejected");
    Check(ept.MapRange(GuestPhysicalAddress{kGpaBase + 1}, HostPhysicalAddress{kHpaBase}, 1,
                       Permissions{true, true, false}, kSmallPageSize)
              .status == MemoryStatus::AlignmentError,
          "EPT: misaligned range base is rejected");

    const std::size_t before = ept.MappingCount();
    Check(ept.Map(GuestPhysicalAddress{kGpaBase + 6 * kSmallPageSize},
                  HostPhysicalAddress{kHpaBase + 0x1'0000}, {true, true, false})
              .Succeeded(),
          "EPT: seed a conflicting page");
    Check(ept.MapRange(GuestPhysicalAddress{kGpaBase + 5 * kSmallPageSize},
                       HostPhysicalAddress{kHpaBase + 0x2'0000}, 4,
                       Permissions{true, true, false}, kSmallPageSize)
              .status == MemoryStatus::AlreadyMapped,
          "EPT: range mapping fails on a conflicting page");
    Check(ept.MappingCount() == before + 1,
          "EPT: failed range mapping rolls back its already-created leaves");
    Check(ept.Translate(GuestPhysicalAddress{kGpaBase + 5 * kSmallPageSize}, Access::Read)
              .status == MemoryStatus::Unmapped,
          "EPT: rolled-back range leaves are really gone");
}

void TestValidateAndClear() {
    EptManager ept;
    for (std::uint64_t i = 0; i < 8; ++i) {
        Check(ept.Map(GuestPhysicalAddress{kGpaBase + i * kSmallPageSize},
                      HostPhysicalAddress{kHpaBase + i * kSmallPageSize},
                      Permissions{true, true, false})
                  .Succeeded(),
              "EPT: populate page " + std::to_string(i) + " for validation");
    }
    Check(ept.Validate().Succeeded(), "EPT: populated hierarchy validates");
    Check(ept.Enumerate().size() == 8, "EPT: enumeration returns every leaf");
    ept.Clear();
    Check(ept.MappingCount() == 0, "EPT: clear removes every mapping");
    Check(ept.Validate().Succeeded(), "EPT: empty hierarchy validates");
}

void TestConcurrentMappings() {
    EptManager ept;
    constexpr int kThreads = 4;
    constexpr int kPagesPerThread = 32;
    std::atomic<bool> all_ok{true};
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int thread = 0; thread < kThreads; ++thread) {
        workers.emplace_back([&ept, &all_ok, thread]() {
            for (int page = 0; page < kPagesPerThread; ++page) {
                const std::uint64_t gpa = kGpaBase +
                    static_cast<std::uint64_t>(thread) * 0x20'000ULL +
                    static_cast<std::uint64_t>(page) * kSmallPageSize;
                const std::uint64_t hpa = kHpaBase +
                    static_cast<std::uint64_t>(thread) * 0x20'000ULL +
                    static_cast<std::uint64_t>(page) * kSmallPageSize;
                const MemoryResult result =
                    ept.Map(GuestPhysicalAddress{gpa}, HostPhysicalAddress{hpa},
                            Permissions{true, true, false});
                if (!result.Succeeded()) all_ok = false;
            }
        });
    }
    for (std::thread& worker : workers) worker.join();
    Check(all_ok.load(), "EPT: concurrent mappings all succeed");
    Check(ept.MappingCount() == kThreads * kPagesPerThread,
          "EPT: concurrent mapping count is exact");
    Check(ept.Validate().Succeeded(), "EPT: concurrent hierarchy validates");
}

void TestLegacyMemoryManagerIntegration() {
    hv::SyntheticPageTable guest;
    EptManager ept;
    Check(guest.Map(0x1000, 0x2000, {true, true, false}).Succeeded(),
          "integration: legacy guest page maps");
    Check(ept.Map(GuestPhysicalAddress{0x2000}, HostPhysicalAddress{0x9000},
                  Permissions{true, true, false})
              .Succeeded(),
          "integration: typed EPT page maps");

    hv::MemoryManager manager(guest, ept);
    const hv::TranslationResult result =
        manager.Translate({1, true}, 0x1234, hv::Access::Read);
    Check(result.Succeeded(), "integration: MemoryManager uses the typed EPT model");
    Check(result.host_physical_address == 0x9234,
          "integration: typed two-stage translation preserves the offset");
    Check(result.page_size == hv::kPageSize,
          "integration: typed translation reports the legacy page size");
    Check(manager.Translate({1, true}, 0x1234, hv::Access::Execute).status ==
              hv::TranslationStatus::AccessDenied,
          "integration: typed EPT permission denial reaches MemoryManager");
    Check(manager.Translate({0, false}, 0x1234, hv::Access::Read).status ==
              hv::TranslationStatus::InvalidContext,
          "integration: invalid context is rejected before translation");
    Check(manager.Translate({1, true}, 0xA000, hv::Access::Read).status ==
              hv::TranslationStatus::UnmappedGuestVirtual,
          "integration: unmapped GVA is reported by the first stage");

    hv::NestedPageTable legacy_nested;
    Check(legacy_nested.Map(0x2000, 0xA000, {true, true, false}).Succeeded(),
          "compatibility: legacy NestedPageTable still maps");
    hv::MemoryManager legacy_manager(guest, legacy_nested);
    const hv::TranslationResult legacy =
        legacy_manager.Translate({2, true}, 0x1000, hv::Access::Read);
    Check(legacy.Succeeded() && legacy.host_physical_address == 0xA000,
          "compatibility: legacy MemoryManager constructor still works");
}

} // namespace

int main() {
    TestSmallPageMappingAndTranslation();
    TestLargePageMapping();
    TestDuplicateAndOverlappingMappings();
    TestUnmapAndProtect();
    TestAlignmentAndLimits();
    TestRangeMappingAndRangeEnumeration();
    TestValidateAndClear();
    TestConcurrentMappings();
    TestLegacyMemoryManagerIntegration();

    if (test::failures != 0) {
        std::cerr << test::failures << " EPT test(s) failed\n";
        return 1;
    }
    std::cout << "All EPT tests passed\n";
    return 0;
}
