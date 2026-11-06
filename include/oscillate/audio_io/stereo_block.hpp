#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include <oscillate/engine/audio_worker_pool.hpp>
#include <oscillate/engine/graph.hpp>

namespace oscillate::audio_io {

class Metronome;

// Non-owning callback seam. The function and context outlive the output adapter.
// Rendering is called only from the device callback and must be real-time safe.
struct StereoRenderSource {
  void* context = nullptr;
  bool (*render)(void*, std::span<engine::StereoFrame>, engine::BlockContext) noexcept = nullptr;
  // Optional callback-only observation after render(). A true result means the
  // renderer emitted a bounded silent hold block: replay its exact project
  // sample/generation on the next callback. Existing sources leave this null.
  bool (*freeze_cursor)(void*) noexcept = nullptr;
  // Optional helpers for render() (ADR-0048). An output that can give
  // threads the callback's real-time priority starts wanted_workers() of
  // them on run_worker() while it streams; others leave the pool idle, and
  // the renderer then works on the callback thread alone.
  engine::AudioWorkerPool* workers = nullptr;
};

enum class BlockResult : std::uint8_t;

// When the device will play the block being rendered: the CLOCK_MONOTONIC
// time its first frame is heard, and the rate frames advance at. Zero
// means unknown (no audible point is published).
struct BlockTiming {
  std::int64_t first_frame_heard_ns = 0;
  std::uint32_t sample_rate = 0;
};

// Callback-owned cursor. One control thread may queue one request (a seek, a
// planned loop wrap or an in-place restart) while processing; another request
// is rejected until the callback consumes or the control thread withdraws the
// first. Any request increments generation at a block boundary. No lock,
// allocation or project-model access occurs in next_block(). The owner must
// quiesce the callback before destroying this cursor.
class TransportCursor {
public:
  struct CompletedPosition {
    std::uint64_t next_sample;
    std::uint64_t generation;
  };
  // One render part: a whole device block, or the part of it before or after
  // a scheduled loop boundary.
  struct Part {
    engine::BlockContext context;
    std::size_t frames;
  };
  // Callback-written counters, readable from any thread.
  struct LoopWrapCounts {
    std::uint64_t applied = 0; // planned wraps the callback applied
    std::uint64_t late = 0;    // scheduled wraps applied after their boundary
  };

  explicit TransportCursor(std::uint64_t first_sample = 0) noexcept;
  TransportCursor(const TransportCursor&) = delete;
  TransportCursor& operator=(const TransportCursor&) = delete;

  [[nodiscard]] bool request_seek(std::uint64_t first_sample) noexcept;
  // A planned wrap starts a new source/prefetch generation at the requested
  // position while preserving processor history. The caller must have primed
  // the next loop generation before requesting this boundary. If the source
  // holds the wrap block anyway, the replayed block starts a new processor
  // generation, as a seek does.
  [[nodiscard]] bool request_loop_wrap(std::uint64_t first_sample) noexcept;
  // The same planned wrap, scheduled at the loop end `boundary` (nonzero).
  // render_transport_block() splits the device block that contains the
  // boundary, so the last frame before the wrap is boundary - 1 and the next
  // is `first_sample`, at any sample position and device period. A request
  // that arrives after the in-flight block already passed the boundary is
  // applied at the next block start and counted late.
  [[nodiscard]] bool request_loop_wrap_at(std::uint64_t boundary,
                                          std::uint64_t first_sample) noexcept;
  // A seek to wherever the transport is at the next block boundary: a new
  // source and processor generation without moving project time. Used to
  // discard prefetched frames queued for a loop pass that was withdrawn.
  [[nodiscard]] bool request_restart_in_place() noexcept;
  // Control thread: withdraws a queued loop wrap the callback has not yet
  // applied. True means it will never apply; false means none was queued or
  // the callback already applied it (and loop_wrap_counts() includes it).
  [[nodiscard]] bool cancel_loop_wrap() noexcept;
  [[nodiscard]] LoopWrapCounts loop_wrap_counts() const noexcept;
  // Called by the single device callback only. Empty means the sample interval
  // or generation cannot be represented; the device writes bounded silence.
  // The whole block is one context: a scheduled wrap whose boundary falls
  // inside it applies at the next block. render_transport_block() instead
  // renders the parts from next_part().
  [[nodiscard]] std::optional<engine::BlockContext> next_block(std::size_t frames) noexcept;
  // Device callback only. Like next_block(), but stops the part at a pending
  // scheduled wrap's boundary; the next call then starts the wrap there.
  [[nodiscard]] std::optional<Part> next_part(std::size_t frames) noexcept;

  // Coherent observation of the next sample after the last completed render
  // block. A reserved/in-flight block is never included. The callback writer
  // uses only lock-free atomics; a concurrent control reader may retry.
  [[nodiscard]] CompletedPosition completed_snapshot() const noexcept;

  // When a project sample is heard (ADR-0086): the device callback publishes
  // a block's first transport sample with the CLOCK_MONOTONIC time it
  // reaches the speakers, so a recording can place a key press where the
  // player heard the song. Empty until a block with a known delay renders.
  struct AudiblePoint {
    std::uint64_t sample;
    std::int64_t heard_ns;
  };
  [[nodiscard]] std::optional<AudiblePoint> audible_point() const noexcept;

  // The device-only metronome render_transport_block() mixes in, if any.
  // Set on the control thread before the output starts; it must outlive the
  // stream.
  void set_metronome(Metronome* metronome) noexcept {
    metronome_.store(metronome, std::memory_order_release);
  }
  [[nodiscard]] Metronome* metronome() const noexcept {
    return metronome_.load(std::memory_order_acquire);
  }

  // Control thread only, after the device callback and prefetch worker are
  // quiesced. Discards a pending request, restores the completed boundary and
  // increments the generation so old prefetched frames cannot be reused.
  [[nodiscard]] std::optional<CompletedPosition> restart_from_completed_after_quiesce() noexcept;
  // Control thread only, after the device callback has joined. Positions the
  // next block at an explicit sample and changes generation so old prefetched
  // frames cannot enter a new transport interval.
  [[nodiscard]] std::optional<CompletedPosition>
  seek_after_quiesce(std::uint64_t first_sample) noexcept;

private:
  enum class RequestKind : std::uint8_t { seek, loop_wrap, restart_in_place };
  [[nodiscard]] bool queue_request(RequestKind kind, std::uint64_t boundary,
                                   std::uint64_t first_sample) noexcept;
  [[nodiscard]] std::optional<Part> take_part(std::size_t frames, bool split) noexcept;
  void publish_completed(CompletedPosition value) noexcept;
  void publish_audible(AudiblePoint point) noexcept;
  void complete_block(engine::BlockContext context, std::size_t frames, bool frozen) noexcept;
  friend BlockResult render_transport_block(StereoRenderSource, TransportCursor&, bool,
                                            std::span<engine::StereoFrame>, std::span<float>,
                                            BlockTiming) noexcept;

  // Request slot. Even = empty, odd = one request pending; every transition
  // (queue, apply, withdraw) increments it, so a value is never reused. The
  // control thread writes the fields only while it is even and publishes
  // them by the release store that makes it odd. The callback reads them
  // after an acquire load and applies them only if its compare-exchange from
  // that same odd value succeeds; withdrawal is the same compare-exchange
  // from the control side, so exactly one side wins.
  std::atomic<std::uint64_t> request_state_{0};
  std::atomic<std::uint64_t> requested_sample_{0};
  std::atomic<std::uint64_t> requested_boundary_{0};
  std::atomic<RequestKind> requested_kind_{RequestKind::seek};
  std::atomic<std::uint64_t> applied_loop_wraps_{0};
  std::atomic<std::uint64_t> late_loop_wraps_{0};
  std::uint64_t position_ = 0;              // callback-owned
  std::uint64_t generation_ = 1;            // callback-owned, nonzero
  std::uint64_t processing_generation_ = 1; // callback-owned processor epoch
  bool generation_exhausted_ = false;       // callback-owned
  bool frozen_ = false;                     // callback-owned; queued requests wait until release
  std::atomic<std::uint64_t> completed_sequence_{0};
  std::atomic<std::uint64_t> completed_next_sample_;
  std::atomic<std::uint64_t> completed_generation_{1};
  std::atomic<std::uint64_t> audible_sequence_{0};
  std::atomic<std::uint64_t> audible_sample_{0};
  std::atomic<std::int64_t> audible_heard_ns_{0}; // 0: none published
  std::atomic<Metronome*> metronome_{nullptr};
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
  static_assert(std::atomic<RequestKind>::is_always_lock_free);
};

enum class BlockResult : std::uint8_t { rendered, silent_render_failure, invalid_buffer };

// All spans are preallocated. Output is interleaved stereo. A failed render
// writes silence, including a partial write by the renderer. No allocation.
[[nodiscard]] BlockResult render_stereo_block(StereoRenderSource source,
                                              engine::BlockContext context,
                                              std::span<engine::StereoFrame> stereo_scratch,
                                              std::span<float> interleaved_output) noexcept;

// Device-side wrapper. Transport advances only for a valid negotiated format
// and valid buffer, so a lost device does not silently move the playhead. A
// block containing a scheduled loop boundary renders as two parts (two
// render() calls, each with its own context); if the source freezes the
// first part, the rest of the block is silent and that part is replayed.
// A count-in the cursor's metronome has pending plays first, holding the
// transport still; then the song's clicks are mixed into each part. With a
// known `timing`, each part's first sample is published as the audible point.
[[nodiscard]] BlockResult render_transport_block(StereoRenderSource source,
                                                 TransportCursor& transport, bool format_ready,
                                                 std::span<engine::StereoFrame> stereo_scratch,
                                                 std::span<float> interleaved_output,
                                                 BlockTiming timing = {}) noexcept;

} // namespace oscillate::audio_io
