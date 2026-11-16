#pragma once
#include "hv/backend.hpp"
#include "hv/lifecycle.hpp"
#include <mutex>
#include <string>

namespace hv {

class Hypervisor final {
public:
    Hypervisor(IHostBackend& host, IGuestBackend& guest) : host_(host), guest_(guest) {}
    [[nodiscard]] Result Start();
    [[nodiscard]] Result Stop();
    [[nodiscard]] State GetState() const { return lifecycle_.GetState(); }
    [[nodiscard]] std::string GetLastError() const { return lifecycle_.GetLastError(); }
private:
    [[nodiscard]] Result Rollback();
    IHostBackend& host_;
    IGuestBackend& guest_;
    Lifecycle lifecycle_;
    mutable std::mutex operation_mutex_;
    bool host_initialized_{false};
    bool guest_started_{false};
};

} // namespace hv
