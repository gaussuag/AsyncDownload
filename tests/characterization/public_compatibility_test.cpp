#include <chrono>
#include <filesystem>

#include <gtest/gtest.h>

#include "asyncdownload/client.hpp"
#include "asyncdownload/error.hpp"

TEST(PublicCompatibilityTest, KeepsDownloadOptionDefaults) {
    const asyncdownload::DownloadOptions options{};

    EXPECT_EQ(options.max_connections, 4U);
    EXPECT_EQ(options.queue_capacity_packets, 4096U);
    EXPECT_EQ(options.scheduler_window_bytes, 4U * 1024U * 1024U);
    EXPECT_EQ(options.backpressure_high_bytes, 256U * 1024U * 1024U);
    EXPECT_EQ(options.backpressure_low_bytes, 128U * 1024U * 1024U);
    EXPECT_EQ(options.block_size, 64U * 1024U);
    EXPECT_EQ(options.io_alignment, 4U * 1024U);
    EXPECT_EQ(options.max_gap_bytes, 32U * 1024U * 1024U);
    EXPECT_EQ(options.flush_threshold_bytes, 16U * 1024U * 1024U);
    EXPECT_EQ(options.flush_interval, std::chrono::milliseconds(2000));
    EXPECT_TRUE(options.overwrite_existing);
}

TEST(PublicCompatibilityTest, SupportsAggregateRequestInitialization) {
    const asyncdownload::DownloadRequest request{
        "https://example.invalid/object",
        std::filesystem::path("output.bin"),
        asyncdownload::DownloadOptions{},
        {}};

    EXPECT_EQ(request.url, "https://example.invalid/object");
    EXPECT_EQ(request.output_path, std::filesystem::path("output.bin"));
    EXPECT_EQ(request.options.max_connections, 4U);
    EXPECT_FALSE(request.progress_callback);
}

TEST(PublicCompatibilityTest, SupportsPositionalDownloadOptionsInitialization) {
    const asyncdownload::DownloadOptions options{
        4,
        4096,
        4 * 1024 * 1024,
        256 * 1024 * 1024,
        128 * 1024 * 1024,
        64 * 1024,
        4 * 1024,
        32 * 1024 * 1024,
        16 * 1024 * 1024,
        std::chrono::milliseconds(2000),
        true};

    EXPECT_EQ(options.max_connections, 4U);
    EXPECT_EQ(options.queue_capacity_packets, 4096U);
    EXPECT_EQ(options.scheduler_window_bytes, 4U * 1024U * 1024U);
    EXPECT_EQ(options.flush_interval, std::chrono::milliseconds(2000));
    EXPECT_TRUE(options.overwrite_existing);
}

TEST(PublicCompatibilityTest, RejectsMissingUrlBeforeIo) {
    asyncdownload::DownloadClient client;
    asyncdownload::DownloadRequest request{};
    request.output_path = "unused.bin";

    const auto result = client.download(request);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.error,
        asyncdownload::make_error_code(asyncdownload::DownloadErrc::invalid_request));
}

TEST(PublicCompatibilityTest, RejectsMissingOutputPathBeforeIo) {
    asyncdownload::DownloadClient client;
    asyncdownload::DownloadRequest request{};
    request.url = "https://example.invalid/object";

    const auto result = client.download(request);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.error,
        asyncdownload::make_error_code(asyncdownload::DownloadErrc::invalid_request));
}
