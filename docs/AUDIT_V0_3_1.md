# v0.3.1 Source Audit

## Source Examined

The supplied upstream URL resolves to the public `dev` branch at commit `b7232e6` (`fix(preflight): improve CPUID validation and platform diagnostics`). Its parent is `9f66bfd` (`Initial commit: Geraduo Hypervisor Core v0.3.0`). The branch had three commits and no version tags. The audited tree contained CMake, the `include/hv` API, `src`, `examples`, and `tests`; it did not contain a maintained README or a `docs/` directory.

The v0.3.1 CMake project requires CMake 3.20, declares C++20, and builds `hv_core`, `hv_demo`, `hv_launch`, `hv_preflight`, `hv_tests`, and `hv_preflight_tests`. The original test suite was configured, built, and run before adding v0.4.0 code: both tests passed with CMake 4.4.5 and GNU C++ 12.2 on Linux.

## Existing Interfaces

- `hv::IHostBackend`: `Initialize()` and `Shutdown()` contracts.
- `hv::IGuestBackend`: `StartTestSession()` and `StopTestSession()`; the contract is explicitly synthetic.
- `hv::Lifecycle`: guarded `Stopped`, `Initializing`, `Running`, `ShuttingDown`, and `Failed` transitions.
- `hv::Hypervisor`: orchestrates host/guest lifecycle and rollback; its names do not imply a hardware VMM implementation.
- `hv::SyntheticPageTable`: GVA-to-GPA mapping model.
- `hv::NestedPageTable`, aliases `EptManager` and `NptManager`: GPA-to-host-address software mapping model.
- `hv::MemoryManager`: two-stage translation over those software tables.
- `hv::ProgramRunner`: starts ordinary host processes using platform-specific process APIs.
- `hv::Diagnostics`: thread-safe in-memory diagnostic event collection.
- `hv::ICpuidProvider`, `NativeCpuidProvider`, and `FakeCpuidProvider`: raw CPUID access seam and deterministic test provider.
- `RunPlatformPreflight`, `PreflightReport`, `FormatReportText`, and `FormatReportJson`: user-mode CPUID interpretation and serialization.
- `hv::Status` and `hv::Result`: shared result/status convention.

No existing interfaces were removed. New Windows interfaces are isolated under `include/hv/platform` and depend on `IPlatformProvider`, `ICpuidProvider`, and `IKernelDriverInterface` seams.

## v0.3.0 To v0.3.1

The commit diff changes the project version from 0.3.0 to 0.3.1. It leaves the lifecycle, synthetic paging, process runner, diagnostics, and demo code unchanged. Most changes are in Platform Preflight and its tests:

- The maximum basic CPUID leaf is range-checked before querying leaf 1; reserved maximum values no longer drive unbounded or malformed queries.
- The hypervisor leaf range is validated. A hypervisor-present bit and vendor string are treated as evidence for review, not proof of an incompatible second hypervisor. `HypervisorPresentReviewRequired` is the preferred verdict name and `HypervisorConflict` remains a compatibility alias.
- The VMX bit is interpreted only for GenuineIntel. AMD/Hygon SVM probing first reads and validates `0x80000000` before reading `0x80000001`.
- Optional missing leaves, malformed maximum values, unsupported CPUID, and source read failures are handled distinctly. The fake CPUID provider gained per-leaf injected failures and records calls for ordering tests.
- CPU vendor bytes are sanitized; unknown and empty vendor strings do not become Intel. Hypervisor maximum-leaf ranges and AMD extended-leaf ranges are checked.
- Kernel-only MSR, CR, EPT, and Windows virtualization checks remain explicitly `NOT CHECKED`. The preflight has no "ready" outcome.
- JSON escaping and report assertions were extended for control characters, absent leaves, and kernel-verification boundaries.

The v0.3.1 preflight tests exercise Intel VMX present/absent, family/model decoding, AMD/Hygon/unknown vendors, extended-leaf ordering and failures, hypervisor vendor reporting, malformed data, and JSON/text serialization.

## Component Behavior And Limits

- `page_table.cpp`, `nested_paging.cpp`, and `memory_manager.cpp` maintain maps in ordinary user-mode memory. They preserve page offsets and enforce modeled permissions, but they do not configure CPU page tables or access physical memory.
- `EptManager` and `NptManager` are aliases of the synthetic nested page-table model.
- The demo's host and guest backends are sample implementations. The guest session is not an OS boot.
- `ProgramRunner` launches normal host child processes; it is not a guest launcher.
- Native CPUID is an unprivileged instruction. The existing provider never executes RDMSR/WRMSR or VMX instructions.
- `Hypervisor` lifecycle orchestration is useful as an interface model, not evidence that VMXON, VM-entry, VM-exit, or guest execution exists.

## CMake And Test Baseline

Before changes, the upstream v0.3.1 tree was configured and built with CMake 4.4.5, GNU C++ 12.2, and the Release configuration on Linux. `hv_tests` and `hv_preflight_tests` both passed. This verifies the available portable baseline only; it does not test a Windows build or driver.