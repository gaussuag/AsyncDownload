#include "core/memory_accounting.hpp"

#include <gtest/gtest.h>

TEST(MemoryAccountingTest, AllowsFirstPacketAboveHighWatermark) {
    EXPECT_FALSE(asyncdownload::core::should_pause_for_backpressure(0, 128 * 1024, 1024));
}

TEST(MemoryAccountingTest, PausesWhenQueuedBytesAlreadyExistAndThresholdWouldBeExceeded) {
    EXPECT_TRUE(asyncdownload::core::should_pause_for_backpressure(64 * 1024, 64 * 1024, 96 * 1024));
}

TEST(MemoryAccountingTest, IgnoresEmptyIncomingPacket) {
    EXPECT_FALSE(asyncdownload::core::should_pause_for_backpressure(64 * 1024, 0, 1));
}
