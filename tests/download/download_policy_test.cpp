#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

#include <gtest/gtest.h>

#include "asyncdownload/error.hpp"
#include "core/constants.hpp"
#include "asyncdownload/types.hpp"
#include "core/models.hpp"
#include "download/download_policy.hpp"

namespace {

using asyncdownload::DownloadErrc;
using asyncdownload::DownloadOptions;
using asyncdownload::download::DownloadPolicyErrc;

void expect_raw_options_equal(const DownloadOptions& actual,
                              const DownloadOptions& expected) {
    EXPECT_EQ(actual.max_connections, expected.max_connections);
    EXPECT_EQ(actual.queue_capacity_packets, expected.queue_capacity_packets);
    EXPECT_EQ(actual.scheduler_window_bytes, expected.scheduler_window_bytes);
    EXPECT_EQ(actual.backpressure_high_bytes, expected.backpressure_high_bytes);
    EXPECT_EQ(actual.backpressure_low_bytes, expected.backpressure_low_bytes);
    EXPECT_EQ(actual.block_size, expected.block_size);
    EXPECT_EQ(actual.io_alignment, expected.io_alignment);
    EXPECT_EQ(actual.max_gap_bytes, expected.max_gap_bytes);
    EXPECT_EQ(actual.flush_threshold_bytes, expected.flush_threshold_bytes);
    EXPECT_EQ(actual.flush_interval, expected.flush_interval);
    EXPECT_EQ(actual.overwrite_existing, expected.overwrite_existing);
}

void expect_validation_failure(const DownloadOptions& options,
                               const DownloadPolicyErrc reason) {
    const auto result =
        asyncdownload::download::validate_download_options(options);
    EXPECT_FALSE(result.ok());
    EXPECT_FALSE(result.value.has_value());
    EXPECT_EQ(result.failure.reason, reason);
    EXPECT_EQ(
        result.failure.error,
        asyncdownload::make_error_code(DownloadErrc::invalid_request));
}

asyncdownload::download::DownloadPolicyResult<
    asyncdownload::download::EffectiveDownloadPolicy>
bind_defaults(const asyncdownload::download::RemoteObjectFacts facts) {
    const auto validated =
        asyncdownload::download::validate_download_options(DownloadOptions{});
    EXPECT_TRUE(validated.ok());
    return asyncdownload::download::bind_remote_facts(*validated.value, facts);
}

}

TEST(DownloadPolicyTest, AcceptsCurrentDefaultsWithoutChangingRawValues) {
    const DownloadOptions options{};
    const auto result =
        asyncdownload::download::validate_download_options(options);

    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.failure.reason, DownloadPolicyErrc::none);
    EXPECT_FALSE(result.failure.error);
    expect_raw_options_equal(result.value->raw_options(), options);
}

TEST(DownloadPolicyTest, AcceptsZeroLowWatermark) {
    DownloadOptions options{};
    options.backpressure_low_bytes = 0;

    EXPECT_TRUE(asyncdownload::download::validate_download_options(options).ok());
}

TEST(DownloadPolicyTest, AcceptsEqualWatermarks) {
    DownloadOptions options{};
    options.backpressure_low_bytes = options.backpressure_high_bytes;

    EXPECT_TRUE(asyncdownload::download::validate_download_options(options).ok());
}

TEST(DownloadPolicyTest, AcceptsZeroFlushInterval) {
    DownloadOptions options{};
    options.flush_interval = std::chrono::milliseconds{0};

    EXPECT_TRUE(asyncdownload::download::validate_download_options(options).ok());
}

TEST(DownloadPolicyTest, AcceptsWindowSmallerThanBlock) {
    DownloadOptions options{};
    options.scheduler_window_bytes = options.block_size / 2;

    EXPECT_TRUE(asyncdownload::download::validate_download_options(options).ok());
}

TEST(DownloadPolicyTest, AcceptsPowerOfTwoIoAlignmentBelowTailCapacity) {
    DownloadOptions options{};
    options.io_alignment =
        asyncdownload::core::TAIL_BUFFER_CAPACITY_BYTES / 2;

    EXPECT_TRUE(asyncdownload::download::validate_download_options(options).ok());
}

TEST(DownloadPolicyTest, RejectsZeroMaxConnections) {
    DownloadOptions options{};
    options.max_connections = 0;

    expect_validation_failure(
        options,
        DownloadPolicyErrc::max_connections_zero);
}

TEST(DownloadPolicyTest, RejectsUnrepresentableMaxConnections) {
    if constexpr (sizeof(std::size_t) <= sizeof(long)) {
        GTEST_SKIP();
    } else {
        DownloadOptions options{};
        options.max_connections =
            static_cast<std::size_t>(std::numeric_limits<long>::max()) + 1;

        expect_validation_failure(
            options,
            DownloadPolicyErrc::max_connections_not_representable);
    }
}

TEST(DownloadPolicyTest, RejectsZeroQueueCapacity) {
    DownloadOptions options{};
    options.queue_capacity_packets = 0;

    expect_validation_failure(
        options,
        DownloadPolicyErrc::queue_capacity_zero);
}

TEST(DownloadPolicyTest, RejectsUnrepresentableQueueCapacity) {
    if constexpr (sizeof(std::size_t) <= sizeof(std::ptrdiff_t)) {
        GTEST_SKIP();
    } else {
        DownloadOptions options{};
        options.queue_capacity_packets =
            static_cast<std::size_t>(
                std::numeric_limits<std::ptrdiff_t>::max()) + 1;

        expect_validation_failure(
            options,
            DownloadPolicyErrc::queue_capacity_not_representable);
    }
}

TEST(DownloadPolicyTest, RejectsZeroSchedulerWindow) {
    DownloadOptions options{};
    options.scheduler_window_bytes = 0;

    expect_validation_failure(
        options,
        DownloadPolicyErrc::scheduler_window_zero);
}

TEST(DownloadPolicyTest, RejectsUnrepresentableSchedulerWindow) {
    if constexpr (sizeof(std::size_t) < sizeof(std::int64_t)) {
        GTEST_SKIP();
    } else {
        DownloadOptions options{};
        options.scheduler_window_bytes =
            static_cast<std::size_t>(
                std::numeric_limits<std::int64_t>::max()) + 1;

        expect_validation_failure(
            options,
            DownloadPolicyErrc::scheduler_window_not_representable);
    }
}

TEST(DownloadPolicyTest, RejectsZeroHighWatermark) {
    DownloadOptions options{};
    options.backpressure_high_bytes = 0;
    options.backpressure_low_bytes = 0;

    expect_validation_failure(
        options,
        DownloadPolicyErrc::backpressure_high_zero);
}

TEST(DownloadPolicyTest, RejectsLowWatermarkAboveHighWatermark) {
    DownloadOptions options{};
    options.backpressure_low_bytes = options.backpressure_high_bytes + 1;

    expect_validation_failure(
        options,
        DownloadPolicyErrc::backpressure_low_above_high);
}

TEST(DownloadPolicyTest, RejectsZeroBlockSize) {
    DownloadOptions options{};
    options.block_size = 0;

    expect_validation_failure(options, DownloadPolicyErrc::block_size_zero);
}

TEST(DownloadPolicyTest, RejectsNonPowerOfTwoBlockSize) {
    DownloadOptions options{};
    options.block_size = 3;

    expect_validation_failure(
        options,
        DownloadPolicyErrc::block_size_not_power_of_two);
}

TEST(DownloadPolicyTest, RejectsUnrepresentableBlockSize) {
    if constexpr (sizeof(std::size_t) <= sizeof(std::uint32_t)) {
        GTEST_SKIP();
    } else {
        DownloadOptions options{};
#ifdef _WIN32
        options.block_size =
            static_cast<std::size_t>(std::uint64_t{1} << 32);
#else
        options.block_size =
            static_cast<std::size_t>(std::uint64_t{1} << 63);
#endif

        expect_validation_failure(
            options,
            DownloadPolicyErrc::block_size_not_representable);
    }
}

TEST(DownloadPolicyTest, RejectsZeroIoAlignment) {
    DownloadOptions options{};
    options.io_alignment = 0;

    expect_validation_failure(options, DownloadPolicyErrc::io_alignment_zero);
}

TEST(DownloadPolicyTest, RejectsNonPowerOfTwoIoAlignment) {
    DownloadOptions options{};
    options.io_alignment = 3;

    expect_validation_failure(
        options,
        DownloadPolicyErrc::io_alignment_not_power_of_two);
}

TEST(DownloadPolicyTest, RejectsIoAlignmentAboveTailCapacity) {
    DownloadOptions options{};
    options.io_alignment =
        asyncdownload::core::TAIL_BUFFER_CAPACITY_BYTES * 2;

    expect_validation_failure(
        options,
        DownloadPolicyErrc::io_alignment_exceeds_tail_capacity);
}

TEST(DownloadPolicyTest, RejectsBlockSizeNotDivisibleByIoAlignment) {
    DownloadOptions options{};
    options.block_size = 2048;
    options.io_alignment = 4096;

    expect_validation_failure(
        options,
        DownloadPolicyErrc::block_size_not_aligned_for_io);
}

TEST(DownloadPolicyTest, RejectsZeroMaxGap) {
    DownloadOptions options{};
    options.max_gap_bytes = 0;

    expect_validation_failure(options, DownloadPolicyErrc::max_gap_zero);
}

TEST(DownloadPolicyTest, RejectsUnrepresentableMaxGap) {
    if constexpr (sizeof(std::size_t) < sizeof(std::int64_t)) {
        GTEST_SKIP();
    } else {
        DownloadOptions options{};
        options.max_gap_bytes =
            static_cast<std::size_t>(
                std::numeric_limits<std::int64_t>::max()) + 1;

        expect_validation_failure(
            options,
            DownloadPolicyErrc::max_gap_not_representable);
    }
}

TEST(DownloadPolicyTest, RejectsZeroFlushThreshold) {
    DownloadOptions options{};
    options.flush_threshold_bytes = 0;

    expect_validation_failure(
        options,
        DownloadPolicyErrc::flush_threshold_zero);
}

TEST(DownloadPolicyTest, RejectsNegativeFlushInterval) {
    DownloadOptions options{};
    options.flush_interval = std::chrono::milliseconds{-1};

    expect_validation_failure(
        options,
        DownloadPolicyErrc::flush_interval_negative);
}

TEST(DownloadPolicyTest, ReturnsFirstFailureInDocumentedOrder) {
    DownloadOptions options{};
    options.max_connections = 0;
    options.queue_capacity_packets = 0;
    options.scheduler_window_bytes = 0;

    expect_validation_failure(
        options,
        DownloadPolicyErrc::max_connections_zero);
}

TEST(DownloadPolicyTest, RejectsNonPositiveRemoteSize) {
    const auto zero_result = bind_defaults({0, true});
    EXPECT_FALSE(zero_result.ok());
    EXPECT_EQ(
        zero_result.failure.reason,
        DownloadPolicyErrc::remote_size_invalid);
    EXPECT_EQ(
        zero_result.failure.error,
        asyncdownload::make_error_code(DownloadErrc::http_probe_failed));

    const auto negative_result = bind_defaults({-1, true});
    EXPECT_FALSE(negative_result.ok());
    EXPECT_EQ(
        negative_result.failure.reason,
        DownloadPolicyErrc::remote_size_invalid);
    EXPECT_EQ(
        negative_result.failure.error,
        asyncdownload::make_error_code(DownloadErrc::http_probe_failed));
}

TEST(DownloadPolicyTest, RejectsUnrepresentableRemoteBlockCount) {
    if constexpr (sizeof(std::size_t) >= sizeof(std::int64_t)) {
        GTEST_SKIP();
    } else {
        DownloadOptions options{};
        options.block_size = 1;
        options.io_alignment = 1;
        const auto validated =
            asyncdownload::download::validate_download_options(options);
        ASSERT_TRUE(validated.ok());

        const auto result = asyncdownload::download::bind_remote_facts(
            *validated.value,
            {static_cast<std::int64_t>(
                std::numeric_limits<std::size_t>::max()) + 1, true});

        EXPECT_FALSE(result.ok());
        EXPECT_EQ(
            result.failure.reason,
            DownloadPolicyErrc::remote_block_count_not_representable);
        EXPECT_EQ(
            result.failure.error,
            asyncdownload::make_error_code(DownloadErrc::http_invalid_response));
    }
}

TEST(DownloadPolicyTest, PreservesRangeConnectionLimit) {
    DownloadOptions options{};
    options.max_connections = 7;
    const auto validated =
        asyncdownload::download::validate_download_options(options);
    ASSERT_TRUE(validated.ok());

    const auto result = asyncdownload::download::bind_remote_facts(
        *validated.value,
        {1024, true});

    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value->scheduling().connection_limit, 7);
    EXPECT_TRUE(result.value->scheduling().issue_range_requests);
}

TEST(DownloadPolicyTest, ClampsRangeWindowToRemoteSize) {
    const auto result = bind_defaults({1024, true});

    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value->scheduling().transfer_window_bytes, 1024);
}

TEST(DownloadPolicyTest, EnablesWorkStealingOnlyForMultipleRangeConnections) {
    DownloadOptions one_connection{};
    one_connection.max_connections = 1;
    const auto validated_one =
        asyncdownload::download::validate_download_options(one_connection);
    ASSERT_TRUE(validated_one.ok());
    const auto one = asyncdownload::download::bind_remote_facts(
        *validated_one.value,
        {1024, true});

    DownloadOptions multiple_connections{};
    multiple_connections.max_connections = 2;
    const auto validated_multiple =
        asyncdownload::download::validate_download_options(
            multiple_connections);
    ASSERT_TRUE(validated_multiple.ok());
    const auto multiple = asyncdownload::download::bind_remote_facts(
        *validated_multiple.value,
        {1024, true});

    ASSERT_TRUE(one.ok());
    ASSERT_TRUE(multiple.ok());
    EXPECT_FALSE(one.value->scheduling().allow_work_stealing);
    EXPECT_TRUE(multiple.value->scheduling().allow_work_stealing);
}

TEST(DownloadPolicyTest, DowngradesNonRangeToOneConnectionAndFullWindow) {
    const auto result = bind_defaults({12345, false});

    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value->scheduling().connection_limit, 1);
    EXPECT_EQ(result.value->scheduling().transfer_window_bytes, 12345);
    EXPECT_FALSE(result.value->scheduling().issue_range_requests);
    EXPECT_FALSE(result.value->scheduling().allow_work_stealing);
}

TEST(DownloadPolicyTest, DisablesSparseResumeWithoutRangeSupport) {
    const auto result = bind_defaults({1024, false});

    ASSERT_TRUE(result.ok());
    EXPECT_FALSE(result.value->recovery_identity().allow_sparse_resume);
}

TEST(DownloadPolicyTest, PreservesRawOptionsAfterNonRangeDowngrade) {
    DownloadOptions options{};
    options.max_connections = 7;
    options.scheduler_window_bytes = 8192;
    const auto validated =
        asyncdownload::download::validate_download_options(options);
    ASSERT_TRUE(validated.ok());

    const auto result = asyncdownload::download::bind_remote_facts(
        *validated.value,
        {1024, false});

    ASSERT_TRUE(result.ok());
    expect_raw_options_equal(result.value->raw_options(), options);
}

TEST(DownloadPolicyTest, CopiesFlowAndPersistenceValuesExactly) {
    DownloadOptions options{};
    options.queue_capacity_packets = 13;
    options.backpressure_high_bytes = 32768;
    options.backpressure_low_bytes = 1234;
    options.block_size = 8192;
    options.io_alignment = 2048;
    options.max_gap_bytes = 7777;
    options.flush_threshold_bytes = 8888;
    options.flush_interval = std::chrono::milliseconds{99};
    options.overwrite_existing = false;
    const auto validated =
        asyncdownload::download::validate_download_options(options);
    ASSERT_TRUE(validated.ok());

    const auto result = asyncdownload::download::bind_remote_facts(
        *validated.value,
        {1024, true});

    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value->flow_control().packet_budget, 13);
    EXPECT_EQ(result.value->flow_control().memory_high_bytes, 32768);
    EXPECT_EQ(result.value->flow_control().memory_low_bytes, 1234);
    EXPECT_EQ(result.value->persistence().block_bytes, 8192);
    EXPECT_EQ(result.value->persistence().io_alignment_bytes, 2048);
    EXPECT_EQ(result.value->persistence().max_gap_bytes, 7777);
    EXPECT_EQ(result.value->persistence().flush_threshold_bytes, 8888);
    EXPECT_EQ(
        result.value->persistence().flush_interval,
        std::chrono::milliseconds{99});
    EXPECT_FALSE(result.value->persistence().overwrite_existing);
    EXPECT_EQ(result.value->recovery_identity().block_bytes, 8192);
    EXPECT_EQ(result.value->recovery_identity().io_alignment_bytes, 2048);
    EXPECT_TRUE(result.value->recovery_identity().allow_sparse_resume);
}
