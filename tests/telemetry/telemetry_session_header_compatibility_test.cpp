#include <gtest/gtest.h>

#include "asyncdownload/telemetry/telemetry_session.hpp"

namespace {

TEST(TelemetryHeaderCompatibilityTest, SessionHeaderExposesCollector) {
    asyncdownload::telemetry::TelemetrySession session;
    asyncdownload::telemetry::TelemetryCollector collector;

    EXPECT_EQ(session.final_summary().total_pause_count, 0U);
    EXPECT_EQ(collector.final_summary().total_pause_count, 0U);
}

}
