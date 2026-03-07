#include "core/block_bitmap.hpp"

#include <gtest/gtest.h>

TEST(BlockBitmapTest, MarksRealDownloadPartitionAsFinished) {
    constexpr std::int64_t total_size = 207366576;
    constexpr std::size_t block_size = 64 * 1024;

    asyncdownload::core::AtomicBlockBitmap bitmap(
        asyncdownload::core::required_block_count(total_size, block_size));

    struct Range {
        std::int64_t start;
        std::int64_t persisted;
    };

    const Range ranges[] = {
        {0, 51904512},
        {51904512, 103743488},
        {103743488, 146866176},
        {146866176, 148111360},
        {148111360, 150601728},
        {150601728, 155582464},
        {155582464, 204537856},
        {204537856, 207366576},
    };

    for (const auto& range : ranges) {
        bitmap.mark_finished_range(range.start, range.persisted, block_size, total_size);
    }

    EXPECT_EQ(bitmap.contiguous_finished_bytes(block_size, total_size), total_size);
}
