# Driver Integration (v0.5.0)

The v0.4.0 diagnostic WDM driver is **unchanged**. v0.5.0 only changes how user mode consumes it.

## Boundary

| User mode | Kernel mode |
|---|---|
| `VmxReadinessManager`, `VmxLifecycleController`, `IntelVirtualizationBackend`, `VmxDiagnostics` | `GeraduoDriver` (one buffered, read-only `IOCTL_QUERY_PLATFORM_INFO`) |
| Talks to the driver only through `IKernelDriverInterface` | Returns OS version, architecture, processor count, page size, flags |

The driver does not execute VMX instructions, does not read/write MSRs, and exposes **no** physical, process or device memory.
The protocol (`hv/platform/driver_protocol.h`) is version 1 and is unchanged.

## How readiness uses the driver

1. If `IsConnected()` is false, call `Connect()`. `KernelDriverInterface` enumerates the device interface, opens it read-only
   and validates the v1 handshake.
2. Call `QueryPlatformInfo()`.
3. Compare the response with the user-mode `WindowsPlatformBackend` snapshot.
4. Run `PlatformCompatibilityManager::Evaluate()` while the driver is connected so its own driver checks run.

| Condition ids | Meaning |
|---|---|
| `vmx.driver.connection` | Device opened |
| `vmx.driver.protocol` | Protocol v1 accepted (`NOT_CHECKED` when the connection or query failed first) |
| `vmx.driver.platform_info` | Response received and validated |
| `vmx.driver.consistency` | Architecture, processor count, page size (**FAIL** on mismatch); OS version, VMX flag, enumerated processor count (**WARN**) |

## Error mapping

| Driver status | `VmxFailureReason` | Verdict |
|---|---|---|
| `DriverNotFound` | `DriverNotFound` | Blocked |
| `AccessDenied` | `DriverAccessDenied` | Blocked |
| `ProtocolMismatch` (connect or query) | `DriverProtocolMismatch` | Blocked |
| other connect errors | `DriverConnectionFailed` | Blocked |
| query errors (`InvalidResponse`, `BufferTooSmall`, `TransportError`, ...) | `DriverQueryFailed` | Blocked |
| mismatch with user-mode data | `DriverDataInconsistent` | Blocked |

With `require_driver = false` these become `WARN` and the verdict is not affected.

## Resource handling

* A failed `Connect()` is followed by `Disconnect()` to clean up a partial connection.
* The manager remembers whether it opened the connection (`connected_by_readiness`). Any verdict other than `ReadyForReview`
  closes it; `ReleaseDriver()` / `IntelVirtualizationBackend::Shutdown()` close a retained one.
* A connection opened by another component is never closed by this layer.

## Logging

* `DriverDiagnostics` (existing) logs driver traffic. Pass it to `IntelVirtualizationBackend` to include `driver_events` in reports.
* `VmxDiagnostics` (new) logs readiness events. They are separate logs.

## What the driver does not provide yet

Kernel checks (`IA32_FEATURE_CONTROL`, VMX capability MSRs, CR0/CR4 fixed bits, VMXON permission) require a protocol extension
and remain `NOT_CHECKED`. If added, keep the interface read-only and diagnostic.

## Building and installing

Unchanged from v0.4.0: see `docs/DRIVER_ARCHITECTURE.md`. Use an isolated test VM and a test-signed package.
