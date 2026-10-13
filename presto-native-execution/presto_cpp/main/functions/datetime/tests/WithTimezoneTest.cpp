/*
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <gtest/gtest.h>

#include "presto_cpp/main/functions/datetime/DateTimeFunctionsRegistration.h"
#include "velox/common/base/tests/GTestUtils.h"
#include "velox/functions/prestosql/tests/utils/FunctionBaseTest.h"
#include "velox/functions/prestosql/types/TimestampWithTimeZoneType.h"

namespace facebook::presto::functions::test {
namespace {

using velox::Timestamp;

// 2020-01-01 10:00:00 as seconds since epoch, read as wall time or as a UTC
// instant depending on legacy_timestamp.
constexpr int64_t kJan1At10 = 1577872800;

class WithTimezoneTest : public velox::functions::test::FunctionBaseTest {
 protected:
  static void SetUpTestCase() {
    velox::functions::test::FunctionBaseTest::SetUpTestCase();
    registerDateTimeFunctions();
  }

  // Checks both the zone-column path, resolved per row, and the zone-literal
  // path, resolved once in initialize().
  void assertWithTimezone(
      const Timestamp& timestamp,
      const std::string& zone,
      int64_t expectedMillisUtc) {
    const auto expected =
        velox::pack(expectedMillisUtc, velox::tz::getTimeZoneID(zone));
    EXPECT_EQ(expected, withTimezone(timestamp, zone)) << zone;
    EXPECT_EQ(
        expected,
        evaluateOnce<int64_t>(
            fmt::format("with_timezone(c0, '{}')", zone),
            std::optional(timestamp)))
        << zone;
  }

  std::optional<int64_t> withTimezone(
      std::optional<Timestamp> timestamp,
      std::optional<std::string> zone) {
    return evaluateOnce<int64_t>(
        "with_timezone(c0, c1)",
        {velox::TIMESTAMP(), velox::VARCHAR()},
        timestamp,
        zone);
  }
};

TEST_F(WithTimezoneTest, wallTime) {
  // TIMESTAMP is wall time, so it is interpreted in the target zone.
  assertWithTimezone(
      Timestamp(kJan1At10, 123'000'000), "America/New_York", 1577890800123);
  assertWithTimezone(Timestamp(kJan1At10, 0), "UTC", 1577872800000);
  assertWithTimezone(Timestamp(kJan1At10, 0), "+05:30", 1577853000000);
  assertWithTimezone(Timestamp(kJan1At10, 0), "-08:00", 1577901600000);
}

TEST_F(WithTimezoneTest, daylightSavingTransitions) {
  // 2020-03-08 02:30:00 does not exist in America/New_York and is moved
  // forward by the gap to 03:30 EDT (07:30 UTC).
  assertWithTimezone(
      Timestamp(1583634600, 0), "America/New_York", 1583652600000);
  // 2020-11-01 01:30:00 occurs twice in America/New_York and resolves to the
  // earlier EDT instant (05:30 UTC).
  assertWithTimezone(
      Timestamp(1604194200, 0), "America/New_York", 1604208600000);
}

TEST_F(WithTimezoneTest, legacyTimestamp) {
  // legacy_timestamp=true maps to adjust_timestamp_to_session_timezone=true.
  // TIMESTAMP is then a UTC instant, so the instant is kept and only the zone
  // changes, regardless of the session zone. No DST adjustment applies.
  setTimezone("Pacific/Apia");
  assertWithTimezone(
      Timestamp(kJan1At10, 123'000'000), "America/New_York", 1577872800123);
  assertWithTimezone(
      Timestamp(1583634600, 0), "America/New_York", 1583634600000);
}

TEST_F(WithTimezoneTest, nulls) {
  EXPECT_EQ(std::nullopt, withTimezone(std::nullopt, "UTC"));
  EXPECT_EQ(std::nullopt, withTimezone(Timestamp(0, 0), std::nullopt));
}

TEST_F(WithTimezoneTest, invalidTimezone) {
  VELOX_ASSERT_THROW(
      withTimezone(Timestamp(0, 0), "Not/A/Zone"),
      "Unknown time zone: 'Not/A/Zone'");
  VELOX_ASSERT_THROW(
      evaluateOnce<int64_t>(
          "with_timezone(c0, 'Not/A/Zone')", std::optional(Timestamp(0, 0))),
      "Unknown time zone: 'Not/A/Zone'");
  VELOX_ASSERT_THROW(withTimezone(Timestamp(0, 0), ""), "Unknown time zone");
}

} // namespace
} // namespace facebook::presto::functions::test
