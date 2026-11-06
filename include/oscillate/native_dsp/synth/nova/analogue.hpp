#pragma once

#include "oscillate/project/model.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Nova Analogue (ADR-0073, contract 08a §2.12b and §3.7): every voice, each
// played note's and each unison voice, gets its own fixed offsets from a
// seed and its number, scaled by its part's tolerance and Age, plus a slow
// drift. Voice Lanes are the per-voice values, painted values overriding
// the seeded ones. The panel reads the lanes through analogue_lane_values,
// the same code the kernel runs, so what it draws is what plays.
namespace oscillate::native_dsp::synth::nova {

inline constexpr double analogue_seed_min = 1.0;
inline constexpr double analogue_seed_max = 9'999.0;
inline constexpr double analogue_seed_default = 4'127.0;

// A seeded offset u (uniform in -1..1) becomes u × tolerance × Age × this,
// clamped to -1..1: at Age 1 and full tolerance most voices sit near their
// part's maximum and the widest third are pinned to it, as a worn card's
// trimmers would be.
inline constexpr double analogue_age_headroom = 1.6;

// What a lane value of ±1 means for each part.
inline constexpr double analogue_pitch_cents = 12.0;
// Cutoff and envelope times scale by (1 + ratio)^value: symmetric in
// octaves, so a dark voice is as far below as a bright one is above.
inline constexpr double analogue_cutoff_ratio = 0.18;
inline constexpr double analogue_env_ratio = 0.25;
// Portamento times scale the same way: a worn card's glide slew is up to
// this much faster or slower.
inline constexpr double analogue_glide_ratio = 0.35;
inline constexpr double analogue_glide_default = 0.3;
// Resonance scales by 1 + ratio × value.
inline constexpr double analogue_resonance_ratio = 0.10;
inline constexpr double analogue_level_db = 1.5;
// Added to the pan (-1..1), and to an oscillator's pulse width, position and
// warp amounts (0..1).
inline constexpr double analogue_pan = 0.40;
inline constexpr double analogue_shape = 0.08;
// The Phase lane offsets the start phase by up to half a cycle (±180°).
inline constexpr double analogue_phase_cycles = 0.5;
// Drift: at Amount 1 a voice wanders up to this far; Rate 0..1 maps
// linearly onto min..max Hz; On Cutoff 1 moves the cutoff by (1 + ratio)^±1.
inline constexpr double analogue_drift_cents = 8.0;
inline constexpr double analogue_drift_min_hz = 0.02;
inline constexpr double analogue_drift_max_hz = 0.62;
inline constexpr double analogue_drift_cutoff_ratio = 0.10;

// Voice cards (nova.analogue.cards): 0 numbers note voices by the
// allocator's slot; 1..max gives the synth that many cards, each new note
// taking the next one round-robin (6 is a Prophet-6).
inline constexpr double analogue_cards_max = 16.0;
// Re-roll (nova.analogue.reroll) at 1 adds to each part, at every note, a
// fresh seeded offset as wide as the fixed seeded one can be.
//
// Flutter: fast, low-passed seeded pitch jitter per voice, up to ±this many
// cents at Amount 1 (useful well below that: "almost inaudible"); Tone is
// the noise's low-pass corner, log min..max Hz.
inline constexpr double analogue_flutter_cents = 15.0;
inline constexpr double analogue_flutter_tone_min_hz = 2.0;
inline constexpr double analogue_flutter_tone_max_hz = 60.0;
inline constexpr double analogue_flutter_tone_default_hz = 12.0;

// Note voices are numbered by the voice allocator's slot: 32 playing plus
// room for stolen voices' fades. Unison voices by their place in an
// oscillator's stack.
inline constexpr std::size_t analogue_note_voices = 40;
static_assert(analogue_cards_max <= static_cast<double>(analogue_note_voices));
inline constexpr std::size_t analogue_unison_voices = 16;
inline constexpr std::size_t analogue_oscillators = 3;

enum class AnalogueKind : std::uint8_t { notes, unison };
inline constexpr std::array<std::string_view, 2> analogue_kind_ids{"notes", "unison"};

// The Voice Lanes, in the panel's order.
enum class AnalogueLane : std::uint8_t { pitch, cutoff, env, level, pan, phase, mod_a, mod_b };
inline constexpr std::size_t analogue_lane_count = 8;
inline constexpr std::array<std::string_view, analogue_lane_count> analogue_lane_ids{
    "pitch", "cutoff", "env", "level", "pan", "phase", "moda", "modb"};

[[nodiscard]] std::optional<AnalogueKind> analogue_kind_from_id(std::string_view id) noexcept;
[[nodiscard]] std::optional<AnalogueLane> analogue_lane_from_id(std::string_view id) noexcept;
// Unison voices share their note's filter, envelopes and modulation, so
// only Pitch, Level, Pan and Phase reach them; note voices take every lane.
[[nodiscard]] bool analogue_lane_applies(AnalogueKind kind, AnalogueLane lane) noexcept;
// Voices a kind numbers: analogue_note_voices or analogue_unison_voices.
[[nodiscard]] std::size_t analogue_voice_limit(AnalogueKind kind) noexcept;
// The lane's document id (08a §3.7): "notes.<lane>" or "unison.<o>.<lane>"
// with <o> = a, b, c. `osc` is ignored for notes.
[[nodiscard]] std::string analogue_lane_document_id(AnalogueKind kind, std::size_t osc,
                                                    AnalogueLane lane);

// One voice's lane value, -1..1, and whether the user painted it.
struct AnalogueLaneValue {
  double value = 0.0;
  bool edited = false;
};

// Voices 0..voice_count-1 of a lane (voice_count clamped to the kind's
// limit), as the kernel uses them: the painted value, else the seeded one
// at the current seed, tolerance and Age. Pitch, Cutoff, Env, Level and Pan
// scale with their tolerance; Phase with Age alone; Mod A and Mod B are the
// seeded values unscaled (a route's amount scales them). `values` are
// Nova's flat values in table order. Analogue's on switch and scope do not
// change what a lane holds, only whether the kernel applies it. Empty when
// `values` is not Nova's table, `osc` is out of range or the lane does not
// apply to the kind. Control only.
[[nodiscard]] std::vector<AnalogueLaneValue>
analogue_lane_values(std::span<const double> values, const project::NativeSynthDocument& document,
                     AnalogueKind kind, std::size_t osc, AnalogueLane lane,
                     std::size_t voice_count);

// Paints one voice (the value clamped to -1..1) or, with nullopt, returns it
// to the seed; a lane left with no values leaves the document. False, with
// the document unchanged, when the lane does not apply to the kind, `osc`
// or `voice` is out of range, the value is not finite or the lane is full.
[[nodiscard]] bool set_analogue_lane_value(project::NativeSynthDocument& document,
                                           AnalogueKind kind, std::size_t osc, AnalogueLane lane,
                                           std::size_t voice, std::optional<double> value);
// Returns every voice of the lane to the seed. False when it had none.
bool clear_analogue_lane(project::NativeSynthDocument& document, AnalogueKind kind, std::size_t osc,
                         AnalogueLane lane);

} // namespace oscillate::native_dsp::synth::nova
