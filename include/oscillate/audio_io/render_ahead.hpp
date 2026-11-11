#pragma once

#include "oscillate/audio_io/stereo_block.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <thread>
#include <vector>

namespace oscillate::audio_io {

// Render-ahead for playback (ADR-0128). A producer thread renders the
// transport ahead of the device into a bounded window of blocks; the device
// callback copies them out and renders just in time only when the window is
// empty. The producer owns the transport while it renders: it advances the
// cursor, applies seeks and planned loop wraps, and every edit reaches the
// graph at the producer's position, so an edit is heard after the window,
// through the engine's usual click-free switches. One thread renders at a
// time: the producer, or the callback when the window is empty.
//
// The window holds up to max_window_blocks device blocks, no more than the
// limit in milliseconds. It starts full, shrinks by a block after
// shrink_after_blocks blocks in which the callback never found it empty,
// down to min_window_blocks, and grows by a block each time it does. While live input
// plays (set_live_input), or with a zero limit, nothing renders ahead: the
// window drains and the callback renders every block just in time.
//
// A seek or an in-place restart drops the window: the blocks after it are
// rendered from the new position. Stop fades the audible output over
// stop_fade_frames (fade_out) instead of draining the graph.
class RenderAhead final {
public:
  static constexpr std::size_t min_window_blocks = 2;
  static constexpr std::size_t max_window_blocks = 8;
  // Two spare slots: one for the block in flight, one for a block rendered
  // while a seek drops the rest.
  static constexpr std::size_t slot_count = max_window_blocks + 2;
  static constexpr std::uint32_t default_limit_ms = 50;
  static constexpr std::uint32_t max_limit_ms = 200;
  static constexpr std::uint64_t shrink_after_blocks = 2048;
  static constexpr std::size_t stop_fade_frames = 256;

  // `max_block_frames`: the largest device block the output may ask for.
  RenderAhead(StereoRenderSource source, TransportCursor& transport, std::size_t max_block_frames);
  RenderAhead(const RenderAhead&) = delete;
  RenderAhead& operator=(const RenderAhead&) = delete;
  ~RenderAhead();

  // The render source an output plays: render_transport_block calls in the
  // output become render() through StereoRenderSource::ahead.
  [[nodiscard]] StereoRenderSource device_source() noexcept;

  // Device callback, in place of render_transport_block.
  [[nodiscard]] BlockResult render(bool format_ready, std::span<engine::StereoFrame> stereo_scratch,
                                   std::span<float> interleaved_output,
                                   BlockTiming timing) noexcept;

  // Producer thread entry: renders ahead until request_stop(). An output
  // starts it at the callback's priority; tests and the fake callback start
  // an ordinary thread (start_owned_thread).
  void run_producer() noexcept;
  void request_stop() noexcept;
  void reset_after_join() noexcept;
  void start_owned_thread();
  void stop_owned_thread() noexcept;

  // What the device plays, block by block, with the position it starts at:
  // meters and monitors that must follow the ear, not the render, observe it
  // here and may change it (a mono check). Set before the output starts.
  struct Tap {
    void* context = nullptr;
    void (*played)(void* context, std::span<engine::StereoFrame> frames,
                   engine::BlockContext heard) noexcept = nullptr;
  };
  void set_tap(Tap tap) noexcept { tap_ = tap; }

  // Control side.
  // The window's limit in milliseconds at `sample_rate` (0: render just in
  // time). At most max_limit_ms.
  void set_limit(std::uint32_t milliseconds, std::uint32_t sample_rate) noexcept;
  // Live input is playing (the computer keyboard is on, recording is armed):
  // render just in time, as soon as the window has drained.
  void set_live_input(bool live) noexcept;
  // Fades out what is audible and then plays silence, for Stop.
  void fade_out() noexcept;
  [[nodiscard]] bool fading() const noexcept { return fading_.load(std::memory_order_acquire); }
  [[nodiscard]] bool faded() const noexcept;
  // Whether a window is set (a nonzero limit) and a producer runs to fill
  // it.
  [[nodiscard]] bool renders_ahead() const noexcept {
    return limit_frames_.load(std::memory_order_acquire) != 0 &&
           producing_.load(std::memory_order_acquire);
  }
  // After the callback and the producer have stopped (pause, a paused seek):
  // drops the window. A pause that resumes keeps it instead.
  void clear_after_quiesce() noexcept;

  struct Status {
    std::size_t window_blocks = 0; // the current target
    std::size_t buffered_frames = 0;
    std::uint64_t ahead_blocks = 0; // device blocks played from the window
    std::uint64_t just_in_time_blocks = 0;
    std::uint64_t empty_window_blocks = 0; // found empty while it should not be
  };
  [[nodiscard]] Status status() const noexcept;
  // The next sample after the last one the device played, and its
  // generation: what is audible, where the transport cursor's completed
  // position is what has been rendered.
  [[nodiscard]] TransportCursor::CompletedPosition audible_position() const noexcept;
  // The longest the window can make an edit wait, in frames, at the
  // current device block: the limit plus one block.
  [[nodiscard]] std::size_t worst_edit_delay_frames() const noexcept;

private:
  struct Part {
    engine::BlockContext context;
    std::size_t offset = 0;
    std::size_t frames = 0;
  };
  struct Slot {
    std::vector<float> samples;
    std::size_t frames = 0;
    std::array<Part, 2> parts{};
    std::size_t part_count = 0;
    BlockResult result = BlockResult::rendered;
  };

  static bool capture(void* context, std::span<engine::StereoFrame> output,
                      engine::BlockContext block) noexcept;
  static bool capture_frozen(void* context) noexcept;
  [[nodiscard]] bool produce_one() noexcept;
  [[nodiscard]] bool wants_block() const noexcept;
  [[nodiscard]] bool acquire_render() noexcept;
  void release_render() noexcept;
  void wake_producer() noexcept;
  // The window's target in blocks, at most the limit (0: just in time).
  [[nodiscard]] std::size_t target_blocks() const noexcept;
  // The oldest block still to play.
  [[nodiscard]] std::uint64_t first_unread() const noexcept;
  void publish_position(std::uint64_t next_sample, std::uint64_t generation) noexcept;

  StereoRenderSource source_;
  Tap tap_;
  TransportCursor& transport_;
  std::size_t max_block_frames_;
  std::array<Slot, slot_count> slots_;
  std::vector<engine::StereoFrame> producer_scratch_;
  // Ring indices: the producer writes write_, the callback read_; the
  // producer drops blocks before discard_before_ (a seek).
  std::atomic<std::uint64_t> write_{0};
  std::atomic<std::uint64_t> read_{0};
  std::atomic<std::uint64_t> discard_before_{0};
  std::size_t read_offset_ = 0; // callback-owned
  // One renderer at a time: 1 while the producer or the callback renders.
  std::atomic<std::uint32_t> rendering_{0};
  // Bumped by every callback and control change: the producer's wake word.
  std::atomic<std::uint32_t> wake_{0};
  std::atomic<bool> stop_{false};
  std::atomic<bool> producing_{false}; // a producer thread is running
  std::atomic<bool> active_{false};    // the device plays and may render
  std::atomic<bool> held_{false};      // the source holds (a seek primes)
  std::atomic<bool> live_input_{false};
  std::atomic<bool> fading_{false};
  std::atomic<bool> faded_{false};
  std::atomic<std::size_t> block_frames_{0}; // the device's block, callback-written
  std::atomic<std::size_t> limit_frames_{0};
  std::atomic<std::size_t> window_blocks_{max_window_blocks};
  std::uint64_t blocks_since_empty_ = 0;   // callback-owned
  bool served_last_ = false;               // callback-owned: the last block came from the window
  std::size_t fade_remaining_ = 0;         // callback-owned
  std::uint64_t seen_discontinuities_ = 0; // producer-owned
  // The processing generation of the last block rendered, by whichever
  // thread holds rendering_.
  std::uint64_t last_processing_generation_ = 0;
  // Producer-owned capture of the block being rendered.
  Slot* capturing_ = nullptr;
  bool capture_frozen_ = false;
  std::atomic<std::uint64_t> ahead_blocks_{0};
  std::atomic<std::uint64_t> just_in_time_blocks_{0};
  std::atomic<std::uint64_t> empty_window_blocks_{0};
  // Seqlock over the audible position, callback-written.
  std::atomic<std::uint64_t> audible_sequence_{0};
  std::atomic<std::uint64_t> audible_next_sample_{0};
  std::atomic<std::uint64_t> audible_generation_{0};
  std::thread owned_;
};

// What a device output renders each callback: through the source's
// render-ahead when it has one, otherwise render_transport_block().
[[nodiscard]] BlockResult render_device_block(StereoRenderSource source, TransportCursor& transport,
                                              bool format_ready,
                                              std::span<engine::StereoFrame> stereo_scratch,
                                              std::span<float> interleaved_output,
                                              BlockTiming timing = {}) noexcept;

// The producer thread entry an output starts beside its workers.
void* render_ahead_entry(void* ahead) noexcept;

} // namespace oscillate::audio_io
