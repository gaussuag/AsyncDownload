#pragma once

#include "recovery_types.hpp"

#include <cstddef>
#include <memory>
#include <span>
#include <system_error>
#include <vector>

namespace asyncdownload::recovery {

class PreparedCheckpoint {
public:
    PreparedCheckpoint(
        PreparedCheckpoint&&) noexcept;
    PreparedCheckpoint& operator=(
        PreparedCheckpoint&&) noexcept;
    ~PreparedCheckpoint();

    PreparedCheckpoint(
        const PreparedCheckpoint&) = delete;
    PreparedCheckpoint& operator=(
        const PreparedCheckpoint&) = delete;

private:
    class Implementation;

    explicit PreparedCheckpoint(
        std::unique_ptr<Implementation>
            implementation) noexcept;

    std::unique_ptr<Implementation> implementation_;

    friend class RecoveryCheckpoint;
};

struct PrepareCheckpointResult {
    std::unique_ptr<PreparedCheckpoint> checkpoint;
    std::error_code error;
};

class RecoveryCheckpoint;

struct RecoveryOpenResult {
    std::unique_ptr<RecoveryCheckpoint> checkpoint;
    RestoredCheckpoint restored;
    CleanupResult stale_cleanup;
    std::error_code error;
};

class RecoveryCheckpoint {
public:
    [[nodiscard]] static RecoveryOpenResult open(
        const RecoveryOpenRequest& request) noexcept;

    ~RecoveryCheckpoint();

    RecoveryCheckpoint(
        const RecoveryCheckpoint&) = delete;
    RecoveryCheckpoint& operator=(
        const RecoveryCheckpoint&) = delete;

    [[nodiscard]] std::error_code write(
        std::int64_t offset,
        std::span<const std::uint8_t> bytes) noexcept;

    [[nodiscard]] PrepareCheckpointResult prepare(
        std::span<const std::uint8_t> bitmap_states,
        std::span<const RecoveryRangeFact> ranges) noexcept;

    [[nodiscard]] CheckpointCommitResult commit(
        std::unique_ptr<PreparedCheckpoint>
            checkpoint) noexcept;

    [[nodiscard]] FinalizeResult finalize() noexcept;

    void close_preserving_artifacts() noexcept;

    [[nodiscard]] const core::SessionPaths& paths()
        const noexcept;

private:
    class Implementation;

    explicit RecoveryCheckpoint(
        std::unique_ptr<Implementation>
            implementation) noexcept;

    [[nodiscard]] CleanupResult discard_candidate(
        DiscardReason reason) noexcept;

    std::unique_ptr<Implementation> implementation_;
};

}
