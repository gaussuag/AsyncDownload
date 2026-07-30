#pragma once

#include "core/models.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace asyncdownload::recovery::detail {

struct MetadataEncodeResult {
    std::string value;
    std::error_code error;
};

struct MetadataDecodeResult {
    std::optional<core::MetadataState> state;
    std::error_code error;
};

[[nodiscard]] MetadataEncodeResult encode_metadata(
    const core::MetadataState& state) noexcept;

[[nodiscard]] MetadataDecodeResult decode_metadata(
    std::string_view value) noexcept;

}
