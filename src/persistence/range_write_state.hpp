#pragma once

#include "core/constants.hpp"
#include "flow/packet_flow.hpp"
#include "range/range_fact_slot.hpp"
#include "range/range_types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>

namespace asyncdownload::core {
struct RangeContext;
}

namespace asyncdownload::persistence {

struct RangeTailBuffer {
    std::array<
        std::uint8_t,
        core::TAIL_BUFFER_CAPACITY_BYTES> data{};
    std::size_t length = 0;
    range::ByteOffset offset = 0;
};

struct RangeWriteState {
    range::RangeId id{};
    range::ByteSpan bytes{};
    std::uint64_t geometry_revision = 0;
    std::uint64_t last_observed_lease_generation = 0;
    std::optional<range::ByteSpan>
        last_observed_lease_span;
    range::ByteOffset observed_dispatch_through = 0;
    range::ByteOffset persisted_through = 0;
    bool gap_blocked = false;
    bool local_failure = false;
    bool observed_activity = false;
    bool committed = false;
    RangeTailBuffer tail;
    std::map<range::ByteOffset, flow::PacketLease>
        out_of_order;
    std::optional<range::CompletionId>
        pending_completion;
    range::RangeFactPublisher facts;
    core::RangeContext* legacy_projection = nullptr;
};

}
