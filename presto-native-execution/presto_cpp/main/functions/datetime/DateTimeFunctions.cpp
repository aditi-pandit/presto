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
#include "presto_cpp/main/functions/datetime/DateTimeFunctionsRegistration.h"

#include <string_view>

#include "velox/core/QueryConfig.h"
#include "velox/functions/Macros.h"
#include "velox/functions/Registerer.h"
#include "velox/functions/prestosql/types/TimestampWithTimeZoneType.h"
#include "velox/type/tz/TimeZoneMap.h"

namespace facebook::presto::functions {

namespace {

/// with_timezone(timestamp, varchar) -> timestamp with time zone. Mirrors
/// TimestampOperators.toTimestampWithTimeZone in presto-main-base: with
/// legacy_timestamp (adjust_timestamp_to_session_timezone) the instant is kept,
/// otherwise the timestamp is wall time in the target zone.
template <typename T>
struct WithTimezoneFunction {
  VELOX_DEFINE_FUNCTION_TYPES(T);

  FOLLY_ALWAYS_INLINE void initialize(
      const std::vector<velox::TypePtr>& /*inputTypes*/,
      const velox::core::QueryConfig& config,
      const arg_type<velox::Timestamp>* /*timestamp*/,
      const arg_type<velox::Varchar>* zone) {
    legacyTimestamp_ = config.adjustTimestampToTimezone();
    if (zone != nullptr) {
      constantZone_ = locateTargetZone(*zone);
    }
  }

  FOLLY_ALWAYS_INLINE void call(
      out_type<velox::TimestampWithTimezone>& result,
      const arg_type<velox::Timestamp>& timestamp,
      const arg_type<velox::Varchar>& zone) {
    const auto* targetZone =
        constantZone_ != nullptr ? constantZone_ : locateTargetZone(zone);

    if (legacyTimestamp_) {
      result = velox::pack(timestamp, targetZone->id());
      return;
    }

    // Joda's convertLocalToUTC(local, false) moves a wall time in a DST gap
    // forward by the gap. toGMT fails on such times, so correct them first;
    // toGMT already resolves DST overlaps to the earlier instant.
    velox::Timestamp utc(
        targetZone
            ->correct_nonexistent_time(
                std::chrono::seconds(timestamp.getSeconds()))
            .count(),
        timestamp.getNanos());
    utc.toGMT(*targetZone);
    result = velox::pack(utc, targetZone->id());
  }

 private:
  // Throws a user error for unknown or empty zone names.
  static const velox::tz::TimeZone* locateTargetZone(
      const arg_type<velox::Varchar>& zone) {
    return velox::tz::locateZone(std::string_view(zone.data(), zone.size()));
  }

  bool legacyTimestamp_{false};
  // Set when the zone argument is constant, so it is resolved once per query.
  const velox::tz::TimeZone* constantZone_{nullptr};
};

} // namespace

void registerDateTimeFunctions(const std::string& prefix) {
  velox::registerFunction<
      WithTimezoneFunction,
      velox::TimestampWithTimezone,
      velox::Timestamp,
      velox::Varchar>({prefix + "with_timezone"});
}

} // namespace facebook::presto::functions
