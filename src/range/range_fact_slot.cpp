#include "range_fact_slot.hpp"

#include "asyncdownload/error.hpp"

#include <algorithm>

namespace asyncdownload::range {

RangeFactSlot::RangeFactSlot(
    const RangeId range,
    const ByteOffset initial_offset) noexcept
    : range_(range),
      persisted_through_(initial_offset) {}

RangeFactPublisher RangeFactSlot::publisher() noexcept {
    return RangeFactPublisher(*this);
}

std::optional<RangeFactSnapshot>
RangeFactSlot::read_since(
    const std::uint64_t last_revision) const noexcept {
    const auto revision =
        revision_.load(std::memory_order_acquire);
    if (revision == last_revision) {
        return std::nullopt;
    }
    return RangeFactSnapshot{
        persisted_through_.load(std::memory_order_acquire),
        gap_paused_.load(std::memory_order_acquire),
        committed_generation_.load(std::memory_order_acquire),
        revision
    };
}

RangeFactPublisher::RangeFactPublisher(
    RangeFactSlot& slot) noexcept
    : slot_(&slot) {}

void RangeFactPublisher::publish_persisted_through(
    const ByteOffset offset) noexcept {
    if (slot_ == nullptr) {
        return;
    }
    const auto current =
        slot_->persisted_through_.load(
            std::memory_order_relaxed);
    slot_->persisted_through_.store(
        std::max(current, offset),
        std::memory_order_release);
    slot_->revision_.fetch_add(
        1,
        std::memory_order_release);
}

void RangeFactPublisher::publish_gap_pause(
    const bool active) noexcept {
    if (slot_ == nullptr) {
        return;
    }
    slot_->gap_paused_.store(
        active,
        std::memory_order_release);
    slot_->revision_.fetch_add(
        1,
        std::memory_order_release);
}

std::error_code RangeFactPublisher::publish_committed(
    const CompletionId completion,
    const ByteOffset offset) noexcept {
    if (slot_ == nullptr) {
        return {};
    }
    if (completion.range != slot_->range_ ||
        completion.generation == 0) {
        return make_error_code(DownloadErrc::internal_error);
    }
    const auto committed =
        slot_->committed_generation_.load(
            std::memory_order_acquire);
    if (committed != 0 &&
        committed != completion.generation) {
        return make_error_code(DownloadErrc::internal_error);
    }
    const auto persisted =
        slot_->persisted_through_.load(
            std::memory_order_relaxed);
    slot_->persisted_through_.store(
        std::max(persisted, offset),
        std::memory_order_release);
    slot_->committed_generation_.store(
        completion.generation,
        std::memory_order_release);
    slot_->revision_.fetch_add(
        1,
        std::memory_order_release);
    return {};
}

}
