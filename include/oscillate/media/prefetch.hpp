#pragma once

#include "oscillate/foundation/error.hpp"
#include "oscillate/project/model.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace oscillate::media {

using ClipId = project::ClipId;

// One project-rate frame for this first slice. Mono WAV is duplicated to both channels.
struct StereoFrame {
  float left = 0.0F;
  float right = 0.0F;
};

// Worker-only reader. The source is never modified or treated as a cache.
class WavReader {
public:
  WavReader(const WavReader&) = delete;
  WavReader& operator=(const WavReader&) = delete;
  WavReader(WavReader&& other) noexcept;
  WavReader& operator=(WavReader&& other) noexcept;
  ~WavReader();

  [[nodiscard]] static foundation::Result<WavReader> open(const std::filesystem::path& path);
  // Duplicates a pinned regular-file descriptor. The caller keeps ownership of fd.
  [[nodiscard]] static foundation::Result<WavReader> open_fd(int fd);
  [[nodiscard]] foundation::Result<std::size_t> read_at(std::uint64_t source_frame,
                                                        std::span<StereoFrame> output);
  [[nodiscard]] std::uint32_t sample_rate() const noexcept { return sample_rate_; }
  [[nodiscard]] std::uint16_t channels() const noexcept { return channels_; }
  [[nodiscard]] std::uint64_t frame_count() const noexcept { return frame_count_; }

private:
  WavReader() = default;
  int fd_ = -1;
  std::uint64_t data_offset_ = 0;
  std::uint64_t frame_count_ = 0;
  std::uint32_t sample_rate_ = 0;
  std::uint16_t channels_ = 0;
  std::uint16_t format_ = 0;
  std::uint16_t bits_ = 0;
  std::uint16_t block_align_ = 0;
};

struct TaggedFrame {
  ClipId clip;
  std::uint64_t generation = 0; // Monotone; control side must not wrap it.
  std::uint64_t source_frame = 0;
  StereoFrame audio;
};

struct PrefetchCursor {
  std::uint64_t generation;
  std::uint64_t source_frame;
};

struct UnderrunEvent {
  ClipId clip;
  std::uint64_t generation = 0;
  std::uint64_t first_source_frame = 0;
  std::size_t missing_frames = 0;
};

struct ConsumeResult {
  std::size_t copied = 0;
  std::size_t silent = 0;
  std::size_t stale_dropped = 0;
  // Producer continuity gives at most a leading and trailing gap per block.
  std::array<std::optional<UnderrunEvent>, 2> underruns{};
};

enum class HandoffWindowStatus : std::uint8_t { ready, waiting, missed, invalid };

// One producer (prefetch worker), one consumer (audio callback). Each instance is
// dedicated to one clip. Construction and destruction occur off the callback.
class PrefetchRing {
public:
  // Preliminary callback work bound; profile on reference hardware before
  // ER-11 qualification. Control side resets stale generations before priming.
  static constexpr std::size_t max_stale_scan_per_block = 32;
  PrefetchRing(ClipId clip, std::size_t capacity);
  PrefetchRing(const PrefetchRing&) = delete;
  PrefetchRing& operator=(const PrefetchRing&) = delete;

  [[nodiscard]] ClipId clip() const noexcept { return clip_; }
  [[nodiscard]] std::size_t capacity() const noexcept { return frames_.size(); }
  [[nodiscard]] std::size_t occupancy() const noexcept;
  [[nodiscard]] std::size_t free_slots() const noexcept { return capacity() - occupancy(); }
  [[nodiscard]] bool try_push(const TaggedFrame& frame) noexcept;
  // Producer side. Declares a deliberate forward jump within the current
  // generation, so the next push may be tagged `next_frame` instead of the
  // contiguous successor. The consumer reports the skipped interval as an
  // underrun gap. False (and no change) for any other generation, a
  // backward or zero-length jump, or before the first push.
  [[nodiscard]] bool skip_forward(std::uint64_t generation, std::uint64_t next_frame) noexcept;
  // Producer side, only after the consumer has discarded every pending frame
  // for a new generation (the seek handshake). Forgets the last pushed tag,
  // so the next push may start anywhere, even in a generation whose
  // discarded frames (for example a pre-primed loop) used later tags.
  void restart_producer() noexcept { have_last_tag_ = false; }

  // Control side only, before this ring is bound to an audio callback. The
  // producer may continue pushing, but there must be no other consumer.
  // Drops only the published prefix before the requested first frame, then
  // proves that the exact generation and contiguous source interval are
  // resident. A ready result permits the first callback consume without a
  // stale-scan gap; waiting needs more producer data, while missed requires a
  // fresh ring/generation because a later tag is already resident. An empty
  // ring returns waiting even if its producer has finished: the caller must
  // separately track producer completion and cancel that candidate.
  [[nodiscard]] HandoffWindowStatus prime_handoff_window(std::uint64_t generation,
                                                         std::uint64_t source_frame,
                                                         std::size_t frames) noexcept;

  // Callback-side only. Old generation/position tags are discarded; future tags
  // remain queued. Work is bounded by max_stale_scan_per_block + output.size(). Silence during
  // priming is not counted. Steady gaps return fixed-size intervals for an
  // off-thread diagnostic queue.
  [[nodiscard]] ConsumeResult consume(std::uint64_t generation, std::uint64_t source_frame,
                                      std::span<StereoFrame> output, bool steady) noexcept;
  // Callback-side seek helper. Quiesce the producer before calling when a seek
  // requires retaining newly published frames; otherwise concurrent frames may
  // also be discarded. A new generation can then be queued and primed.
  void discard_pending() noexcept;
  // Producer side. The generation and one-past-last frame the consumer last
  // asked for, so a producer that fell behind can resume ahead of it instead
  // of queueing frames the consumer has already passed. Advisory only: the
  // two values are read without a joint snapshot.
  struct ConsumerPosition {
    std::uint64_t generation = 0;
    std::uint64_t next_frame = 0;
  };
  [[nodiscard]] ConsumerPosition consumer_position() const noexcept {
    return {consumed_generation_.load(std::memory_order_acquire),
            consumed_end_.load(std::memory_order_acquire)};
  }
  [[nodiscard]] std::uint64_t underrun_count() const noexcept {
    return underrun_count_.load(std::memory_order_relaxed);
  }
  // Callback producer / control-thread consumer. Events are dropped when this
  // bounded queue is full; the total and drop counters remain observable.
  [[nodiscard]] std::optional<UnderrunEvent> pop_underrun() noexcept;
  [[nodiscard]] std::uint64_t dropped_underrun_events() const noexcept {
    return dropped_underrun_events_.load(std::memory_order_relaxed);
  }

private:
  static constexpr std::size_t underrun_event_capacity = 64;
  void push_underrun(UnderrunEvent event) noexcept;
  ClipId clip_;
  std::vector<TaggedFrame> frames_;
  alignas(64) std::atomic<std::uint64_t> write_{0};
  alignas(64) std::atomic<std::uint64_t> read_{0};
  std::atomic<std::uint64_t> underrun_count_{0};
  // Consumer-written position, beside the other consumer-written counter.
  std::atomic<std::uint64_t> consumed_end_{0};
  std::atomic<std::uint64_t> consumed_generation_{0};
  std::array<std::optional<UnderrunEvent>, underrun_event_capacity> underrun_events_{};
  alignas(64) std::atomic<std::uint64_t> underrun_event_write_{0};
  alignas(64) std::atomic<std::uint64_t> underrun_event_read_{0};
  std::atomic<std::uint64_t> dropped_underrun_events_{0};
  // Producer-owned; source frames are contiguous within each generation.
  bool have_last_tag_ = false;
  std::uint64_t last_generation_ = 0;
  std::uint64_t last_source_frame_ = 0;
};

// Worker-side fill. Caller supplies reusable scratch. Rate mismatch is rejected;
// source-rate conversion belongs to a later codec/DSP slice.
[[nodiscard]] foundation::Result<std::size_t>
fill_from_wav(WavReader& source, std::uint32_t project_rate, PrefetchRing& ring,
              PrefetchCursor cursor, std::span<StereoFrame> scratch);

} // namespace oscillate::media
