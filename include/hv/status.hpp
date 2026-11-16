#pragma once
#include <string>
#include <utility>

namespace hv {

enum class Status {
    Success,
    InvalidArgument,
    InvalidState,
    AlreadyMapped,
    NotMapped,
    NotFound,
    Timeout,
    AccessDenied,
    InitializationFailed,
    ShutdownFailed,
    InternalError,
    Unsupported
};

struct Result {
    Status status{Status::Success};
    std::string message{};

    [[nodiscard]] bool Succeeded() const noexcept {
        return status == Status::Success;
    }

    static Result Ok() { return {}; }

    static Result Error(Status code, std::string text) {
        return {code, std::move(text)};
    }
};

} // namespace hv
