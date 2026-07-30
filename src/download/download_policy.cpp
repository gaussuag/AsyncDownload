#include "download_policy.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>

#include "asyncdownload/error.hpp"
#include "core/constants.hpp"

namespace asyncdownload::download {
namespace {

[[nodiscard]] bool is_power_of_two(const std::size_t value) noexcept {
    return value != 0 && (value & (value - 1)) == 0;
}

[[nodiscard]] bool fits_positive_int64(const std::size_t value) noexcept {
    return std::in_range<std::int64_t>(value);
}

[[nodiscard]] bool fits_long(const std::size_t value) noexcept {
    return std::in_range<long>(value);
}

[[nodiscard]] bool fits_queue_index(const std::size_t value) noexcept {
    return std::in_range<std::ptrdiff_t>(value);
}

[[nodiscard]] std::size_t maximum_crc_read_bytes() noexcept {
#ifdef _WIN32
    return std::min(
        std::numeric_limits<std::size_t>::max(),
        static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()));
#else
    return std::min(
        std::numeric_limits<std::size_t>::max(),
        static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()));
#endif
}

template <typename T>
[[nodiscard]] DownloadPolicyResult<T> reject(
    const DownloadPolicyErrc reason,
    const DownloadErrc public_error) noexcept {
    return {
        std::nullopt,
        DownloadPolicyFailure{reason, make_error_code(public_error)}
    };
}

}

ValidatedDownloadPolicy::ValidatedDownloadPolicy(DownloadOptions raw_options) noexcept
    : raw_options_(std::move(raw_options)) {}

const DownloadOptions& ValidatedDownloadPolicy::raw_options() const noexcept {
    return raw_options_;
}

const DownloadOptions& EffectiveDownloadPolicy::raw_options() const noexcept {
    return raw_options_;
}

const SchedulingPolicy& EffectiveDownloadPolicy::scheduling() const noexcept {
    return scheduling_;
}

const FlowControlPolicy& EffectiveDownloadPolicy::flow_control() const noexcept {
    return flow_control_;
}

const PersistencePolicy& EffectiveDownloadPolicy::persistence() const noexcept {
    return persistence_;
}

const RecoveryIdentityPolicy&
EffectiveDownloadPolicy::recovery_identity() const noexcept {
    return recovery_identity_;
}

const RemoteObjectFacts& EffectiveDownloadPolicy::remote_facts() const noexcept {
    return remote_facts_;
}

DownloadPolicyResult<ValidatedDownloadPolicy>
validate_download_options(const DownloadOptions& options) noexcept {
    if (options.max_connections == 0) {
        return reject<ValidatedDownloadPolicy>(
            DownloadPolicyErrc::max_connections_zero,
            DownloadErrc::invalid_request);
    }
    if (!fits_long(options.max_connections)) {
        return reject<ValidatedDownloadPolicy>(
            DownloadPolicyErrc::max_connections_not_representable,
            DownloadErrc::invalid_request);
    }
    if (options.queue_capacity_packets == 0) {
        return reject<ValidatedDownloadPolicy>(
            DownloadPolicyErrc::queue_capacity_zero,
            DownloadErrc::invalid_request);
    }
    if (!fits_queue_index(options.queue_capacity_packets)) {
        return reject<ValidatedDownloadPolicy>(
            DownloadPolicyErrc::queue_capacity_not_representable,
            DownloadErrc::invalid_request);
    }
    if (options.scheduler_window_bytes == 0) {
        return reject<ValidatedDownloadPolicy>(
            DownloadPolicyErrc::scheduler_window_zero,
            DownloadErrc::invalid_request);
    }
    if (!fits_positive_int64(options.scheduler_window_bytes)) {
        return reject<ValidatedDownloadPolicy>(
            DownloadPolicyErrc::scheduler_window_not_representable,
            DownloadErrc::invalid_request);
    }
    if (options.backpressure_high_bytes == 0) {
        return reject<ValidatedDownloadPolicy>(
            DownloadPolicyErrc::backpressure_high_zero,
            DownloadErrc::invalid_request);
    }
    if (options.backpressure_low_bytes > options.backpressure_high_bytes) {
        return reject<ValidatedDownloadPolicy>(
            DownloadPolicyErrc::backpressure_low_above_high,
            DownloadErrc::invalid_request);
    }
    if (options.block_size == 0) {
        return reject<ValidatedDownloadPolicy>(
            DownloadPolicyErrc::block_size_zero,
            DownloadErrc::invalid_request);
    }
    if (!is_power_of_two(options.block_size)) {
        return reject<ValidatedDownloadPolicy>(
            DownloadPolicyErrc::block_size_not_power_of_two,
            DownloadErrc::invalid_request);
    }
    if (!fits_positive_int64(options.block_size) ||
        options.block_size > maximum_crc_read_bytes()) {
        return reject<ValidatedDownloadPolicy>(
            DownloadPolicyErrc::block_size_not_representable,
            DownloadErrc::invalid_request);
    }
    if (options.io_alignment == 0) {
        return reject<ValidatedDownloadPolicy>(
            DownloadPolicyErrc::io_alignment_zero,
            DownloadErrc::invalid_request);
    }
    if (!is_power_of_two(options.io_alignment)) {
        return reject<ValidatedDownloadPolicy>(
            DownloadPolicyErrc::io_alignment_not_power_of_two,
            DownloadErrc::invalid_request);
    }
    if (options.io_alignment > core::TAIL_BUFFER_CAPACITY_BYTES) {
        return reject<ValidatedDownloadPolicy>(
            DownloadPolicyErrc::io_alignment_exceeds_tail_capacity,
            DownloadErrc::invalid_request);
    }
    if (options.block_size % options.io_alignment != 0) {
        return reject<ValidatedDownloadPolicy>(
            DownloadPolicyErrc::block_size_not_aligned_for_io,
            DownloadErrc::invalid_request);
    }
    if (options.max_gap_bytes == 0) {
        return reject<ValidatedDownloadPolicy>(
            DownloadPolicyErrc::max_gap_zero,
            DownloadErrc::invalid_request);
    }
    if (!fits_positive_int64(options.max_gap_bytes)) {
        return reject<ValidatedDownloadPolicy>(
            DownloadPolicyErrc::max_gap_not_representable,
            DownloadErrc::invalid_request);
    }
    if (options.flush_threshold_bytes == 0) {
        return reject<ValidatedDownloadPolicy>(
            DownloadPolicyErrc::flush_threshold_zero,
            DownloadErrc::invalid_request);
    }
    if (options.flush_interval.count() < 0) {
        return reject<ValidatedDownloadPolicy>(
            DownloadPolicyErrc::flush_interval_negative,
            DownloadErrc::invalid_request);
    }

    return {
        ValidatedDownloadPolicy(options),
        DownloadPolicyFailure{}
    };
}

DownloadPolicyResult<EffectiveDownloadPolicy>
bind_remote_facts(const ValidatedDownloadPolicy& policy,
                  const RemoteObjectFacts facts) noexcept {
    if (facts.total_size <= 0) {
        return reject<EffectiveDownloadPolicy>(
            DownloadPolicyErrc::remote_size_invalid,
            DownloadErrc::http_probe_failed);
    }

    const auto& raw = policy.raw_options_;
    const auto block_bytes = static_cast<std::int64_t>(raw.block_size);
    const auto quotient = facts.total_size / block_bytes;
    const auto remainder = facts.total_size % block_bytes;
    const auto block_count = quotient + (remainder == 0 ? 0 : 1);
    if constexpr (sizeof(std::size_t) < sizeof(std::int64_t)) {
        if (block_count > static_cast<std::int64_t>(
                std::numeric_limits<std::size_t>::max())) {
            return reject<EffectiveDownloadPolicy>(
                DownloadPolicyErrc::remote_block_count_not_representable,
                DownloadErrc::http_invalid_response);
        }
    }

    EffectiveDownloadPolicy effective;
    effective.raw_options_ = raw;
    effective.remote_facts_ = facts;
    effective.scheduling_ = SchedulingPolicy{
        facts.accept_ranges ? raw.max_connections : 1,
        facts.accept_ranges
            ? std::min(static_cast<std::int64_t>(raw.scheduler_window_bytes),
                       facts.total_size)
            : facts.total_size,
        block_bytes,
        facts.accept_ranges,
        facts.accept_ranges && raw.max_connections > 1
    };
    effective.flow_control_ = FlowControlPolicy{
        raw.queue_capacity_packets,
        raw.backpressure_high_bytes,
        raw.backpressure_low_bytes
    };
    effective.persistence_ = PersistencePolicy{
        raw.block_size,
        raw.io_alignment,
        static_cast<std::int64_t>(raw.max_gap_bytes),
        raw.flush_threshold_bytes,
        raw.flush_interval,
        raw.overwrite_existing
    };
    effective.recovery_identity_ = RecoveryIdentityPolicy{
        raw.block_size,
        raw.io_alignment,
        facts.accept_ranges
    };

    return {
        std::move(effective),
        DownloadPolicyFailure{}
    };
}

}
