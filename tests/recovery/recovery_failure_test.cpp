#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "asyncdownload/error.hpp"
#include "metadata/metadata_store.hpp"
#include "recovery/recovery_checkpoint.hpp"
#include "recovery/recovery_fault_adapter.hpp"

namespace {

class RecoveryFailureTest : public ::testing::Test {
protected:
    void SetUp() override {
        static std::atomic<std::uint64_t> next_id{0};
        root_ = std::filesystem::temp_directory_path() /
            ("asyncdownload_recovery_failure_" +
             std::to_string(next_id.fetch_add(
                 1,
                 std::memory_order_relaxed)));
        std::error_code ec;
        const auto removed =
            std::filesystem::remove_all(root_, ec);
        static_cast<void>(removed);
        ec.clear();
        std::filesystem::create_directories(root_, ec);
        ASSERT_FALSE(ec);
        asyncdownload::recovery::detail::
            recovery_fault_plan().reset();
    }

    void TearDown() override {
        asyncdownload::recovery::detail::
            recovery_fault_plan().reset();
        std::error_code ec;
        const auto removed =
            std::filesystem::remove_all(root_, ec);
        static_cast<void>(removed);
    }

    [[nodiscard]] asyncdownload::recovery::
    RecoveryOpenRequest request() const {
        asyncdownload::recovery::RecoveryOpenRequest
            value{};
        value.paths.output_path =
            root_ / "artifact.bin";
        value.paths.temporary_path =
            root_ / "artifact.bin.part";
        value.paths.metadata_path =
            root_ / "artifact.bin.config.json";
        value.remote.url =
            "https://example.com/artifact.bin";
        value.remote.total_size = 8192;
        value.remote.accept_ranges = true;
        value.policy.block_bytes = 4096;
        value.policy.io_alignment_bytes = 4096;
        value.policy.allow_sparse_resume = true;
        value.overwrite_existing = true;
        return value;
    }

    [[nodiscard]] std::vector<std::uint8_t>
    write_mismatched_pair(
        const asyncdownload::recovery::
            RecoveryOpenRequest& request) const {
        std::vector<std::uint8_t> bytes(8192);
        for (std::size_t index = 0;
             index < bytes.size();
             ++index) {
            bytes[index] =
                static_cast<std::uint8_t>(
                    index % 251);
        }
        std::ofstream part(
            request.paths.temporary_path,
            std::ios::binary | std::ios::trunc);
        EXPECT_TRUE(part.is_open());
        part.write(
            reinterpret_cast<const char*>(
                bytes.data()),
            static_cast<std::streamsize>(
                bytes.size()));
        part.close();
        EXPECT_TRUE(part.good());

        asyncdownload::core::MetadataState state{};
        state.url =
            "https://example.com/other.bin";
        state.output_path = request.paths.output_path;
        state.temporary_path =
            request.paths.temporary_path;
        state.total_size = request.remote.total_size;
        state.vdl_offset = 4096;
        state.accept_ranges = true;
        state.block_size = request.policy.block_bytes;
        state.io_alignment =
            request.policy.io_alignment_bytes;
        state.bitmap_states = {2, 0};
        asyncdownload::metadata::MetadataStore store(
            request.paths.metadata_path);
        EXPECT_FALSE(store.save(state));
        return bytes;
    }

    [[nodiscard]] static std::vector<std::uint8_t>
    read_file(const std::filesystem::path& path) {
        std::ifstream stream(path, std::ios::binary);
        EXPECT_TRUE(stream.is_open());
        return {
            std::istreambuf_iterator<char>(stream),
            std::istreambuf_iterator<char>()
        };
    }

    std::filesystem::path root_;
};

}

TEST_F(
    RecoveryFailureTest,
    InvalidationFailurePreservesOriginalPair) {
    const auto open_request = request();
    const auto part_before =
        write_mismatched_pair(open_request);
    const auto metadata_before =
        read_file(open_request.paths.metadata_path);
    asyncdownload::recovery::detail::
        recovery_fault_plan().
            fail_next_metadata_invalidate.store(
                true,
                std::memory_order_release);

    auto opened =
        asyncdownload::recovery::RecoveryCheckpoint::open(
            open_request);

    EXPECT_EQ(
        opened.error,
        asyncdownload::make_error_code(
            asyncdownload::DownloadErrc::
                metadata_save_failed));
    EXPECT_EQ(opened.checkpoint, nullptr);
    EXPECT_EQ(
        read_file(open_request.paths.temporary_path),
        part_before);
    EXPECT_EQ(
        read_file(open_request.paths.metadata_path),
        metadata_before);
}

TEST_F(
    RecoveryFailureTest,
    CrashAfterPartResetCannotLeaveStalePair) {
    const auto open_request = request();
    const auto part_before =
        write_mismatched_pair(open_request);
    asyncdownload::recovery::detail::
        recovery_fault_plan().
            stop_after_part_reset.store(
                true,
                std::memory_order_release);

    auto opened =
        asyncdownload::recovery::RecoveryCheckpoint::open(
            open_request);

    EXPECT_EQ(
        opened.error,
        asyncdownload::make_error_code(
            asyncdownload::DownloadErrc::
                internal_error));
    EXPECT_EQ(opened.checkpoint, nullptr);
    EXPECT_FALSE(std::filesystem::exists(
        open_request.paths.metadata_path));
    EXPECT_NE(
        read_file(open_request.paths.temporary_path),
        part_before);
}

TEST_F(
    RecoveryFailureTest,
    CrashAfterInvalidationKeepsOldPartUntrusted) {
    const auto open_request = request();
    const auto part_before =
        write_mismatched_pair(open_request);
    asyncdownload::recovery::detail::
        recovery_fault_plan().
            stop_after_metadata_invalidation.store(
                true,
                std::memory_order_release);

    auto opened =
        asyncdownload::recovery::RecoveryCheckpoint::open(
            open_request);

    EXPECT_EQ(
        opened.error,
        asyncdownload::make_error_code(
            asyncdownload::DownloadErrc::
                internal_error));
    EXPECT_EQ(opened.checkpoint, nullptr);
    EXPECT_FALSE(std::filesystem::exists(
        open_request.paths.metadata_path));
    EXPECT_EQ(
        read_file(open_request.paths.temporary_path),
        part_before);
}
