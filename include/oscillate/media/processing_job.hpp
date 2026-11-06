#pragma once

#include <oscillate/foundation/error.hpp>
#include <oscillate/media/cache_identity.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>

namespace oscillate::media {

// Immutable request identity. Workers may check cancelled() to stop early,
// but cancellation is advisory: a completion must pass the final control-side
// claim even if the worker never observed cancellation.
class JobTicket {
public:
  [[nodiscard]] const CacheKey& key() const noexcept { return key_; }
  [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
  [[nodiscard]] bool cancelled() const noexcept {
    return !state_ || state_->load(std::memory_order_acquire) != State::active;
  }

private:
  friend class CompletionGate;
  enum class State : std::uint8_t { active, cancelled, claimed };
  JobTicket(CacheKey key, std::uint64_t generation, std::shared_ptr<std::atomic<State>> state)
      : key_(key), generation_(generation), state_(std::move(state)) {}
  CacheKey key_;
  std::uint64_t generation_;
  std::shared_ptr<std::atomic<State>> state_;
};

// All methods except JobTicket::cancelled() run on one serialized control
// sequencer. issue() supersedes its prior ticket. The caller recomputes the key
// from the current authoritative inputs and validates output bytes before
// claim_install(). A successful claim is one-shot; no file or project mutation
// occurs here. Separate gates serve independently superseded artifact slots.
class CompletionGate {
public:
  CompletionGate() = default;
  CompletionGate(const CompletionGate&) = delete;
  CompletionGate& operator=(const CompletionGate&) = delete;
  CompletionGate(CompletionGate&&) = delete;
  CompletionGate& operator=(CompletionGate&&) = delete;
  ~CompletionGate() { cancel(); }

  [[nodiscard]] foundation::Result<JobTicket> issue(CacheKey key);
  void cancel() noexcept;
  [[nodiscard]] bool claim_install(const JobTicket& ticket,
                                   const CacheKey& current_inputs) noexcept;

private:
  static_assert(std::atomic<JobTicket::State>::is_always_lock_free);
  std::uint64_t generation_ = 0;
  std::optional<JobTicket> current_;
};

} // namespace oscillate::media
