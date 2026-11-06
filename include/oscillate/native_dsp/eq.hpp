#pragma once

#include "oscillate/engine/graph.hpp"
#include "oscillate/foundation/error.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace oscillate::native_dsp {

inline constexpr std::size_t eq_band_limit = 8;
// A pass filter is a cascade of 12 dB/octave stages: up to 8 (96 dB/octave).
inline constexpr std::size_t eq_max_pass_stages = 8;
// A prepared cascade runs one biquad section per stage.
inline constexpr std::size_t eq_section_limit = eq_band_limit * eq_max_pass_stages;
// A pass section's Q may exceed the user's 10: a steep cascade's last stage
// is resonant even when flat (8 stages: about 10.2), and Q scales it.
inline constexpr double eq_pass_section_q_limit = 40.0;

enum class EqBandKind : std::uint8_t {
  low_pass,
  high_pass,
  low_shelf,
  high_shelf,
  peaking,
};

struct EqBandSettings {
  EqBandKind kind;
  double frequency_hz;
  double shape;   // Q for pass/peak; shelf slope for shelves.
  double gain_db; // Used only by peak/shelf; must be zero for pass filters.
};

// A pass filter of `stages` matched 12 dB/octave stages at one corner
// (12 * stages dB/octave), written to `out` (at least `stages` long); returns
// the stage count. The stages are Butterworth, so at q = dsp::butterworth_q the
// cascade is maximally flat with -3 dB at the corner; q scales the last
// (most resonant) stage's Q, shaping the corner as it does one stage's, held
// within eq_pass_section_q_limit. One stage is the band itself, q unchanged.
// `stages` is 1 to eq_max_pass_stages; `kind` is low_pass or high_pass.
std::size_t eq_pass_stages(EqBandKind kind, double frequency_hz, double q, std::size_t stages,
                           std::span<EqBandSettings> out) noexcept;

struct EqCoefficients {
  double b0;
  double b1;
  double b2;
  double a1;
  double a2;
};

class PreparedEq {
public:
  [[nodiscard]] std::size_t band_count() const noexcept { return band_count_; }
  [[nodiscard]] std::uint32_t sample_rate() const noexcept { return sample_rate_; }
  [[nodiscard]] const EqCoefficients& coefficients(std::size_t band) const& noexcept {
    return coefficients_[band]; // Caller must supply band < band_count().
  }

private:
  struct Metadata {
    std::size_t band_count;
    std::uint32_t sample_rate;
  };

  PreparedEq(std::array<EqCoefficients, eq_section_limit> coefficients, Metadata metadata) noexcept
      : coefficients_(coefficients), band_count_(metadata.band_count),
        sample_rate_(metadata.sample_rate) {}

  std::array<EqCoefficients, eq_section_limit> coefficients_{};
  std::size_t band_count_ = 0;
  std::uint32_t sample_rate_ = 0;

  friend foundation::Result<PreparedEq> prepare_eq(std::span<const EqBandSettings>, std::uint32_t);
};

// Control thread only. Copies validated, normalized coefficients into fixed
// storage, one biquad section per setting (at most eq_section_limit; a steep
// pass filter is several, from eq_pass_stages). Frequency is 10 Hz through
// min(20 kHz, 0.45*sample rate); shape is [0.1, 10] for a bell's Q, [0.1,
// eq_pass_section_q_limit] for a pass section's and [0.1, 1] for shelf
// slope. Active gain is [-24, 24] dB. Empty settings produce a bypass
// processor.
[[nodiscard]] foundation::Result<PreparedEq> prepare_eq(std::span<const EqBandSettings> bands,
                                                        std::uint32_t sample_rate);

// Conservative settling duration for a prepared EQ cascade, approximately
// -180 dB per section. Rejects estimates beyond ten seconds rather than
// allowing an offline render to silently truncate its IIR response.
[[nodiscard]] foundation::Result<std::uint64_t> estimate_eq_tail_frames(const PreparedEq& prepared);

// Control/UI side only. Evaluates the actual prepared digital biquad cascade
// at a frequency from DC through Nyquist; invalid frequencies return no value.
[[nodiscard]] std::optional<double> eq_magnitude_db(const PreparedEq& prepared,
                                                    double frequency_hz) noexcept;

// Construct/reclaim on control. A single live callback exclusively owns process().
// Compatible topology changes reuse this instance and therefore retain DSP history.
// Coefficient and one-band structural changes are prepared off callback and
// adopted through the bounded mailbox; the running runtime retains compatible
// section history. Arbitrary cascade replacement is not a live edit.
class EqRuntime final : public engine::ProcessorRuntime {
public:
  [[nodiscard]] bool control_overlay_revision_independent() const noexcept override { return true; }
  explicit EqRuntime(PreparedEq prepared) noexcept
      : prepared_(prepared), band_count_(prepared.band_count()),
        sample_rate_(prepared.sample_rate()), acknowledged_band_count_(prepared.band_count()) {
    for (std::size_t band = 0; band < band_count_; ++band)
      current_[band] = prepared.coefficients(band);
  }

  // The routed publisher has acknowledged a callback hold. Replace only a
  // compatible band's coefficients while no callback can access this runtime;
  // preserve its filter delay state across the next block boundary.
  [[nodiscard]] PreparedEq prepared_while_held() const noexcept { return prepared_; }
  void replace_prepared_while_held(PreparedEq prepared) noexcept {
    prepared_ = prepared;
    for (std::size_t band = 0; band < band_count_; ++band)
      current_[band] = prepared.coefficients(band);
    transition_remaining_ = 0;
  }

  // One control producer may queue validated coefficients without holding the
  // callback. The callback adopts the newest queued revision at its next block
  // start and keeps filter history. Coefficients move to the target over 64
  // samples; acknowledgement follows the final transition sample. A full
  // mailbox rejects the offer; callers
  // retain the project edit and must retry or report its inactive status.
  [[nodiscard]] bool offer_prepared(PreparedEq prepared, std::uint64_t revision) noexcept;
  // Control offers one appended or removed band only after the prior revision
  // has been acknowledged. Surviving sections retain their delay history.
  [[nodiscard]] bool offer_band_change(PreparedEq prepared, std::size_t changed_band,
                                       std::uint64_t revision) noexcept;
  [[nodiscard]] std::uint64_t applied_prepared_revision() const noexcept {
    return applied_revision_.load(std::memory_order_acquire);
  }

  void process(std::span<engine::StereoFrame> stereo,
               engine::BlockContext context) noexcept override;

  // Control thread only after the callback has stopped and joined.
  void reset_after_quiesce() noexcept;
  [[nodiscard]] std::uint64_t guarded_frames_after_quiesce() const noexcept {
    return guarded_frames_;
  }

private:
  struct PendingCoefficients {
    PreparedEq prepared;
    std::uint64_t revision;
    std::optional<std::size_t> changed_band;
  };
  static constexpr std::size_t mailbox_capacity = 4;
  static constexpr std::size_t transition_frames = 64;
  void adopt_pending() noexcept;
  void advance_transition() noexcept;
  void finish_structural_transition() noexcept;

  struct ChannelState {
    double z1 = 0.0;
    double z2 = 0.0;
  };

  PreparedEq prepared_;
  std::size_t band_count_;
  const std::uint32_t sample_rate_;
  std::array<EqCoefficients, eq_section_limit> current_{};
  bool structural_transition_ = false;
  std::optional<std::size_t> removing_band_;
  std::atomic<std::size_t> acknowledged_band_count_{0};
  std::size_t transition_remaining_ = 0;
  std::uint64_t transition_revision_ = 0;
  std::array<std::optional<PendingCoefficients>, mailbox_capacity> pending_{};
  std::atomic<std::uint64_t> pending_write_{0};
  std::atomic<std::uint64_t> pending_read_{0};
  std::atomic<std::uint64_t> applied_revision_{0};
  std::uint64_t offered_revision_ = 0;            // control producer only
  std::uint64_t structural_offered_revision_ = 0; // control producer only
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
  static_assert(std::atomic<std::size_t>::is_always_lock_free);
  std::array<std::array<ChannelState, 2>, eq_section_limit> state_{};
  std::uint64_t generation_ = 0;
  std::uint64_t guarded_frames_ = 0;
};

} // namespace oscillate::native_dsp
