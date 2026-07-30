#pragma once

#include <cstdint>

namespace asyncdownload::range {

using ByteOffset = std::int64_t;

struct RangeId {
    std::uint64_t value = 0;

    friend bool operator==(const RangeId&, const RangeId&) = default;
};

struct LeaseId {
    RangeId range{};
    std::uint64_t generation = 0;

    friend bool operator==(const LeaseId&, const LeaseId&) = default;
};

struct CompletionId {
    RangeId range{};
    std::uint64_t generation = 0;

    friend bool operator==(const CompletionId&, const CompletionId&) = default;
};

struct ByteSpan {
    ByteOffset begin = 0;
    ByteOffset end = 0;

    friend bool operator==(const ByteSpan&, const ByteSpan&) = default;
};

}
