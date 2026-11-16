#include "hv/platform/cpu_capability_manager.hpp"
#include <array>
#include <cctype>

namespace hv {
namespace {

constexpr std::uint32_t kExtendedLeaf0 = 0x80000000u;
constexpr std::uint32_t kBrandLeafFirst = 0x80000002u;
constexpr std::uint32_t kBrandLeafLast = 0x80000004u;
constexpr std::uint32_t kMaxExtendedLeaf = 0x8FFFFFFFu;

bool IsOptionalFailure(Status status) {
    return status == Status::NotFound || status == Status::Unsupported;
}

void AppendRegister(std::string& text, std::uint32_t value) {
    for (unsigned int shift = 0; shift < 32; shift += 8) {
        const auto character = static_cast<unsigned char>((value >> shift) & 0xFFu);
        if (character == 0) {
            text.push_back(' ');
        } else if (std::isprint(character) != 0) {
            text.push_back(static_cast<char>(character));
        } else {
            text.push_back(' ');
        }
    }
}

std::string CleanBrandString(std::string text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    text = text.substr(first, last - first + 1);
    for (char& character : text) {
        const auto value = static_cast<unsigned char>(character);
        if (value < 0x20 || value > 0x7E) character = '?';
    }
    return text;
}

} // namespace

Result CpuCapabilityManager::Query(CpuCapabilityReport& out) {
    out = CpuCapabilityReport{};
    const auto preflight_result = RunPlatformPreflight(cpuid_, out.preflight);
    if (!preflight_result.Succeeded()) {
        out.manufacturer = out.preflight.cpu_vendor_string;
        out.model_detail = preflight_result.message;
        return preflight_result;
    }

    out.manufacturer = out.preflight.cpu_vendor_string;
    CpuidRegisters maximum{};
    const auto maximum_result = cpuid_.Query(kExtendedLeaf0, 0, maximum);
    if (!maximum_result.Succeeded()) {
        out.model_detail = IsOptionalFailure(maximum_result.status)
                               ? "Extended CPUID brand leaves are unavailable."
                               : "Could not read the maximum extended CPUID leaf: " + maximum_result.message;
        return Result::Ok();
    }
    if (maximum.eax < kBrandLeafLast || maximum.eax > kMaxExtendedLeaf) {
        out.model_detail = "The advertised extended CPUID range does not include the CPU brand string.";
        return Result::Ok();
    }

    std::array<CpuidRegisters, 3> brand_registers{};
    for (std::uint32_t leaf = kBrandLeafFirst; leaf <= kBrandLeafLast; ++leaf) {
        CpuidRegisters registers{};
        const auto result = cpuid_.Query(leaf, 0, registers);
        if (!result.Succeeded()) {
            out.model_detail = "Could not read CPU brand leaf " + std::to_string(leaf) + ": " + result.message;
            return Result::Ok();
        }
        brand_registers[leaf - kBrandLeafFirst] = registers;
    }

    std::string brand;
    brand.reserve(48);
    for (const auto& registers : brand_registers) {
        AppendRegister(brand, registers.eax);
        AppendRegister(brand, registers.ebx);
        AppendRegister(brand, registers.ecx);
        AppendRegister(brand, registers.edx);
    }
    out.model = CleanBrandString(std::move(brand));
    out.model_string_read = !out.model.empty();
    out.model_detail = out.model_string_read ? "CPU brand read from CPUID leaves 0x80000002-0x80000004."
                                            : "The CPU brand leaves returned an empty string.";
    return Result::Ok();
}

} // namespace hv