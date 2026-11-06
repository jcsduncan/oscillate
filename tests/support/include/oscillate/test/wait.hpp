#pragma once

// Waiting on another thread without making time the oracle: wait_until polls
// a condition and fails only when a deadline far beyond any loaded run passes
// (a hang guard, not a timing claim). Prefer a fake clock or a callback the
// code under test signals; use this where the code exposes neither.
//
//   oscillate::test::wait_until([&] { return monitor.latest().has_value(); },
//                               "the spectrum worker published a reading");

#include "oscillate/test/check.hpp"

#include <chrono>
#include <source_location>
#include <string_view>
#include <thread>

namespace oscillate::test {

// Long enough that only a hang reaches it on a loaded sanitizer run.
inline constexpr std::chrono::seconds default_wait_deadline{60};
// Between polls the waiting thread sleeps this long, so it does not spin a
// core the worker needs.
inline constexpr std::chrono::milliseconds wait_poll_interval{1};

// Returns once `ready()` is true; fails with `what` at the deadline.
template <typename Ready>
void wait_until(Ready&& ready, std::string_view what,
                std::chrono::steady_clock::duration deadline = default_wait_deadline,
                std::chrono::steady_clock::duration poll = wait_poll_interval,
                std::source_location where = std::source_location::current()) {
  const auto give_up = std::chrono::steady_clock::now() + deadline;
  while (!ready()) {
    if (std::chrono::steady_clock::now() >= give_up) {
      detail::fail("timed out waiting", what, where);
    }
    std::this_thread::sleep_for(poll);
  }
}

} // namespace oscillate::test
