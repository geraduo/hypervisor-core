# Driver Architecture

## Scope

`GeraduoDriver` is an optional WDM kernel driver whose only device-control
operation is:

```text
IOCTL_GERADUO_QUERY_PLATFORM_INFO
```

It returns cached, read-only platform data. It is a diagnostic component, not a
hypervisor. It does not execute VMX instructions, create VMCS or EPT/NPT
hardware structures, access physical memory, inject interrupts, launch a guest,
or conceal anything.

## Source layout

| File | Purpose |
| --- | --- |
| `driver/include/geraduo_driver.hpp` | Kernel-only types: device names, device extension, snapshot, helper declarations |
| `driver/src/driver_entry.cpp` | `DriverEntry`, device creation, IRP dispatch, IOCTL handling, cleanup |
| `driver/GeraduoDriver.vcxproj` | WDK WDM project, x64 Debug/Release, `/W4`, warnings as errors |
| `include/hv/platform/driver_protocol.hpp` | Shared kernel/user ABI, no `Windows.h`, no STL |

## Initialization

`DriverEntry`:

1. Rejects a null driver object.
2. Registers `IRP_MJ_CREATE`, `IRP_MJ_CLOSE`, `IRP_MJ_CLEANUP` and
   `IRP_MJ_DEVICE_CONTROL` handlers.
3. Sets `DriverUnload`.
4. Calls `CreateDevice`.

`CreateDevice`:

1. Builds `\Device\GeraduoDriver`.
2. Calls `IoCreateDeviceSecure` with:
   - device type `FILE_DEVICE_UNKNOWN`,
   - characteristic `FILE_DEVICE_SECURE_OPEN`,
   - SDDL `D:P(A;;GA;;;SY)(A;;GR;;;BA)`,
   - the Geraduo interface class GUID.
3. Zeroes the device extension.
4. Enables `DO_BUFFERED_IO` and clears `DO_DIRECT_IO`.
5. Creates `\DosDevices\GeraduoDriver`.
6. Registers the device interface with `IoRegisterDeviceInterface`.
7. Enables it with `IoSetDeviceInterfaceState`.
8. Captures the OS version, page size and physical-memory snapshot.
9. Clears `DO_DEVICE_INITIALIZING`.

Every failure after device creation is unwound in reverse order by
`DestroyDevice`.

## Device extension

```cpp
struct DeviceExtension {
    UNICODE_STRING interface_name;
    BOOLEAN interface_registered;
    BOOLEAN interface_enabled;
    BOOLEAN symbolic_link_created;
    LONG open_handles;
    SystemSnapshot snapshot;
};
```

The snapshot is captured once during driver load:

- `RtlGetVersion` for major/minor/build.
- `PAGE_SIZE` for page size.
- `MmGetPhysicalMemoryRanges` for total physical memory, summing a bounded,
  zero-terminated array and detecting overflow.
- Active processors are queried per response with
  `KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS)`.

The driver does not keep user-mode handles, process references or MDLs.

## IRP dispatch

| IRP | Behavior |
| --- | --- |
| `IRP_MJ_CREATE` | Increments the open-handle counter, completes with `STATUS_SUCCESS` |
| `IRP_MJ_CLEANUP` | Decrements the counter, completes with `STATUS_SUCCESS` |
| `IRP_MJ_CLOSE` | Completes with `STATUS_SUCCESS` |
| `IRP_MJ_DEVICE_CONTROL` | Dispatches the one supported IOCTL; everything else returns `STATUS_INVALID_DEVICE_REQUEST` |

`GeraduoDeviceControl`:

1. Reads the current IO stack location.
2. Compares `IoControlCode` with `IOCTL_GERADUO_QUERY_PLATFORM_INFO`.
3. Calls `QueryPlatformInfo` for the supported code.
4. For any other code, logs the code at warning level, sets `Information = 0`
   and returns `STATUS_INVALID_DEVICE_REQUEST`.
5. Completes the IRP once through `CompleteIrp`.

## Buffer handling

The IOCTL uses `METHOD_BUFFERED` only.

- The I/O manager allocates one system buffer sized to the larger of input and
  output lengths.
- Input and output lengths are checked independently:
  - input must be at least `sizeof(DriverQueryPlatformInfoRequest)` (24 bytes);
  - output must be at least `sizeof(DriverPlatformInfoResponse)` (80 bytes).
- The request header is copied into a local before the response overwrites the
  shared system buffer.
- `SystemBuffer` is checked for null.
- No user-mode pointer, embedded pointer or `METHOD_NEITHER` path exists.
- The response is zeroed and then filled with fixed-size scalar fields only.

If a buffer is too small, the driver returns `STATUS_INVALID_PARAMETER` or
`STATUS_BUFFER_TOO_SMALL` with `Information = 0`.

If the buffer sizes are valid but the request header is semantically invalid
(bad magic, wrong protocol version, wrong structure size, nonzero reserved
field, nonzero status or zero request id), the driver still writes a complete
80-byte response with `DriverProtocolStatus` set to `InvalidRequest` or
`UnsupportedProtocolVersion`, records `Information = 80`, and returns
`STATUS_SUCCESS`. This lets user mode report a precise protocol problem instead
of a generic Win32 failure.

## Request validation

The driver checks:

1. `magic == kGeraduoRequestMagic`.
2. `protocol_version == kGeraduoProtocolVersion`.
3. `structure_size == sizeof(DriverQueryPlatformInfoRequest)`.
4. `status == DriverProtocolStatus::Success`.
5. `reserved == 0`.
6. `request_id != 0`.

The response:

- uses `kGeraduoResponseMagic`;
- repeats the protocol version and structure size;
- echoes `request_id`;
- sets a protocol status;
- reports capability flags and only fixed scalar fields;
- sets `reserved = 0`.

## Response fields

```text
magic, protocol_version, structure_size, request_id, status, capability_flags
driver_version_major/minor/patch, architecture
os_major/minor/build/revision
active_processor_count, page_size, physical_memory_bytes, reserved
```

Capability flags indicate which fields were populated:

| Flag | Meaning |
| --- | --- |
| `kGeraduoCapabilityDiagnosticQuery` | This is a diagnostic query response |
| `kGeraduoCapabilityOsVersion` | OS version fields are populated |
| `kGeraduoCapabilityProcessorInfo` | Processor/page fields are populated |
| `kGeraduoCapabilityPhysicalMemory` | Physical memory was available |
| `kGeraduoCapabilityCachedSnapshot` | Values come from the load-time snapshot |

## Unload and cleanup

`GeraduoDriverUnload`:

1. Reads the first device object.
2. Logs if any handles remain open.
3. Calls `DestroyDevice`.

`DestroyDevice`:

1. Disables the device interface if enabled.
2. Frees the interface string with `RtlFreeUnicodeString` if registered.
3. Deletes the symbolic link if created.
4. Deletes the device object.

The driver allocates no pool memory per request, so there is no per-IRP free
path. The snapshot is copied into the device extension at load time.

## Security properties

- `IoCreateDeviceSecure` with a restrictive SDDL.
- `FILE_DEVICE_SECURE_OPEN`.
- Only SYSTEM and Builtin Administrators can open the device; administrators
  receive read access.
- The user-mode client opens with `GENERIC_READ`, matching `FILE_READ_DATA` in
  the IOCTL definition.
- No raw device path from a caller is accepted.
- No arbitrary read/write, physical-memory, MSR or pointer operation is
  exposed.
- The driver never installs itself, modifies policy or hides its presence.

## Build and verification status

The driver has been compiled but not executed:

- It was compiled and linked with WDK/SDK 10.0.28000.0 and MSVC 14.52.37025,
  Release and Debug x64, with `/W4 /WX`. The direct `cl.exe`/`link.exe` path
  was used because the WDK Visual Studio integration is not registered in the
  installed VS2026; see `driver/build-driver.ps1`.
- The resulting image is an x64 native-subsystem driver with `GsDriverEntry`,
  NX compatibility and dynamic base. Release: 27,136 bytes; Debug: 27,648
  bytes. The reported driver version is 0.5.0; protocol version 1 and both
  structure layouts are unchanged.
- It was **not signed, installed, loaded or IOCTL-tested**.
- The user-mode ABI, request construction, response validation and error paths
  are tested with a scripted transport against the same shared header.

See [DRIVER_SETUP.md](DRIVER_SETUP.md) for the required build, signing,
installation and verification steps.
