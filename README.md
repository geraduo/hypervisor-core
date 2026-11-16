# Geraduo Hypervisor Core

## v0.5.0 – Intel VT-x virtualization foundation (readiness and state management)

This release adds the infrastructure that **checks, tracks and reports** whether a Windows x64 host is a candidate for an
Intel VT-x backend. It is **not a hardware hypervisor**. It does not execute `VMXON`, create a VMCS, enter a guest, handle
VM exits or create hardware EPT/NPT tables. EPT/NPT in the library remain software models. `IsHypervisorRunning()` is always
`false` and `StartVirtualization()` always returns `Unsupported`.

### New in v0.5.0

* `VmxCapabilityManager` – per-logical-processor VMX detection through CPUID; no result is propagated between processors.
* `VmxReadinessManager` – combines platform snapshot, capability, compatibility and diagnostic-driver status; lists every unverified condition as `NOT_CHECKED`.
* `IVirtualizationBackend` / `IntelVirtualizationBackend` – one interface: `Initialize`, `RunDiagnostics`, `StartVirtualization`, `Shutdown`, `GetState`.
* `VmxLifecycleController` – thread-safe state machine: `Uninitialized`, `CheckingCapabilities`, `NotSupported`, `Blocked`, `ReadyForReview`, `Failed`, `Stopped`.
* `VmxDiagnostics` – structured events, text/JSON reports, failure reasons.
* `hv_vmx_diagnostics.exe [--json]`.

There is intentionally no "Ready" or "Running" state: the best outcome is `ReadyForReview`, meaning user-mode and driver evidence
is consistent with VT-x while kernel checks (`IA32_FEATURE_CONTROL`, VMX MSRs, CR0/CR4 fixed bits, VMXON) are still `NOT_CHECKED`.

Documentation: [docs/VMX_READINESS.md](docs/VMX_READINESS.md), [docs/DRIVER_INTEGRATION.md](docs/DRIVER_INTEGRATION.md),
[docs/BUILD.md](docs/BUILD.md), [CHANGELOG.md](CHANGELOG.md).

### Usage

```powershell
hv_vmx_diagnostics.exe
hv_vmx_diagnostics.exe --json
```

Output: CPU information, per-processor VMX capability, platform status, driver status, compatibility status,
VirtualizationBackend state and the reasons for an unsupported/blocked configuration.

## Included from earlier versions (preserved)

* `WindowsPlatformBackend` collects Windows version, native architecture, logical processor count, page size, and a structured CPUID report through injectable providers.
* `CpuCapabilityManager` reuses `ICpuidProvider` and `RunPlatformPreflight`, and reads the optional CPUID brand string.
* `PlatformCompatibilityManager` reports Windows x64/version restrictions, CPU preflight evidence, CPUID hypervisor presence/vendor, Device Guard/VBS/HVCI registry configuration, driver connection state, and unverified kernel checks.
* `KernelDriverInterface` enumerates a registered device interface, opens it read-only, checks the v1 protocol response, and supports clean disconnect/reconnect.
* `GeraduoDriver.vcxproj` is a separate WDM PnP driver with one buffered, read-only platform information IOCTL.
* `hv_platform_diagnostics.exe` and `hv_preflight.exe` continue to work; neither claims VMX readiness.
* Core: lifecycle orchestration, host/guest backend contracts, synthetic page tables, nested paging models, program runner.

## Build

Requirements: CMake 3.20+, a C++20 compiler, Visual Studio 2022 for the Windows generator.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Details: [docs/BUILD.md](docs/BUILD.md). Driver: `msbuild driver\GeraduoDriver.vcxproj /m /p:Configuration=Release /p:Platform=x64`
(isolated test VM, test-signed package; see [docs/DRIVER_ARCHITECTURE.md](docs/DRIVER_ARCHITECTURE.md)).

Tests use fake providers and need no physical VT-x, no driver and no Windows.

## Verification status

The v0.5.0 code was written against the v0.4.0 headers of the `dev` branch but was **not compiled or run** in the environment
that produced it. No build or test success is claimed. Run the commands above on Windows 10/11 x64 with Visual Studio 2022 and on a
portable toolchain. The WDM driver is unchanged and was not built here.

## Known limits

* Kernel-only VMX MSR/CR checks are not performed; the driver does not execute VMX instructions.
* Intel VT-x only; AMD SVM is reported as `UnsupportedVendor`.
* VBS and HVCI are reported from registry configuration, not runtime state.
* A CPUID hypervisor-present bit is not proof of a conflict.
* The user-mode client does not install, start, stop or elevate the driver service.
* No arbitrary physical, process or device memory access is exposed.
