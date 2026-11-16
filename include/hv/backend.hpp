#pragma once
#include "hv/status.hpp"

namespace hv {

class IHostBackend {
public:
    virtual ~IHostBackend() = default;
    // A backend must release partially acquired resources before returning failure.
    [[nodiscard]] virtual Result Initialize() = 0;
    [[nodiscard]] virtual Result Shutdown() = 0;
};

class IGuestBackend {
public:
    virtual ~IGuestBackend() = default;
    // Contract for the synthetic guest test session; not a guest OS boot implementation.
    [[nodiscard]] virtual Result StartTestSession() = 0;
    [[nodiscard]] virtual Result StopTestSession() = 0;
};

} // namespace hv
