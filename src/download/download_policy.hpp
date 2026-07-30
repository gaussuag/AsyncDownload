#pragma once

#include "asyncdownload/types.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <system_error>

namespace asyncdownload::download {

enum class DownloadPolicyErrc : std::uint8_t {
    none = 0,
    max_connections_zero,
    max_connections_not_representable,
    queue_capacity_zero,
    queue_capacity_not_representable,
    scheduler_window_zero,
    scheduler_window_not_representable,
    backpressure_high_zero,
    backpressure_low_above_high,
    block_size_zero,
    block_size_not_power_of_two,
    block_size_not_representable,
    io_alignment_zero,
    io_alignment_not_power_of_two,
    io_alignment_exceeds_tail_capacity,
    block_size_not_aligned_for_io,
    max_gap_zero,
    max_gap_not_representable,
    flush_threshold_zero,
    flush_interval_negative,
    remote_size_invalid,
    remote_block_count_not_representable
};

struct DownloadPolicyFailure {
    DownloadPolicyErrc reason = DownloadPolicyErrc::none;
    std::error_code error{};
};

template <typename T>
struct DownloadPolicyResult {
    std::optional<T> value;
    DownloadPolicyFailure failure{};

    [[nodiscard]] bool ok() const noexcept {
        return value.has_value() && !failure.error;
    }
};

struct RemoteObjectFacts {
    std::int64_t total_size = 0;
    bool accept_ranges = false;
};

struct SchedulingPolicy {
    std::size_t connection_limit = 0;
    std::int64_t transfer_window_bytes = 0;
    std::int64_t block_bytes = 0;
    bool issue_range_requests = false;
    bool allow_work_stealing = false;
};

struct FlowControlPolicy {
    std::size_t packet_budget = 0;
    std::size_t memory_high_bytes = 0;
    std::size_t memory_low_bytes = 0;
};

struct PersistencePolicy {
    std::size_t block_bytes = 0;
    std::size_t io_alignment_bytes = 0;
    std::int64_t max_gap_bytes = 0;
    std::size_t flush_threshold_bytes = 0;
    std::chrono::milliseconds flush_interval{};
    bool overwrite_existing = true;
};

struct RecoveryIdentityPolicy {
    std::size_t block_bytes = 0;
    std::size_t io_alignment_bytes = 0;
    bool allow_sparse_resume = false;
};

class ValidatedDownloadPolicy;
class EffectiveDownloadPolicy;

[[nodiscard]] DownloadPolicyResult<ValidatedDownloadPolicy>
validate_download_options(const DownloadOptions& options) noexcept;

[[nodiscard]] DownloadPolicyResult<EffectiveDownloadPolicy>
bind_remote_facts(const ValidatedDownloadPolicy& policy,
                  RemoteObjectFacts facts) noexcept;

class EffectiveDownloadPolicy {
public:
    [[nodiscard]] const DownloadOptions& raw_options() const noexcept;
    [[nodiscard]] const SchedulingPolicy& scheduling() const noexcept;
    [[nodiscard]] const FlowControlPolicy& flow_control() const noexcept;
    [[nodiscard]] const PersistencePolicy& persistence() const noexcept;
    [[nodiscard]] const RecoveryIdentityPolicy& recovery_identity() const noexcept;
    [[nodiscard]] const RemoteObjectFacts& remote_facts() const noexcept;

private:
    EffectiveDownloadPolicy() = default;

    DownloadOptions raw_options_{};
    SchedulingPolicy scheduling_{};
    FlowControlPolicy flow_control_{};
    PersistencePolicy persistence_{};
    RecoveryIdentityPolicy recovery_identity_{};
    RemoteObjectFacts remote_facts_{};

    friend DownloadPolicyResult<EffectiveDownloadPolicy>
    bind_remote_facts(const ValidatedDownloadPolicy&, RemoteObjectFacts) noexcept;
};

class ValidatedDownloadPolicy {
public:
    [[nodiscard]] const DownloadOptions& raw_options() const noexcept;

private:
    explicit ValidatedDownloadPolicy(DownloadOptions raw_options) noexcept;

    DownloadOptions raw_options_{};

    friend DownloadPolicyResult<ValidatedDownloadPolicy>
    validate_download_options(const DownloadOptions&) noexcept;

    friend DownloadPolicyResult<EffectiveDownloadPolicy>
    bind_remote_facts(const ValidatedDownloadPolicy&, RemoteObjectFacts) noexcept;
};

}
