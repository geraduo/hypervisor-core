# IOCTL Protocol

The user-mode library and the kernel driver share
`include/hv/platform/driver_protocol.hpp`. The header uses `<stdint.h>` and
plain fixed-width types so the WDK can compile it without the C++ runtime, STL
or `Windows.h`.

## IOCTL

```text
IOCTL_GERADUO_QUERY_PLATFORM_INFO = 0x00226000
```

Computed with the Windows SDK convention:

```c
CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_READ_DATA)
```

| Component | Value |
| --- | --- |
| `FILE_DEVICE_UNKNOWN` | `0x00000022` |
| Function | `0x0800` |
| Transfer type | `METHOD_BUFFERED` (`0x0`) |
| Required access | `FILE_READ_DATA` (`0x1`) |
| Result | `0x00226000` |

The driver sets `DO_BUFFERED_IO` and rejects every other IOCTL with
`STATUS_INVALID_DEVICE_REQUEST`.

## Protocol version

```text
kGeraduoProtocolVersion = 1
```

The version must be incremented if any field changes size, offset, meaning or
validation rule. Both sides must agree on the version; a mismatch is reported as
`DriverProtocolStatus::UnsupportedProtocolVersion` and mapped to
`Status::Unsupported` by the user-mode client.

Protocol version 1 is unchanged in driver 0.5.0. The reported driver version is
now `0.5.0`, and the readiness layer treats `0.4.x` as the minimum compatible
driver version. A 0.4.x driver therefore remains protocol compatible.

## Request: `DriverQueryPlatformInfoRequest`

Size: **24 bytes**, fixed. Packed with `#pragma pack(push, 8)`.

| Offset | Size | Type | Field | Value / rule |
| ---: | ---: | --- | --- | --- |
| 0 | 4 | `uint32_t` | `magic` | `0x47524451` (`"GDRQ"`) |
| 4 | 4 | `uint32_t` | `protocol_version` | Must be `1` |
| 8 | 4 | `uint32_t` | `structure_size` | Must be `24` |
| 12 | 4 | `uint32_t` | `request_id` | Nonzero, echoed by the driver |
| 16 | 4 | `int32_t` | `status` | Must be `0` (`Success`) |
| 20 | 4 | `uint32_t` | `reserved` | Must be `0` |

Little-endian example for `request_id = 1`:

```text
51 44 52 47  01 00 00 00  18 00 00 00  01 00 00 00  00 00 00 00  00 00 00 00
```

## Response: `DriverPlatformInfoResponse`

Size: **80 bytes**, fixed. Packed with `#pragma pack(push, 8)`.

| Offset | Size | Type | Field | Meaning |
| ---: | ---: | --- | --- | --- |
| 0 | 4 | `uint32_t` | `magic` | `0x47524450` (`"GDRP"`) |
| 4 | 4 | `uint32_t` | `protocol_version` | Must be `1` |
| 8 | 4 | `uint32_t` | `structure_size` | Must be `80` |
| 12 | 4 | `uint32_t` | `request_id` | Echo of the request id |
| 16 | 4 | `int32_t` | `status` | `DriverProtocolStatus` |
| 20 | 4 | `uint32_t` | `capability_flags` | Populated-field flags |
| 24 | 4 | `uint32_t` | `driver_version_major` | `0` |
| 28 | 4 | `uint32_t` | `driver_version_minor` | `5` |
| 32 | 4 | `uint32_t` | `driver_version_patch` | `0` |
| 36 | 4 | `uint32_t` | `architecture` | `DriverArchitecture` enum |
| 40 | 4 | `uint32_t` | `os_major` | Windows major version |
| 44 | 4 | `uint32_t` | `os_minor` | Windows minor version |
| 48 | 4 | `uint32_t` | `os_build` | Windows build number |
| 52 | 4 | `uint32_t` | `os_revision` | Reserved by this version (`0`) |
| 56 | 4 | `uint32_t` | `active_processor_count` | At least 1 |
| 60 | 4 | `uint32_t` | `page_size` | Power of two |
| 64 | 8 | `uint64_t` | `physical_memory_bytes` | Total reported physical memory |
| 72 | 8 | `uint64_t` | `reserved` | Must be `0` |

The layout is asserted at compile time:

```cpp
static_assert(sizeof(DriverQueryPlatformInfoRequest) == 24);
static_assert(sizeof(DriverPlatformInfoResponse) == 80);
```

## Enumerations

### `DriverProtocolStatus` (`int32_t`)

| Value | Name | Meaning |
| ---: | --- | --- |
| 0 | `Success` | Request accepted |
| 1 | `InvalidRequest` | Header or fields invalid |
| 2 | `UnsupportedProtocolVersion` | Version mismatch |
| 3 | `BufferTooSmall` | Reported by user-mode validation; the driver returns an NTSTATUS for provider-level buffer failures |
| 4 | `UnsupportedRequest` | IOCTL/function not supported |
| 5 | `InternalError` | Driver-side failure |
| 6 | `InformationUnavailable` | Data not available |

### `DriverArchitecture` (`uint32_t`)

| Value | Name |
| ---: | --- |
| 0 | `Unknown` (rejected by user mode) |
| 1 | `X86` |
| 2 | `X64` |
| 3 | `Arm64` |

### Capability flags

| Bit | Name | Meaning |
| ---: | --- | --- |
| 0 | `kGeraduoCapabilityDiagnosticQuery` | Response is a diagnostic query |
| 1 | `kGeraduoCapabilityOsVersion` | OS version fields are valid |
| 2 | `kGeraduoCapabilityProcessorInfo` | Processor/page fields are valid |
| 3 | `kGeraduoCapabilityPhysicalMemory` | Physical memory was available |
| 4 | `kGeraduoCapabilityCachedSnapshot` | Snapshot was captured at driver load |

## User-mode validation

`KernelDriverInterface::ValidateResponse` rejects:

- response magic other than `0x47524450`;
- protocol version other than `1`;
- structure size other than `80`;
- request id not equal to the id that was sent;
- any status other than `Success`;
- `Unknown` or unrecognized architecture;
- zero active processors;
- zero or non-power-of-two page size;
- nonzero `reserved`.

In addition, `QueryPlatformInfo` requires the transport to return exactly 80
bytes. Short or oversized replies are rejected before field validation. Stale
request ids are rejected.

## Driver semantic errors

To make protocol problems diagnosable, the driver keeps the IOCTL successful
when buffer sizes are large enough for both structures and writes a complete
response for:

- bad magic;
- wrong protocol version;
- wrong structure size;
- nonzero request status;
- nonzero reserved field;
- zero request id.

The response status is set to `InvalidRequest` or
`UnsupportedProtocolVersion`. User-mode validation then maps and journals the
problem. Buffer-too-small conditions cannot produce a response and instead
return `STATUS_INVALID_PARAMETER` or `STATUS_BUFFER_TOO_SMALL`.

## NTSTATUS / Win32 mapping

The user-mode transport maps common `DeviceIoControl` failures:

| Win32 error | `Status` |
| --- | --- |
| `ERROR_FILE_NOT_FOUND`, `ERROR_PATH_NOT_FOUND`, `ERROR_DEVICE_NOT_CONNECTED`, `ERROR_NO_SUCH_DEVICE` | `NotFound` |
| `ERROR_ACCESS_DENIED`, `ERROR_PRIVILEGE_NOT_HELD` | `AccessDenied` |
| `ERROR_INVALID_FUNCTION`, `ERROR_NOT_SUPPORTED`, `ERROR_REVISION_MISMATCH` | `Unsupported` |
| `ERROR_INVALID_PARAMETER`, `ERROR_INSUFFICIENT_BUFFER` | `InvalidArgument` |
| anything else | `InternalError` |

## Security

- No `METHOD_NEITHER`.
- No embedded user pointers.
- No arbitrary read/write.
- No physical-memory or MSR access.
- Device access is restricted by SDDL to SYSTEM and Builtin Administrators.
- The response contains only fixed scalar values; there is no string or
  variable-length data.
