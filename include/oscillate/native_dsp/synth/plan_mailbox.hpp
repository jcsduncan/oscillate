#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace oscillate::native_dsp::synth {

// Hands immutable plans (each with a `revision`) from one control thread to
// the audio callback without locks. The control thread owns every plan it
// offered; the callback reads the newest through a release/acquire pointer
// and reports the revision it rendered, after which older plans retire. A
// superseded plan stays owned until the callback has moved past it, so the
// callback never reads a freed plan. Used by the synth player and the
// built-in effect stage for their automation lanes.
template <class Plan> class PlanMailbox {
public:
  // Offers the callback has not yet moved past; a full mailbox refuses more.
  static constexpr std::size_t max_owned_plans = 8;

  // Control, before the first callback: the first plan, already applied.
  void install(std::unique_ptr<Plan> plan, std::uint64_t revision) {
    auto* first = plan.get();
    owned_.push_back(std::move(plan));
    last_offered_ = revision;
    offered_.store(first, std::memory_order_release);
    applied_.store(revision, std::memory_order_release);
  }

  // Control: true when `revision` is not newer than the last offer.
  [[nodiscard]] bool stale(std::uint64_t revision) const noexcept {
    return revision <= last_offered_;
  }
  // Control: retires the plans the callback has moved past; false when the
  // mailbox is still full.
  [[nodiscard]] bool make_room() {
    const auto applied = applied_.load(std::memory_order_acquire);
    std::erase_if(owned_, [applied](const auto& plan) { return plan->revision < applied; });
    return owned_.size() < max_owned_plans;
  }
  // Control: offers a plan built for `revision`. Throws std::bad_alloc,
  // leaving the current offer in place.
  void offer(std::unique_ptr<Plan> plan, std::uint64_t revision) {
    auto* offered = plan.get();
    owned_.push_back(std::move(plan));
    last_offered_ = revision;
    offered_.store(offered, std::memory_order_release);
  }

  // Callback (or control): the newest offer; null before install().
  [[nodiscard]] Plan* current() const noexcept { return offered_.load(std::memory_order_acquire); }
  // Callback: a block rendered with the plan of `revision`.
  void applied(std::uint64_t revision) noexcept {
    applied_.store(revision, std::memory_order_release);
  }
  [[nodiscard]] std::uint64_t applied_revision() const noexcept {
    return applied_.load(std::memory_order_acquire);
  }

private:
  std::vector<std::unique_ptr<Plan>> owned_;
  std::atomic<Plan*> offered_{nullptr};
  std::atomic<std::uint64_t> applied_{0};
  std::uint64_t last_offered_ = 0; // The serialized control producer only.
};

} // namespace oscillate::native_dsp::synth
