#pragma once

// Coordinates the first-stage guest page table (GVA -> GPA) and the software
// EPT model (GPA -> HPA).  It also provides permission-checked read/write
// helpers that touch the real backing storage behind the translated HPA.

#include "hv/memory/address_space_manager.hpp"
#include "hv/memory/ept_manager.hpp"
#include "hv/memory/guest_memory_manager.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>

namespace hv::memory {

class MemoryDiagnostics;

struct MappingInfo {
    AddressSpaceId address_space{};
    GuestVirtualAddress virtual_address{};
    GuestPhysicalAddress guest_physical_address{};
    HostPhysicalAddress host_physical_address{};
    std::uint64_t page_size{0};
    Permissions permissions{};
    bool guest_mapped{false};
    bool nested_mapped{false};
    bool backed{false};
    std::string region_name;
};

class MemoryMappingManager final {
public:
    MemoryMappingManager(GuestAddressSpaceManager& spaces, EptManager& ept,
                         GuestMemoryManager& memory,
                         MemoryDiagnostics* diagnostics = nullptr);
    ~MemoryMappingManager() = default;

    MemoryMappingManager(const MemoryMappingManager&) = delete;
    MemoryMappingManager& operator=(const MemoryMappingManager&) = delete;

    // First stage: GVA -> GPA.
    [[nodiscard]] MemoryResult Map(AddressSpaceId id, GuestVirtualAddress virtual_page,
                                   GuestPhysicalAddress guest_physical_page,
                                   Permissions permissions = {});
    [[nodiscard]] MemoryResult Unmap(AddressSpaceId id, GuestVirtualAddress virtual_page);
    [[nodiscard]] MemoryResult Protect(AddressSpaceId id, GuestVirtualAddress virtual_page,
                                       Permissions permissions);
    [[nodiscard]] MemoryResult Query(AddressSpaceId id, GuestVirtualAddress address,
                                     MappingInfo& out) const;
    [[nodiscard]] MemoryResult GetMappingInfo(AddressSpaceId id, GuestVirtualAddress address,
                                              MappingInfo& out) const;

    // Second stage: GPA -> HPA.
    [[nodiscard]] MemoryResult MapNested(GuestPhysicalAddress guest_page,
                                         HostPhysicalAddress host_page,
                                         Permissions permissions = {},
                                         std::uint64_t page_size = kSmallPageSize);
    [[nodiscard]] MemoryResult UnmapNested(GuestPhysicalAddress guest_page);
    [[nodiscard]] MemoryResult ProtectNested(GuestPhysicalAddress guest_page,
                                             Permissions permissions);
    [[nodiscard]] MemoryResult QueryNested(GuestPhysicalAddress address, MappingInfo& out) const;

    // Combined convenience operations using the region's real backing address.
    [[nodiscard]] MemoryResult MapAll(AddressSpaceId id, GuestVirtualAddress virtual_page,
                                      GuestPhysicalAddress guest_physical_page,
                                      Permissions permissions = {});
    // Removes the guest mapping and its nested mapping for the same GPA.  The
    // nested page is shared state: removing it also removes it for any other
    // address space that mapped the same GPA.
    [[nodiscard]] MemoryResult UnmapAll(AddressSpaceId id, GuestVirtualAddress virtual_page);
    // Applies the permission change to the guest mapping and to the nested
    // mapping for the same GPA.
    [[nodiscard]] MemoryResult ProtectAll(AddressSpaceId id, GuestVirtualAddress virtual_page,
                                          Permissions permissions);

    [[nodiscard]] TranslationResult Translate(AddressSpaceId id, GuestVirtualAddress address,
                                              Access access) const;
    [[nodiscard]] TranslationResult TranslatePhysical(GuestPhysicalAddress address,
                                                      Access access) const;

    [[nodiscard]] MemoryResult Read(AddressSpaceId id, GuestVirtualAddress address,
                                    std::span<std::byte> destination);
    [[nodiscard]] MemoryResult Write(AddressSpaceId id, GuestVirtualAddress address,
                                     std::span<const std::byte> source);
    // Checks execute permission only; it never transfers control to guest code.
    [[nodiscard]] MemoryResult Execute(AddressSpaceId id, GuestVirtualAddress address);

    [[nodiscard]] MemoryResult Validate(AddressSpaceId id) const;
    [[nodiscard]] MemoryResult ValidateAll() const;
    [[nodiscard]] MemoryDiagnostics* Diagnostics() const noexcept { return diagnostics_; }

private:
    [[nodiscard]] TranslationResult TranslateLocked(AddressSpaceId id,
                                                    GuestVirtualAddress address,
                                                    Access access) const;
    [[nodiscard]] TranslationResult TranslatePhysicalLocked(GuestPhysicalAddress address,
                                                            Access access) const;
    [[nodiscard]] MemoryResult ValidateLocked(AddressSpaceId id) const;
    [[nodiscard]] MemoryResult ValidateAllLocked() const;
    void RecordTranslation(const TranslationResult& result) const;
    void UpdateGaugesLocked() const;

    GuestAddressSpaceManager& spaces_;
    EptManager& ept_;
    GuestMemoryManager& memory_;
    MemoryDiagnostics* diagnostics_{nullptr};
    mutable std::mutex mutex_;
};

} // namespace hv::memory
