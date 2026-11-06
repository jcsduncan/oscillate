#pragma once

#include "oscillate/clay_dsp/analysis.hpp"
#include "oscillate/clay_dsp/take.hpp"

#include <atomic>
#include <cstdint>
#include <span>
#include <vector>

namespace oscillate::clay_dsp::detail {

// Analysis hops: melodic notes need fine pitch curves; polyphonic and
// percussive analysis uses longer windows.
inline constexpr double melodic_hop_seconds = 256.0 / 48'000.0;
inline constexpr double polyphonic_hop_seconds = 512.0 / 48'000.0;
// Partials above this share of the sample rate are left out.
inline constexpr double partial_ceiling = 0.45;
// Peaks this far under a frame's loudest are noise.
inline constexpr double peak_floor_db = -80.0;

struct Context {
  std::span<const float> mono;
  std::uint32_t sample_rate = 0;
  const AnalysisOptions* options = nullptr;
  const std::atomic<bool>* cancelled = nullptr;

  [[nodiscard]] bool stopped() const noexcept {
    return cancelled != nullptr && cancelled->load(std::memory_order_relaxed);
  }
};

[[nodiscard]] std::int64_t hop_frames(double seconds, std::uint32_t sample_rate) noexcept;
[[nodiscard]] double db_to_ratio(double db) noexcept;

// Each returns notes with id 0, sorted by start; `false` when cancelled.
[[nodiscard]] bool detect_melodic(const Context& context, std::int64_t hop,
                                  std::vector<Note>& notes);
[[nodiscard]] bool detect_polyphonic(const Context& context, std::int64_t hop,
                                     std::vector<Note>& notes);
[[nodiscard]] bool detect_percussive(const Context& context, std::span<const float> left,
                                     std::span<const float> right, std::int64_t hop,
                                     std::vector<Note>& notes);

// Fills a note's envelope rows by interpolating its partial amplitudes
// across frequency (for notes in a mixture, whose own envelope is unknown).
void envelope_from_partials(Note& note);

} // namespace oscillate::clay_dsp::detail
