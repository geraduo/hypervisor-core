# GeraduoDriver

This directory contains the **optional** WDK diagnostic kernel driver shipped
with Geraduo Hypervisor Core v0.6.0. The driver binary itself is unchanged from
v0.5.0. It is not part of the user-mode library and is
not required to build or run `hv_platform_diagnostics.exe` or
`hv_vmx_diagnostics.exe`.

The driver exposes a single IOCTL:

```text
IOCTL_GERADUO_QUERY_PLATFORM_INFO = 0x00226000
```

It returns version, architecture, processor count, page size and physical
memory size. It does **not** execute VMXON, build a VMCS, create hardware
EPT/NPT structures, read or write arbitrary physical memory, access another
process, hide itself, or bypass anti-cheat software.

The protocol version remains `1`, the request remains 24 bytes and the response
remains 80 bytes. The driver still reports version `0.5.0`; the user-mode
readiness layer accepts `0.4.x` and newer. The driver source is the v0.4.0
diagnostic source with the reported version updated to 0.5.0; it was not
changed in v0.6.0.

## Security properties

- `IoCreateDeviceSecure` with `FILE_DEVICE_SECURE_OPEN`.
- SDDL `D:P(A;;GA;;;SY)(A;;GR;;;BA)`: only SYSTEM and Builtin Administrators
  can open the device, and administrators receive read access only.
- `METHOD_BUFFERED` I/O. No `METHOD_NEITHER`, no embedded user pointers.
- Fixed request (24 bytes) and response (80 bytes) layouts shared with the
  user-mode client by `include/hv/platform/driver_protocol.hpp`.
- All request header fields are validated. Unsupported IOCTLs return
  `STATUS_INVALID_DEVICE_REQUEST`; malformed headers return a structured
  `DriverProtocolStatus` in a well-formed response.
- The device interface GUID is
  `{f3a8c7e2-5d41-4a9b-8c6e-2b7d1e9f4a30}`.

## Build

The driver has been compiled and linked against WDK/SDK **10.0.28000.0** with
MSVC **14.52.37025**, Release and Debug x64, using `/W4 /WX`. It has **not**
been signed, installed, loaded or IOCTL-tested. The build machine is a Windows
Server 2025 VM; loading a kernel driver there is left to an isolated test VM.

### Option A: Visual Studio 2022 + WDK Visual Studio integration

With Visual Studio 2022, the WDK and the matching SDK installed:

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DHV_BUILD_DRIVER=ON
cmake --build build --config Release --target GeraduoDriver
```

Or open `driver/GeraduoDriver.vcxproj` directly in Visual Studio and build
`Release|x64`. The output is `build\driver\Release\GeraduoDriver.sys`.

### Option B: direct WDK compiler/linker (VS 2026 and other installations)

Some Visual Studio installations do not register the WDK
`WindowsKernelModeDriver` MSBuild platform toolset. In that case the
`.vcxproj` cannot resolve `km\ntddk.h`. The PowerShell script uses the same
WDK headers and libraries directly with `cl.exe` and `link.exe`:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File driver\build-driver.ps1 -Configuration Release
```

Output: `build-driver\Release\GeraduoDriver.sys`.

The script accepts `-Configuration Debug|Release`, `-WdkVersion`, `-OutputDir`
and `-Clean`. It never signs, installs, loads or starts the driver.

Through CMake, the fallback is a separate target:

```bat
cmake -S . -B build -G "Visual Studio 18 2026" -A x64 -DHV_BUILD_DRIVER=ON
cmake --build build --config Release --target GeraduoDriverDirect
```

Output: `build\driver\Release\GeraduoDriver.sys`.

See `docs/DRIVER_SETUP.md` for signing, installation, test-signing, loading and
removal steps.

## Status

| Item | Status |
| --- | --- |
| Source implemented | Yes |
| User-mode protocol tests | Yes (scripted transport, no driver needed) |
| Compiled and linked against the WDK | Yes: WDK 10.0.28000.0, MSVC 14.52, Release + Debug x64, `/W4 /WX` |
| Binary format | x64 native subsystem, `GsDriverEntry`, NX-compatible, dynamic base |
| Release / Debug size | 27,136 / 27,648 bytes |
| Signed | No (`NotSigned`) |
| Loaded / IOCTL tested on Windows | No |
