# Diagnostics and Error Interpretation

## Running

```bat
build\Release\hv_platform_diagnostics.exe
build\Release\hv_platform_diagnostics.exe --json
build\Release\hv_platform_diagnostics.exe --no-driver
build\Release\hv_platform_diagnostics.exe --json --no-driver
```

| Option | Effect |
| --- | --- |
| `--json` | Emit one JSON document |
| `--no-driver` | Do not enumerate the device interface |
| `--help`, `-h` | Print usage |

Exit codes:

| Code | Meaning |
| ---: | --- |
| 0 | Platform, CPU and compatibility layers completed (unsupported hardware is still a completed diagnostic) |
| 1 | A platform, CPU or compatibility probe failed internally |
| 2 | Invalid command line |

A missing driver does **not** produce a nonzero exit code. It is a normal,
explicit diagnostic result.

## Report sections

### Platform backend

Example from the authoring host:

```text
=== Windows platform backend ===
Geraduo Windows platform backend
State: Initialized
Support: SupportedWithWarnings
Summary: The Windows platform backend is initialized with warnings. No hardware virtualization was started; kernel-mode checks remain required.
OS: Windows Server 2025 10.0.26200.9974 (server=yes)
Architecture: x64 native, x64 process
Logical processors: 12
Page size: 4096
Physical memory: 17102315520 bytes
Capabilities:
  [PASS] Windows 10/11 or later - Windows Server 2025 10.0.26200.0 is supported.
  [WARN] Client Windows edition - Windows Server was detected; v0.5.0 validates Windows 10/11 client x64.
  ...
  [NOT CHECKED] Virtualization firmware state - IA32_FEATURE_CONTROL, CR0/CR4 fixed bits and VMX capability MSRs require kernel mode.
```

`SupportedWithWarnings` is expected for a Windows Server edition. It does not
imply hardware virtualization support.

### CPU capabilities

The CPU report includes vendor, brand, family/model/stepping, maximum leaves,
VMX/SVM/hypervisor bits and eligibility. `Kernel-mode verification required: yes`
is always printed.

On the authoring host (AMD Ryzen 5 5600 under Hyper-V):

```text
Vendor: AuthenticAMD [AMD]
Brand: AMD Ryzen 5 5600 6-Core Processor
Signature: family 25, model 33, stepping 2
Features: VMX reported=no, SVM reported=no, hypervisor present=yes
Eligibility: Unsupported
Preflight verdict: UnsupportedPlatform
Summary: CPU vendor "AuthenticAMD" has no implemented virtualization backend.
```

This is the correct, honest result: no AMD-V/SVM backend exists in v0.5.0.

### Compatibility

On the same host:

```text
Verdict: UnsupportedPlatform
Environment: MicrosoftHyperV
VBS: EnabledConfigured, HVCI: EnabledConfigured
WinHvPlatform API: available, hypervisor reported: yes
Conflict detected: yes
```

CPU support takes precedence over VBS/HVCI/conflict warnings. Even when a
hypervisor is detected, the report does not claim that the CPU can or cannot run
a nested hypervisor; it says direct ownership is expected to conflict unless
nested virtualization or another design is used.

### Driver

When the driver is absent:

```text
=== Geraduo diagnostic driver ===
Not connected. The Geraduo driver device interface is not present. The driver is not installed, not loaded, or the device failed to start.
The user-mode diagnostics above are still valid. The driver is optional and is not required for platform/CPU reporting.

Geraduo driver diagnostics journal
  [ERROR] DeviceInterfaceMissing: The Geraduo driver device interface is not present. The driver is not installed, not loaded, or the device failed to start.
```

When connected, the section prints the driver response and the journal. See
[IOCTL_PROTOCOL.md](IOCTL_PROTOCOL.md) for every field.

## VT-x diagnostics (`hv_vmx_diagnostics.exe`)

v0.5.0 adds a second CLI that composes the VT-x capability, readiness,
lifecycle and journal layers:

```bat
build\Release\hv_vmx_diagnostics.exe
build\Release\hv_vmx_diagnostics.exe --json
build\Release\hv_vmx_diagnostics.exe --no-driver
build\Release\hv_vmx_diagnostics.exe --json --no-driver
```

| Option | Effect |
| --- | --- |
| `--json` | Emit one JSON document (`schema: geraduo.vmx.diagnostics.v1`) |
| `--no-driver` | Skip device discovery; driver compatibility becomes `NOT CHECKED` and blocks review |
| `--help`, `-h` | Print usage |

Exit codes:

| Code | Meaning |
| ---: | --- |
| 0 | The lifecycle produced a verdict (`NotSupported`, `Blocked` or `ReadyForReview`) |
| 1 | A diagnostic component failed internally |
| 2 | Invalid command line |

A missing driver does not produce a nonzero exit code. It is a normal `Blocked`
verdict with reason `DriverNotConnected`.

On the authoring AMD host with `--no-driver`, the tool reports:

```text
State: NotSupported
Reason: UnsupportedCpuVendor
Error class: None
Summary: CPU vendor "AuthenticAMD" has no implemented virtualization backend.
         The Intel VT-x foundation requires GenuineIntel.
Hardware VMX started: no
Hardware VMX start supported: no
```

With driver discovery enabled, the same host additionally reports the driver
section as `NOT CHECKED` with `DeviceInterfaceMissing`, because no driver is
installed. This is expected and does not change the CPU verdict.

The JSON document contains `backend`, `capability`, `readiness`, `events` and a
`boundary` object that explicitly records:

```json
{
  "vmxon_executed": false,
  "vmcs_accessed": false,
  "vm_entry_exit_executed": false,
  "hardware_ept_configured": false,
  "privileged_instructions_executed": false
}
```

See [VMX_READINESS.md](VMX_READINESS.md) for the full state, reason and error
tables.

## Interpreting results

### Platform support

| Value | What it means | What it does not mean |
| --- | --- | --- |
| `Supported` | Required user-mode platform checks passed | Hardware virtualization works |
| `SupportedWithWarnings` | Usable user-mode diagnostics with warnings | VMX is available |
| `Unsupported` | A required user-mode platform property is missing (for example non-x64) | Nothing about kernel-mode VMX state |

### CPU eligibility

| Value | What it means |
| --- | --- |
| `Unsupported` | No implemented backend, missing VMX, unknown vendor or probe failure |
| `ReviewRequired` | A hypervisor is reported; ownership cannot be assumed |
| `KernelVerificationRequired` | CPUID is consistent with Intel VT-x; kernel checks are mandatory |

There is no `Ready` value.

### Compatibility verdict

| Value | What it means |
| --- | --- |
| `NoConflictDetected` | No known user-mode conflict; still not proof that `VMXON` succeeds |
| `ReviewRequired` | Unknown/configured VBS or HVCI, unrecognized hypervisor, or probes unavailable |
| `KnownConflict` | Hyper-V, third-party hypervisor or WHPX reports a hypervisor |
| `UnsupportedPlatform` | No implemented CPU backend or VMX not reported |
| `Unknown` | CPUID probing failed |

## Driver diagnostics journal

`DriverDiagnostics` is bounded (256 events by default) and thread-safe. Each
event has a level, a code, a message and a timestamp.

| Code | Typical cause | Action |
| --- | --- | --- |
| `DeviceInterfaceMissing` | SetupAPI found no interface | Install/start the driver, or accept user-mode-only diagnostics |
| `DeviceOpenFailed` | `CreateFileW` failed | Check service state, path and permissions |
| `DeviceAccessDenied` | SDDL / UAC denied access | Run elevated; do not weaken the driver SDDL casually |
| `IoctlFailed` | `DeviceIoControl` failed | Check driver version, Event Viewer / DebugView, and the Win32 error detail |
| `ProtocolVersionMismatch` | Driver and client speak different versions | Rebuild both from the same release |
| `ResponseMalformed` | Magic, size, id, architecture, processor, page size or reserved field invalid | Treat the driver as faulty; do not trust the result |
| `NotConnected` | Query or operation attempted before `Connect()` | Connect first |
| `Connected` | Device opened | Informational |
| `Disconnected` | Device closed | Informational |
| `QuerySucceeded` | Valid response accepted | Informational |
| `QueryFailed` | Driver reported a non-success protocol status | Read the message and protocol status |
| `InitializationFailed` | No transport or platform initialization failed | Check configuration and error detail |

## Error detail sources

`GetLastError()` combines the mapped `Result` message with a transport detail:

- Win32 error text from `FormatMessageW` for `CreateFileW` / `DeviceIoControl`
  failures;
- a stable explanation for missing device interfaces;
- protocol-specific text for version, structure, request-id or status errors.

The error is retained until the next successful operation or disconnect.

## JSON output

`hv_platform_diagnostics.exe --json` emits a single JSON object with four
top-level members:

```json
{
  "platform": { ... },
  "cpu": { ... },
  "compatibility": { ... },
  "driver": {
    "skipped": false,
    "connected": false,
    "query_succeeded": false,
    "error": "...",
    "info": null,
    "diagnostics": { "events": [] }
  }
}
```

`platform_error` is also present when the platform layer fails. All text is
escaped. A successful connected query replaces `info` with the driver response
object. The JSON can be validated with `ConvertFrom-Json`, `jq` or any standard
parser.

`hv_vmx_diagnostics.exe --json` emits a different document with schema
`geraduo.vmx.diagnostics.v1` and top-level `tool_version`, `backend`,
`capability`, `readiness`, `events` and `boundary` members. Both tools emit
exactly one JSON document and no other stdout text in JSON mode.

## Data handling

- All diagnostics are local and read-only.
- No telemetry or network access is performed.
- No process memory, physical memory or user files are read by the driver.
- The JSON report contains OS build, processor count, memory size, CPU brand and
  policy state. Treat it as machine fingerprinting data if you publish it.

## Common false conclusions

| Observation | Correct conclusion |
| --- | --- |
| CPUID reports VMX | CPU is a candidate for kernel checks; not ready |
| `NoConflictDetected` | No known user-mode conflict; not proof `VMXON` works |
| VBS disabled | HVCI is inactive, not that VMX is available |
| WHPX API present | API exists; not proof a hypervisor is active |
| Driver connects | Driver works; not proof hardware virtualization works |
| `SupportedWithWarnings` | Diagnostics work with warnings; not a virtualization result |
