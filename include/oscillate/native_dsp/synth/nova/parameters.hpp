#pragma once

#include "oscillate/native_dsp/synth/nova/arp.hpp"
#include "oscillate/native_dsp/synth/parameters.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// Nova's flat parameter table (contract 08a §2): built once in the
// contract's order, with every index the kernel reads resolved up front so
// the callback never looks an id up.
namespace oscillate::native_dsp::synth::nova {

inline constexpr std::size_t oscillator_count = 3;
inline constexpr std::size_t filter_count = 2;
inline constexpr std::size_t envelope_count = 4;
inline constexpr std::size_t lfo_count = 8;
inline constexpr std::size_t macro_count = 8;
inline constexpr std::size_t chaos_count = 2;
inline constexpr std::size_t route_slots = 256;
inline constexpr std::size_t fx_uids = 24;
// p0..p15 per FX slot uid (08a §2.10); what each means depends on the module.
inline constexpr std::size_t fx_slot_params = 16;
inline constexpr std::size_t warp_slots = 2;

enum class OscType : std::uint8_t { classic, wavetable, sample, multisample, granular, spectral };
enum class ClassicShape : std::uint8_t { sine, triangle, saw, square, pulse };
enum class WarpMode : std::uint8_t {
  off,
  bend_plus,
  bend_minus,
  sync,
  window_sync,
  mirror,
  asym,
  quantise,
  pwm,
  flip,
  remap,
  fold,
  fm,
  pm,
  am,
  rm,
};
enum class WarpSource : std::uint8_t { osc_a, osc_b, osc_c, sub, noise };
// Inverse and Random were appended (ADR-0073), so saved choices keep their
// meaning.
enum class UnisonCurve : std::uint8_t { linear, exponential, super, inverse, random };
enum class UnisonStack : std::uint8_t { off, octave, two_octaves, fifth, fifth_octave, chord };
enum class SourceRoute : std::uint8_t { filter_1, filter_2, direct };
enum class FilterRouting : std::uint8_t { serial, parallel, split };
enum class LfoMode : std::uint8_t { trig, env, free };
enum class Voicing : std::uint8_t { poly, mono, legato };
enum class Quality : std::uint8_t { eco, high, ultra };
// Which voices Analogue (ADR-0073) gives their own offsets and drift.
enum class AnalogueScope : std::uint8_t { notes_and_unison, notes, unison };
// Which oscillator, in the same voice, the noise layer's level follows
// (nova.noise.am.source).
enum class NoiseAmSource : std::uint8_t { off, osc_a, osc_b, osc_c };

// nova.lfo.<l>.sync choices after "Hz", in quarter-note beats; bar entries
// are whole bars of the meter at the playhead.
inline constexpr std::size_t lfo_sync_count = 21;
struct LfoDivision {
  double length;
  bool bars;
};
inline constexpr std::array<LfoDivision, lfo_sync_count> lfo_divisions{{
    {0.0, false}, // Hz
    {8.0, true},        {4.0, true},        {2.0, true},         {1.0, true},
    {3.0, false},       {2.0, false},       {4.0 / 3.0, false},  {1.5, false},
    {1.0, false},       {2.0 / 3.0, false}, {0.75, false},       {0.5, false},
    {1.0 / 3.0, false}, {0.375, false},     {0.25, false},       {1.0 / 6.0, false},
    {0.1875, false},    {0.125, false},     {1.0 / 12.0, false}, {0.0625, false},
}};

// nova.global.portamento_division choices, in quarter-note beats: the
// glide's length with nova.global.portamento_sync on (per octave in
// Constant rate).
inline constexpr std::size_t portamento_division_count = 16;
inline constexpr std::array<double, portamento_division_count> portamento_division_beats{
    4.0,   2.0,  1.5,       1.0,    2.0 / 3.0, 0.75,       0.5,    1.0 / 3.0,
    0.375, 0.25, 1.0 / 6.0, 0.1875, 0.125,     1.0 / 12.0, 0.0625, 0.03125};
// 1/32: a short, tight slide at house and techno tempos.
inline constexpr double portamento_division_default = 12.0;

struct OscIndex {
  std::size_t on, type, level, pan, position, shape, pw, sync, drift, interpolation;
  std::array<std::size_t, warp_slots> warp_mode, warp_amount, warp_source;
  std::size_t unison_voices, unison_detune, unison_curve, unison_blend, unison_width, unison_stack,
      unison_position_spread, unison_warp_spread;
  std::size_t octave, semitone, fine, pitch, phase, random_phase, route;
  std::size_t sample_root, sample_keytrack, sample_start, sample_loop, sample_loop_start,
      sample_loop_end, sample_crossfade;
  std::size_t grain_size, grain_density, grain_scan_rate, grain_spray, grain_pitch_spray,
      grain_window, grain_reverse;
  std::size_t spectral_fft, spectral_shift, spectral_stretch, spectral_smear, spectral_tilt,
      spectral_freeze;
};
struct FilterIndex {
  std::size_t on, model, cutoff, resonance, drive, fat, keytrack, pan, mix;
};
struct EnvelopeIndex {
  std::size_t attack, hold, decay, sustain, release, attack_curve, decay_curve, release_curve, loop,
      legato, velocity;
};
struct LfoIndex {
  std::size_t rate, sync, mode, rise, delay, smooth, phase, scope, grid;
};
struct FxSlotIndex {
  std::array<std::size_t, fx_slot_params> p;
  std::size_t mix, on;
};
// One arp lane (ADR-0078): its length and its 32 step values.
struct ArpLaneIndex {
  std::size_t length;
  std::array<std::size_t, arp_max_steps> step;
};
// Analogue (ADR-0073, 08a §2.12b): per-voice tolerances, Age and drift.
struct AnalogueIndex {
  std::size_t on, age, seed;
  std::size_t pitch, cutoff, resonance, env, level, pan, shape;
  std::size_t drift_amount, drift_rate, drift_cutoff;
  std::size_t freerun, scope;
  // Voice cards, re-roll per note and flutter.
  std::size_t cards, reroll, flutter_amount, flutter_tone;
  // Glide tolerance.
  std::size_t glide;
};

struct ParameterIndex {
  std::size_t master;
  std::array<OscIndex, oscillator_count> osc;
  std::size_t sub_on, sub_shape, sub_octave, sub_level, sub_pan, sub_route;
  std::size_t noise_on, noise_level, noise_pitch, noise_keytrack, noise_tone, noise_mode, noise_pan,
      noise_route;
  std::array<FilterIndex, filter_count> filter;
  std::size_t filter_routing;
  std::array<EnvelopeIndex, envelope_count> env;
  std::array<LfoIndex, lfo_count> lfo;
  std::array<std::size_t, macro_count> macro;
  std::array<std::size_t, chaos_count> chaos_rate, chaos_smooth;
  std::array<std::size_t, route_slots> route_amount;
  std::size_t fx_split, fx_split_frequency, fx_main_oversample;
  std::array<std::size_t, 2> fx_bus_oversample, fx_bus_send, fx_bus_send_point, fx_bus_return;
  std::array<FxSlotIndex, fx_uids> fx_slot;
  std::size_t arp_on, arp_mode, arp_rate, arp_gate, arp_swing, arp_octaves, arp_velocity,
      arp_retrigger, arp_latch;
  // The arp's pattern engine (ADR-0078).
  std::size_t arp_note_mode, arp_scale, arp_root, arp_passing, arp_strum, arp_euclid_hits,
      arp_euclid_steps, arp_euclid_rotate, arp_fill, arp_variation, arp_seed, arp_groove_amount,
      arp_groove_length, arp_groove_grid;
  std::array<std::size_t, arp_groove_steps> arp_groove_timing, arp_groove_velocity;
  std::array<ArpLaneIndex, arp_lane_count> arp_lane;
  std::size_t seq_on, seq_clip, seq_trigger, seq_root, seq_follow;
  std::size_t voicing, polyphony, portamento, portamento_curve, portamento_mode, portamento_rate,
      drift, bend_range, velocity_curve, mpe, mpe_range, quality, tuning, a4;
  // Performance controls: MIDI bend, mod wheel and pressure reach native
  // synths as clip automation of these (ADR-0035).
  std::size_t perf_bend, perf_modwheel, perf_aftertouch;
  std::size_t lock_on, lock_duck, lock_crossover, lock_mono;
  AnalogueIndex analogue;
  // The noise layer's level follows an oscillator.
  std::size_t noise_am_source, noise_am_amount, noise_am_curve;
  // Portamento in note values.
  std::size_t portamento_sync, portamento_division;
};

// The table (stable for the process) and its resolved indices.
[[nodiscard]] const EngineDescriptor& descriptor();
[[nodiscard]] const ParameterIndex& index();

} // namespace oscillate::native_dsp::synth::nova
