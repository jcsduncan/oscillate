#pragma once

#include "oscillate/native_dsp/synth/parameters.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace oscillate::quick {

// Engine-specific drawing helpers for a built-in synth panel. Everything else
// a panel shows comes from the engine's parameter table.
class NativeSynthVisuals {
public:
  virtual ~NativeSynthVisuals() = default;
  // Analog-prototype filter magnitude in dB at `hz`, for the patch `values`
  // (dense, in `engine` table order).
  [[nodiscard]] virtual double filter_response_db(const native_dsp::synth::EngineDescriptor& engine,
                                                  std::span<const double> values,
                                                  double hz) const = 0;
};

// The helpers for a registered engine id, or nullptr when the engine draws no
// curves (its panel then shows none).
[[nodiscard]] const NativeSynthVisuals* visuals_for(std::string_view engine_id) noexcept;

// One cycle of a generated wavetable frame (the fullest band), sampled at
// `points` evenly spaced phases from 0 to just below 1. Empty for an unknown
// table or frame. The first call builds the shared tables (control thread).
[[nodiscard]] std::vector<double> wavetable_frame_points(std::size_t table, std::size_t frame,
                                                         std::size_t points);

// Pulse pad sketches, from the laws the Pulse kernel runs. `pad` is 0..11 in
// key order from C1. Each is empty (or 0 dB) unless `engine` is Pulse.
// Amplitude at `points` times evenly spaced over [0, seconds], peak 1, at the
// reference velocity (where velocity leaves the filters alone).
[[nodiscard]] std::vector<double>
pulse_pad_envelope(const native_dsp::synth::EngineDescriptor& engine,
                   std::span<const double> values, std::size_t pad, double seconds,
                   std::size_t points);
// Pitch of the swept sine body in Hz at the same times; empty for pads with
// no swept body (everything but the kicks and toms).
[[nodiscard]] std::vector<double> pulse_pad_pitch(const native_dsp::synth::EngineDescriptor& engine,
                                                  std::span<const double> values, std::size_t pad,
                                                  double seconds, std::size_t points);
// The pad's noise or metal shaping filters in dB at `hz`; 0 for the kicks and
// toms, which have none.
[[nodiscard]] double pulse_pad_band_db(const native_dsp::synth::EngineDescriptor& engine,
                                       std::span<const double> values, std::size_t pad, double hz);

// Halo routing diagram for algorithm `index` (0..31), from the engine's
// algorithm data: the operators each one modulates (bit t is operator t + 1),
// the heard carriers and the self-feedback operator, laid out with carriers
// on row 0 and every modulator one row above its highest target. Columns are
// in units of one operator box; operators never share a cell.
struct FmAlgorithmLayout {
  bool valid = false;
  std::array<std::uint8_t, 6> modulates{};
  std::uint8_t carriers = 0;
  std::size_t feedback = 0;      // The operator feedback enters.
  std::size_t feedback_from = 0; // Whose output it is: itself, or a loop's last operator.
  std::array<double, 6> column{};
  std::array<int, 6> row{};
  double columns = 0.0;
  int rows = 0;
};
[[nodiscard]] FmAlgorithmLayout fm_algorithm_layout(std::size_t index);

// Halo operator `op` (0..5) envelope sketch: its four-rate, four-level
// envelope run as the kernel runs it (on middle C), through L1, L2 and L3,
// a short hold, then release to L4. `points` levels evenly spaced over
// `seconds`, on the envelope's own log scale (1 is full level, 0 the
// floor), so decays draw as the straight lines they are in decibels;
// `release_at` is when the sketch lets go. Empty unless `engine` is Halo.
// Drift's amp sketch is linear, peak 1.
struct EnvelopeSketch {
  std::vector<double> points;
  double seconds = 0.0;
  double release_at = 0.0;
};
[[nodiscard]] EnvelopeSketch
halo_operator_envelope(const native_dsp::synth::EngineDescriptor& engine,
                       std::span<const double> values, std::size_t op, std::size_t points);

// Torque sketches, from the laws the Torque kernel runs; empty unless
// `engine` is Torque. One cycle of the current model's oscillator mix at
// `points` phases, scaled to peak 1: the Acid saw or tilted square, the Moog
// three-oscillator mix, the Reese stack (drifted as at 0.2 s into a C2
// note) with its sub, or the Sub sine with its layer.
[[nodiscard]] std::vector<double>
torque_oscillator_cycle(const native_dsp::synth::EngineDescriptor& engine,
                        std::span<const double> values, std::size_t points);
// The Sub/808 pitch drop: semitones above the note at `points` times evenly
// spaced over [0, seconds]. It falls in Hz, f = f0 + (f_start - f0) e^(-t/tau).
[[nodiscard]] std::vector<double>
torque_pitch_drop(const native_dsp::synth::EngineDescriptor& engine, std::span<const double> values,
                  double seconds, std::size_t points);
// The note the Reese comb sketch is tuned to (C2); the kernel tunes it to
// the playing note.
inline constexpr double torque_comb_reference_hz = 65.40639132514966;

// Drift sketches, from the laws the Drift kernel runs; empty unless `engine`
// is Drift.
//
// A deterministic preview of the first note's grain cloud, not telemetry:
// the grains the kernel's scheduler law would spawn over [0, seconds) for
// these settings at rest (LFOs centred, synced divisions at 120 BPM), with
// the first note's seed and the kernel's draw order. Each grain's buffer
// position is its scan centre plus spray, in seconds behind the write head
// (0..4); its pitch is the base pitch plus interval plus jitter.
struct GrainPreview {
  double time = 0.0;      // Spawn time, seconds.
  double position = 0.0;  // Seconds behind the write head.
  double semitones = 0.0; // Playback pitch offset.
  double length = 0.0;    // Seconds.
  bool reverse = false;
  double pan = 0.0; // -1..1.
};
[[nodiscard]] std::vector<GrainPreview>
drift_grain_cloud(const native_dsp::synth::EngineDescriptor& engine, std::span<const double> values,
                  double seconds, std::size_t max_grains);
// The partial bank's sixteen lines: frequency ratio n sqrt(1 + B n^2) and
// level (n^-tilt with the odd/even balance), scaled to peak 1.
struct PartialLine {
  double ratio = 0.0;
  double level = 0.0;
};
[[nodiscard]] std::vector<PartialLine>
drift_partial_spectrum(const native_dsp::synth::EngineDescriptor& engine,
                       std::span<const double> values);
// Grain window `shape` (0 Hann, 1 Tukey, 2 Decay, 3 Swell) at `points` evenly
// spaced positions over [0, 1]; empty for an unknown shape.
[[nodiscard]] std::vector<double> drift_grain_window(int shape, std::size_t points);
// The amp envelope, run with the kernel's envelope and curve.
inline constexpr double drift_envelope_curve = 0.6;
[[nodiscard]] EnvelopeSketch drift_amp_envelope(const native_dsp::synth::EngineDescriptor& engine,
                                                std::span<const double> values, std::size_t points);

} // namespace oscillate::quick
