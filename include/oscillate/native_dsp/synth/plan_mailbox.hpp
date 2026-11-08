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

  // Room for every plan up front, so an offer into a mailbox with room
  // never reallocates (and so never throws).
  PlanMailbox() { owned_.reserve(max_owned_plans); }

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
  // Control: true when `revision` is older than the last offer or stage. A
  // switch withdrawn before it took over stages the same revision again.
  [[nodiscard]] bool stale_for_stage(std::uint64_t revision) const noexcept {
    return revision < last_offered_;
  }
  // Control: retires the plans the callback has moved past; false when the
  // mailbox is still full.
  [[nodiscard]] bool make_room() {
    const auto applied = applied_.load(std::memory_order_acquire);
    std::erase_if(owned_, [applied](const auto& plan) { return plan->revision < applied; });
    return owned_.size() < max_owned_plans;
  }
  // Control: offers a plan built for `revision`. After make_room() returned
  // true it cannot throw.
  void offer(std::unique_ptr<Plan> plan, std::uint64_t revision) {
    auto* offered = plan.get();
    owned_.push_back(std::move(plan));
    last_offered_ = revision;
    drop_staged();
    offered_.store(offered, std::memory_order_release);
  }
  // Control: holds a plan built for `revision` back until the callback
  // releases it (release_staged) at the block a live switch of that revision
  // takes over, so the plan and the switch start on the same block. A later
  // offer or stage supersedes it. After make_room() returned true it cannot
  // throw.
  void stage(std::unique_ptr<Plan> plan, std::uint64_t revision) {
    drop_staged();
    auto* staged = plan.get();
    owned_.push_back(std::move(plan));
    last_offered_ = revision;
    staged_.store(staged, std::memory_order_release);
  }

  // Callback (or control): the newest offer; null before install().
  [[nodiscard]] Plan* current() const noexcept { return offered_.load(std::memory_order_acquire); }
  // Callback: the plan staged for `revision`, if any, becomes the current
  // one. The control stages only while that switch is pending and offers
  // nothing else meanwhile, so the two never race.
  void release_staged(std::uint64_t revision) noexcept {
    auto* staged = staged_.load(std::memory_order_acquire);
    if (staged == nullptr || staged->revision != revision)
      return;
    staged_.store(nullptr, std::memory_order_relaxed);
    offered_.store(staged, std::memory_order_release);
  }
  // Callback: a block rendered with the plan of `revision`.
  void applied(std::uint64_t revision) noexcept {
    applied_.store(revision, std::memory_order_release);
  }
  [[nodiscard]] std::uint64_t applied_revision() const noexcept {
    return applied_.load(std::memory_order_acquire);
  }

private:
  // Control: frees a staged plan whose switch never took over. Nothing
  // pending can release it any more (a stage or offer happens only once
  // that switch is withdrawn), and the callback never read it.
  void drop_staged() noexcept {
    auto* superseded = staged_.exchange(nullptr, std::memory_order_acq_rel);
    if (superseded != nullptr)
      std::erase_if(owned_, [superseded](const auto& plan) { return plan.get() == superseded; });
  }

  std::vector<std::unique_ptr<Plan>> owned_;
  std::atomic<Plan*> offered_{nullptr};
  std::atomic<Plan*> staged_{nullptr};
  std::atomic<std::uint64_t> applied_{0};
  std::uint64_t last_offered_ = 0; // The serialized control producer only.
};

} // namespace oscillate::native_dsp::synth
