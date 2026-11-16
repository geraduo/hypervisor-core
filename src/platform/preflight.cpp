#include "hv/platform/preflight.hpp"
#include <string>
#include <utility>

namespace hv {
namespace {

constexpr std::uint32_t kHypervisorLeaf = 0x40000000u;
constexpr std::uint32_t kExtendedLeaf1 = 0x80000001u;

// Keeps only printable ASCII so that hostile or garbled CPUID data cannot corrupt reports.
std::string Sanitize(std::string text) {
    while (!text.empty() && text.back() == '\0') text.pop_back();
    for (char& ch : text) {
        const auto value = static_cast<unsigned char>(ch);
        if (value < 0x20 || value > 0x7E) ch = '?';
    }
    return text;
}

std::string Unpack(std::uint32_t first, std::uint32_t second, std::uint32_t third) {
    std::string text;
    text.reserve(12);
    for (const std::uint32_t reg : {first, second, third})
        for (int i = 0; i < 4; ++i) text.push_back(static_cast<char>((reg >> (8 * i)) & 0xFFu));
    return Sanitize(std::move(text));
}

CpuVendor ClassifyCpu(const std::string& vendor) {
    if (vendor == "GenuineIntel") return CpuVendor::Intel;
    if (vendor == "AuthenticAMD") return CpuVendor::Amd;
    if (vendor == "HygonGenuine") return CpuVendor::Hygon;
    return CpuVendor::Other;
}

HypervisorVendor ClassifyHypervisor(const std::string& vendor) {
    if (vendor == "Microsoft Hv") return HypervisorVendor::MicrosoftHyperV;
    if (vendor == "KVMKVMKVM") return HypervisorVendor::Kvm;
    if (vendor == "VMwareVMware") return HypervisorVendor::VMware;
    if (vendor == "XenVMMXenVMM") return HypervisorVendor::Xen;
    if (vendor == "VBoxVBoxVBox") return HypervisorVendor::VirtualBox;
    if (vendor == "TCGTCGTCGTCG") return HypervisorVendor::QemuTcg;
    return HypervisorVendor::Other;
}

void Add(PreflightReport& r, std::string id, std::string title, CheckStatus status,
         std::string detail) {
    r.checks.push_back({std::move(id), std::move(title), status, std::move(detail)});
}

// Checks that user mode cannot perform. They are listed explicitly as NotChecked so that a report
// can never be read as "VMX is ready".
void AddKernelModeChecks(PreflightReport& r) {
    Add(r, "msr.ia32_feature_control", "IA32_FEATURE_CONTROL (MSR 0x3A) lock and VMXON enable bits",
        CheckStatus::NotChecked, "Requires RDMSR: kernel-mode component (Stage 2).");
    Add(r, "msr.vmx_capabilities", "VMX capability MSRs (0x480-0x48F) and CR0/CR4 fixed bits",
        CheckStatus::NotChecked, "Requires RDMSR/CR access: kernel-mode component (Stage 3).");
    Add(r, "msr.ept_vpid_cap", "EPT/VPID capabilities (IA32_VMX_EPT_VPID_CAP, MSR 0x48C)",
        CheckStatus::NotChecked, "Requires RDMSR: kernel-mode component (Stage 5).");
    Add(r, "windows.hyperv_vbs", "Windows Hyper-V / VBS / HVCI state",
        CheckStatus::NotChecked, "Not queried by CPUID preflight; PlatformCompatibilityManager task.");
}

void Finish(PreflightReport& r, PreflightVerdict verdict, std::string summary) {
    r.verdict = verdict;
    r.summary = std::move(summary);
    AddKernelModeChecks(r);
}

} // namespace

Result RunPlatformPreflight(ICpuidProvider& cpuid, PreflightReport& out) {
    out = PreflightReport{};
    PreflightReport& r = out;
    CpuidRegisters regs;

    // --- Leaf 0: vendor and maximum basic leaf.
    auto q = cpuid.Query(0, 0, regs);
    if (!q.Succeeded()) {
        Add(r, "cpuid.leaf0", "CPUID leaf 0 readable", CheckStatus::Fail, q.message);
        if (q.status == Status::Unsupported) {
            Finish(r, PreflightVerdict::UnsupportedPlatform,
                   "CPUID is not available on this build/architecture.");
            return Result::Ok();
        }
        Finish(r, PreflightVerdict::ProbeFailed, "CPUID leaf 0 could not be read; nothing was concluded.");
        return Result::Error(q.status, "CPUID leaf 0 could not be read: " + q.message);
    }
    r.max_basic_leaf = regs.eax;
    r.cpu_vendor_string = Unpack(regs.ebx, regs.edx, regs.ecx);   // EBX, EDX, ECX order
    r.cpu_vendor = ClassifyCpu(r.cpu_vendor_string);
    Add(r, "cpuid.leaf0", "CPUID leaf 0 readable", CheckStatus::Pass,
        "vendor=\"" + r.cpu_vendor_string + "\", max basic leaf=" + std::to_string(r.max_basic_leaf));

    // --- Leaf 1: signature, VMX bit, hypervisor-present bit.
    if (r.max_basic_leaf >= 1) {
        q = cpuid.Query(1, 0, regs);
        if (!q.Succeeded()) {
            Add(r, "cpuid.01h", "CPUID leaf 1 readable", CheckStatus::Fail, q.message);
            Finish(r, PreflightVerdict::ProbeFailed, "CPUID leaf 1 could not be read; nothing was concluded.");
            return Result::Error(q.status, "CPUID leaf 1 could not be read: " + q.message);
        }
        r.leaf1_read = true;
        const std::uint32_t base_family = (regs.eax >> 8) & 0xFu;
        const std::uint32_t base_model = (regs.eax >> 4) & 0xFu;
        r.stepping = regs.eax & 0xFu;
        r.family = base_family == 0xFu ? base_family + ((regs.eax >> 20) & 0xFFu) : base_family;
        r.model = (base_family == 0x6u || base_family == 0xFu)
                      ? (((regs.eax >> 16) & 0xFu) << 4) + base_model : base_model;
        r.vmx_cpuid_bit = ((regs.ecx >> 5) & 1u) != 0;
        r.hypervisor_present_bit = ((regs.ecx >> 31) & 1u) != 0;
        Add(r, "cpuid.01h", "CPUID leaf 1 readable", CheckStatus::Pass,
            "family=" + std::to_string(r.family) + ", model=" + std::to_string(r.model) +
            ", stepping=" + std::to_string(r.stepping));
    } else {
        Add(r, "cpuid.01h", "CPUID leaf 1 readable", CheckStatus::Fail,
            "Maximum basic leaf is " + std::to_string(r.max_basic_leaf) + "; leaf 1 was not queried.");
    }

    // --- Hypervisor vendor leaf: only meaningful when ECX[31] is set.
    if (r.hypervisor_present_bit) {
        q = cpuid.Query(kHypervisorLeaf, 0, regs);
        if (q.Succeeded()) {
            r.max_hypervisor_leaf = regs.eax;
            r.hypervisor_vendor_string = Unpack(regs.ebx, regs.ecx, regs.edx);  // EBX, ECX, EDX order
            r.hypervisor_vendor = ClassifyHypervisor(r.hypervisor_vendor_string);
        } else {
            r.hypervisor_vendor = HypervisorVendor::Other;
        }
    }

    // --- AMD/Hygon: SVM bit is recorded for the future backend only.
    if (r.cpu_vendor == CpuVendor::Amd || r.cpu_vendor == CpuVendor::Hygon) {
        q = cpuid.Query(kExtendedLeaf1, 0, regs);
        if (q.Succeeded()) {
            r.svm_read = true;
            r.svm_cpuid_bit = ((regs.ecx >> 2) & 1u) != 0;
        }
    }

    // --- Individual checks.
    Add(r, "cpu.vendor", "CPU vendor is GenuineIntel",
        r.cpu_vendor == CpuVendor::Intel ? CheckStatus::Pass : CheckStatus::Fail,
        r.cpu_vendor == CpuVendor::Intel
            ? "Intel VT-x backend applies."
            : "Vendor \"" + r.cpu_vendor_string + "\": only the Intel VT-x backend exists; AMD-V/SVM is planned, not implemented.");
    if (r.svm_read)
        Add(r, "cpuid.80000001h.ecx2.svm", "CPUID.80000001H:ECX[2] (SVM), informational",
            CheckStatus::Info, r.svm_cpuid_bit ? "SVM reported (backend not implemented)." : "SVM not reported.");
    if (r.leaf1_read) {
        Add(r, "cpuid.01h.ecx5.vmx", "CPUID.01H:ECX[5] (VMX)",
            r.vmx_cpuid_bit ? CheckStatus::Pass : CheckStatus::Fail,
            r.vmx_cpuid_bit ? "CPU reports VMX. This does not prove VMXON is permitted."
                            : "CPU does not report VMX (or it is masked by firmware/another hypervisor).");
        if (r.hypervisor_present_bit) {
            const std::string vendor = r.hypervisor_vendor_string.empty()
                ? std::string("unknown (leaf 0x40000000 unreadable)") : "\"" + r.hypervisor_vendor_string + "\"";
            Add(r, "cpuid.01h.ecx31.hypervisor", "CPUID.01H:ECX[31] (hypervisor present)", CheckStatus::Warn,
                "A hypervisor is present, vendor " + vendor +
                ". Reported CPU features may be virtualized; VMX ownership cannot be assumed.");
        } else {
            Add(r, "cpuid.01h.ecx31.hypervisor", "CPUID.01H:ECX[31] (hypervisor present)", CheckStatus::Pass,
                "No hypervisor reported via CPUID.");
        }
    }

    // --- Verdict.
    if (r.cpu_vendor != CpuVendor::Intel)
        Finish(r, PreflightVerdict::UnsupportedPlatform,
               "CPU vendor \"" + r.cpu_vendor_string + "\" has no implemented virtualization backend.");
    else if (!r.leaf1_read)
        Finish(r, PreflightVerdict::UnsupportedPlatform, "CPUID leaf 1 is not available on this CPU.");
    else if (!r.vmx_cpuid_bit)
        Finish(r, PreflightVerdict::VmxNotReported, "CPUID.01H:ECX[5] is clear: VMX is not reported.");
    else if (r.hypervisor_present_bit)
        Finish(r, PreflightVerdict::HypervisorConflict,
               "Another hypervisor is present; VMX availability must be established by kernel-mode checks.");
    else
        Finish(r, PreflightVerdict::PendingKernelVerification,
               "CPUID is consistent with VT-x. VMX is NOT confirmed ready: kernel-mode checks are still required.");
    return Result::Ok();
}

} // namespace hv
