#include "metadata_codec.hpp"

#include "asyncdownload/error.hpp"

#include <cstdint>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace asyncdownload::recovery::detail {
namespace {

using nlohmann::json;

[[nodiscard]] json encode_range(
    const core::RangeStateSnapshot& range) {
    return {
        {"range_id", range.range_id},
        {"start_offset", range.start_offset},
        {"end_offset", range.end_offset},
        {"current_offset", range.current_offset},
        {"persisted_offset", range.persisted_offset},
        {"status", range.status}
    };
}

[[nodiscard]] core::RangeStateSnapshot decode_range(
    const json& value) {
    core::RangeStateSnapshot range{};
    range.range_id = value.value(
        "range_id",
        std::size_t{0});
    range.start_offset = value.value(
        "start_offset",
        std::int64_t{0});
    range.end_offset = value.value(
        "end_offset",
        std::int64_t{0});
    range.current_offset = value.value(
        "current_offset",
        std::int64_t{0});
    range.persisted_offset = value.value(
        "persisted_offset",
        std::int64_t{0});
    range.status = value.value(
        "status",
        std::uint8_t{0});
    return range;
}

[[nodiscard]] json encode_crc_sample(
    const core::BlockCrcSample& sample) {
    return {
        {"offset", sample.offset},
        {"crc32", sample.crc32},
        {"length", sample.length}
    };
}

[[nodiscard]] core::BlockCrcSample decode_crc_sample(
    const json& value) {
    core::BlockCrcSample sample{};
    sample.offset = value.value(
        "offset",
        std::int64_t{0});
    sample.crc32 = value.value(
        "crc32",
        std::uint32_t{0});
    sample.length = value.value(
        "length",
        std::size_t{0});
    return sample;
}

}

MetadataEncodeResult encode_metadata(
    const core::MetadataState& state) noexcept {
    MetadataEncodeResult result{};
    try {
        json value;
        value["url"] = state.url;
        value["output_path"] =
            state.output_path.string();
        value["temporary_path"] =
            state.temporary_path.string();
        value["total_size"] = state.total_size;
        value["vdl_offset"] = state.vdl_offset;
        value["accept_ranges"] = state.accept_ranges;
        value["resumed"] = state.resumed;
        value["etag"] = state.etag;
        value["last_modified"] = state.last_modified;
        value["block_size"] = state.block_size;
        value["io_alignment"] = state.io_alignment;
        value["bitmap_states"] = state.bitmap_states;
        value["ranges"] = json::array();
        for (const auto& range : state.ranges) {
            value["ranges"].push_back(
                encode_range(range));
        }
        value["crc_samples"] = json::array();
        for (const auto& sample : state.crc_samples) {
            value["crc_samples"].push_back(
                encode_crc_sample(sample));
        }
        result.value = value.dump(2);
    } catch (...) {
        result.error = make_error_code(
            DownloadErrc::metadata_save_failed);
    }
    return result;
}

MetadataDecodeResult decode_metadata(
    const std::string_view encoded) noexcept {
    MetadataDecodeResult result{};
    try {
        const auto value = json::parse(
            encoded,
            nullptr,
            false);
        if (value.is_discarded()) {
            result.error = make_error_code(
                DownloadErrc::metadata_parse_failed);
            return result;
        }

        core::MetadataState state{};
        state.url = value.value(
            "url",
            std::string{});
        state.output_path = value.value(
            "output_path",
            std::string{});
        state.temporary_path = value.value(
            "temporary_path",
            std::string{});
        state.total_size = value.value(
            "total_size",
            std::int64_t{0});
        state.vdl_offset = value.value(
            "vdl_offset",
            std::int64_t{0});
        state.accept_ranges = value.value(
            "accept_ranges",
            false);
        state.resumed = value.value(
            "resumed",
            false);
        state.etag = value.value(
            "etag",
            std::string{});
        state.last_modified = value.value(
            "last_modified",
            std::string{});
        state.block_size = value.value(
            "block_size",
            std::size_t{0});
        state.io_alignment = value.value(
            "io_alignment",
            std::size_t{0});
        state.bitmap_states = value.value(
            "bitmap_states",
            std::vector<std::uint8_t>{});

        const auto ranges = value.find("ranges");
        if (ranges != value.end() &&
            ranges->is_array()) {
            for (const auto& range : *ranges) {
                state.ranges.push_back(
                    decode_range(range));
            }
        }

        const auto samples =
            value.find("crc_samples");
        if (samples != value.end() &&
            samples->is_array()) {
            for (const auto& sample : *samples) {
                state.crc_samples.push_back(
                    decode_crc_sample(sample));
            }
        }

        result.state.emplace(std::move(state));
    } catch (...) {
        result.error = make_error_code(
            DownloadErrc::metadata_parse_failed);
    }
    return result;
}

}
