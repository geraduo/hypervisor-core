# Diagnostic WDM Driver

## Scope

The driver is a Windows x64 WDM PnP software device with one diagnostic operation. It is not a hypervisor and performs no VMXON, VMCS, VM-entry, VM-exit, hardware EPT/NPT, physical memory, or process memory operation.

## Device And PnP Lifecycle

`DriverEntry` installs dispatch routines and an `AddDevice` callback. The INF describes the root-enumerated hardware ID `ROOT\GERADUOHYPERVISOR`. In `AddDevice`, the driver creates an unnamed FDO, attaches to its PDO stack, and calls `IoRegisterDeviceInterface` on the PDO using the Geraduo interface-class GUID. The interface is enabled after `IRP_MN_START_DEVICE` succeeds and disabled on stop, surprise removal, or removal.

The PnP path forwards requests to the lower device stack. Start and remove are forwarded synchronously where the driver must update device/interface state. `IRP_MJ_CREATE`, `IRP_MJ_CLEANUP`, and `IRP_MJ_CLOSE` are completed locally. The device-control dispatcher handles only the single supported IOCTL; unknown control codes and unsupported operations are rejected. `IRP_MJ_POWER` and system-control requests are forwarded.

On remove, the driver disables the interface, forwards the remove IRP, detaches from the lower stack, frees the interface name, deletes the FDO, and completes the request. Driver unload emits a diagnostic message. No long-lived pool allocation or per-request kernel allocation is required.

## Access Control

The INF applies `D:P(A;;GA;;;SY)(A;;GR;;;BA)`: SYSTEM receives full access and built-in Administrators receive generic read only. The IOCTL access field is `FILE_READ_DATA`, and the user client opens the interface with `GENERIC_READ`. The device type is Geraduo's vendor-defined `0x8000`, and the function number is in the vendor range.

The response is bounded and contains only system version, architecture, processor count, page size, and CPUID feature flags. No user pointer is accepted. The driver does not expose arbitrary read/write, map/unmap, physical memory, process memory, or device-memory operations.

## Request Handling

1. Confirm the IOCTL is exactly `IOCTL_GERADUO_QUERY_PLATFORM_INFO`.
2. Check output capacity for the fixed response and input capacity for the fixed request.
3. Copy the request header from the I/O manager's `SystemBuffer` to a local value before overwriting the shared buffered I/O buffer.
4. Validate protocol version, exact structure size, exact input length, and nonzero request ID through `ValidateQueryRequest()`.
5. Zero the complete response, set the current protocol version/size, and echo the request ID.
6. Query OS version with `RtlGetVersion`, active processor count with `KeQueryActiveProcessorCountEx`, page size with `PAGE_SIZE`, and unprivileged CPUID feature bits.
7. Set `IoStatus.Information` to exactly the fixed response size. On short buffers or unknown IOCTLs, return an error with zero information.

The kernel dispatch and user-mode client share the protocol layout and constants. Cross-platform unit tests exercise structure validation, protocol mismatch, truncated responses, request-ID mismatch, reconnect, missing device, and cleanup through fake transports. Those tests do not execute the WDM dispatch routine.

## Build With Visual Studio 2022 And WDK

Install Visual Studio 2022 with the Desktop development with C++ workload and a Windows 10/11 WDK. Open `driver/GeraduoDriver.vcxproj`, select `Release | x64`, and build. Or run:

```powershell
msbuild driver\GeraduoDriver.vcxproj /m /p:Configuration=Release /p:Platform=x64
```

The project output path is `driver\bin\Release\GeraduoDriver.sys`. The WDK must validate the INF/catalog for the installed kit. This repository does not include a production certificate, catalog signature, or prebuilt driver binary.

## Install In An Isolated Test VM

1. Use a disposable Windows 10/11 x64 VM with a matching test-signing setup. Kernel drivers must satisfy the signing policy for the current Windows build. Follow Microsoft WDK test-signing guidance; do not weaken a production host's Secure Boot or code-integrity policy to run this sample.
2. Build and test-sign the generated driver package using the WDK workflow for that VM.
3. From an elevated terminal, create/install the root-enumerated test device with `devcon` from the WDK:

```powershell
devcon install driver\GeraduoDriver.inf ROOT\GERADUOHYPERVISOR
```

4. Run `build\Release\hv_platform_diagnostics.exe`. If the device is present and accessible, SetupAPI finds the registered interface and the utility issues only the platform-info query.
5. Remove the sample device when finished:

```powershell
devcon remove ROOT\GERADUOHYPERVISOR
```

This installation procedure is documentation only; it has not been executed in the current Linux environment. `DriverEntry`, PnP behavior, INF installation, driver signing, and IOCTL execution still require Windows/WDK validation.

## Unimplemented And Unsupported

- No VMX/SVM activation or teardown.
- No VMCS, VMCB, VM entry/exit, EPT/NPT hardware tables, or guest execution.
- No physical or process memory access.
- No hypervisor hiding, anti-cheat bypass, or kernel patching.
- No service installer, automatic driver load, or privilege escalation in the user-mode client.
- No claim of Windows driver test or successful WDK build.