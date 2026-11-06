#pragma once

#include "document.hpp"
#include "modulation.hpp"
#include "oscillate/native_dsp/synth/nova/filter.hpp"
#include "oscillate/native_dsp/synth/oversampler.hpp"
#include "oscillator.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

// One Nova voice (spec 08 §3–§6): three oscillators, sub and noise at the
// oversampled rate, two filters routed serial, parallel or split, the amp
// envelope, three modulation envelopes and eight voice-scope LFOs feeding
// the route matrix. prepare() on control; the rest is callback safe.
namespace oscillate::native_dsp::synth::nova {

// Sources the kernel owns, copied into every voice each tick.
struct GlobalSources {
  std::array<double, lfo_count> lfo{};
  std::array<double, lfo_count> lfo_gain{};
  std::array<double, lfo_count> lfo_phase{};
  std::array<double, macro_count> macro{};
  std::array<double, chaos_count> chaos{};
  double modwheel = 0.0, aftertouch = 0.0, bend = 0.5;
};

// One tick's musical context for a voice.
struct VoiceTick {
  std::span<const double> values; // Plain, table order, automation applied.
  const NovaState* state = nullptr;
  const GlobalSources* global = nullptr;
  std::uint32_t frames = 0; // Base-rate frames to the next tick.
  double beats_per_second = 2.0;
  double beats_per_bar = 4.0;
  double song_beat = 0.0;
  double song_bar = 0.0;
};

// An LFO's settings from modulated values, and the phase the song position
// implies for it (Free mode).
[[nodiscard]] LfoSettings lfo_settings(const Modulated& mod, std::size_t l, const VoiceTick& tick,
                                       double& song_cycles) noexcept;

// Per-chunk source buffers at the oversampled rate, shared by every voice a
// kernel renders in turn: A, B, C, Sub, Noise.
struct VoiceScratch {
  std::array<std::array<double, max_render_samples>, 5> left{};
  std::array<std::array<double, max_render_samples>, 5> right{};
};

class Voice {
public:
  // `base_rate` is the engine rate; the oversampling factor is fixed per
  // note from nova.global.quality. `slot` is the voice's place in the
  // kernel's allocator: Analogue's note-voice number.
  void prepare(double base_rate, std::size_t slot);
  void reset() noexcept;

  // A note starts on this voice. `glide_from` is the key the previous note
  // ended on (for portamento), or negative for none; `legato` is true when a
  // mono voice moves to a new note without a release. `project_frame` is
  // where the note starts: free-running phases and drift are read there, so
  // export and playback agree. `note_number` counts the notes that took a
  // fresh voice since the kernel's last reset: it picks the voice card
  // (with nova.analogue.cards) and seeds the note's re-roll.
  void start(const NoteEvent& note, const VoiceTick& tick, double glide_from, bool legato,
             std::uint64_t order, std::uint64_t project_frame, std::uint64_t note_number) noexcept;
  // The arp step that plays the next start() (ADR-0078): its Mod lane value
  // and accent, which this voice publishes as arp.mod and arp.accent until
  // another note starts on it. A played note sets 0 and no accent.
  void set_arp(double mod, bool accent) noexcept {
    arp_mod_ = mod;
    arp_accent_ = accent ? 1.0 : 0.0;
  }
  void release() noexcept;
  // A stolen voice fades out over 3 ms and then stops.
  void kill() noexcept;

  void control(const VoiceTick& tick) noexcept;
  // Adds `out.size()` base frames (at most one control tick).
  void render(std::span<engine::StereoFrame> out, VoiceScratch& scratch) noexcept;

  [[nodiscard]] bool active() const noexcept { return active_; }
  [[nodiscard]] bool released() const noexcept { return released_; }
  [[nodiscard]] bool dying() const noexcept { return kill_ > 0; }
  [[nodiscard]] std::uint64_t order() const noexcept { return order_; }
  [[nodiscard]] const NoteEvent& note() const noexcept { return note_; }
  [[nodiscard]] double key() const noexcept { return key_; }
  [[nodiscard]] double loudness() const noexcept { return amp_.level(); }
  [[nodiscard]] const SourceValues& sources() const noexcept { return sources_; }
  [[nodiscard]] const Modulated& modulated() const noexcept { return mod_; }
  [[nodiscard]] const Envelope& envelope(std::size_t index) const noexcept {
    return index == 0 ? amp_ : env_[index - 1];
  }
  [[nodiscard]] const Oscillator& oscillator(std::size_t index) const noexcept {
    return osc_[index];
  }
  [[nodiscard]] double lfo_phase(std::size_t index) const noexcept { return lfo_[index].phase(); }
  // Peaks of A, B, C, Sub, Noise since the last call, then decayed.
  void take_peaks(std::span<float, 5> out) noexcept;
  [[nodiscard]] double filter_cutoff(std::size_t index) const noexcept { return cutoff_[index]; }
  [[nodiscard]] double filter_resonance(std::size_t index) const noexcept {
    return resonance_[index];
  }
  [[nodiscard]] std::size_t slot() const noexcept { return slot_; }
  // Analogue's pitch drift on this note voice now, in cents (0 when off).
  [[nodiscard]] double analogue_drift_cents() const noexcept { return analogue_.drift_cents; }
  // Analogue's note-voice number: the voice card this note took, or the
  // allocator slot when there are no cards.
  [[nodiscard]] std::size_t card() const noexcept { return analogue_.card; }

private:
  [[nodiscard]] OscSettings osc_settings(std::size_t o) const noexcept;
  void update_pitch(const VoiceTick& tick) noexcept;
  // Analogue (ADR-0073): draws this note's seeded units and starts its drift
  // clock where the project is; then, each tick, advances the clock by
  // `seconds` and resolves the offsets and the voice sources.
  void analogue_start(std::span<const double> values, std::uint64_t project_frame,
                      std::uint64_t note_number) noexcept;
  void analogue_tick(const NovaState& state, double seconds) noexcept;
  // Start phases (free-running or offset) for oscillator `o`'s unison voices.
  void analogue_phases(const NovaState& state, std::size_t o, double osc_hz,
                       std::uint64_t project_frame) noexcept;

  double base_rate_ = 48'000.0;
  std::uint32_t oversample_ = 2;
  bool active_ = false, released_ = false;
  std::uint32_t kill_ = 0, kill_length_ = 1;
  std::uint64_t order_ = 0;
  NoteEvent note_{};
  double key_ = 60.0;        // Sounding key, with glide.
  double glide_from_ = -1.0; // Key the glide started at.
  double glide_elapsed_ = 0.0, glide_seconds_ = 0.0;
  double velocity_ = 0.8, random_ = 0.0;
  double arp_mod_ = 0.0, arp_accent_ = 0.0;

  Modulated mod_{descriptor().parameters.size()};
  SourceValues sources_{};
  Envelope amp_;
  std::array<Envelope, envelope_count - 1> env_{};
  std::array<Lfo, lfo_count> lfo_{};
  double voice_seconds_ = 0.0;

  std::array<Oscillator, oscillator_count> osc_{};
  SubOscillator sub_;
  NoiseOscillator noise_;
  std::array<VoiceFilter, filter_count> filter_{};
  CrossInputs cross_{};
  std::array<double, filter_count> cutoff_{}, resonance_{};

  // Per tick, ramped per sample.
  double amp_gain_ = 0.0, amp_velocity_ = 1.0;
  std::array<double, filter_count> filter_mix_{1.0, 1.0}, filter_pan_{};
  std::array<bool, filter_count> filter_on_{true, false};
  FilterRouting routing_ = FilterRouting::serial;
  std::array<SourceRoute, 5> route_{};

  std::array<StereoDownsampler2x, 2> down_{};
  std::array<StereoSample, 4> stage_{};
  // After the filters, before the amp: a saturating filter turns an
  // asymmetric wave (a filtered saw) into a little DC, which the amp
  // envelope would otherwise shape into a thump at the note's ends.
  double dc_pole_ = 1.0;
  StereoSample dc_in_{}, dc_out_{};
  float sub_value_ = 0.0F;
  // The noise layer's level follows an oscillator (nova.noise.am.*): which
  // one (off for none), how far, and the curve's exponent. Per tick.
  NoiseAmSource noise_am_ = NoiseAmSource::off;
  double noise_am_amount_ = 0.0, noise_am_exponent_ = 1.0;

  std::size_t slot_ = 0;
  // Analogue: seeded units (-1..1) drawn at the note's start, by part, for
  // the note voice and for each oscillator's unison voices; the drift clock
  // in cycles; and this tick's resolved offsets.
  struct AnalogueVoice {
    AnalogueSettings settings{};
    bool notes = false, unison = false; // Applied this tick.
    // The note voice's number: its card, or its slot without cards.
    std::size_t card = 0;
    std::array<double, analogue_part_count> unit{};
    std::array<std::array<std::array<double, analogue_part_count>, max_unison>, oscillator_count>
        unison_unit{};
    // This note's re-roll units, by part, for the note voice and each
    // unison voice.
    std::array<double, analogue_part_count> reroll{};
    std::array<std::array<std::array<double, analogue_part_count>, max_unison>, oscillator_count>
        unison_reroll{};
    // Flutter generators: the note voice's and each unison voice's.
    Flutter flutter{};
    std::array<std::array<Flutter, max_unison>, oscillator_count> unison_flutter{};
    double drift_cycles = 0.0;
    double cents = 0.0, drift_cents = 0.0, cutoff_octaves = 0.0;
    double resonance = 1.0, env = 1.0, gain = 1.0, pan = 0.0, shape = 0.0;
    double glide = 1.0; // Portamento time scale.
  };
  AnalogueVoice analogue_{};
  std::array<UnisonAnalogue, oscillator_count> unison_analogue_{};
};

} // namespace oscillate::native_dsp::synth::nova
