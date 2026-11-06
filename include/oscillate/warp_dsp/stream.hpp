#pragma once

#include "oscillate/foundation/error.hpp"
#include "oscillate/warp_dsp/analysis.hpp"
#include "oscillate/warp_dsp/source_view.hpp"
#include "oscillate/warp_dsp/time_map.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace oscillate::warp_dsp {

enum class QualityTier : std::uint8_t { q0 = 0, q1 = 1, q2 = 2 };
[[nodiscard]] constexpr bool valid_quality_tier(QualityTier tier) noexcept {
  return tier == QualityTier::q0 || tier == QualityTier::q1 || tier == QualityTier::q2;
}

enum class StreamStatus : std::uint8_t {
  ok,
  not_started,
  // A segment exceeds the mode's supported speed range.
  unsupported_map,
  // The requested output frame is at or beyond the end of the map.
  out_of_range,
  source_failed,
  // A stream requiring prepared map-specific state was started with another map.
  unprepared,
};

// Splice (loop join or slice jump) crossfade length in output frames.
inline constexpr std::uint32_t splice_crossfade_frames = 64;

struct StreamLimits {
  std::uint32_t sample_rate = 48'000;
  // Largest render() request; larger requests are served partially.
  std::uint32_t max_render_frames = 8'192;
};

// Worker-only continuous warp renderer. create_*() and explicit preparation
// perform DSP-owned allocations; start() and render() do not allocate, lock or
// wait within the DSP. A decoder-backed SourceView may allocate or perform I/O,
// so no WarpStream may run on the audio callback. A continuous render is
// independent of render() call partitioning. Seek equivalence is mode-specific:
// Re-Pitch, Beats and prepared Tones reproduce continuous samples. Complex and
// Texture may converge after bounded pre-roll without bit identity at every seek.
class WarpStream {
public:
  WarpStream() = default;
  WarpStream(const WarpStream&) = delete;
  WarpStream& operator=(const WarpStream&) = delete;
  WarpStream(WarpStream&&) = delete;
  WarpStream& operator=(WarpStream&&) = delete;
  virtual ~WarpStream() = default;

  // The map and source must outlive rendering or the next start().
  [[nodiscard]] virtual StreamStatus start(const WarpTimeMap& map, SourceView& source,
                                           std::uint64_t output_frame) noexcept = 0;
  // Writes frames starting at next_output_frame(). Returns the count written;
  // fewer than requested means the end of the map or a failure in status().
  [[nodiscard]] virtual std::size_t render(std::span<float> left,
                                           std::span<float> right) noexcept = 0;
  [[nodiscard]] virtual std::uint64_t next_output_frame() const noexcept = 0;
  [[nodiscard]] virtual StreamStatus status() const noexcept = 0;
};

// Re-Pitch: band-limited varispeed. Pitch follows speed, as on a turntable.
inline constexpr double repitch_max_speed = 16.0;
[[nodiscard]] foundation::Result<std::unique_ptr<WarpStream>>
create_repitch_stream(const StreamLimits& limits, QualityTier tier);

// Beats: each slice between preserve points plays unstretched from the frame
// where the map reaches its start, so attacks land exactly. A slice that runs
// out before the next one fills the gap per `loop`; a slice cut short fades.
enum class BeatsLoop : std::uint8_t { off, forward, back_and_forth };
inline constexpr std::int32_t max_transpose_cents = 4'800;
inline constexpr std::size_t max_beats_slices = 1U << 20;
struct BeatsSettings {
  // Preserve points (transients or grid positions) as strictly increasing
  // source frames.
  std::vector<std::uint64_t> slice_starts;
  BeatsLoop loop = BeatsLoop::forward;
  // Transient envelope 0..100: below 100 each slice decays to silence over
  // that percentage of its output duration.
  float envelope = 100.0F;
  // Resamples each slice (pitch and slice length change together).
  std::int32_t transpose_cents = 0;
};
[[nodiscard]] foundation::Result<std::unique_ptr<WarpStream>>
create_beats_stream(const StreamLimits& limits, QualityTier tier, BeatsSettings settings);

// Complex: phase vocoder with identity phase locking shared across channels.
// Frames straddling an anchor (clip start/end, onsets, loop splices) are
// aligned at unity speed and masked at the anchor, so attacks land exactly
// without pre-echo and nothing past the clip end leaks in. Output is
// invariant to render partitioning and identical for identical start frames;
// a seek pre-rolls from the latest phase-reset epoch (bounded), so a seek far
// from any epoch converges in timing and level but not bit-for-bit in phase.
inline constexpr std::size_t max_complex_onsets = 1U << 20;
//
// Transpose time-scales an intermediate stream by the pitch ratio p and
// resamples it by p, so every anchor still lands on its output frame. Unity
// transpose bypasses resampling entirely.
struct ComplexSettings {
  // Anchor onsets (detected transients) as strictly increasing source frames.
  std::vector<std::uint64_t> onsets;
  std::int32_t transpose_cents = 0;
};
[[nodiscard]] foundation::Result<std::unique_ptr<WarpStream>>
create_complex_stream(const StreamLimits& limits, QualityTier tier, ComplexSettings settings);

// Complex Pro: Complex plus formant preservation under transpose. `formants`
// (0..100) is how much of the original spectral envelope stays in place;
// `envelope` (8..256) sets the envelope's detail: a cepstral cutoff of
// envelope / 2 samples at 44.1 kHz, scaled with the rate.
struct ComplexProSettings {
  std::vector<std::uint64_t> onsets;
  std::int32_t transpose_cents = 0;
  float formants = 100.0F;
  float envelope = 128.0F;
};
[[nodiscard]] foundation::Result<std::unique_ptr<WarpStream>>
create_complex_pro_stream(const StreamLimits& limits, QualityTier tier,
                          ComplexProSettings settings);

// Tones: pitch-synchronous overlap-add (TD-PSOLA) for monophonic, pitched
// material. Each output period takes a two-period grain from the nearest
// source pitch mark; transposition spaces output periods by period / p, which
// keeps formants. Unvoiced stretches use plain grains of `grain_size_ms`.
// Output periods depend on the preceding pitch mark. An indexed stream uses
// worker-prepared checkpoints for exact, bounded-distance random seeks.
struct TonesSettings {
  std::vector<analysis::PitchMark> marks;
  float grain_size_ms = 40.0F; // 5..200
  std::int32_t transpose_cents = 0;
};
class TonesSeekIndex {
public:
  struct Impl;
  struct Statistics {
    std::size_t runs = 0;
    std::size_t grains = 0;
    std::size_t checkpoints = 0;
    // Retained container storage, excluding allocator/control-block overhead.
    std::size_t estimated_bytes = 0;
  };
  [[nodiscard]] const Impl& impl() const noexcept;
  [[nodiscard]] Statistics statistics() const noexcept;

private:
  explicit TonesSeekIndex(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}
  std::shared_ptr<const Impl> impl_;
  friend foundation::Result<std::shared_ptr<const TonesSeekIndex>>
  prepare_tones_seek_index(const WarpTimeMap&, const StreamLimits&, QualityTier,
                           const TonesSettings&);
};

// Preparation may allocate and traverse the complete map. The map must outlive
// the index and streams using it; map/settings changes require a new index.
[[nodiscard]] foundation::Result<std::shared_ptr<const TonesSeekIndex>>
prepare_tones_seek_index(const WarpTimeMap& map, const StreamLimits& limits, QualityTier tier,
                         const TonesSettings& settings);

// Indexed factory: validates the index against settings before construction.
// start() returns unprepared for any other map, with no callback allocation.
[[nodiscard]] foundation::Result<std::unique_ptr<WarpStream>>
create_tones_stream(const StreamLimits& limits, QualityTier tier, TonesSettings settings,
                    std::shared_ptr<const TonesSeekIndex> index);

// Unindexed reference factory (worker-only, used by tests as the exact
// oracle): exact, but distant seeks may replay a whole run.
[[nodiscard]] foundation::Result<std::unique_ptr<WarpStream>>
create_tones_stream(const StreamLimits& limits, QualityTier tier, TonesSettings settings);

// Texture: granular resynthesis on a fixed output grid (4x overlap) for
// textures and ambiences. `flux` (0..100) jitters each grain's source
// position by up to half a grain, keyed on the grain index and `seed`; WSOLA
// alignment then keeps sustained tones from drifting in pitch. Grains are
// normalised by their measured coherence, so decorrelated texture keeps its
// level. Transposition resamples each grain. Output is partition-invariant;
// a seek replays the alignment chain from the run start or at most 2 s
// earlier, so it is bit-exact within that span and converges beyond it.
struct TextureSettings {
  float grain_size_ms = 100.0F; // 10..500
  float flux = 0.0F;            // 0..100
  std::int32_t transpose_cents = 0;
  std::uint64_t seed = 0;
};
[[nodiscard]] foundation::Result<std::unique_ptr<WarpStream>>
create_texture_stream(const StreamLimits& limits, QualityTier tier, TextureSettings settings);

} // namespace oscillate::warp_dsp
