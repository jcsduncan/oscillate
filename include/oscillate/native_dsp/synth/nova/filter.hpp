#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <vector>

// Nova's voice filters (spec 08 §4, contract 08a §2.4): 22 models behind one
// per-voice stereo filter. Driven from the control tick with targets reached
// by linear ramps, so fast modulation is zipper-free.
namespace oscillate::native_dsp::synth::nova {

// In the contract's index order (nova.filter.<f>.model).
enum class FilterModel : std::uint8_t {
  ladder24,
  ladder12,
  diode,
  mg_low,
  ota,
  lp12,
  lp24,
  bp12,
  bp24,
  hp12,
  hp24,
  notch,
  peak,
  formant,
  vowel,
  comb_plus,
  comb_minus,
  phaser,
  flanger,
  morph_lbh,
  dual_peak,
  ring,
};
inline constexpr std::size_t filter_model_count = 22;
[[nodiscard]] std::string_view filter_model_name(FilterModel model) noexcept;

struct FilterSettings {
  FilterModel model = FilterModel::ladder24;
  double cutoff_hz = 2'000.0; // 8..22000; keytrack is already applied.
  double resonance = 0.2;     // 0..1; analog models self-oscillate near 1.
  double drive_db = 0.0;      // 0..24: pre-filter saturation.
  // 0..1, model-specific: low-end compensation for the ladders and diode,
  // a second stage for the clean models, formant/vowel morph position, comb
  // and flanger damping, phaser stage spread, morph-model sweep shape.
  double fat = 0.0;
};

// One voice's stereo filter. prepare() on control; everything else is
// callback safe and allocation-free.
class VoiceFilter {
public:
  // `sample_rate` is the voice's internal (oversampled) rate.
  void prepare(double sample_rate);
  void reset() noexcept;
  // At a control tick: the new targets, reached linearly over `frames`
  // samples (cutoff in log-frequency). A model change crossfades the old and
  // new models over 5 ms, so a switch never clicks.
  void set(const FilterSettings& settings, std::uint32_t frames) noexcept;
  // One sample; left and right are filtered independently (same settings).
  void process(float& left, float& right) noexcept;
  // The reference path: every model derives its coefficients at every
  // refresh, blending none (see blends()). For tests; off by default.
  void set_exact(bool exact) noexcept { exact_ = exact; }

  // The ramped settings a model derives from.
  struct Params {
    double cutoff_hz = 2'000.0;
    double resonance = 0.2;
    double drive_db = 0.0;
    double fat = 0.0;
  };

private:
  struct Impl;
  friend struct FilterResponseProbe;

  // What a model derives from the ramped settings, refreshed every few
  // samples. Each model reads only the fields it needs (filter.cpp).
  struct Band {
    double g = 0.0;    // Prewarped centre, tan(pi f / fs).
    double k = 1.0;    // Damping, or a one-pole gain.
    double gain = 0.0; // Mix level or bell boost.
  };
  struct Coefficients {
    double g = 0.0;  // Prewarped cutoff (model-scaled), or the ring increment.
    double G = 0.0;  // Trapezoidal one-pole gain g / (1 + g).
    double k = 0.0;  // Damping or loop feedback.
    double k2 = 0.0; // Second-stage damping, or a bell's boost.
    double solve = 1.0;
    double input_gain = 1.0;
    double output_gain = 1.0;
    double delay = 2.0;   // Delay-line models: loop delay in frames.
    double damping = 1.0; // Delay-line models: loop one-pole gain.
    double sign = 1.0;
    double weight_low = 1.0;
    double weight_band = 0.0;
    double weight_high = 0.0;
    double stage_drive = 1.0; // FAT's second-stage saturation.
    double stage_makeup = 1.0;
    double stage_mix = 0.0;
    std::array<Band, 8> band{};
  };
  static constexpr std::size_t memory_size = 12;
  // One model instance: its coefficients and both channels' memory. Two
  // slots let a model change crossfade.
  struct Slot {
    FilterModel model = FilterModel::ladder24;
    Coefficients c{};
    std::array<std::array<double, memory_size>, 2> memory{};
    std::array<std::uint32_t, 2> noise{};
    std::size_t write = 0; // Delay-line write position, shared by both channels.
  };

  void refresh() noexcept;
  void refresh_drive(double drive_db) noexcept;
  void start_fade() noexcept;
  // Formant, Vowel and Phaser derive many bands, each through a tangent
  // and powers. During a gentle ramp (cutoff, resonance and FAT each moving
  // at most blend_step in one tick: 1/384 octave for the cutoff) they derive
  // exactly at its ends and move linearly between in their coefficients.
  // That differs from the exact path by the curvature of one step, at most
  // (ln 2 / 384)^2 / 8, under 0.5 ppm of a coefficient. Faster ramps (a
  // wobble's square LFO, an envelope sweep) and ramps that cross from one
  // pair of vowels to the next (where the bands turn) derive at every
  // refresh, as all other models do.
  static constexpr double blend_step = 1.0 / 384.0;
  [[nodiscard]] static bool blends(FilterModel model) noexcept;
  [[nodiscard]] static Params params(const std::array<double, 4>& value) noexcept;
  void clear(std::size_t slot) noexcept;
  [[nodiscard]] double drive(std::size_t channel, double x) noexcept;

  std::array<Slot, 2> slots_{};
  std::size_t active_ = 0;
  // Delay lines for combs and flangers, [slot][channel][delay_size_], sized
  // at prepare; no allocation afterwards.
  std::vector<float> delay_;
  std::size_t delay_size_ = 0;
  double sample_rate_ = 48'000.0;
  // Ramped values: log2 cutoff, resonance, drive dB, FAT.
  std::array<double, 4> value_{10.965784284662087, 0.2, 0.0, 0.0};
  std::array<double, 4> step_{};
  std::array<double, 4> goal_{10.965784284662087, 0.2, 0.0, 0.0};
  std::uint32_t ramp_ = 0;
  FilterModel target_model_ = FilterModel::ladder24;
  bool primed_ = false;
  bool still_ = false; // This ramp's steps are all zero.
  bool dirty_ = true;
  std::uint32_t tick_ = 0;
  std::uint32_t interval_ = 1;
  // Model crossfade: frames left, of fade_length_.
  std::uint32_t fade_ = 0;
  std::uint32_t fade_length_ = 240;
  // A banked model's ramp: blending from_ to to_ over span_ samples.
  bool exact_ = false;
  bool blend_ = false;
  Coefficients from_{}, to_{};
  std::uint32_t span_ = 1;
  // Pre-filter drive, derived with the coefficients, for drive_db_.
  double drive_db_ = std::numeric_limits<double>::quiet_NaN();
  double drive_gain_ = 1.0;
  double drive_makeup_ = 1.0;
  double drive_bias_ = 0.0;
  double drive_bias_level_ = 0.0;
  double drive_slope_ = 1.0;
  double drive_mix_ = 0.0;
  double dc_pole_ = 0.999;
  std::array<double, 2> dc_in_{};
  std::array<double, 2> dc_out_{};
};

// The magnitude response in dB of a model at `frequency_hz`, for analysis
// and tests (the panel draws its own curves).
[[nodiscard]] double filter_response_db(const FilterSettings& settings, double frequency_hz,
                                        double sample_rate);

} // namespace oscillate::native_dsp::synth::nova
