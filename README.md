# Geraduo Hypervisor Core v0.4.0

Windows Platform Backend and a diagnostic-only WDM driver interface. The existing core from v0.3.1 is preserved: lifecycle orchestration, host/guest backend contracts, synthetic page tables, nested paging models, program runner, diagnostics, and platform preflight remain in place.

This release is **not a hardware hypervisor**. It does not execute VMXON, create a VMCS, enter a guest, handle VM exits, or create hardware EPT/NPT tables. EPT/NPT in the existing library remain software models.

## What Is Included

- `WindowsPlatformBackend` collects Windows version, native architecture, logical processor count, page size, and a structured CPUID report through injectable providers.
- `CpuCapabilityManager` reuses the existing `ICpuidProvider` and `RunPlatformPreflight`, and reads the optional CPUID brand string when advertised.
- `PlatformCompatibilityManager` reports Windows x64/version restrictions, CPU preflight evidence, CPUID hypervisor presence/vendor, Device Guard/VBS/HVCI registry configuration, driver connection state, and kernel checks that remain unverified.
- `KernelDriverInterface` enumerates a registered device interface, opens it with read access only, checks the v1 protocol response, and supports clean disconnect/reconnect.
- `GeraduoDriver.vcxproj` is a separate WDM PnP driver project with one buffered, read-only platform information IOCTL.
- `hv_platform_diagnostics.exe` connects to the diagnostic device when available and prints a structured text report. A missing driver is reported; it is not loaded automatically.
- Unit tests use `FakeKernelDriverInterface`, `FakePlatformProvider`, and a scripted driver transport. No driver or hardware virtualization is simulated by the fakes.

## Audit And Compatibility

The source audit for the upstream `dev` branch, including comparison with v0.3.0, is in [docs/AUDIT_V0_3_1.md](docs/AUDIT_V0_3_1.md). The Windows layer extends the current APIs rather than replacing them. The original core targets and test executables remain in the root `CMakeLists.txt`.

## Build User-Mode Components

Requirements: CMake 3.20+, a C++20 compiler, and Visual Studio 2022 for the Windows generator.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The same CMake targets build on Linux for portable development and tests. The native Windows provider and device transport return `Unsupported` on non-Windows hosts; fake providers are used for tests.

To run the host diagnostics application on Windows:

```powershell
.\build\Release\hv_platform_diagnostics.exe
```

The `hv_preflight.exe` executable continues to report the original CPUID-only preflight. Neither tool claims that VMX is ready.

## Build And Install The Driver

The driver is not part of the normal portable CMake build. Install Visual Studio 2022 with the **Desktop development with C++** and **Windows Driver Kit** components, then build separately:

```powershell
msbuild driver\GeraduoDriver.vcxproj /m /p:Configuration=Release /p:Platform=x64
```

The output is under `driver\bin\Release\GeraduoDriver.sys`. Use an isolated test VM and a test-signed package for installation. The INF describes the root-enumerated device ID `ROOT\GERADUOHYPERVISOR`; create/install it with the WDK `devcon` tool from an elevated prompt:

```powershell
devcon install driver\GeraduoDriver.inf ROOT\GERADUOHYPERVISOR
```

Driver signing and test-signing requirements depend on Secure Boot and the Windows build. Follow the WDK signing workflow; do not use an unsigned diagnostic driver on a production system. Remove the test device with `devcon remove ROOT\GERADUOHYPERVISOR`.

See [docs/DRIVER_ARCHITECTURE.md](docs/DRIVER_ARCHITECTURE.md) for the protocol, security model, PnP lifecycle, and detailed instructions. See [docs/WINDOWS_PLATFORM.md](docs/WINDOWS_PLATFORM.md) for the user-mode architecture and diagnostic limits.

## Verification Status

The portable C++ user-mode targets were configured and built with CMake 4.4.5 and GNU C++ 12.2 on Linux. All four CTest suites passed. The Vite website production build also passed.

The WDM driver was **not built or run on Windows** in this environment. Its Visual Studio/WDK project, INF, and kernel source are provided for Windows validation; no driver build or runtime success is claimed.

## Known Limits

- VBS and HVCI are reported from read-only Device Guard registry configuration. This version does not query `Win32_DeviceGuard` WMI runtime state.
- A CPUID hypervisor-present bit is not proof of a conflict. Hyper-V vendor identification is informational.
- Kernel-only VMX MSR/CR checks are not performed. The driver does not execute VMX instructions.
- The user-mode client does not install, start, stop, or elevate the driver service.
- Only Windows x64 is targeted by the WDM project. No AMD SVM backend is implemented.
- No arbitrary physical memory, process memory, or device memory access is exposed.
- The driver and Windows API paths require validation on Windows 10/11 x64 with Visual Studio 2022 and the WDK.

## Browser Preview

The Vite application is an architecture and protocol viewer. It clearly labels itself as a browser preview and does not pretend to access the host OS or the kernel driver. Build it with `npm run build`.