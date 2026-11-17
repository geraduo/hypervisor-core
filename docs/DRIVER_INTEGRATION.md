# Driver Integration and Protocol Compatibility

## Role of the driver

`GeraduoDriver` is an **optional, read-only diagnostic driver**. Its only
operation is a fixed-size platform-information query. It is not a
virtualization driver: it does not execute `VMXON`, create a VMCS, configure
EPT, access physical memory, access another process, hide itself or bypass
anti-cheat or code-integrity software.

The user-mode foundation works without the driver. A missing driver is an
explicit `Blocked`/`DriverNotConnected` verdict, not a crash and not an internal
error.

## Integration points

`VmxReadinessManager` consumes the existing v0.4.0 platform components:

| Component | Use in the foundation |
| --- | --- |
| `WindowsPlatformBackend` | OS version, architecture, processor count, platform support |
| `PlatformCompatibilityManager` | Hyper-V/third-party conflict, VBS, HVCI, WHPX policy |
| `KernelDriverInterface` | Device interface discovery, connect, fixed-size IOCTL, response validation |
| `DriverDiagnostics` | Bounded journal of driver connection/protocol events |

The driver connection is established per readiness evaluation. `Stop()` on the
lifecycle controller calls `VmxReadinessManager::ReleaseConnections()`, which
disconnects the driver. No handle is left open after a stop.

## Protocol compatibility

Protocol version `1` is unchanged from v0.4.0:

| Property | Value |
| --- | --- |
| `kGeraduoProtocolVersion` | `1` |
| Request | `DriverQueryPlatformInfoRequest`, 24 bytes |
| Response | `DriverPlatformInfoResponse`, 80 bytes |
| IOCTL | `IOCTL_GERADUO_QUERY_PLATFORM_INFO = 0x00226000` |
| Transfer | `METHOD_BUFFERED` |
| Access | SDDL-restricted to SYSTEM and Builtin Administrators |
| Device interface GUID | `{f3a8c7e2-5d41-4a9b-8c6e-2b7d1e9f4a30}` |

The driver reports version `0.5.0`. The user-mode readiness layer defines:

```text
kGeraduoMinimumCompatibleDriverMajor = 0
kGeraduoMinimumCompatibleDriverMinor = 4
```

Therefore a 0.4.x driver remains protocol compatible, while 0.3.x and older is
rejected with `Blocked`/`DriverVersionIncompatible` and error class `Protocol`.
This policy exists because the 0.4.0 and 0.5.0 layouts and validation rules are
identical; only the reported version changed.

## Driver validation performed by the readiness layer

| Check | Failure result |
| --- | --- |
| Response magic equals `0x47524450` | `Failed`/`DriverResponseInvalid`, `Protocol` |
| Protocol version equals `1` | `Blocked`/`DriverProtocolMismatch`, `Protocol` |
| Structure size equals 80 | `Blocked`/`DriverProtocolMismatch`, `Protocol` |
| Driver status is `Success` | `Failed`/`DriverQueryFailed`, `Driver` |
| Architecture is known and x64 | `Blocked`/`DriverArchitectureMismatch`, `Protocol` |
| Driver version at least 0.4 | `Blocked`/`DriverVersionIncompatible`, `Protocol` |
| Active processors nonzero, reserved zero | `Failed`/`DriverResponseInvalid`, `Protocol` |
| Processor count matches Windows | Warning only; does not block review |

Expected conditions are separated from internal failures:

- Device interface absent (`Status::NotFound`) → `Blocked`, `DriverNotConnected`.
- Device open denied (`Status::AccessDenied`) → `Blocked`, `DriverAccessDenied`.
- Protocol version mismatch reported by the client (`Status::Unsupported`) →
  `Blocked`, `DriverProtocolMismatch`, `Protocol`.
- Malformed payload (`Status::InvalidArgument`) → `Blocked`,
  `DriverResponseInvalid`, `Protocol`.
- Any other transport/IOCTL error → `Failed`, `DriverQueryFailed`, `Driver`.

## Security boundaries

The driver and client preserve the v0.4.0 boundaries:

- No `METHOD_NEITHER`, no embedded user pointers, no raw caller-supplied device
  path.
- Fixed request and response layouts; no variable-length data, no strings.
- No physical-memory API, no arbitrary kernel or process memory read.
- No MSR, control-register or VMX instruction access.
- No write operation of any kind.
- Unsupported IOCTLs return `STATUS_INVALID_DEVICE_REQUEST`.
- Malformed headers produce a structured protocol status in a well-formed
  response when the buffers are large enough.

The v0.5.0 driver source is the v0.4.0 diagnostic source with the reported
version updated. No new IOCTL, field or capability was added.

## Checklist for an isolated test VM

The driver has **not** been signed, installed, loaded or IOCTL-tested in the
authoring environment. To validate it, use a disposable VM:

1. Enable test signing (`bcdedit /set testsigning on`) or sign the driver with a
   test certificate.
2. Disable or relax HVCI only if the test policy requires it; never do this on
   a machine you do not own.
3. Install the service with `sc.exe` as documented in
   `docs/DRIVER_SETUP.md`.
4. Start the service from an elevated prompt.
5. Run `hv_vmx_diagnostics.exe` and confirm the driver section reports
   `Status: PASS`, `connected: yes`, protocol `1`, response 80 bytes,
   architecture `X64`, version `0.5.0`, compatible driver version and a
   matching processor count.
6. Stop the service and rerun the tool; confirm the result becomes
   `Blocked`/`DriverNotConnected` without a nonzero exit code.
7. Remove the service and the test certificate afterwards.

Do not report the driver as runtime-validated until this checklist has actually
been executed and its output retained.
