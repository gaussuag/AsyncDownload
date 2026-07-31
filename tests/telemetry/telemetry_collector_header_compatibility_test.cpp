#include <gtest/gtest.h>

#include "asyncdownload/telemetry/telemetry_collector.hpp"

namespace {

TEST(TelemetryHeaderCompatibilityTest, CollectorHeaderExposesSession) {
    asyncdownload::telemetry::TelemetrySession session;
    asyncdownload::telemetry::TelemetryCollector collector;

    EXPECT_EQ(session.final_summary().total_pause_count, 0U);
    EXPECT_EQ(collector.final_summary().total_pause_count, 0U);
}

}
