# Unsupported Features and Safety Boundaries

v0.6.0 is a software-memory and diagnostic release. The following are **not
implemented**, and none of them are simulated in a way that could be mistaken
for a working implementation.

## Hardware virtualization

| Feature | Status | Notes |
| --- | --- | --- |
| `VMXON` / `VMXOFF` | Not implemented | No VMX instruction is executed |
| VMCS allocation, loading, clearing | Not implemented | No VMCS structure exists |
| VM entry / VM exit | Not implemented | No guest is launched |
| VM-exit reason handling | Not implemented | No exit handler exists |
| Hardware EPT | Not implemented | `hv::memory::EptManager` is a four-level software model only |
| Hardware NPT | Not implemented | `NptManager` is a software abstraction only |
| VPID / EPT TLB invalidation | Not implemented | No `INVEPT` / `INVVPID` |
| MSR bitmaps / I/O bitmaps | Not implemented | No VMCS controls |
| Interrupt injection / APIC virtualization | Not implemented | No guest interrupts |
| Guest memory isolation | Not implemented | No hardware second-level translation |
| AMD-V / SVM backend | Not implemented | AMD and Hygon CPUs remain `Unsupported` |
| Windows Hypervisor Platform backend | Not implemented | WHPX is only probed for presence |
| IA32_FEATURE_CONTROL / VMX capability MSR verification | Not implemented | Requires kernel mode; stays `NOT CHECKED` |

`VmxSupportLevel::Supported` and `VmxReadinessState::ReadyForReview` are
user-mode diagnostic results. They do not mean VMXON can execute.

## Kernel capabilities

| Feature | Status |
| --- | --- |
| Physical-memory read/write | Not implemented |
| Arbitrary kernel virtual memory access | Not implemented |
| Arbitrary process memory access | Not implemented |
| MSR read/write | Not implemented |
| CR0/CR4 modification | Not implemented |
| Kernel code patching / hooking | Not implemented |
| Driver hiding / object unlinking | Not implemented |
| Anti-cheat bypass / anti-detection | Not implemented |
| Privilege escalation | Not implemented |
| Disabling VBS / HVCI / Hyper-V | Not implemented (read-only policy inspection) |
| Driver signing bypass | Not implemented |
| Persistent installation | Not implemented; `sc.exe` steps are manual and documented |

## Driver boundaries

The one IOCTL returns only:

- protocol and driver version;
- architecture;
- Windows major/minor/build/revision;
- active processor count;
- page size;
- total physical memory;
- capability flags.

It accepts no pointer, address, size, path, process id, MSR number or command
payload. There is no write operation. It uses `METHOD_BUFFERED` only and rejects
all other IOCTLs.

The driver was not extended for v0.5.0 or v0.6.0. It remains a read-only
diagnostic query and reports version 0.5.0.

## Evidence and validation gaps

| Item | Status |
| --- | --- |
| User-mode build | Built and tested with Visual Studio 2026 / MSVC 19.52.37029.0 x64 |
| User-mode tests | Twelve test executables, 12/12 passed |
| `hv_vmx_diagnostics.exe` | Text and JSON modes executed on the authoring host; JSON parsed |
| Real Intel VT-x hardware | Not available; no `Supported`/`ReadyForReview` hardware run |
| Requested VS2022 generator | Not available on the authoring machine; not exercised |
| WDK driver compilation | Compiled and linked with WDK 10.0.28000.0 / MSVC 14.52, Release and Debug x64, `/W4 /WX`; MSBuild WDK toolset not available |
| Driver signing | Not performed (`NotSigned`) |
| Driver loading | Not performed |
| Driver IOCTL execution on hardware | Not performed |
| Real VMXON / guest execution | Not performed |
| AMD-V backend | Not implemented |
| Production readiness | None claimed |

The user-mode tests exercise the protocol header with a scripted transport,
including malformed structures, version mismatch, missing device, access denial,
reconnect, rollback, concurrency and resource release. This validates the
user-mode contract, not kernel execution.

## Safety rules for users

- Do not load the driver on a production machine. Use a disposable VM.
- Do not disable Secure Boot, HVCI or VBS on a machine you do not own or
  administer.
- Do not publish test-signed drivers.
- Do not treat `Supported`, `NoConflictDetected`, `ReadyForReview` or
  `KernelVerificationRequired` as proof that hardware virtualization works.
- Do not add physical-memory or VMX operations to the diagnostic driver and call
  them "tested" without kernel-mode tests.
- If a future release implements VMX/SVM, it needs its own kernel-mode test
  matrix, hardware coverage, security review and signing story.

## Why the project stops here

Implementing a correct hypervisor requires:

- per-processor VMXON/VMCS setup;
- IA32_FEATURE_CONTROL and CR0/CR4 fixed-bit validation;
- VM-exit handling and guest state management;
- interrupt, exception and MSR policies;
- second-level address translation;
- robust teardown on every failure path;
- kernel debugging and hardware validation.

The 0.x line deliberately provides a verifiable capability/readiness/lifecycle
structure, protocol validation and a safe read-only driver interface as the
foundation for that work. v0.6.0 adds a software-only memory model for research,
tests and diagnostics; it does not pretend the hardware stages are complete.
