#include <cstdint>
#include <limits>

#include <gtest/gtest.h>

#include "core/block_geometry.hpp"

TEST(BlockGeometryTest, RoundsMaximumInt64TotalUp) {
    const auto total = std::numeric_limits<std::int64_t>::max();
    const auto result =
        asyncdownload::core::required_block_count(total, 4096);

    if constexpr (sizeof(std::size_t) < sizeof(std::int64_t)) {
        EXPECT_FALSE(result.has_value());
    } else {
        const auto expected = static_cast<std::size_t>(total / 4096 + 1);
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(*result, expected);
    }
}

TEST(BlockGeometryTest, RejectsInvalidSizes) {
    EXPECT_FALSE(
        asyncdownload::core::required_block_count(0, 4096).has_value());
    EXPECT_FALSE(
        asyncdownload::core::required_block_count(-1, 4096).has_value());
    EXPECT_FALSE(
        asyncdownload::core::required_block_count(1, 0).has_value());
}

TEST(BlockGeometryTest, LargeBlockStillCoversOnePositiveObject) {
    const auto result =
        asyncdownload::core::required_block_count(
            1,
            static_cast<std::size_t>(
                std::numeric_limits<std::int64_t>::max()));

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 1U);
}
