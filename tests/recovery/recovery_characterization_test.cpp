#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <system_error>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "asyncdownload/error.hpp"
#include "metadata/metadata_store.hpp"

namespace {

class TempDirectory {
public:
    explicit TempDirectory(const std::string& label) {
        static std::atomic<std::uint64_t> next_id{0};
        path_ = std::filesystem::temp_directory_path() /
            (label + "_" +
             std::to_string(next_id.fetch_add(
                 1,
                 std::memory_order_relaxed)));
        std::error_code ec;
        const auto removed =
            std::filesystem::remove_all(path_, ec);
        static_cast<void>(removed);
        ec.clear();
        std::filesystem::create_directories(path_, ec);
        EXPECT_FALSE(ec);
    }

    ~TempDirectory() {
        std::error_code ec;
        const auto removed =
            std::filesystem::remove_all(path_, ec);
        static_cast<void>(removed);
    }

    [[nodiscard]] const std::filesystem::path& path()
        const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void write_text(
    const std::filesystem::path& path,
    const std::string& value) {
    std::ofstream stream(
        path,
        std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(stream.is_open());
    stream << value;
    stream.close();
    ASSERT_TRUE(stream.good());
}

[[nodiscard]] std::string read_text(
    const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    EXPECT_TRUE(stream.is_open());
    return {
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()
    };
}

[[nodiscard]] asyncdownload::core::MetadataState
full_legacy_state(const std::filesystem::path& root) {
    asyncdownload::core::MetadataState state{};
    state.url = "https://example.com/artifact.bin";
    state.output_path = root / "artifact.bin";
    state.temporary_path = root / "artifact.bin.part";
    state.total_size = 131072;
    state.vdl_offset = 65536;
    state.accept_ranges = true;
    state.resumed = true;
    state.etag = "legacy-etag";
    state.last_modified =
        "Mon, 09 Mar 2026 08:50:10 GMT";
    state.block_size = 65536;
    state.io_alignment = 4096;
    state.bitmap_states = {2, 1};
    state.ranges.push_back({
        7,
        0,
        131071,
        98304,
        65536,
        3
    });
    state.crc_samples.push_back({
        65536,
        305419896U,
        65536
    });
    return state;
}

void expect_full_legacy_state(
    const asyncdownload::core::MetadataState& state,
    const std::filesystem::path& root) {
    EXPECT_EQ(
        state.url,
        "https://example.com/artifact.bin");
    EXPECT_EQ(state.output_path, root / "artifact.bin");
    EXPECT_EQ(
        state.temporary_path,
        root / "artifact.bin.part");
    EXPECT_EQ(state.total_size, 131072);
    EXPECT_EQ(state.vdl_offset, 65536);
    EXPECT_TRUE(state.accept_ranges);
    EXPECT_TRUE(state.resumed);
    EXPECT_EQ(state.etag, "legacy-etag");
    EXPECT_EQ(
        state.last_modified,
        "Mon, 09 Mar 2026 08:50:10 GMT");
    EXPECT_EQ(state.block_size, 65536U);
    EXPECT_EQ(state.io_alignment, 4096U);
    EXPECT_EQ(
        state.bitmap_states,
        (std::vector<std::uint8_t>{2, 1}));
    ASSERT_EQ(state.ranges.size(), 1U);
    EXPECT_EQ(state.ranges[0].range_id, 7U);
    EXPECT_EQ(state.ranges[0].start_offset, 0);
    EXPECT_EQ(state.ranges[0].end_offset, 131071);
    EXPECT_EQ(state.ranges[0].current_offset, 98304);
    EXPECT_EQ(state.ranges[0].persisted_offset, 65536);
    EXPECT_EQ(state.ranges[0].status, 3U);
    ASSERT_EQ(state.crc_samples.size(), 1U);
    EXPECT_EQ(state.crc_samples[0].offset, 65536);
    EXPECT_EQ(
        state.crc_samples[0].crc32,
        305419896U);
    EXPECT_EQ(state.crc_samples[0].length, 65536U);
}

}

TEST(
    RecoveryCharacterizationTest,
    LoadsCurrentPrettyPrintedMetadataFixture) {
    TempDirectory temp(
        "asyncdownload_legacy_fixture");
    const auto metadata_path =
        temp.path() / "artifact.bin.config.json";
    const auto fixture_path =
        std::filesystem::path(__FILE__).parent_path() /
        "fixtures" / "legacy_checkpoint.json";
    std::error_code ec;
    std::filesystem::copy_file(
        fixture_path,
        metadata_path,
        std::filesystem::copy_options::overwrite_existing,
        ec);
    ASSERT_FALSE(ec);
    asyncdownload::metadata::MetadataStore store(
        metadata_path);

    const auto [error, loaded] = store.load();

    ASSERT_FALSE(error);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(
        loaded->url,
        "https://example.com/artifact.bin");
    EXPECT_EQ(
        loaded->output_path.generic_string(),
        "downloads/artifact.bin");
    EXPECT_EQ(
        loaded->temporary_path.generic_string(),
        "downloads/artifact.bin.part");
    EXPECT_EQ(loaded->total_size, 131072);
    EXPECT_EQ(loaded->vdl_offset, 65536);
    EXPECT_TRUE(loaded->accept_ranges);
    EXPECT_TRUE(loaded->resumed);
    EXPECT_EQ(loaded->bitmap_states.size(), 2U);
    ASSERT_EQ(loaded->ranges.size(), 1U);
    EXPECT_EQ(loaded->ranges[0].persisted_offset, 65536);
    ASSERT_EQ(loaded->crc_samples.size(), 1U);
    EXPECT_EQ(
        loaded->crc_samples[0].crc32,
        305419896U);
}

TEST(
    RecoveryCharacterizationTest,
    RoundTripsEveryLegacyFieldWithoutAddingVersion) {
    TempDirectory temp(
        "asyncdownload_legacy_roundtrip");
    const auto metadata_path =
        temp.path() / "artifact.bin.config.json";
    asyncdownload::metadata::MetadataStore store(
        metadata_path);
    const auto original = full_legacy_state(temp.path());

    ASSERT_FALSE(store.save(original));
    const auto text = read_text(metadata_path);
    const auto json = nlohmann::json::parse(text);
    const std::set<std::string> expected_keys{
        "accept_ranges",
        "bitmap_states",
        "block_size",
        "crc_samples",
        "etag",
        "io_alignment",
        "last_modified",
        "output_path",
        "ranges",
        "resumed",
        "temporary_path",
        "total_size",
        "url",
        "vdl_offset"
    };
    std::set<std::string> actual_keys;
    for (const auto& [key, value] : json.items()) {
        static_cast<void>(value);
        actual_keys.insert(key);
    }
    EXPECT_EQ(actual_keys, expected_keys);
    EXPECT_EQ(json.find("version"), json.end());
    EXPECT_NE(text.find("\n  \"url\""), std::string::npos);

    const auto [error, loaded] = store.load();
    ASSERT_FALSE(error);
    ASSERT_TRUE(loaded.has_value());
    expect_full_legacy_state(*loaded, temp.path());
}

TEST(
    RecoveryCharacterizationTest,
    IgnoresUnknownFieldsAndMissingArrays) {
    TempDirectory temp(
        "asyncdownload_legacy_unknown");
    const auto metadata_path =
        temp.path() / "artifact.bin.config.json";
    write_text(
        metadata_path,
        "{\n"
        "  \"url\": \"https://example.com/file.bin\",\n"
        "  \"total_size\": 4096,\n"
        "  \"unknown\": {\"nested\": true}\n"
        "}");
    asyncdownload::metadata::MetadataStore store(
        metadata_path);

    const auto [error, loaded] = store.load();

    ASSERT_FALSE(error);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(
        loaded->url,
        "https://example.com/file.bin");
    EXPECT_EQ(loaded->total_size, 4096);
    EXPECT_TRUE(loaded->bitmap_states.empty());
    EXPECT_TRUE(loaded->ranges.empty());
    EXPECT_TRUE(loaded->crc_samples.empty());
}

TEST(
    RecoveryCharacterizationTest,
    TreatsNonArrayLegacyCollectionsAsEmpty) {
    TempDirectory temp(
        "asyncdownload_legacy_nonarrays");
    const auto metadata_path =
        temp.path() / "artifact.bin.config.json";
    write_text(
        metadata_path,
        "{\n"
        "  \"ranges\": {},\n"
        "  \"crc_samples\": null\n"
        "}");
    asyncdownload::metadata::MetadataStore store(
        metadata_path);

    const auto [error, loaded] = store.load();

    ASSERT_FALSE(error);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_TRUE(loaded->ranges.empty());
    EXPECT_TRUE(loaded->crc_samples.empty());
}

TEST(
    RecoveryCharacterizationTest,
    ReplacesMetadataAndLeavesNoTemporaryFile) {
    TempDirectory temp(
        "asyncdownload_legacy_replace");
    const auto metadata_path =
        temp.path() / "artifact.bin.config.json";
    const auto temporary_path =
        std::filesystem::path(
            metadata_path.string() + ".tmp");
    asyncdownload::metadata::MetadataStore store(
        metadata_path);
    auto first = full_legacy_state(temp.path());
    auto second = first;
    second.vdl_offset = second.total_size;

    ASSERT_FALSE(store.save(first));
    ASSERT_FALSE(store.save(second));

    EXPECT_TRUE(std::filesystem::exists(metadata_path));
    EXPECT_FALSE(std::filesystem::exists(temporary_path));
    const auto [error, loaded] = store.load();
    ASSERT_FALSE(error);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->vdl_offset, second.total_size);
}

TEST(
    RecoveryCharacterizationTest,
    IgnoresOrphanTemporaryMetadata) {
    TempDirectory temp(
        "asyncdownload_orphan_tmp");
    const auto metadata_path =
        temp.path() / "artifact.bin.config.json";
    write_text(
        std::filesystem::path(
            metadata_path.string() + ".tmp"),
        "{\"total_size\": 4096}");
    asyncdownload::metadata::MetadataStore store(
        metadata_path);

    const auto [error, loaded] = store.load();

    EXPECT_FALSE(error);
    EXPECT_FALSE(loaded.has_value());
}

TEST(
    RecoveryCharacterizationTest,
    CorruptFormalMetadataReturnsParseFailureUnchanged) {
    TempDirectory temp(
        "asyncdownload_corrupt_metadata");
    const auto metadata_path =
        temp.path() / "artifact.bin.config.json";
    const std::string corrupt = "{not-json";
    write_text(metadata_path, corrupt);
    asyncdownload::metadata::MetadataStore store(
        metadata_path);

    const auto [error, loaded] = store.load();

    EXPECT_EQ(
        error,
        asyncdownload::make_error_code(
            asyncdownload::DownloadErrc::
                metadata_parse_failed));
    EXPECT_FALSE(loaded.has_value());
    EXPECT_EQ(read_text(metadata_path), corrupt);
}

TEST(
    RecoveryCharacterizationTest,
    CurrentRemovalSuppressesFilesystemFailure) {
    TempDirectory temp(
        "asyncdownload_hidden_remove_error");
    const auto metadata_path =
        temp.path() / "artifact.bin.config.json";
    std::error_code ec;
    std::filesystem::create_directories(
        metadata_path,
        ec);
    ASSERT_FALSE(ec);
    write_text(metadata_path / "child", "occupied");
    asyncdownload::metadata::MetadataStore store(
        metadata_path);

    const auto error = store.remove();

    EXPECT_FALSE(error);
    EXPECT_TRUE(std::filesystem::exists(metadata_path));
}

TEST(
    RecoveryExpectedRedTest,
    DISABLED_ReportsMetadataRemovalFailure) {
    TempDirectory temp(
        "asyncdownload_remove_error_red");
    const auto metadata_path =
        temp.path() / "artifact.bin.config.json";
    std::error_code ec;
    std::filesystem::create_directories(
        metadata_path,
        ec);
    ASSERT_FALSE(ec);
    write_text(metadata_path / "child", "occupied");
    asyncdownload::metadata::MetadataStore store(
        metadata_path);

    EXPECT_TRUE(store.remove());
    EXPECT_TRUE(std::filesystem::exists(metadata_path));
}
