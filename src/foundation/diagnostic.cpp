#include "oscillate/foundation/diagnostic.hpp"

namespace oscillate::foundation {
namespace {
constexpr std::uint64_t mask = DiagnosticRing::capacity - 1;
constinit DiagnosticRing process_ring;
} // namespace

// A cell at index i is free for position p when turn == p - i, and holds
// p's record when turn == p - i + 1; taking it sets turn to p + capacity - i.
bool DiagnosticRing::try_publish(DiagnosticEvent event) noexcept {
  auto position = head_.load(std::memory_order_relaxed);
  for (;;) {
    Cell& cell = cells_[position & mask];
    const auto index = position & mask;
    const auto turn = cell.turn.load(std::memory_order_acquire);
    const auto difference =
        static_cast<std::int64_t>(turn) - static_cast<std::int64_t>(position - index);
    if (difference == 0) {
      if (head_.compare_exchange_weak(position, position + 1, std::memory_order_relaxed)) {
        cell.event = event;
        cell.turn.store(position - index + 1, std::memory_order_release);
        return true;
      }
    } else if (difference < 0) {
      dropped_.fetch_add(1, std::memory_order_relaxed);
      return false;
    } else {
      position = head_.load(std::memory_order_relaxed);
    }
  }
}

bool DiagnosticRing::try_take(DiagnosticEvent& event) noexcept {
  auto position = tail_.load(std::memory_order_relaxed);
  for (;;) {
    Cell& cell = cells_[position & mask];
    const auto index = position & mask;
    const auto turn = cell.turn.load(std::memory_order_acquire);
    const auto difference =
        static_cast<std::int64_t>(turn) - static_cast<std::int64_t>(position - index + 1);
    if (difference == 0) {
      if (tail_.compare_exchange_weak(position, position + 1, std::memory_order_relaxed)) {
        event = cell.event;
        cell.turn.store(position - index + capacity, std::memory_order_release);
        return true;
      }
    } else if (difference < 0) {
      return false;
    } else {
      position = tail_.load(std::memory_order_relaxed);
    }
  }
}

std::uint64_t DiagnosticRing::take_dropped() noexcept {
  return dropped_.exchange(0, std::memory_order_relaxed);
}

DiagnosticRing& realtime_diagnostics() noexcept { return process_ring; }

} // namespace oscillate::foundation
