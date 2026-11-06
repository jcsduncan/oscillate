#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace oscillate::audio_io {

// The metronome (ADR-0086): a click on every beat of the song while the
// transport plays, accented on each bar's first beat, and a count-in of
// clicks before a recording starts. It sounds on the device output only:
// render_transport_block() mixes it in when a TransportCursor carries one,
// and export never attaches one.
//
// The click is a short decaying sine, a pure function of the frames since
// its beat, so seeks, loop wraps and any block split render it identically.

// One beat: the project sample it falls on, and whether it starts a bar.
struct ClickBeat {
  std::uint64_t sample = 0;
  bool accent = false;
};

// Beats a schedule holds: at 120 BPM in 4/4, over nine hours of song.
inline constexpr std::size_t max_click_beats = 65'536;
// The click's sound.
inline constexpr double click_seconds = 0.03;
inline constexpr double click_decay_seconds = 0.006;
inline constexpr double click_hz = 1'000.0;
inline constexpr double accent_click_hz = 1'500.0;
inline constexpr float click_gain = 0.3F;
inline constexpr float accent_click_gain = 0.45F;

// A click's sample `frame` frames after its beat (0 past its end).
[[nodiscard]] float click_sample(std::uint64_t frame, bool accent,
                                 std::uint32_t sample_rate) noexcept;
// Frames a click lasts at `sample_rate`.
[[nodiscard]] std::uint64_t click_frames(std::uint32_t sample_rate) noexcept;

class Metronome {
public:
  Metronome();
  Metronome(const Metronome&) = delete;
  Metronome& operator=(const Metronome&) = delete;

  // ---- control thread ----
  void set_enabled(bool enabled) noexcept { enabled_.store(enabled, std::memory_order_release); }
  [[nodiscard]] bool enabled() const noexcept { return enabled_.load(std::memory_order_acquire); }
  // Replaces the song's beats (ascending samples; beyond max_click_beats are
  // dropped). Waits, without locking the callback, until the callback is
  // not reading the buffer it writes.
  void publish(std::span<const ClickBeat> beats, std::uint32_t sample_rate);
  // Before the next block the transport waits `beats` clicks `beat_frames`
  // apart (the first accented), rendering only the clicks: a count-in. It
  // sounds whether or not the click is enabled.
  void request_count_in(std::uint32_t beats, std::uint64_t beat_frames) noexcept;
  void cancel_count_in() noexcept;
  // True from a request until the callback has rendered its last frame.
  [[nodiscard]] bool counting_in() const noexcept;

  // ---- device callback only ----
  // Renders count-in frames (silence and clicks) into the interleaved stereo
  // `output`, from its start; returns how many frames it used (0 when no
  // count-in is due).
  std::size_t render_count_in(std::span<float> output) noexcept;
  // Adds the song's clicks to interleaved stereo `output`, whose first frame
  // is project sample `first_sample`, when enabled.
  void add_clicks(std::uint64_t first_sample, std::span<float> output) noexcept;

private:
  struct Schedule {
    std::vector<ClickBeat> beats;
    std::size_t count = 0;
    std::uint32_t sample_rate = 0;
  };
  std::array<Schedule, 2> schedules_;
  std::atomic<std::uint32_t> active_{0};
  // The schedule the callback is reading, plus one (0: none).
  std::atomic<std::uint32_t> reading_{0};
  std::atomic<bool> enabled_{false};
  // Count-in: requests increment the sequence; the callback adopts a new one.
  std::atomic<std::uint64_t> count_in_sequence_{0};
  std::atomic<std::uint32_t> count_in_beats_{0};
  std::atomic<std::uint64_t> count_in_beat_frames_{0};
  std::atomic<bool> counting_in_{false};
  std::uint64_t adopted_sequence_ = 0;  // callback-owned
  std::uint64_t count_in_position_ = 0; // callback-owned
  std::uint64_t count_in_total_ = 0;    // callback-owned
  std::uint64_t count_in_beat_ = 0;     // callback-owned
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
};

} // namespace oscillate::audio_io
