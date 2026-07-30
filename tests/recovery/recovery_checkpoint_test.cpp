#include <atomic>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "asyncdownload/error.hpp"
#include "core/crc32.hpp"
#include "download/download_policy.hpp"
#include "metadata/metadata_store.hpp"
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

[[nodiscard]] std::vector<std::uint8_t>
part_contents() {
    std::vector<std::uint8_t> bytes(8192);
    for (std::size_t index = 0;
         index < bytes.size();
         ++index) {
        bytes[index] = static_cast<std::uint8_t>(
            index % 251);
    }
    return bytes;
}

void write_part(
    const std::filesystem::path& path,
    const std::vector<std::uint8_t>& bytes) {
    std::ofstream stream(
        path,
        std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(stream.is_open());
    stream.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    stream.close();
    ASSERT_TRUE(stream.good());
}

[[nodiscard]] asyncdownload::core::MetadataState
candidate_state(
    const asyncdownload::recovery::
        RecoveryOpenRequest& request) {
    asyncdownload::core::MetadataState state{};
    state.url = request.remote.url;
    state.output_path = request.paths.output_path;
    state.temporary_path =
        request.paths.temporary_path;
    state.total_size = request.remote.total_size;
    state.vdl_offset = 4096;
    state.accept_ranges =
        request.remote.accept_ranges;
    state.resumed = true;
    state.etag = request.remote.etag;
    state.last_modified =
        request.remote.last_modified;
    state.block_size = request.policy.block_bytes;
    state.io_alignment =
        request.policy.io_alignment_bytes;
    state.bitmap_states = {2, 0};
    return state;
}

void save_candidate(
    const asyncdownload::recovery::
        RecoveryOpenRequest& request,
    const asyncdownload::core::MetadataState& state,
    const std::vector<std::uint8_t>& bytes) {
    write_part(request.paths.temporary_path, bytes);
    asyncdownload::metadata::MetadataStore store(
        request.paths.metadata_path);
    ASSERT_FALSE(store.save(state));
}

[[nodiscard]] std::vector<std::uint8_t>
read_file(
    const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    EXPECT_TRUE(stream.is_open());
    return {
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()
    };
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

TEST(
    RecoveryCheckpointTest,
    RestoresShortBitmapAndResetsDownloading) {
    RecoveryTempDirectory temp(
        "asyncdownload_recovery_short_bitmap");
    auto request = fresh_request(temp.path());
    request.remote.etag = "remote-etag";
    request.remote.last_modified = "remote-date";
    auto state = candidate_state(request);
    state.etag.clear();
    state.last_modified.clear();
    state.accept_ranges = false;
    state.resumed = false;
    state.bitmap_states = {1};
    state.vdl_offset = 0;
    save_candidate(
        request,
        state,
        part_contents());

    auto opened =
        asyncdownload::recovery::RecoveryCheckpoint::open(
            request);

    ASSERT_FALSE(opened.error);
    ASSERT_NE(opened.checkpoint, nullptr);
    EXPECT_EQ(
        opened.restored.disposition,
        asyncdownload::recovery::
            RecoveryDisposition::resumed);
    EXPECT_EQ(
        opened.restored.bitmap_states,
        (std::vector<std::uint8_t>{0, 0}));
    EXPECT_EQ(opened.restored.trusted_bytes, 0);
    EXPECT_EQ(opened.restored.safe_vdl, 0);
}

TEST(
    RecoveryCheckpointTest,
    RequiresExactCoreIdentityAndRejectsConflicts) {
    RecoveryTempDirectory temp(
        "asyncdownload_recovery_identity_matrix");
    constexpr std::array<std::string_view, 8>
        cases{
            "url",
            "output",
            "temporary",
            "size",
            "block",
            "alignment",
            "etag",
            "last_modified"
        };

    for (std::size_t index = 0;
         index < cases.size();
         ++index) {
        SCOPED_TRACE(cases[index]);
        const auto root =
            temp.path() /
            std::string(cases[index]);
        std::error_code ec;
        std::filesystem::create_directories(root, ec);
        ASSERT_FALSE(ec);
        auto request = fresh_request(root);
        auto state = candidate_state(request);
        switch (index) {
        case 0:
            state.url =
                "https://example.com/other.bin";
            break;
        case 1:
            state.output_path =
                root / "other.bin";
            break;
        case 2:
            state.temporary_path =
                root / "other.bin.part";
            break;
        case 3:
            state.total_size = 4096;
            break;
        case 4:
            state.block_size = 2048;
            break;
        case 5:
            state.io_alignment = 2048;
            break;
        case 6:
            request.remote.etag = "remote-etag";
            state.etag = "other-etag";
            break;
        case 7:
            request.remote.last_modified =
                "remote-date";
            state.last_modified = "other-date";
            break;
        default:
            FAIL();
        }
        save_candidate(
            request,
            state,
            part_contents());

        auto opened =
            asyncdownload::recovery::
                RecoveryCheckpoint::open(request);

        ASSERT_FALSE(opened.error);
        ASSERT_NE(opened.checkpoint, nullptr);
        EXPECT_EQ(
            opened.restored.disposition,
            asyncdownload::recovery::
                RecoveryDisposition::fresh);
    }
}

TEST(
    RecoveryCheckpointTest,
    ProjectsLegacyPersistedOffsetsWithoutStatus) {
    RecoveryTempDirectory temp(
        "asyncdownload_recovery_projection");
    const auto request = fresh_request(temp.path());
    auto state = candidate_state(request);
    state.bitmap_states = {0};
    state.vdl_offset = 4096;
    state.ranges.push_back({
        3,
        0,
        8191,
        8192,
        4096,
        4
    });
    save_candidate(
        request,
        state,
        part_contents());

    auto opened =
        asyncdownload::recovery::RecoveryCheckpoint::open(
            request);

    ASSERT_FALSE(opened.error);
    ASSERT_NE(opened.checkpoint, nullptr);
    EXPECT_EQ(
        opened.restored.bitmap_states,
        (std::vector<std::uint8_t>{2, 0}));
    EXPECT_EQ(opened.restored.trusted_bytes, 4096);
    EXPECT_EQ(opened.restored.safe_vdl, 4096);
}

TEST(
    RecoveryCheckpointTest,
    MissingCrcBeyondVdlRollsBackOnlyThatBlock) {
    RecoveryTempDirectory temp(
        "asyncdownload_recovery_missing_crc");
    const auto request = fresh_request(temp.path());
    auto state = candidate_state(request);
    state.bitmap_states = {2, 2};
    save_candidate(
        request,
        state,
        part_contents());

    auto opened =
        asyncdownload::recovery::RecoveryCheckpoint::open(
            request);

    ASSERT_FALSE(opened.error);
    ASSERT_NE(opened.checkpoint, nullptr);
    EXPECT_EQ(
        opened.restored.bitmap_states,
        (std::vector<std::uint8_t>{2, 0}));
    EXPECT_EQ(opened.restored.trusted_bytes, 4096);
    EXPECT_EQ(opened.restored.safe_vdl, 4096);
}

TEST(
    RecoveryCheckpointTest,
    MismatchedCrcBeyondVdlRollsBackOnlyThatBlock) {
    RecoveryTempDirectory temp(
        "asyncdownload_recovery_bad_crc");
    const auto request = fresh_request(temp.path());
    auto state = candidate_state(request);
    state.bitmap_states = {2, 2};
    state.crc_samples.push_back({
        4096,
        0,
        4096
    });
    save_candidate(
        request,
        state,
        part_contents());

    auto opened =
        asyncdownload::recovery::RecoveryCheckpoint::open(
            request);

    ASSERT_FALSE(opened.error);
    ASSERT_NE(opened.checkpoint, nullptr);
    EXPECT_EQ(
        opened.restored.bitmap_states,
        (std::vector<std::uint8_t>{2, 0}));
}

TEST(
    RecoveryCheckpointTest,
    ValidCrcCanProduceCompleteCheckpoint) {
    RecoveryTempDirectory temp(
        "asyncdownload_recovery_valid_crc");
    const auto request = fresh_request(temp.path());
    const auto bytes = part_contents();
    auto state = candidate_state(request);
    state.bitmap_states = {2, 2};
    const auto tail = std::span<const std::uint8_t>(
        bytes.data() + 4096,
        4096);
    state.crc_samples.push_back({
        4096,
        asyncdownload::core::crc32(
            std::as_bytes(tail)),
        4096
    });
    save_candidate(request, state, bytes);

    auto opened =
        asyncdownload::recovery::RecoveryCheckpoint::open(
            request);

    ASSERT_FALSE(opened.error);
    ASSERT_NE(opened.checkpoint, nullptr);
    EXPECT_EQ(
        opened.restored.disposition,
        asyncdownload::recovery::
            RecoveryDisposition::complete);
    EXPECT_EQ(
        opened.restored.bitmap_states,
        (std::vector<std::uint8_t>{2, 2}));
    EXPECT_EQ(opened.restored.trusted_bytes, 8192);
    EXPECT_EQ(opened.restored.safe_vdl, 8192);
    EXPECT_EQ(opened.restored.completed_ranges, 2U);
}

TEST(
    RecoveryCheckpointTest,
    ConflictingIdentityRestartsFresh) {
    RecoveryTempDirectory temp(
        "asyncdownload_recovery_identity");
    const auto request = fresh_request(temp.path());
    auto state = candidate_state(request);
    state.url = "https://example.com/other.bin";
    save_candidate(
        request,
        state,
        part_contents());

    auto opened =
        asyncdownload::recovery::RecoveryCheckpoint::open(
            request);

    ASSERT_FALSE(opened.error);
    ASSERT_NE(opened.checkpoint, nullptr);
    EXPECT_EQ(
        opened.restored.disposition,
        asyncdownload::recovery::
            RecoveryDisposition::fresh);
    EXPECT_EQ(opened.restored.trusted_bytes, 0);
    EXPECT_FALSE(std::filesystem::exists(
        request.paths.metadata_path));
}

TEST(
    RecoveryCheckpointTest,
    CrcReadFailurePreservesCandidateArtifacts) {
    RecoveryTempDirectory temp(
        "asyncdownload_recovery_crc_read");
    const auto request = fresh_request(temp.path());
    auto state = candidate_state(request);
    state.bitmap_states = {2, 2};
    state.crc_samples.push_back({
        4096,
        0,
        8192
    });
    save_candidate(
        request,
        state,
        part_contents());

    auto opened =
        asyncdownload::recovery::RecoveryCheckpoint::open(
            request);

    EXPECT_EQ(
        opened.error,
        asyncdownload::make_error_code(
            asyncdownload::DownloadErrc::
                file_read_failed));
    EXPECT_EQ(opened.checkpoint, nullptr);
    EXPECT_TRUE(std::filesystem::exists(
        request.paths.temporary_path));
    EXPECT_TRUE(std::filesystem::exists(
        request.paths.metadata_path));
}

TEST(
    RecoveryCheckpointTest,
    RejectsHoleBeforeSerializedVdlWithoutMutation) {
    RecoveryTempDirectory temp(
        "asyncdownload_recovery_vdl_hole");
    const auto request = fresh_request(temp.path());
    auto state = candidate_state(request);
    state.bitmap_states = {2, 0};
    state.vdl_offset = 8192;
    const auto part_before = part_contents();
    save_candidate(
        request,
        state,
        part_before);
    const auto metadata_before =
        read_file(request.paths.metadata_path);

    auto opened =
        asyncdownload::recovery::RecoveryCheckpoint::open(
            request);

    EXPECT_EQ(
        opened.error,
        asyncdownload::make_error_code(
            asyncdownload::DownloadErrc::
                metadata_parse_failed));
    EXPECT_EQ(opened.checkpoint, nullptr);
    EXPECT_EQ(
        read_file(request.paths.temporary_path),
        part_before);
    EXPECT_EQ(
        read_file(request.paths.metadata_path),
        metadata_before);
    EXPECT_FALSE(std::filesystem::exists(
        request.paths.output_path));
}

TEST(
    RecoveryCheckpointTest,
    SerializesPreparedImagesAndGenerations) {
    RecoveryTempDirectory temp(
        "asyncdownload_recovery_generations");
    const auto request = fresh_request(temp.path());
    auto opened =
        asyncdownload::recovery::RecoveryCheckpoint::open(
            request);
    ASSERT_FALSE(opened.error);
    ASSERT_NE(opened.checkpoint, nullptr);
    const std::vector<std::uint8_t> bitmap{0, 0};
    const std::vector<
        asyncdownload::recovery::RecoveryRangeFact>
        ranges;

    auto first =
        opened.checkpoint->prepare(bitmap, ranges);
    auto overlapping =
        opened.checkpoint->prepare(bitmap, ranges);

    ASSERT_FALSE(first.error);
    ASSERT_NE(first.checkpoint, nullptr);
    EXPECT_EQ(overlapping.checkpoint, nullptr);
    EXPECT_EQ(
        overlapping.error,
        asyncdownload::make_error_code(
            asyncdownload::DownloadErrc::
                internal_error));
    const auto first_result =
        opened.checkpoint->commit(
            std::move(first.checkpoint));
    ASSERT_FALSE(first_result.error);
    EXPECT_EQ(first_result.generation, 1U);
    EXPECT_EQ(first_result.committed_vdl, 0);

    auto second =
        opened.checkpoint->prepare(bitmap, ranges);
    ASSERT_FALSE(second.error);
    ASSERT_NE(second.checkpoint, nullptr);
    const auto second_result =
        opened.checkpoint->commit(
            std::move(second.checkpoint));
    ASSERT_FALSE(second_result.error);
    EXPECT_EQ(second_result.generation, 2U);
    EXPECT_EQ(second_result.committed_vdl, 0);
}
