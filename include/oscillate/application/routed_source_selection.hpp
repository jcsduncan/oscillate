#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>

namespace oscillate::application {

// A source and the graph revision whose source-fill callback may use it.
// Source destruction may join decoder workers, so owners are retired off the
// callback and handed to an off-UI retirement task.
struct RoutedSourceBinding {
  std::uint64_t revision = 0;
  std::shared_ptr<void> source;
};

// A source-selection primitive, not a live-edit adapter. The control owner
// serializes all mutation. Exactly one audio callback enters at a time. Graph
// publication must still prepare an exact future source window before holding.
class RoutedSourceSelection final {
  struct Descriptor;

public:
  class CallbackLease final {
  public:
    CallbackLease(const CallbackLease&) = delete;
    CallbackLease& operator=(const CallbackLease&) = delete;
    CallbackLease(CallbackLease&& other) noexcept;
    CallbackLease& operator=(CallbackLease&&) = delete;
    ~CallbackLease();

    // One descriptor is sampled for the whole callback. A failed or stale
    // candidate revision cannot consume the active source by accident.
    [[nodiscard]] void* source_for(std::uint64_t revision) const noexcept;
    // A graph-only revision continues to use the active source. The exact
    // candidate revision selects its newly prepared source instead.
    [[nodiscard]] void* source_for_graph_revision(std::uint64_t revision) const noexcept;

  private:
    friend class RoutedSourceSelection;
    CallbackLease(RoutedSourceSelection& owner, const Descriptor* descriptor) noexcept;
    RoutedSourceSelection* owner_;
    const Descriptor* descriptor_;
  };

  class Retired final {
  public:
    Retired(const Retired&) = delete;
    Retired& operator=(const Retired&) = delete;
    Retired(Retired&&) noexcept;
    Retired& operator=(Retired&&) noexcept;
    ~Retired();

  private:
    friend class RoutedSourceSelection;
    explicit Retired(std::unique_ptr<Descriptor> descriptor) noexcept;
    std::unique_ptr<Descriptor> descriptor_;
  };

  explicit RoutedSourceSelection(RoutedSourceBinding initial);
  RoutedSourceSelection(const RoutedSourceSelection&) = delete;
  RoutedSourceSelection& operator=(const RoutedSourceSelection&) = delete;
  // Stop/join the callback before destroying the selector itself. Destruction
  // of the current source can join its worker, so destroy off the UI thread.
  ~RoutedSourceSelection();

  // Control only. One candidate and one retired descriptor are the fixed
  // bounds. Failure leaves the published selection unchanged. A cancelled
  // candidate is never promoted by an unrelated graph revision.
  // Stage only after off-thread preparation and exact future-block readiness.
  // Promote only after the matching graph block has rendered and activated;
  // cancel only while the candidate graph has failed or remains unpublished.
  [[nodiscard]] bool stage_candidate(RoutedSourceBinding candidate);
  [[nodiscard]] bool promote_candidate(std::uint64_t revision);
  [[nodiscard]] bool cancel_candidate(std::uint64_t revision);

  // Callback only. The lease must cover all use of source_for() and rendering
  // through the returned pointer; its destructor publishes callback exit.
  [[nodiscard]] CallbackLease enter_callback() noexcept;

  // Control only. A returned owner bundle is safe to destroy after this
  // call, but source destruction/worker joining belongs on a retirement
  // worker, never the UI or callback. Polling never waits for the callback.
  [[nodiscard]] std::optional<Retired> take_retired_if_quiescent() noexcept;

private:
  [[nodiscard]] bool replace(std::unique_ptr<Descriptor> next) noexcept;

  std::unique_ptr<Descriptor> current_;
  std::unique_ptr<Descriptor> retired_;
  std::atomic<const Descriptor*> published_{nullptr};
  // One callback increments on entry (odd) and exit (even). All entry,
  // publication, and exit operations are seq_cst so a callback starting after
  // publication cannot still load the predecessor descriptor.
  std::atomic<std::uint64_t> callback_epoch_{0};
  std::uint64_t retire_after_epoch_ = 0; // control only
  static_assert(std::atomic<const Descriptor*>::is_always_lock_free);
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
};

} // namespace oscillate::application
