#pragma once

#include "core/models.hpp"
#include "download/download_policy.hpp"
#include "range/range_types.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace asyncdownload::recovery {

using CheckpointGeneration = std::uint64_t;

enum class RecoveryDisposition : std::uint8_t {
    fresh = 0,
    resumed = 1,
    complete = 2
};

enum class CleanupStatus : std::uint8_t {
    removed = 0,
    not_found = 1,
    failed = 2
};

enum class DiscardReason : std::uint8_t {
    identity_mismatch = 0,
    orphan_artifact = 1,
    non_range_partial = 2
};

struct RemoteRecoveryIdentity {
    std::string url;
    std::int64_t total_size = 0;
    bool accept_ranges = false;
    std::string etag;
    std::string last_modified;
};

struct RecoveryOpenRequest {
    core::SessionPaths paths;
    RemoteRecoveryIdentity remote;
    download::RecoveryIdentityPolicy policy;
    bool overwrite_existing = false;
};

struct RecoveryRangeFact {
    range::RangeId id{};
    range::ByteSpan bytes{};
    range::ByteOffset dispatch_cursor = 0;
    range::ByteOffset persisted_through = 0;
    std::uint8_t legacy_status = 0;
};

struct RestoredCheckpoint {
    RecoveryDisposition disposition =
        RecoveryDisposition::fresh;
    std::vector<std::uint8_t> bitmap_states;
    std::int64_t trusted_bytes = 0;
    std::int64_t safe_vdl = 0;
    std::size_t completed_ranges = 0;
};

struct CheckpointCommitResult {
    CheckpointGeneration generation = 0;
    std::int64_t committed_vdl = 0;
    std::error_code error;
};

struct CleanupResult {
    CleanupStatus status = CleanupStatus::not_found;
    std::error_code error;
};

struct FinalizeResult {
    bool output_available = false;
    CleanupResult metadata_cleanup;
    std::error_code error;
};

}
