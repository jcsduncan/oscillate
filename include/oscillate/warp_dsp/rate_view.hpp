#pragma once

#include "oscillate/foundation/error.hpp"
#include "oscillate/warp_dsp/source_view.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace oscillate::warp_dsp {

// Identity of the conversion filter; part of every processed-render cache key
// (ADR-0026 point 7). Bump the version whenever converted samples change.
inline constexpr std::string_view rate_view_algorithm = "oscillate.warp_dsp.rate_view";
inline constexpr std::uint32_t rate_view_algorithm_version = 1;

// Same rule as media::source_rate_frame_count: nearest project frame for a
// source-rate duration. Fails on unsupported rates or overflow.
[[nodiscard]] foundation::Result<std::uint64_t> project_rate_frames(std::uint64_t source_frames,
                                                                    std::uint32_t source_rate,
                                                                    std::uint32_t project_rate);

// Worker-side project-rate view of a source at another rate (ADR-0026 point 6).
//
// Output frame n samples the source at exactly n * S / P source frames
// (S source rate, P project rate) through an exact-rational polyphase bank:
// one row per residue of n * S modulo P / gcd(S, P), so there is no
// fractional-position rounding and no state. Every output frame is a pure
// function of the source, which makes reads exact after any seek and
// independent of partitioning. The filter is a Kaiser-windowed sinc with its
// cutoff just below the lower Nyquist (passband to ~93%, stopband from
// Nyquist at ~99 dB). Rows are normalised to unity DC gain.
//
// Equal rates bypass the filter and forward reads bit-exactly. Output frames
// outside [0, frames()) read as silence. create() allocates; read() does not,
// although the wrapped view may (decoder-backed views allocate or do I/O).
class RateConvertedSourceView final : public SourceView {
public:
  // Zero crossings of the windowed sinc at the lower of the two rates.
  static constexpr std::uint32_t zero_crossings = 96;
  static constexpr double kaiser_beta = 10.0;
  // Cutoff as a fraction of the lower Nyquist; half the Kaiser transition
  // band sits below Nyquist so the stopband starts there.
  static constexpr double cutoff = 0.967;
  static constexpr std::uint32_t max_phases = 4'096;
  // Bound on the polyphase bank (phases x padded taps), 16 MiB of floats.
  static constexpr std::size_t max_bank_coefficients = std::size_t{1} << 22;
  // Output frames converted per wrapped read; larger requests are chunked.
  static constexpr std::size_t block_frames = 2'048;

  // Rate checks done by create(), available before any source is opened.
  [[nodiscard]] static foundation::Result<void> supports(std::uint32_t source_rate,
                                                         std::uint32_t project_rate);
  // The wrapped view reports source-rate frames and must outlive this view.
  [[nodiscard]] static foundation::Result<std::unique_ptr<RateConvertedSourceView>>
  create(SourceView& source, std::uint32_t source_rate, std::uint32_t project_rate);

  [[nodiscard]] std::uint64_t frames() const noexcept override { return frames_; }
  // A failed wrapped read silences the whole request and returns false.
  [[nodiscard]] bool read(std::int64_t first, std::span<float> left,
                          std::span<float> right) noexcept override;

  [[nodiscard]] bool bypass() const noexcept { return bank_.empty(); }
  [[nodiscard]] std::uint32_t phases() const noexcept { return phases_; }
  [[nodiscard]] std::uint32_t taps() const noexcept { return taps_; }

private:
  RateConvertedSourceView(SourceView& source, std::uint64_t frames) noexcept
      : source_(source), frames_(frames) {}

  [[nodiscard]] bool convert(std::int64_t first, std::span<float> left,
                             std::span<float> right) noexcept;

  SourceView& source_;
  std::uint64_t frames_;
  std::int64_t step_ = 1;       // S / gcd
  std::uint32_t phases_ = 1;    // P / gcd
  std::uint32_t taps_ = 0;      // padded to whole lanes
  std::int64_t half_width_ = 0; // taps span frames [i - half_width_ + 1, ...]
  std::int64_t max_position_ = 0;
  std::vector<float> bank_;
  std::vector<float> scratch_left_;
  std::vector<float> scratch_right_;
};

// Direct double-precision evaluation of one converted frame, for oracles.
[[nodiscard]] double reference_rate_converted_sample(std::span<const float> source,
                                                     std::uint32_t source_rate,
                                                     std::uint32_t project_rate,
                                                     std::int64_t output_frame);

} // namespace oscillate::warp_dsp
