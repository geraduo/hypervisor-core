#pragma once

namespace hv {

// Shared status vocabulary for user-mode platform diagnostics. It is intentionally
// independent of the Win32 API so reports can be produced and tested everywhere.
enum class CheckStatus { Pass, Fail, Warn, Info, NotChecked };

[[nodiscard]] const char* ToString(CheckStatus value) noexcept;

} // namespace hv
