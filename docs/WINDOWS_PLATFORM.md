# Windows Platform Backend

## Architecture

The Windows layer extends the existing platform preflight rather than changing `Lifecycle`, `Hypervisor`, `IHostBackend`, `IGuestBackend`, `MemoryManager`, `SyntheticPageTable`, `NestedPageTable`, `EptManager`, `NptManager`, `ProgramRunner`, or `Diagnostics`.

```text
WindowsPlatformBackend
  +-- IPlatformProvider
  |     +-- WindowsPlatformProvider (Win32 / registry, read-only)
  |     +-- FakePlatformProvider (tests)
  +-- CpuCapabilityManager
        +-- existing ICpuidProvider
              +-- NativeCpuidProvider
              +-- existing FakeCpuidProvider

PlatformCompatibilityManager
  +-- Windows system and mitigation evidence
  +-- CPU preflight evidence
  +-- IKernelDriverInterface
        +-- KernelDriverInterface (SetupAPI + DeviceIoControl)
        +-- FakeKernelDriverInterface (tests)
```

### WindowsPlatformBackend

`WindowsPlatformBackend::Initialize()` obtains the OS version through `RtlGetVersion`, native architecture and processor/page data through `GetNativeSystemInfo`, and a `CpuCapabilityReport`. The backend has independent `Stopped`, `Initializing`, `Ready`, and `Failed` states; it does not reuse the hypervisor lifecycle state or imply that a VMM is running. `GetSnapshot()` only succeeds in `Ready` state. `Shutdown()` clears the snapshot and is safe to call repeatedly.

The native Windows provider reports Windows 10/11 build identifiers, x86/x64/ARM64 architecture, processor count, and page size. Non-Windows builds return `Unsupported` for native Windows data; they remain useful for fake-provider tests.

### CpuCapabilityManager

The manager calls `RunPlatformPreflight()` through the existing `ICpuidProvider`, preserving its vendor validation, family/model decode, virtualization bits, and explicit kernel checks. It reads the 48-byte CPUID brand string only when the extended maximum leaf advertises `0x80000004`. Missing optional brand leaves do not invalidate an otherwise useful report. No privileged instruction is executed.

### PlatformCompatibilityManager

The compatibility report contains an overall diagnostic verdict plus individual check IDs, statuses, and details. It applies the current scope restrictions (Windows 10 or newer, x64, Intel VMX reported by CPUID), reports CPUID hypervisor/vendor evidence, and queries the `DeviceGuard` and HVCI registry values with read-only access. Configured VBS/HVCI/Credential Guard values are warnings; a registry value does not prove a feature is active after boot. The backend intentionally does not call WMI `Win32_DeviceGuard` in v0.4.0.

If the driver is connected, the manager queries its platform response and compares OS version and architecture. If it is not connected, kernel checks remain `NOT CHECKED`; the report remains advisory. A known Microsoft hypervisor vendor is not treated as an automatic conflict.

### KernelDriverInterface

The public API is `Connect()`, `Disconnect()`, `IsConnected()`, `QueryPlatformInfo()`, and `GetLastError()`. On Windows, `Connect()` enumerates the Geraduo device interface GUID with SetupAPI and opens the interface with `GENERIC_READ`. It does not load a driver, create a service, prompt for elevation, or request write access. `Disconnect()` closes the handle; the interface supports repeated connection cycles.

The user-mode response validator checks the byte count, protocol version, response structure size, matching request ID, and operation status before copying fields to the caller. A missing device maps to `DriverNotFound`; access and transport failures keep their own status and details.

### DriverDiagnostics

The user-mode event journal is thread-safe, sequence-numbered, and bounded (128 events by default). It records connect/disconnect, missing-device/access errors, query failures, protocol mismatches, and invalid responses. Kernel messages use `DbgPrintEx` and include driver entry, PnP device creation/start/remove, unsupported IOCTLs, query completion, and initialization failures. Kernel debugger output is separate from the user-mode event journal.

## IOCTL Protocol

The shared protocol is `include/hv/platform/driver_protocol.h`, version 1:

- `IOCTL_GERADUO_QUERY_PLATFORM_INFO` is `CTL_CODE(0x8000, 0x800, METHOD_BUFFERED, FILE_READ_DATA)` (`0x80006000`).
- Request: 16 bytes (`protocol_version`, `structure_size`, nonzero `request_id`). The current version requires the exact request size.
- Response: 48 bytes (`protocol_version`, `structure_size`, matching `request_id`, operation status, and platform information).
- Platform information: OS major/minor/build, x64 architecture, logical processor count, page size, and CPUID hypervisor/Intel VMX flags.
- Protocol errors are returned in the fixed response status when the output buffer is large enough. Buffer truncation and unknown IOCTLs fail the IRP without reporting uninitialized output.

The `METHOD_BUFFERED` request has no embedded pointers. The driver checks the input and output lengths before use and returns only `sizeof(GERADUO_QUERY_RESPONSE)` bytes. Unsupported IOCTLs return `STATUS_INVALID_DEVICE_REQUEST`. `METHOD_NEITHER` is not used.

## Error And Diagnostic Semantics

- No registered interface / absent device: `DriverNotFound`; no auto-load is attempted.
- Access denied: `AccessDenied`, commonly caused by a non-administrator token or device ACL.
- Disconnected query: `DeviceNotConnected`.
- Wrong protocol version: `ProtocolMismatch`.
- Short, mismatched, or malformed response: `InvalidResponse`.
- OS or registry configuration unavailable: corresponding checks are `NOT CHECKED` or `Inconclusive`; absence of evidence is not reported as a pass.
- Kernel-only checks (MSRs, control registers, VMXON, EPT capability) always remain unverified.

See [DRIVER_ARCHITECTURE.md](DRIVER_ARCHITECTURE.md) for the PnP driver and WDK workflow.