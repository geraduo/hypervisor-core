# Windows Platform Layer

The `hv_platform` library adds deterministic Windows diagnostics without adding
privileged operations. Everything documented here runs in user mode.

## Header map

| Header | Contents |
| --- | --- |
| `hv/platform/windows_platform_backend.hpp` | `SystemInfo`, `PlatformCapabilityReport`, `IPlatformProvider`, `NativePlatformProvider`, `WindowsPlatformBackend` |
| `hv/platform/cpu_capability_manager.hpp` | `CpuCapabilityReport`, `CpuEligibility`, `CpuCapabilityManager` |
| `hv/platform/platform_compatibility_manager.hpp` | `CompatibilityReport`, `ICompatibilityEnvironment`, `NativeCompatibilityEnvironment`, `PlatformCompatibilityManager` |
| `hv/platform/kernel_driver_interface.hpp` | `IDriverTransport`, `NativeDriverTransport`, `IKernelDriverInterface`, `KernelDriverInterface` |
| `hv/platform/driver_protocol.hpp` | Shared driver request/response ABI and IOCTL constants |
| `hv/platform/driver_diagnostics.hpp` | `DriverDiagnostics`, event codes and formatters |
| `hv/platform/fake_platform_provider.hpp` | `FakePlatformProvider` |
| `hv/platform/fake_kernel_driver_interface.hpp` | `FakeKernelDriverInterface` |
| `hv/platform/check_status.hpp` | Shared `CheckStatus` vocabulary |

## WindowsPlatformBackend

```cpp
#include "hv/platform/windows_platform_backend.hpp"

hv::NativePlatformProvider provider;
hv::WindowsPlatformBackend backend(provider);

hv::Result initialized = backend.Initialize();
if (!initialized.Succeeded()) {
    std::cerr << backend.GetLastError() << '\n';
    return 1;
}

hv::SystemInfo system;
hv::PlatformCapabilityReport capabilities;
backend.GetSystemInfo(system);
backend.GetCapabilities(capabilities);

std::cout << hv::FormatPlatformReportJson(system, capabilities, backend.GetState());
```

### State machine

| State | Meaning |
| --- | --- |
| `Uninitialized` | No snapshot; getters return `Status::InvalidState` |
| `Initialized` | A validated snapshot and capability report are cached |
| `Failed` | The provider or snapshot validation failed; `GetLastError()` explains why |

`Initialize()` is idempotent while initialized. `Refresh()` always probes again.
`Shutdown()` clears the snapshot and returns to `Uninitialized`.

### Data sources

| Field | Source |
| --- | --- |
| OS version | `RtlGetVersion` from `ntdll.dll`; UBR revision from the read-only `CurrentVersion` registry value |
| OS name | Version/build mapping and `GetProductInfo` edition |
| Native architecture | `GetNativeSystemInfo`; `IsWow64Process2` when available |
| Process architecture / WOW64 | `GetSystemInfo`, `IsWow64Process2`, `IsWow64Process` fallback |
| Logical processors | `GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)` |
| Page size | `SYSTEM_INFO.dwPageSize` |
| Physical memory | `GlobalMemoryStatusEx().ullTotalPhys` |
| WinHvPlatform presence | `LoadLibraryW("WinHvPlatform.dll")` and `GetProcAddress("WHvGetCapability")` |
| CPUID availability | `NativeCpuidProvider::IsAvailableOnThisBuild()` |
| Driver and firmware state | Deliberately marked `RequiresKernelVerification` |

No handle survives a query. `Shutdown()` releases the cached strings and
vectors.

### Platform support

| Value | Meaning |
| --- | --- |
| `Supported` | All required checks pass |
| `SupportedWithWarnings` | A supported configuration with non-fatal warnings (for example a Server edition) |
| `Unsupported` | At least one required check failed (for example native ARM64) |

## CpuCapabilityManager

```cpp
#include "hv/platform/cpu_capability_manager.hpp"

hv::NativeCpuidProvider cpuid;
hv::CpuCapabilityManager cpu(cpuid);

hv::CpuCapabilityReport report;
hv::Result result = cpu.Probe(report);
std::cout << hv::FormatCpuCapabilityReportText(report);
```

Eligibility values:

| Value | Meaning |
| --- | --- |
| `Unsupported` | No implemented backend for this CPU, missing VMX, or CPUID failure |
| `ReviewRequired` | A hypervisor environment is reported; VMX ownership cannot be assumed |
| `KernelVerificationRequired` | CPUID is consistent with Intel VT-x; kernel checks are still mandatory |

There is intentionally no `Ready`. `kernel_verification_required` is always
`true`.

The brand string is read only when CPUID advertises leaves
`0x80000002`–`0x80000004`. Malformed maximum leaves are rejected without reading
unadvertised leaves.

## PlatformCompatibilityManager

```cpp
#include "hv/platform/platform_compatibility_manager.hpp"

hv::NativeCpuidProvider cpuid;
hv::NativeCompatibilityEnvironment environment;
hv::PlatformCompatibilityManager compatibility(cpuid, environment);

hv::CompatibilityReport report;
hv::Result result = compatibility.Evaluate(report);
std::cout << hv::FormatCompatibilityReportJson(report);
```

Verdicts:

| Verdict | Meaning |
| --- | --- |
| `NoConflictDetected` | No known user-mode conflict; still not proof that `VMXON` succeeds |
| `ReviewRequired` | VBS/HVCI unknown or configured, unrecognized hypervisor, or unavailable Windows probes |
| `KnownConflict` | Hyper-V, a third-party hypervisor, or WHPX reports a hypervisor |
| `UnsupportedPlatform` | The CPU has no implemented backend, or VMX is not reported |
| `Unknown` | CPUID probing itself failed |

CPU unsupported takes precedence over conflict warnings. A detected hypervisor
does not prove that any particular host owns VMX; it only means direct ownership
is expected to conflict unless nested virtualization or another design is used.

### Registry values

The native environment provider reads, without modifying:

```text
HKLM\SYSTEM\CurrentControlSet\Control\DeviceGuard
    EnableVirtualizationBasedSecurity (REG_DWORD)

HKLM\SYSTEM\CurrentControlSet\Control\DeviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity
    Enabled (REG_DWORD)
```

These values describe configured policy; they do not prove that VBS or HVCI is
currently running.

### WHPX probe

`NativeCompatibilityEnvironment` dynamically loads `WinHvPlatform.dll` and calls
`WHvGetCapability(0, &present, sizeof(present), &written)`, where capability code
`0` is `WHvCapabilityCodeHypervisorPresent`. The DLL is optional. A missing DLL
or a failed call is reported as informational, not as a conflict.

## KernelDriverInterface

```cpp
#include "hv/platform/kernel_driver_interface.hpp"

hv::KernelDriverInterface driver;
if (driver.Connect().Succeeded()) {
    hv::DriverPlatformInfoResponse info;
    if (driver.QueryPlatformInfo(info).Succeeded()) {
        std::cout << hv::FormatDriverPlatformInfoText(info);
    }
    driver.Disconnect();
} else {
    std::cerr << driver.GetLastError() << '\n';
}
```

The interface:

1. Opens the device interface registered under
   `{f3a8c7e2-5d41-4a9b-8c6e-2b7d1e9f4a30}`.
2. Sends a fixed 24-byte request with a nonzero monotonic request id.
3. Requires exactly 80 returned bytes.
4. Validates magic, protocol version, structure size, echoed request id,
   protocol status, architecture, processor count, page size and reserved field.
5. Records success and failure events in `DriverDiagnostics`.

The transport is injected through `IDriverTransport`, which is why tests can
exercise reconnect, malformed replies, stale ids and version mismatch without a
driver.

## Determinism and fakes

`FakePlatformProvider` returns a configured snapshot or a configured failure and
counts queries. `FakeKernelDriverInterface` scripts connect/query results and
counts calls. A test-only `IDriverTransport` can return raw bytes, so the real
`KernelDriverInterface` validation path is tested.

## Thread safety

- `WindowsPlatformBackend`, `CpuCapabilityManager`,
  `PlatformCompatibilityManager`, `KernelDriverInterface` and
  `DriverDiagnostics` protect mutable state with mutexes.
- `NativePlatformProvider`, `NativeCompatibilityEnvironment`,
  `NativeDriverTransport` and the fakes are not documented as concurrently
  reusable; use one instance per thread or external synchronization.
- `KernelDriverInterface` serializes connect, disconnect, query and error reads
  through one mutex.

## What this layer does not do

It does not read MSRs, execute VMX instructions, create VMCS or EPT/NPT hardware
structures, access physical memory, launch a guest, modify VBS/HVCI policy,
install the driver, or conceal a hypervisor.
