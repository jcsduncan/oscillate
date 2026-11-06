#include "oscillate/application/routed_source_selection.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/check.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

namespace {
using oscillate::application::RoutedSourceBinding;
using oscillate::application::RoutedSourceSelection;

using oscillate::test::require;

struct Source {
  int value;
  std::atomic<int>* destroyed;
  ~Source() { destroyed->fetch_add(1, std::memory_order_relaxed); }
};

RoutedSourceBinding binding(std::uint64_t revision, int value, std::atomic<int>& destroyed) {
  return {revision, std::make_shared<Source>(value, &destroyed)};
}

void failure_cancel_and_reclamation() {
  std::atomic<int> old_destroyed{0};
  std::atomic<int> candidate_destroyed{0};
  std::atomic<int> next_destroyed{0};
  RoutedSourceSelection selector(binding(1, 11, old_destroyed));

  std::atomic<bool> entered{false};
  std::atomic<bool> release{false};
  std::atomic<int> callback_value{0};
  std::thread callback([&] {
    auto lease = selector.enter_callback();
    auto* old = static_cast<Source*>(lease.source_for(1));
    entered.store(true, std::memory_order_release);
    while (!release.load(std::memory_order_acquire))
      std::this_thread::yield();
    callback_value.store(old == nullptr ? -1 : old->value, std::memory_order_relaxed);
  });
  while (!entered.load(std::memory_order_acquire))
    std::this_thread::yield();

  const bool staged = selector.stage_candidate(binding(2, 22, candidate_destroyed));
  const bool rejected_while_busy = !selector.take_retired_if_quiescent() &&
                                   !selector.stage_candidate(binding(3, 33, next_destroyed)) &&
                                   !selector.promote_candidate(3) && !selector.cancel_candidate(3);
  release.store(true, std::memory_order_release);
  callback.join();
  REQUIRE_TRUE(staged, "prepublication callback lost its source or accepted a second candidate");
  REQUIRE_TRUE(rejected_while_busy,
               "prepublication callback lost its source or accepted a second candidate");
  REQUIRE_EQ(callback_value.load(std::memory_order_relaxed), 11,
             "prepublication callback lost its source or accepted a second candidate");
  REQUIRE_EQ(old_destroyed.load(std::memory_order_relaxed), 0,
             "prepublication callback lost its source or accepted a second candidate");
  auto retired_first = selector.take_retired_if_quiescent();
  REQUIRE_OK(retired_first,
             "callback completion did not release exactly one predecessor descriptor");
  REQUIRE_TRUE(!selector.take_retired_if_quiescent(),
               "callback completion did not release exactly one predecessor descriptor");
  retired_first.reset();

  {
    counting_allocations = true;
    const auto before = counted_allocations;
    bool found = false;
    {
      auto lease = selector.enter_callback();
      const auto* active = static_cast<Source*>(lease.source_for(1));
      const auto* candidate = static_cast<Source*>(lease.source_for(2));
      found = active != nullptr && active->value == 11 && candidate != nullptr &&
              candidate->value == 22 && lease.source_for(3) == nullptr &&
              lease.source_for_graph_revision(1) == active &&
              lease.source_for_graph_revision(2) == candidate &&
              lease.source_for_graph_revision(3) == nullptr;
    }
    const auto after = counted_allocations;
    counting_allocations = false;
    REQUIRE_TRUE(found, "callback entry, selection or exit allocated or changed revision");
    REQUIRE_EQ(before, after, "callback entry, selection or exit allocated or changed revision");
  }
  std::atomic<bool> candidate_entered{false};
  std::atomic<bool> candidate_release{false};
  std::atomic<int> candidate_callback_value{0};
  std::thread candidate_callback([&] {
    auto lease = selector.enter_callback();
    auto* candidate = static_cast<Source*>(lease.source_for(2));
    candidate_entered.store(true, std::memory_order_release);
    while (!candidate_release.load(std::memory_order_acquire))
      std::this_thread::yield();
    candidate_callback_value.store(candidate == nullptr ? -1 : candidate->value,
                                   std::memory_order_relaxed);
  });
  while (!candidate_entered.load(std::memory_order_acquire))
    std::this_thread::yield();
  const bool cancelled_safely = selector.cancel_candidate(2) &&
                                !selector.take_retired_if_quiescent() &&
                                candidate_destroyed.load(std::memory_order_relaxed) == 0;
  candidate_release.store(true, std::memory_order_release);
  candidate_callback.join();
  REQUIRE_TRUE(cancelled_safely, "cancelled candidate was destroyed before its callback exited");
  REQUIRE_EQ(candidate_callback_value.load(std::memory_order_relaxed), 22,
             "cancelled candidate was destroyed before its callback exited");
  {
    auto lease = selector.enter_callback();
    REQUIRE_EQ(lease.source_for(2), nullptr,
               "cancelled candidate remained selectable or muted the old source");
    REQUIRE_EQ(static_cast<Source*>(lease.source_for(1))->value, 11,
               "cancelled candidate remained selectable or muted the old source");
    REQUIRE_EQ(lease.source_for_graph_revision(3), lease.source_for(1),
               "cancelled candidate remained selectable or muted the old source");
  }
  auto cancelled = selector.take_retired_if_quiescent();
  REQUIRE_OK(cancelled, "cancelled candidate was released before explicit retirement");
  REQUIRE_EQ(candidate_destroyed.load(std::memory_order_relaxed), 0,
             "cancelled candidate was released before explicit retirement");
  std::thread cancel_retirement([owner = std::move(cancelled)]() mutable { owner.reset(); });
  cancel_retirement.join();
  REQUIRE_EQ(candidate_destroyed.load(std::memory_order_relaxed), 1,
             "cancelled candidate owner was not returned for retirement");

  REQUIRE_TRUE(!selector.stage_candidate(binding(1, 0, next_destroyed)),
               "stale revision was accepted or a new candidate could not be staged");
  REQUIRE_TRUE(selector.stage_candidate(binding(4, 44, next_destroyed)),
               "stale revision was accepted or a new candidate could not be staged");
  selector.take_retired_if_quiescent().reset();
  std::atomic<bool> old_entered{false};
  std::atomic<bool> old_release{false};
  std::atomic<int> old_callback_value{0};
  std::thread old_callback([&] {
    auto lease = selector.enter_callback();
    auto* old = static_cast<Source*>(lease.source_for(1));
    old_entered.store(true, std::memory_order_release);
    while (!old_release.load(std::memory_order_acquire))
      std::this_thread::yield();
    old_callback_value.store(old == nullptr ? -1 : old->value, std::memory_order_relaxed);
  });
  while (!old_entered.load(std::memory_order_acquire))
    std::this_thread::yield();
  const bool promoted_safely = selector.promote_candidate(4) &&
                               !selector.take_retired_if_quiescent() &&
                               old_destroyed.load(std::memory_order_relaxed) == 0;
  old_release.store(true, std::memory_order_release);
  old_callback.join();
  REQUIRE_TRUE(promoted_safely,
               "promoted graph reclaimed an old source while its callback still rendered");
  REQUIRE_EQ(old_callback_value.load(std::memory_order_relaxed), 11,
             "promoted graph reclaimed an old source while its callback still rendered");
  {
    auto lease = selector.enter_callback();
    REQUIRE_EQ(lease.source_for(1), nullptr,
               "promoted candidate did not become the only active source");
    REQUIRE_EQ(static_cast<Source*>(lease.source_for(4))->value, 44,
               "promoted candidate did not become the only active source");
  }
  auto predecessor = selector.take_retired_if_quiescent();
  REQUIRE_OK(predecessor, "old source was reclaimed before callback grace period ended");
  REQUIRE_EQ(old_destroyed.load(std::memory_order_relaxed), 0,
             "old source was reclaimed before callback grace period ended");
  std::thread predecessor_retirement([owner = std::move(predecessor)]() mutable { owner.reset(); });
  predecessor_retirement.join();
  REQUIRE_EQ(old_destroyed.load(std::memory_order_relaxed), 1,
             "old source was not returned for off-thread retirement");
}
} // namespace

int main() {
  try {
    failure_cancel_and_reclamation();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
