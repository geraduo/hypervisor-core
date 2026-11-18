# Changelog

All notable changes to Geraduo Hypervisor Core are documented here. The project
does not claim production readiness, hardware virtualization execution or driver
runtime validation at any version below.

## v0.6.0 — Software Memory Layer

### Added

- Software memory model in `hv_core`, namespace `hv::memory`, with no hardware
  EPT or VMX dependency.
- Strongly typed `GuestVirtualAddress`, `GuestPhysicalAddress`,
  `HostPhysicalAddress` and `AddressSpaceId`, plus `MemoryStatus`,
  `MemoryResult`, `TranslationResult`, `Permissions` checks and explicit
  conversion helpers for the legacy `uint64_t` aliases.
- `IMemoryBackingProvider` and `FakeMemoryBackingProvider`: real aligned
  allocations, allocation tracking, byte limits, OOM injection, `Shutdown`,
  `ReleaseAll` and re-initialization.
- `GuestAddressSpaceManager`: named address-space contexts, Map/Unmap/Protect/
  Query/Translate/Enumerate/Clear and context isolation.
- `GuestMemoryManager`: guest RAM regions, real backing, GPA → HPA translation,
  permission-checked Read/Write, direct snapshot access, region removal and
  `ReleaseAll`.
- `EptPageTable` and typed `EptManager`: four-level PML4/PDPT/PD/PT software
  hierarchy, 4 KiB/2 MiB/1 GiB pages, mapping ranges, rollback, protection,
  queries, validation and concurrency.
- `MemoryMappingManager`: combined GVA → GPA → HPA translation, cross-stage
  permission intersection, `MapAll`, `UnmapAll`, `ProtectAll`, `Read`, `Write`,
  `Execute` (permission check only), `Query`, `GetMappingInfo` and validation.
- `MemorySnapshotManager`: in-memory snapshot creation, metadata, FNV-1a
  checksum, verification, restore, deletion and a configurable byte limit.
- `MemoryDiagnostics`: translation/error/permission counters, mapped pages,
  address spaces, guest regions, mapping ranges, snapshot summaries and JSON
  export.
- `hv_memory_demo.exe`: address space, synthetic pages, GPA → HPA, two-stage
  translation, permission denials, snapshot restore and JSON diagnostics.
- Tests: `hv_ept_manager_tests`, `hv_guest_memory_tests`,
  `hv_address_space_tests`, `hv_memory_snapshot_tests` and
  `hv_memory_integration_tests`.
- Documentation: `docs/MEMORY_MANAGER.md`, `docs/ADDRESS_TRANSLATION.md`,
  `docs/EPT_MODEL.md` and `docs/MEMORY_SNAPSHOTS.md`.
- `hv::MemoryManager` integration constructor taking `hv::memory::EptManager&`.

### Changed

- CMake project version is `0.6.0`; the `hv_memory_demo` and five memory test
  targets are added to the existing build and CTest registration.
- `SyntheticPageTable` gains additive `Protect`, `Query`, `Enumerate` and
  `Clear` operations. Existing Map/Unmap/Translate behavior is unchanged.
- `MemoryManager` supports both the legacy `NestedPageTable&` second stage and
  the new typed software `hv::memory::EptManager&` second stage.
- `Permissions::Allows` is `constexpr`.
- README, architecture and build documentation describe the software memory
  layer, the GVA/GPA/HPA model and the hardware boundary.

### Compatibility

- All public v0.5.0 APIs remain available.
- The legacy `hv::GuestVirtualAddress`, `hv::GuestPhysicalAddress`,
  `hv::HostPhysicalAddress` and `hv::AddressSpaceId` aliases are unchanged.
- The legacy flat `hv::NestedPageTable` / `hv::EptManager` remains available;
  `hv::memory::EptManager` is a separate typed type.
- Lifecycle, `Hypervisor`, host/guest backends, `ProgramRunner`, platform,
  VT-x and driver components are unchanged.
- Driver version remains **0.5.0** intentionally: the driver binary was not
  changed in v0.6.0. Protocol version 1, the 24-byte request, the 80-byte
  response and `IOCTL_GERADUO_QUERY_PLATFORM_INFO = 0x00226000` are unchanged.

### Explicitly not included

- Hardware EPT/NPT, EPTP programming, VPID, EPT invalidation, accessed/dirty
  bits or hardware page-walk behavior.
- `VMXON` / `VMXOFF`, VMCS creation or access, VM entry/exit, exit handling.
- Physical-memory access, arbitrary process-memory access, MMIO or DMA.
- Guest execution, interrupt injection or device emulation.
- AMD-V/SVM or Windows Hypervisor Platform backends.
- Disk-backed snapshots, incremental snapshots, compression or encryption.
- Huge-page splitting/merging in the EPT model.
- A signed, installed or loaded driver.

### Validation

Validated in the authoring workspace (Windows Server 2025 build
`10.0.26200.9974`, x64, AMD Ryzen 5 5600 VM under Microsoft Hyper-V):

- Release x64 build with Visual Studio 2026 / MSVC 19.52.37029.0,
  `/W4 /WX /permissive- /EHsc`, no warnings.
- `ctest`: 12/12 test executables passed, including all five new memory suites.
- `hv_memory_demo.exe` completed all five stages and exported
  `hv_memory_demo_diagnostics.json`; the JSON reports `"model": "software"`
  and `"hardware_ept": false`.
- Software two-stage translation, permission denials, snapshot byte/permission
  restore, OOM handling, corrupt-snapshot rejection and concurrent access are
  covered by the new tests.
- The driver binaries from v0.5.0 remain unloaded and untested at runtime.

Not validated:

- No hardware VT-x execution and no hardware EPT were tested; no Intel host was
  available.
- The driver was not signed, installed, loaded or IOCTL-tested.
- The requested Visual Studio 2022 generator was unavailable; Visual Studio 18
  2026 was used.

## v0.5.0 — Intel VT-x Virtualization Foundation

### Added

- `hv_virtualization` library, layered on top of `hv_platform`.
- `VmxCapabilityManager`: Intel vendor/VMX evaluation built on
  `CpuCapabilityManager`, an optional per-logical-processor CPUID probe and
  explicit `NotChecked` semantics for incomplete coverage.
- `VmxReadinessManager`: combines VT-x capability, `WindowsPlatformBackend`,
  `PlatformCompatibilityManager`, `KernelDriverInterface` and
  `DriverDiagnostics` into a readiness verdict for kernel-stage review.
- `VirtualizationBackend` (`IVirtualizationBackend`): platform-neutral backend
  contract with the fixed state vocabulary `Uninitialized`,
  `CheckingCapabilities`, `NotSupported`, `Blocked`, `ReadyForReview`, `Failed`,
  `Stopped`.
- `VmxLifecycleController`: thread-safe, transactional lifecycle with rollback
  of a failed `Refresh()`, rejection of repeated `Initialize()`/`Stop()`,
  terminal `Stop()` and driver-connection release.
- `VmxDiagnostics`: bounded, thread-safe event journal with composed text and
  JSON report serialization.
- `hv_vmx_diagnostics.exe`: text, `--json`, `--no-driver` and `--help` modes;
  exit codes 0 (verdict), 1 (internal failure), 2 (invalid command line).
- `NativeLogicalProcessorProbe`: affinity-pinned CPUID observation per active
  logical processor, restoring the previous group affinity.
- Tests: `hv_vmx_capability_tests`, `hv_vmx_readiness_tests`,
  `hv_vmx_lifecycle_tests`, including rollback, repeated-transition,
  concurrency, journal bounds/thread safety, JSON escaping and `NOT_CHECKED`
  coverage.
- Fakes: `FakeLogicalProcessorProbe`, `FakeCompatibilityEnvironment`, driver
  response overrides in `FakeKernelDriverInterface`.
- Documentation: `docs/VMX_READINESS.md`, `docs/DRIVER_INTEGRATION.md`,
  refreshed architecture, build, diagnostics, IOCTL and unsupported-feature
  pages, and an architecture diagram in the README.

### Changed

- CMake project version is `0.5.0`.
- `driver_protocol.hpp` reports driver version `0.5.0` and defines
  `kGeraduoMinimumCompatibleDriverMajor` / `...Minor` as `0.4`.
- Version strings in the platform layer now refer to v0.5.0.
- `docs/BUILDING.md` was renamed to `docs/BUILD.md`.
- The driver remains a read-only diagnostic query; only its reported version
  changed.

### Compatibility

- All public v0.4.0 APIs remain available and unchanged in behavior.
- Protocol version stays `1`, the request stays 24 bytes, the response stays
  80 bytes and `IOCTL_GERADUO_QUERY_PLATFORM_INFO` stays `0x00226000`.
- A 0.4.x diagnostic driver remains protocol compatible. The readiness layer
  checks the reported driver version against the minimum `0.4` policy.
- The driver is not required to build or run the user-mode targets.

### Explicitly not included

- `VMXON` / `VMXOFF`, VMCS creation or access, VM entry/exit, exit handling.
- Hardware EPT/NPT, VPID, MSR bitmaps, interrupt injection, guest execution.
- AMD-V/SVM, Windows Hypervisor Platform backend.
- Physical-memory access, arbitrary process memory access, MSR access from
  user mode, `METHOD_NEITHER` IOCTLs or pointer escapes.
- Hypervisor concealment, anti-cheat bypass or anti-detection behavior.
- A signed, installed or loaded driver.

### Validation

Validated in the authoring workspace (Windows Server 2025 build
`10.0.26200.9974`, x64, AMD Ryzen 5 5600 VM under Microsoft Hyper-V):

- Release x64 build with Visual Studio 2026 / MSVC 19.52.37029.0, `/W4 /WX`,
  no warnings.
- `ctest`: 7/7 test executables passed.
- `hv_vmx_diagnostics.exe` text and JSON output; JSON parsed with
  `ConvertFrom-Json`.
- Host verdict: `NotSupported` / `UnsupportedCpuVendor`, error class `None`,
  readiness `NotSupported`, `hardware_vmx_started=false`. This is correct for
  an AMD host without an SVM backend.
- Driver compiled against WDK/SDK 10.0.28000.0, Release 27,136 bytes and Debug
  27,648 bytes, x64 native, `GsDriverEntry`, NX-compatible, dynamic base,
  unsigned.

Not validated:

- No hardware VT-x execution was tested. No Intel host was available and no
  VMXON/VMCS/VM-entry/exit code exists.
- The driver was not signed, installed, loaded or IOCTL-tested. That requires an
  elevated isolated test VM with test signing and a compatible HVCI policy.
- The requested Visual Studio 2022 generator was unavailable; Visual Studio 18
  2026 was used. VS2022 commands are documented but were not executed locally.

## v0.4.0 — Windows Platform Diagnostics

### Added

- `hv_platform` library with `WindowsPlatformBackend`
  (`IPlatformProvider`/`NativePlatformProvider`), `CpuCapabilityManager`,
  `PlatformCompatibilityManager` and `DriverDiagnostics`.
- `KernelDriverInterface` with a typed `IDriverTransport`, SetupAPI device
  interface discovery, fixed-size IOCTL validation and NTSTATUS/Win32 mapping.
- `hv_platform_diagnostics.exe` with text, `--json`, `--no-driver` and `--help`.
- `hv_windows_platform_tests` and `hv_platform_compatibility_tests`.
- Optional WDK driver `GeraduoDriver` exposing one read-only diagnostic IOCTL
  (`IOCTL_GERADUO_QUERY_PLATFORM_INFO = 0x00226000`) with `METHOD_BUFFERED` I/O,
  SDDL-restricted access and a fixed 24-byte request / 80-byte response.
- `driver/build-driver.ps1` and the `GeraduoDriverDirect` CMake target as a
  fallback when the WDK MSBuild toolset is not registered.

### Validation

- User-mode Release x64 build and tests passed.
- Driver compiled and linked; not signed, loaded or IOCTL-tested.
- No VMXON, VMCS, EPT, physical-memory or process-memory operation was added.

## v0.3.1 and earlier

The synthetic-memory core (`Lifecycle`, `Hypervisor`, `MemoryManager`,
`SyntheticPageTable`, `NestedPageTable`, `EptManager`, `NptManager`,
`ProgramRunner`, CPUID preflight and diagnostics primitives) is preserved.
Those releases never implemented hardware virtualization.
