#pragma once

#include "hv/platform/driver_protocol.h"
#include <cstddef>
#include <cstdint>

namespace hv::platform::protocol {

enum class QueryRequestValidation { Valid, VersionMismatch, InvalidStructure };

[[nodiscard]] inline QueryRequestValidation ValidateQueryRequest(
    const GERADUO_QUERY_REQUEST& request, std::size_t input_size) noexcept {
    if (request.protocol_version != GERADUO_DRIVER_PROTOCOL_VERSION)
        return QueryRequestValidation::VersionMismatch;
    if (input_size != sizeof(GERADUO_QUERY_REQUEST) ||
        request.structure_size != sizeof(GERADUO_QUERY_REQUEST) || request.request_id == 0) {
        return QueryRequestValidation::InvalidStructure;
    }
    return QueryRequestValidation::Valid;
}

} // namespace hv::platform::protocol