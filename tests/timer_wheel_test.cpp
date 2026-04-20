#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "nimblefix/runtime/timer_wheel.h"

#include "test_support.h"

namespace {

auto
PopExpired(nimble::runtime::TimerWheel* wheel, std::uint64_t now_ns) -> std::vector<std::uint64_t>
{
  std::vector<std::uint64_t> expired;
  wheel->PopExpired(now_ns, &expired);
  return expired;
}

} // namespace

TEST_CASE("timer-wheel", "[timer-wheel]")
{
  SECTION("basic scheduling and expiration")
  {
    nimble::runtime::TimerWheel wheel(nimble::runtime::TimerWheelOptions{ .tick_ns = 10U, .slot_count = 4U });
    wheel.Schedule(1U, 10U, 0U);
    wheel.Schedule(2U, 80U, 0U);

    REQUIRE(wheel.NextDeadline().has_value());
    REQUIRE(wheel.NextDeadline().value() == 10U);
    REQUIRE(PopExpired(&wheel, 9U).empty());

    const auto expired = PopExpired(&wheel, 10U);
    REQUIRE(expired.size() == 1U);
    REQUIRE(expired.front() == 1U);
    REQUIRE(wheel.NextDeadline().has_value());
    REQUIRE(wheel.NextDeadline().value() == 80U);
  }

  SECTION("reschedule and cancel")
  {
    nimble::runtime::TimerWheel wheel(nimble::runtime::TimerWheelOptions{ .tick_ns = 10U, .slot_count = 4U });
    wheel.Schedule(3U, 80U, 0U);
    wheel.Schedule(4U, 90U, 0U);
    wheel.Schedule(3U, 30U, 10U);
    wheel.Cancel(4U);

    REQUIRE(wheel.NextDeadline().has_value());
    REQUIRE(wheel.NextDeadline().value() == 30U);
    REQUIRE(PopExpired(&wheel, 29U).empty());

    const auto expired = PopExpired(&wheel, 30U);
    REQUIRE(expired.size() == 1U);
    REQUIRE(expired.front() == 3U);
    REQUIRE(!wheel.NextDeadline().has_value());
  }

  SECTION("late expiration")
  {
    nimble::runtime::TimerWheel wheel(nimble::runtime::TimerWheelOptions{ .tick_ns = 10U, .slot_count = 4U });
    wheel.Schedule(5U, 15U, 40U);

    REQUIRE(wheel.NextDeadline().has_value());
    REQUIRE(wheel.NextDeadline().value() == 15U);

    const auto expired = PopExpired(&wheel, 40U);
    REQUIRE(expired.size() == 1U);
    REQUIRE(expired.front() == 5U);
    REQUIRE(!wheel.NextDeadline().has_value());
  }
}