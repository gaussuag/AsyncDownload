#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "download/download_policy.hpp"
#include "recovery/metadata_codec.hpp"
#include "recovery/recovery_checkpoint.hpp"

namespace {

class RecoveryTempDirectory {
public:
    explicit RecoveryTempDirectory(
        const std::string& label) {
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

    ~RecoveryTempDirectory() {
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

[[nodiscard]] asyncdownload::recovery::
RecoveryOpenRequest fresh_request(
    const std::filesystem::path& root) {
    asyncdownload::recovery::RecoveryOpenRequest request{};
    request.paths.output_path = root / "artifact.bin";
    request.paths.temporary_path =
        root / "artifact.bin.part";
    request.paths.metadata_path =
        root / "artifact.bin.config.json";
    request.remote.url =
        "https://example.com/artifact.bin";
    request.remote.total_size = 8192;
    request.remote.accept_ranges = true;
    request.policy.block_bytes = 4096;
    request.policy.io_alignment_bytes = 4096;
    request.policy.allow_sparse_resume = true;
    request.overwrite_existing = true;
    return request;
}

}

TEST(
    RecoveryMetadataCodecTest,
    PreservesLegacyShapeAndPrettyFormatting) {
    asyncdownload::core::MetadataState state{};
    state.url = "https://example.com/artifact.bin";
    state.output_path = "artifact.bin";
    state.temporary_path = "artifact.bin.part";
    state.total_size = 8192;
    state.vdl_offset = 4096;
    state.accept_ranges = true;
    state.resumed = true;
    state.etag = "etag";
    state.last_modified = "last-modified";
    state.block_size = 4096;
    state.io_alignment = 4096;
    state.bitmap_states = {2, 0};
    state.ranges.push_back({
        4,
        0,
        8191,
        4096,
        4096,
        1
    });
    state.crc_samples.push_back({
        4096,
        0x12345678U,
        4096
    });

    const auto encoded =
        asyncdownload::recovery::detail::
            encode_metadata(state);
    ASSERT_FALSE(encoded.error);
    EXPECT_NE(
        encoded.value.find("\n  \"url\""),
        std::string::npos);
    const auto json =
        nlohmann::json::parse(encoded.value);
    EXPECT_EQ(json.size(), 14U);
    EXPECT_EQ(json.find("version"), json.end());

    const auto decoded =
        asyncdownload::recovery::detail::
            decode_metadata(encoded.value);
    ASSERT_FALSE(decoded.error);
    ASSERT_TRUE(decoded.state.has_value());
    EXPECT_EQ(decoded.state->url, state.url);
    EXPECT_EQ(
        decoded.state->bitmap_states,
        state.bitmap_states);
    ASSERT_EQ(decoded.state->ranges.size(), 1U);
    EXPECT_EQ(
        decoded.state->ranges[0].persisted_offset,
        4096);
    ASSERT_EQ(decoded.state->crc_samples.size(), 1U);
    EXPECT_EQ(
        decoded.state->crc_samples[0].crc32,
        0x12345678U);
}

TEST(
    RecoveryCheckpointTest,
    OpensFreshAndOwnsPreallocatedPartFile) {
    RecoveryTempDirectory temp(
        "asyncdownload_recovery_fresh");
    const auto request = fresh_request(temp.path());

    auto opened =
        asyncdownload::recovery::RecoveryCheckpoint::open(
            request);

    ASSERT_FALSE(opened.error);
    ASSERT_NE(opened.checkpoint, nullptr);
    EXPECT_EQ(
        opened.restored.disposition,
        asyncdownload::recovery::
            RecoveryDisposition::fresh);
    EXPECT_EQ(
        opened.restored.bitmap_states,
        (std::vector<std::uint8_t>{0, 0}));
    EXPECT_EQ(opened.restored.trusted_bytes, 0);
    EXPECT_EQ(opened.restored.safe_vdl, 0);
    EXPECT_EQ(opened.restored.completed_ranges, 0U);
    EXPECT_EQ(
        opened.checkpoint->paths().temporary_path,
        request.paths.temporary_path);
    EXPECT_TRUE(std::filesystem::exists(
        request.paths.temporary_path));
    EXPECT_EQ(
        std::filesystem::file_size(
            request.paths.temporary_path),
        8192U);

    opened.checkpoint->close_preserving_artifacts();
    EXPECT_TRUE(std::filesystem::exists(
        request.paths.temporary_path));
    EXPECT_FALSE(std::filesystem::exists(
        request.paths.metadata_path));
}

TEST(
    RecoveryCheckpointTest,
    WritesThroughItsPrivatePartOwner) {
    RecoveryTempDirectory temp(
        "asyncdownload_recovery_write");
    const auto request = fresh_request(temp.path());
    auto opened =
        asyncdownload::recovery::RecoveryCheckpoint::open(
            request);
    ASSERT_FALSE(opened.error);
    ASSERT_NE(opened.checkpoint, nullptr);
    const std::vector<std::uint8_t> payload{
        0x11,
        0x22,
        0x33,
        0x44
    };

    ASSERT_FALSE(opened.checkpoint->write(
        4096,
        std::span<const std::uint8_t>(payload)));
    opened.checkpoint->close_preserving_artifacts();

    std::ifstream stream(
        request.paths.temporary_path,
        std::ios::binary);
    ASSERT_TRUE(stream.is_open());
    stream.seekg(4096);
    std::vector<std::uint8_t> actual(payload.size());
    stream.read(
        reinterpret_cast<char*>(actual.data()),
        static_cast<std::streamsize>(actual.size()));
    EXPECT_EQ(actual, payload);
}
