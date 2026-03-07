#include "metadata_store.hpp"

#include "asyncdownload/error.hpp"

#include <fstream>

#include <nlohmann/json.hpp>

namespace asyncdownload::metadata {
namespace {

using nlohmann::json;

[[nodiscard]] json to_json_value(const core::RangeStateSnapshot& range) {
    // Range 快照保存的是恢复时重新拼出调度状态所需的最小集合，
    // 不把运行期的临时对象细节直接序列化出去。
    return json{{"range_id", range.range_id},
                {"start_offset", range.start_offset},
                {"end_offset", range.end_offset},
                {"current_offset", range.current_offset},
                {"persisted_offset", range.persisted_offset},
                {"status", range.status}};
}

[[nodiscard]] core::RangeStateSnapshot to_range_snapshot(const json& value) {
    core::RangeStateSnapshot range{};
    range.range_id = value.value("range_id", static_cast<std::size_t>(0));
    range.start_offset = value.value("start_offset", static_cast<std::int64_t>(0));
    range.end_offset = value.value("end_offset", static_cast<std::int64_t>(0));
    range.current_offset = value.value("current_offset", static_cast<std::int64_t>(0));
    range.persisted_offset = value.value("persisted_offset", static_cast<std::int64_t>(0));
    range.status = value.value("status", static_cast<std::uint8_t>(0));
    return range;
}

[[nodiscard]] json to_json_value(const core::BlockCrcSample& sample) {
    // CRC 样本只覆盖恢复时需要复核的块，因此结构保持得尽量简单。
    return json{{"offset", sample.offset},
                {"crc32", sample.crc32},
                {"length", sample.length}};
}

[[nodiscard]] core::BlockCrcSample to_crc_sample(const json& value) {
    core::BlockCrcSample sample{};
    sample.offset = value.value("offset", static_cast<std::int64_t>(0));
    sample.crc32 = value.value("crc32", static_cast<std::uint32_t>(0));
    sample.length = value.value("length", static_cast<std::size_t>(0));
    return sample;
}

} // namespace

MetadataStore::MetadataStore(std::filesystem::path path)
    : path_(std::move(path)) {}

const std::filesystem::path& MetadataStore::path() const noexcept {
    return path_;
}

std::error_code MetadataStore::save(const core::MetadataState& state) noexcept {
    try {
        json value;
        // metadata 的目标不是做“运行时镜像”，而是做“恢复时足够用的描述文件”。
        // 因此这里只保存文件身份、位图、range 前沿和 CRC 样本。
        value["url"] = state.url;
        value["output_path"] = state.output_path.string();
        value["temporary_path"] = state.temporary_path.string();
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
            value["ranges"].push_back(to_json_value(range));
        }

        value["crc_samples"] = json::array();
        for (const auto& sample : state.crc_samples) {
            value["crc_samples"].push_back(to_json_value(sample));
        }

        std::error_code ec;
        const auto parent = path_.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, ec);
            if (ec) {
                return make_error_code(DownloadErrc::metadata_save_failed);
            }
        }

        // 保存时先写入临时文件，再原子替换正式 metadata。
        // 这样即使进程在写文件中途崩掉，也最多留下 .tmp，不会破坏上一个可恢复版本。
        const auto tmp_path = path_.string() + ".tmp";
        std::ofstream stream(tmp_path, std::ios::binary | std::ios::trunc);
        if (!stream.is_open()) {
            return make_error_code(DownloadErrc::metadata_save_failed);
        }

        stream << value.dump(2);
        stream.close();

        std::filesystem::remove(path_, ec);
        ec.clear();
        std::filesystem::rename(tmp_path, path_, ec);
        if (ec) {
            std::filesystem::remove(tmp_path, ec);
            return make_error_code(DownloadErrc::metadata_save_failed);
        }
    } catch (...) {
        return make_error_code(DownloadErrc::metadata_save_failed);
    }

    return {};
}

std::pair<std::error_code, std::optional<core::MetadataState>> MetadataStore::load() const noexcept {
    std::error_code ec;
    if (!std::filesystem::exists(path_, ec) || ec) {
        // 没有 metadata 不算错误，这只是说明当前没有可恢复状态。
        return {{}, std::nullopt};
    }

    try {
        std::ifstream stream(path_, std::ios::binary);
        if (!stream.is_open()) {
            return {make_error_code(DownloadErrc::metadata_parse_failed), std::nullopt};
        }

        const auto value = json::parse(stream, nullptr, false);
        if (value.is_discarded()) {
            return {make_error_code(DownloadErrc::metadata_parse_failed), std::nullopt};
        }

        // 读取时尽量宽容：字段缺失就退回默认值，把“兼容旧版本 metadata”这件事
        // 留在解析层解决，而不是让上层到处做空值判断。
        core::MetadataState state{};
        state.url = value.value("url", std::string{});
        state.output_path = value.value("output_path", std::string{});
        state.temporary_path = value.value("temporary_path", std::string{});
        state.total_size = value.value("total_size", static_cast<std::int64_t>(0));
        state.vdl_offset = value.value("vdl_offset", static_cast<std::int64_t>(0));
        state.accept_ranges = value.value("accept_ranges", false);
        state.resumed = value.value("resumed", false);
        state.etag = value.value("etag", std::string{});
        state.last_modified = value.value("last_modified", std::string{});
        state.block_size = value.value("block_size", static_cast<std::size_t>(0));
        state.io_alignment = value.value("io_alignment", static_cast<std::size_t>(0));
        state.bitmap_states = value.value("bitmap_states", std::vector<std::uint8_t>{});

        if (const auto ranges_it = value.find("ranges");
            ranges_it != value.end() && ranges_it->is_array()) {
            for (const auto& item : *ranges_it) {
                state.ranges.push_back(to_range_snapshot(item));
            }
        }

        if (const auto samples_it = value.find("crc_samples");
            samples_it != value.end() && samples_it->is_array()) {
            for (const auto& item : *samples_it) {
                state.crc_samples.push_back(to_crc_sample(item));
            }
        }

        return {{}, std::move(state)};
    } catch (...) {
        return {make_error_code(DownloadErrc::metadata_parse_failed), std::nullopt};
    }
}

std::error_code MetadataStore::remove() noexcept {
    std::error_code ec;
    // 删除恢复元数据是一个尽力而为的清理动作，不把“不存在”或删除失败提升成
    // 主流程错误，避免影响已经成功完成的下载结果。
    std::filesystem::remove(path_, ec);
    return {};
}

} // namespace asyncdownload::metadata
