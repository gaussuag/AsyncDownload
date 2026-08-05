#include <atomic>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "asyncdownload/error.hpp"
#include "core/block_bitmap.hpp"
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

void expect_candidate_rejected_without_mutation(
    const asyncdownload::recovery::RecoveryOpenRequest& request,
    const asyncdownload::core::MetadataState& state,
    const std::vector<std::uint8_t>& part_before) {
    const std::vector<std::uint8_t> output_before(
        31,
        0xA7);
    save_candidate(request, state, part_before);
    write_part(request.paths.output_path, output_before);
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
    EXPECT_EQ(
        read_file(request.paths.output_path),
        output_before);
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
    ReportsNotFoundWhenFinalizeMetadataIsAbsent) {
    RecoveryTempDirectory temp(
        "asyncdownload_recovery_finalize_no_metadata");
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
    ASSERT_TRUE(std::filesystem::remove(
        request.paths.metadata_path));

    const auto result = opened.checkpoint->finalize();

    EXPECT_TRUE(result.output_available);
    EXPECT_FALSE(result.error);
    EXPECT_EQ(
        result.metadata_cleanup.status,
        asyncdownload::recovery::
            CleanupStatus::not_found);
    EXPECT_FALSE(result.metadata_cleanup.error);
    EXPECT_EQ(
        read_file(request.paths.output_path),
        bytes);
}

TEST(
    RecoveryCheckpointTest,
    CorruptOrphanMetadataStartsFresh) {
    RecoveryTempDirectory temp(
        "asyncdownload_recovery_corrupt_orphan");
    const auto request = fresh_request(temp.path());
    {
        std::ofstream metadata(
            request.paths.metadata_path,
            std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(metadata.is_open());
        metadata << "{broken";
    }

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
        opened.stale_cleanup.status,
        asyncdownload::recovery::
            CleanupStatus::removed);
    EXPECT_FALSE(std::filesystem::exists(
        request.paths.metadata_path));
    EXPECT_TRUE(std::filesystem::exists(
        request.paths.temporary_path));
}

TEST(
    RecoveryCheckpointTest,
    CorruptCandidatePairRemainsUntouched) {
    RecoveryTempDirectory temp(
        "asyncdownload_recovery_corrupt_pair");
    const auto request = fresh_request(temp.path());
    const auto bytes = part_contents();
    write_part(request.paths.temporary_path, bytes);
    {
        std::ofstream metadata(
            request.paths.metadata_path,
            std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(metadata.is_open());
        metadata << "{broken";
    }

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
        bytes);
    EXPECT_TRUE(std::filesystem::exists(
        request.paths.metadata_path));
}

TEST(
    RecoveryCheckpointTest,
    OverwriteDisabledPreservesOutputAndCheckpoint) {
    RecoveryTempDirectory temp(
        "asyncdownload_recovery_no_overwrite");
    auto request = fresh_request(temp.path());
    request.overwrite_existing = false;
    const std::vector<std::uint8_t> old_output(
        8192,
        0xB1);
    write_part(
        request.paths.output_path,
        old_output);
    auto opened =
        asyncdownload::recovery::RecoveryCheckpoint::open(
            request);
    ASSERT_FALSE(opened.error);
    ASSERT_NE(opened.checkpoint, nullptr);
    const std::vector<std::uint8_t> checkpoint_bytes(
        8192,
        0xB2);
    ASSERT_FALSE(opened.checkpoint->write(
        0,
        checkpoint_bytes));
    const auto finished =
        static_cast<std::uint8_t>(
            asyncdownload::core::BlockState::finished);
    auto prepared = opened.checkpoint->prepare(
        std::vector<std::uint8_t>{
            finished,
            finished
        },
        std::vector<
            asyncdownload::recovery::RecoveryRangeFact>{{
                {0},
                {0, 8192},
                8192,
                8192,
                2
            }});
    ASSERT_FALSE(prepared.error);
    const auto committed =
        opened.checkpoint->commit(
            std::move(prepared.checkpoint));
    ASSERT_FALSE(committed.error);

    const auto result = opened.checkpoint->finalize();

    EXPECT_FALSE(result.output_available);
    EXPECT_EQ(
        result.error,
        asyncdownload::make_error_code(
            asyncdownload::DownloadErrc::
                file_write_failed));
    EXPECT_EQ(
        read_file(request.paths.output_path),
        old_output);
    EXPECT_EQ(
        read_file(request.paths.temporary_path),
        checkpoint_bytes);
    EXPECT_TRUE(std::filesystem::exists(
        request.paths.metadata_path));
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
    RejectsOversizedCrcReadWithoutMutation) {
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

    expect_candidate_rejected_without_mutation(
        request,
        state,
        part_contents());
}

TEST(RecoveryCheckpointTest, RejectsNegativeVdlWithoutMutation) {
    RecoveryTempDirectory temp("asyncdownload_recovery_negative_vdl");
    const auto request = fresh_request(temp.path());
    auto state = candidate_state(request);
    state.vdl_offset = -1;
    expect_candidate_rejected_without_mutation(
        request, state, part_contents());
}

TEST(RecoveryCheckpointTest, RejectsVdlBeyondTotalWithoutMutation) {
    RecoveryTempDirectory temp("asyncdownload_recovery_large_vdl");
    const auto request = fresh_request(temp.path());
    auto state = candidate_state(request);
    state.vdl_offset = 8193;
    expect_candidate_rejected_without_mutation(
        request, state, part_contents());
}

TEST(RecoveryCheckpointTest, RejectsUnalignedVdlWithoutMutation) {
    RecoveryTempDirectory temp("asyncdownload_recovery_unaligned_vdl");
    const auto request = fresh_request(temp.path());
    auto state = candidate_state(request);
    state.vdl_offset = 1;
    expect_candidate_rejected_without_mutation(
        request, state, part_contents());
}

TEST(RecoveryCheckpointTest, RejectsInvalidBitmapStateWithoutMutation) {
    RecoveryTempDirectory temp("asyncdownload_recovery_bitmap_state");
    const auto request = fresh_request(temp.path());
    auto state = candidate_state(request);
    state.bitmap_states = {3};
    expect_candidate_rejected_without_mutation(
        request, state, part_contents());
}

TEST(RecoveryCheckpointTest, RejectsOversizedBitmapWithoutMutation) {
    RecoveryTempDirectory temp("asyncdownload_recovery_bitmap_size");
    const auto request = fresh_request(temp.path());
    auto state = candidate_state(request);
    state.bitmap_states = {0, 0, 0};
    expect_candidate_rejected_without_mutation(
        request, state, part_contents());
}

TEST(RecoveryCheckpointTest, RejectsOverflowingRangeWithoutMutation) {
    RecoveryTempDirectory temp("asyncdownload_recovery_range_overflow");
    const auto request = fresh_request(temp.path());
    auto state = candidate_state(request);
    state.ranges.push_back({
        0,
        0,
        std::numeric_limits<std::int64_t>::max(),
        0,
        0,
        0
    });
    expect_candidate_rejected_without_mutation(
        request, state, part_contents());
}

TEST(RecoveryCheckpointTest, RejectsOverlappingPersistedRangesWithoutMutation) {
    RecoveryTempDirectory temp("asyncdownload_recovery_range_overlap");
    const auto request = fresh_request(temp.path());
    auto state = candidate_state(request);
    state.ranges = {
        {0, 0, 4095, 4096, 4096, 0},
        {1, 2048, 8191, 4096, 4096, 0}
    };
    expect_candidate_rejected_without_mutation(
        request, state, part_contents());
}

TEST(RecoveryCheckpointTest, RejectsInvalidRangeFrontiersWithoutMutation) {
    RecoveryTempDirectory temp("asyncdownload_recovery_range_frontiers");
    const auto request = fresh_request(temp.path());
    const std::array<asyncdownload::core::RangeStateSnapshot, 5> ranges{{
        {0, -1, 4095, 0, 0, 0},
        {0, 1, 4095, 1, 0, 0},
        {0, 0, 4095, 1024, 2048, 0},
        {0, 0, 4095, 4097, 0, 0},
        {0, 0, 8192, 0, 0, 0}
    }};
    for (const auto& range : ranges) {
        auto state = candidate_state(request);
        state.ranges = {range};
        SCOPED_TRACE(range.start_offset);
        SCOPED_TRACE(range.end_offset);
        SCOPED_TRACE(range.current_offset);
        SCOPED_TRACE(range.persisted_offset);
        expect_candidate_rejected_without_mutation(
            request, state, part_contents());
    }
}

TEST(RecoveryCheckpointTest, RejectsMalformedCrcLengthsWithoutMutation) {
    RecoveryTempDirectory temp("asyncdownload_recovery_crc_lengths");
    const auto request = fresh_request(temp.path());
    for (const auto length : {std::size_t{0}, std::size_t{1}, std::size_t{4095}}) {
        auto state = candidate_state(request);
        state.crc_samples.push_back({4096, 0, length});
        SCOPED_TRACE(length);
        expect_candidate_rejected_without_mutation(
            request, state, part_contents());
    }
}

TEST(RecoveryCheckpointTest, RejectsMalformedCrcOffsetsWithoutMutation) {
    RecoveryTempDirectory temp("asyncdownload_recovery_crc_offsets");
    const auto request = fresh_request(temp.path());
    for (const auto offset : {
             std::int64_t{-4096},
             std::int64_t{1},
             std::int64_t{8192}}) {
        auto state = candidate_state(request);
        state.crc_samples.push_back({offset, 0, 4096});
        SCOPED_TRACE(offset);
        expect_candidate_rejected_without_mutation(
            request, state, part_contents());
    }
}

TEST(RecoveryCheckpointTest, RejectsDuplicateCrcOffsetsWithoutMutation) {
    RecoveryTempDirectory temp("asyncdownload_recovery_crc_duplicate");
    const auto request = fresh_request(temp.path());
    auto state = candidate_state(request);
    state.crc_samples = {
        {4096, 0, 4096},
        {4096, 0, 4096}
    };
    expect_candidate_rejected_without_mutation(
        request, state, part_contents());
}

TEST(RecoveryCheckpointTest, RejectsFullLengthForShortTailWithoutMutation) {
    RecoveryTempDirectory temp("asyncdownload_recovery_crc_tail");
    auto request = fresh_request(temp.path());
    request.remote.total_size = 6144;
    auto state = candidate_state(request);
    state.total_size = 6144;
    state.bitmap_states = {2, 2};
    state.crc_samples = {{4096, 0, 4096}};
    auto bytes = part_contents();
    bytes.resize(6144);
    expect_candidate_rejected_without_mutation(
        request, state, bytes);
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

TEST(RecoveryCheckpointTest, AbandonedPreparedCheckpointReleasesReservation) {
    RecoveryTempDirectory temp("asyncdownload_recovery_abandon");
    const auto request = fresh_request(temp.path());
    auto opened = asyncdownload::recovery::RecoveryCheckpoint::open(request);
    ASSERT_FALSE(opened.error);
    const std::vector<std::uint8_t> bitmap{0, 0};
    const std::vector<asyncdownload::recovery::RecoveryRangeFact> ranges;

    auto abandoned = opened.checkpoint->prepare(bitmap, ranges);
    ASSERT_FALSE(abandoned.error);
    abandoned.checkpoint.reset();
    auto successor = opened.checkpoint->prepare(bitmap, ranges);
    ASSERT_FALSE(successor.error);
    const auto committed = opened.checkpoint->commit(
        std::move(successor.checkpoint));

    EXPECT_FALSE(committed.error);
    EXPECT_EQ(committed.generation, 2U);
}

TEST(RecoveryCheckpointTest, MoveConstructionTransfersPreparedReservation) {
    RecoveryTempDirectory temp("asyncdownload_recovery_move_construct");
    const auto request = fresh_request(temp.path());
    auto opened = asyncdownload::recovery::RecoveryCheckpoint::open(request);
    ASSERT_FALSE(opened.error);
    const std::vector<std::uint8_t> bitmap{0, 0};
    const std::vector<asyncdownload::recovery::RecoveryRangeFact> ranges;
    auto prepared = opened.checkpoint->prepare(bitmap, ranges);
    ASSERT_FALSE(prepared.error);
    asyncdownload::recovery::PreparedCheckpoint moved(
        std::move(*prepared.checkpoint));

    const auto moved_from_result = opened.checkpoint->commit(
        std::move(prepared.checkpoint));
    const auto committed = opened.checkpoint->commit(
        std::make_unique<asyncdownload::recovery::PreparedCheckpoint>(
            std::move(moved)));

    EXPECT_EQ(
        moved_from_result.error,
        asyncdownload::make_error_code(
            asyncdownload::DownloadErrc::internal_error));
    EXPECT_FALSE(committed.error);
    EXPECT_EQ(committed.generation, 1U);
}

TEST(RecoveryCheckpointTest, MoveAssignmentReleasesTargetReservation) {
    RecoveryTempDirectory first_temp("asyncdownload_recovery_move_assign_first");
    RecoveryTempDirectory second_temp("asyncdownload_recovery_move_assign_second");
    const auto first_request = fresh_request(first_temp.path());
    const auto second_request = fresh_request(second_temp.path());
    auto first = asyncdownload::recovery::RecoveryCheckpoint::open(first_request);
    auto second = asyncdownload::recovery::RecoveryCheckpoint::open(second_request);
    ASSERT_FALSE(first.error);
    ASSERT_FALSE(second.error);
    const std::vector<std::uint8_t> bitmap{0, 0};
    const std::vector<asyncdownload::recovery::RecoveryRangeFact> ranges;
    auto first_prepared = first.checkpoint->prepare(bitmap, ranges);
    auto second_prepared = second.checkpoint->prepare(bitmap, ranges);
    ASSERT_FALSE(first_prepared.error);
    ASSERT_FALSE(second_prepared.error);

    *first_prepared.checkpoint = std::move(*second_prepared.checkpoint);
    auto first_successor = first.checkpoint->prepare(bitmap, ranges);
    const auto second_committed = second.checkpoint->commit(
        std::move(first_prepared.checkpoint));

    EXPECT_FALSE(first_successor.error);
    EXPECT_FALSE(second_committed.error);
    EXPECT_EQ(second_committed.generation, 1U);
}

TEST(RecoveryCheckpointTest, ForeignCommitReleasesSourceReservationOnly) {
    RecoveryTempDirectory source_temp("asyncdownload_recovery_foreign_source");
    RecoveryTempDirectory target_temp("asyncdownload_recovery_foreign_target");
    const auto source_request = fresh_request(source_temp.path());
    const auto target_request = fresh_request(target_temp.path());
    auto source = asyncdownload::recovery::RecoveryCheckpoint::open(source_request);
    auto target = asyncdownload::recovery::RecoveryCheckpoint::open(target_request);
    ASSERT_FALSE(source.error);
    ASSERT_FALSE(target.error);
    const std::vector<std::uint8_t> bitmap{0, 0};
    const std::vector<asyncdownload::recovery::RecoveryRangeFact> ranges;
    auto prepared = source.checkpoint->prepare(bitmap, ranges);
    ASSERT_FALSE(prepared.error);

    const auto foreign = target.checkpoint->commit(
        std::move(prepared.checkpoint));
    auto source_successor = source.checkpoint->prepare(bitmap, ranges);
    auto target_first = target.checkpoint->prepare(bitmap, ranges);

    EXPECT_EQ(
        foreign.error,
        asyncdownload::make_error_code(
            asyncdownload::DownloadErrc::internal_error));
    ASSERT_FALSE(source_successor.error);
    ASSERT_FALSE(target_first.error);
    const auto source_committed = source.checkpoint->commit(
        std::move(source_successor.checkpoint));
    const auto target_committed = target.checkpoint->commit(
        std::move(target_first.checkpoint));
    EXPECT_EQ(source_committed.generation, 2U);
    EXPECT_EQ(target_committed.generation, 1U);
}

TEST(RecoveryCheckpointTest, PreparedTokenMayOutliveCheckpointOwner) {
    RecoveryTempDirectory temp("asyncdownload_recovery_owner_first");
    const auto request = fresh_request(temp.path());
    auto opened = asyncdownload::recovery::RecoveryCheckpoint::open(request);
    ASSERT_FALSE(opened.error);
    auto prepared = opened.checkpoint->prepare(
        std::vector<std::uint8_t>{0, 0},
        std::vector<asyncdownload::recovery::RecoveryRangeFact>{});
    ASSERT_FALSE(prepared.error);

    opened.checkpoint.reset();
    prepared.checkpoint.reset();

    SUCCEED();
}

TEST(RecoveryCheckpointTest, NullPreparedCheckpointIsRejectedDeterministically) {
    RecoveryTempDirectory temp("asyncdownload_recovery_null_token");
    const auto request = fresh_request(temp.path());
    auto opened = asyncdownload::recovery::RecoveryCheckpoint::open(request);
    ASSERT_FALSE(opened.error);

    const auto first = opened.checkpoint->commit(nullptr);
    const auto second = opened.checkpoint->commit(nullptr);

    EXPECT_EQ(first.error, second.error);
    EXPECT_EQ(
        first.error,
        asyncdownload::make_error_code(
            asyncdownload::DownloadErrc::internal_error));
}

TEST(
    RecoveryCheckpointTest,
    RejectsFinalizeBeforeFullCommittedVdl) {
    RecoveryTempDirectory temp(
        "asyncdownload_recovery_finalize_incomplete");
    const auto request = fresh_request(temp.path());
    auto opened =
        asyncdownload::recovery::RecoveryCheckpoint::open(
            request);
    ASSERT_FALSE(opened.error);
    ASSERT_NE(opened.checkpoint, nullptr);

    const auto result = opened.checkpoint->finalize();

    EXPECT_FALSE(result.output_available);
    EXPECT_EQ(
        result.error,
        asyncdownload::make_error_code(
            asyncdownload::DownloadErrc::
                internal_error));
    EXPECT_TRUE(std::filesystem::exists(
        request.paths.temporary_path));
    EXPECT_FALSE(std::filesystem::exists(
        request.paths.output_path));
}

TEST(
    RecoveryCheckpointTest,
    FinalizesFullCommittedCheckpoint) {
    RecoveryTempDirectory temp(
        "asyncdownload_recovery_finalize_complete");
    const auto request = fresh_request(temp.path());
    auto opened =
        asyncdownload::recovery::RecoveryCheckpoint::open(
            request);
    ASSERT_FALSE(opened.error);
    ASSERT_NE(opened.checkpoint, nullptr);
    const auto bytes = part_contents();
    ASSERT_FALSE(opened.checkpoint->write(0, bytes));
    const auto finished =
        static_cast<std::uint8_t>(
            asyncdownload::core::BlockState::finished);
    auto prepared = opened.checkpoint->prepare(
        std::vector<std::uint8_t>{
            finished,
            finished
        },
        std::vector<
            asyncdownload::recovery::RecoveryRangeFact>{{
                {0},
                {0, 8192},
                8192,
                8192,
                2
            }});
    ASSERT_FALSE(prepared.error);
    const auto committed =
        opened.checkpoint->commit(
            std::move(prepared.checkpoint));
    ASSERT_FALSE(committed.error);
    ASSERT_EQ(committed.committed_vdl, 8192);

    const auto result = opened.checkpoint->finalize();

    EXPECT_TRUE(result.output_available);
    EXPECT_FALSE(result.error);
    EXPECT_EQ(
        result.metadata_cleanup.status,
        asyncdownload::recovery::
            CleanupStatus::removed);
    EXPECT_FALSE(result.metadata_cleanup.error);
    EXPECT_FALSE(std::filesystem::exists(
        request.paths.temporary_path));
    EXPECT_FALSE(std::filesystem::exists(
        request.paths.metadata_path));
    EXPECT_EQ(
        read_file(request.paths.output_path),
        bytes);
}
