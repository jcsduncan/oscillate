#pragma once

#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/native_dsp/synth/knock.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

// Data tables keep one entry per row; clang-format would split them.
// clang-format off

// Knock v1 parameter table (ADR-0068): eight pads in key order from C1
// (MIDI 36), each with the same 29 controls, then the kit globals. Ids are
// persisted and automated: never rename or reuse one. Continuous controls
// are live and automatable; the choices (striker, materials, Motion, Swing
// and Key follow and the Orbit period) are read at each hit and are not
// automatable. Defaults are the "Minimal" kit.
namespace oscillate::native_dsp::synth::knock_table {

using S = ParameterScale;
using T = ParameterTiming;
using namespace std::string_view_literals;

inline constexpr std::size_t pad_count = 8;
inline constexpr std::size_t pad_parameters = 29;
inline constexpr std::size_t global_parameters = 7;
inline constexpr std::size_t parameter_count = pad_count * pad_parameters + global_parameters;

inline constexpr std::array strikers{"Mallet"sv, "Stick"sv, "Finger"sv, "Brush"sv};
inline constexpr std::array materials{"Wood"sv, "Skin"sv, "Metal"sv, "Glass"sv, "Tube"sv, "Card"sv};
enum Striker : std::uint8_t { mallet, stick, finger, brush };
enum Material : std::uint8_t { wood, skin, metal, glass, tube, card };
// Width's Motion: where each hit lands from the last (ADR-0068, Width).
inline constexpr std::array motions{"Fixed"sv, "Ping-pong"sv, "Scatter"sv, "Orbit"sv};
enum Motion : std::uint8_t { fixed, ping_pong, scatter, orbit };
// Whether a pad's hits follow the kit's Swing.
inline constexpr std::array swing_follows{"Off"sv, "On"sv};
enum SwingFollow : std::uint8_t { swing_off, swing_on };
// Whether a pad's resonator tunes to the project's key: its Pitch moves to
// the nearest note of the key's scale (knock::in_key).
inline constexpr std::array key_follows{"Off"sv, "On"sv};
enum KeyFollow : std::uint8_t { key_off, key_on };
// Orbit's period, in bars of the project's meter.
inline constexpr std::array orbit_periods{"1 bar"sv, "2 bars"sv, "4 bars"sv, "8 bars"sv};
inline constexpr std::array<double, orbit_periods.size()> orbit_bars{1.0, 2.0, 4.0, 8.0};
enum OrbitPeriod : std::uint8_t { one_bar, two_bars, four_bars, eight_bars };

// Length at its maximum means no gate; Lift at its minimum is effectively off.
inline constexpr double length_off = 2.0;
inline constexpr double lift_off = 20.0;
// The global Decay, in percent.
inline constexpr double decay_unity = 100.0;
// Haas delays the far ear by up to this many milliseconds.
inline constexpr double max_haas_ms = 20.0;
// Swing at its maximum plays an off-beat sixteenth on the dotted position.
inline constexpr double max_swing = 0.75;
// A pad's part in a groove (knock::pad_role) and its default width: a
// resonator above this is a high tick or shaker (as is any brush)...
inline constexpr double high_pitch_hz = 1'800.0;
// ...and one below this a low thud.
inline constexpr double low_pitch_hz = 220.0;

// Each pad's controls, in table order.
namespace offset {
inline constexpr std::size_t type = 0;
inline constexpr std::size_t hardness = 1;
inline constexpr std::size_t position = 2;
inline constexpr std::size_t click = 3;
inline constexpr std::size_t material = 4;
inline constexpr std::size_t pitch = 5;
inline constexpr std::size_t tone = 6;
inline constexpr std::size_t decay = 7;
inline constexpr std::size_t damping = 8;
inline constexpr std::size_t bend = 9;
inline constexpr std::size_t couple_material = 10;
inline constexpr std::size_t tuning = 11;
inline constexpr std::size_t amount = 12;
inline constexpr std::size_t size = 13;
inline constexpr std::size_t hollow = 14;
inline constexpr std::size_t snap = 15;
inline constexpr std::size_t length = 16;
inline constexpr std::size_t lift = 17;
inline constexpr std::size_t crush = 18;
inline constexpr std::size_t hand = 19;
inline constexpr std::size_t level = 20;
inline constexpr std::size_t pan = 21;
inline constexpr std::size_t room = 22;
// Width and Swing.
inline constexpr std::size_t spread = 23;
inline constexpr std::size_t motion = 24;
inline constexpr std::size_t depth = 25;
inline constexpr std::size_t haas = 26;
inline constexpr std::size_t swing_follow = 27;
// Key.
inline constexpr std::size_t key_follow = 28;
} // namespace offset

// The globals follow the pads.
namespace global {
inline constexpr std::size_t decay = pad_count * pad_parameters;
inline constexpr std::size_t room_size = decay + 1;
inline constexpr std::size_t room_level = decay + 2;
inline constexpr std::size_t velocity = decay + 3;
inline constexpr std::size_t out_level = decay + 4;
inline constexpr std::size_t swing = decay + 5;
inline constexpr std::size_t orbit = decay + 6;
} // namespace global

// A pad's part in a groove from its striker and resonator pitch.
[[nodiscard]] constexpr knock::PadRole role_of(int type, double pitch) noexcept {
  if (type == brush || pitch > high_pitch_hz) {
    return knock::PadRole::high;
  }
  return pitch < low_pitch_hz ? knock::PadRole::low : knock::PadRole::mid;
}

// A pad's Width: Spread (0..1), Motion, its Depth (0..1) and Haas (ms).
struct PadWidth {
  double spread = 0.0;
  int motion = fixed;
  double depth = 0.0;
  double haas = 0.0;
};

// One pad's sound, in table order. Anything a kit leaves out takes these
// defaults; levels are dB.
struct PadSound {
  int type = stick;
  double hardness = 0.5;
  double position = 0.3;
  double click = 0.3;
  int material = wood;
  double pitch = 400.0;
  double tone = 0.2;
  double decay = 0.1;
  double damping = 0.5;
  double bend = 0.0;
  int couple_material = card;
  double tuning = 1.0;
  double amount = 0.0;
  double size = 0.2;
  double hollow = 0.0;
  double snap = 0.0;
  double length = length_off;
  double lift = lift_off;
  double crush = 0.0;
  double hand = 0.3;
  double level = -3.1;
  double pan = 0.0;
  double room = 0.1;
  // Unset takes default_width(): a width from the sound.
  std::optional<PadWidth> width{};
  int swing_follow = swing_on;
  // Unset takes default_key_follow(): from the sound.
  std::optional<int> key_follow{};
};

// Width from the sound unless a kit says otherwise: lows stay narrow and still, mids spread their
// modes, shakers scatter and high ticks ping-pong.
[[nodiscard]] constexpr PadWidth default_width(const PadSound& s) noexcept {
  if (s.type == brush) {
    return {.spread = 0.6, .motion = scatter, .depth = 0.55, .haas = 3.0};
  }
  switch (role_of(s.type, s.pitch)) {
  case knock::PadRole::low:
    return {.spread = 0.15};
  case knock::PadRole::high:
    return {.spread = 0.5, .motion = ping_pong, .depth = 0.45};
  case knock::PadRole::mid:
    break;
  }
  return {.spread = 0.4};
}

[[nodiscard]] constexpr PadWidth pad_width(const PadSound& s) noexcept {
  return s.width.value_or(default_width(s));
}

// Key from the sound unless a kit says otherwise: low thuds and mid knocks
// (toms, blocks, pipes) tune to the key; high ticks and shakers, mostly
// noise, keep their pitch.
[[nodiscard]] constexpr int default_key_follow(const PadSound& s) noexcept {
  return role_of(s.type, s.pitch) == knock::PadRole::high ? key_off : key_on;
}

[[nodiscard]] constexpr int pad_key_follow(const PadSound& s) noexcept {
  return s.key_follow.value_or(default_key_follow(s));
}

struct Kit {
  std::array<PadSound, pad_count> pads{};
  double decay = decay_unity;
  double room_size = 0.5;
  double room_level = 0.0;
  double velocity = 1.0;
  double out_level = 0.0;
  double swing = 0.0;
  int orbit = two_bars;
};

// The default kit: tight, modern tops for minimal techno.
inline constexpr Kit minimal_kit{.pads = {{
                                     // Plop: a water drop, the pitch rising as it closes.
                                     {.type = finger,
                                      .click = 0.1,
                                      .material = glass,
                                      .pitch = 600,
                                      .tone = 0.7,
                                      .decay = 0.06,
                                      .bend = -0.8,
                                      .snap = 0.3,
                                      .length = 0.09,
                                      .lift = 150,
                                      .hand = 0.25,
                                      .level = 4.4,
                                      .pan = 0.25,
                                      .room = 0.06},
                                     // Rubber: a short, bouncy knock with the tail cut off.
                                     {.hardness = 0.4,
                                      .pitch = 170,
                                      .tone = 0.3,
                                      .decay = 0.08,
                                      .bend = 0.3,
                                      .tuning = 0.6,
                                      .amount = 0.3,
                                      .size = 0.3,
                                      .hollow = 0.3,
                                      .snap = 0.6,
                                      .length = 0.12,
                                      .lift = 60,
                                      .level = 0.9,
                                      .pan = -0.1,
                                      .room = 0.04},
                                     // Click: the smallest hard wooden click.
                                     {.hardness = 1,
                                      .position = 0.1,
                                      .click = 0.8,
                                      .pitch = 3200,
                                      .tone = 0.05,
                                      .decay = 0.015,
                                      .snap = 0.8,
                                      .length = 0.03,
                                      .lift = 800,
                                      .crush = 0.15,
                                      .hand = 0.35,
                                      .level = -2.5,
                                      .pan = -0.3,
                                      .room = 0.05},
                                     // Tick: a crushed tick of card, almost pure transient.
                                     {.hardness = 0.9,
                                      .click = 1,
                                      .material = card,
                                      .pitch = 5000,
                                      .tone = 0,
                                      .decay = 0.02,
                                      .length = 0.025,
                                      .lift = 3000,
                                      .crush = 0.35,
                                      .hand = 0.4,
                                      .level = 0.3,
                                      .pan = 0.35,
                                      .room = 0.05},
                                     // Ping: a small metal ping, gated short.
                                     {.hardness = 0.75,
                                      .material = metal,
                                      .pitch = 1400,
                                      .tone = 0.65,
                                      .decay = 0.25,
                                      .snap = 0.5,
                                      .length = 0.15,
                                      .lift = 500,
                                      .level = -3.8,
                                      .pan = 0.4,
                                      .room = 0.12},
                                     // Bongo: a tight high skin, struck with a fingertip.
                                     {.type = finger,
                                      .hardness = 0.6,
                                      .position = 0.35,
                                      .material = skin,
                                      .pitch = 380,
                                      .tone = 0.45,
                                      .decay = 0.09,
                                      .bend = 0.1,
                                      .size = 0.3,
                                      .hollow = 0.3,
                                      .snap = 0.5,
                                      .length = 0.14,
                                      .lift = 120,
                                      .level = -2.1,
                                      .pan = -0.25,
                                      .room = 0.06},
                                     // Shaker: a tight shaker with nothing below 2.5 kHz.
                                     {.type = brush,
                                      .click = 1,
                                      .material = card,
                                      .pitch = 2500,
                                      .tone = 0,
                                      .decay = 0.03,
                                      .snap = 0.2,
                                      .length = 0.05,
                                      .lift = 2500,
                                      .hand = 0.5,
                                      .level = -4.5,
                                      .pan = 0.15,
                                      .room = 0.04},
                                     // Zap: a pitched tube that falls fast, then crushed.
                                     {.hardness = 0.8,
                                      .material = tube,
                                      .pitch = 900,
                                      .tone = 0.6,
                                      .decay = 0.07,
                                      .bend = 0.9,
                                      .snap = 0.4,
                                      .length = 0.07,
                                      .lift = 300,
                                      .crush = 0.25,
                                      .level = -6.2,
                                      .pan = -0.4,
                                      .room = 0.08},
                                 }}};

constexpr double last(std::size_t size) { return static_cast<double>(size - 1); }
inline constexpr double g = synth_glide_seconds;

// Pad N (1..8) on key C1 + N - 1.
#define KNOCK_PAD(N)                                                                               \
  {"knock.pad" #N ".striker.type",                                                                 \
   "Type",                                                                                         \
   "Pad " #N " Striker",                                                                           \
   "",                                                                                             \
   S::choice,                                                                                      \
   0,                                                                                              \
   last(strikers.size()),                                                                          \
   static_cast<double>(minimal_kit.pads[(N) - 1].type),                                              \
   strikers,                                                                                       \
   T::live,                                                                                        \
   0,                                                                                              \
   false},                                                                                         \
      {"knock.pad" #N ".striker.hardness",                                                         \
       "Hardness",                                                                                 \
       "Pad " #N " Striker",                                                                       \
       "%",                                                                                        \
       S::linear,                                                                                  \
       0,                                                                                          \
       1,                                                                                          \
       minimal_kit.pads[(N) - 1].hardness,                                                           \
       {},                                                                                         \
       T::live,                                                                                    \
       g},                                                                                         \
      {"knock.pad" #N ".striker.position",                                                         \
       "Position",                                                                                 \
       "Pad " #N " Striker",                                                                       \
       "%",                                                                                        \
       S::linear,                                                                                  \
       0.02,                                                                                       \
       0.5,                                                                                        \
       minimal_kit.pads[(N) - 1].position,                                                           \
       {},                                                                                         \
       T::live,                                                                                    \
       g},                                                                                         \
      {"knock.pad" #N ".striker.click",                                                            \
       "Click",                                                                                    \
       "Pad " #N " Striker",                                                                       \
       "%",                                                                                        \
       S::linear,                                                                                  \
       0,                                                                                          \
       1,                                                                                          \
       minimal_kit.pads[(N) - 1].click,                                                              \
       {},                                                                                         \
       T::live,                                                                                    \
       g},                                                                                         \
      {"knock.pad" #N ".res.material",                                                             \
       "Material",                                                                                 \
       "Pad " #N " Resonator",                                                                     \
       "",                                                                                         \
       S::choice,                                                                                  \
       0,                                                                                          \
       last(materials.size()),                                                                     \
       static_cast<double>(minimal_kit.pads[(N) - 1].material),                                      \
       materials,                                                                                  \
       T::live,                                                                                    \
       0,                                                                                          \
       false},                                                                                     \
      {"knock.pad" #N ".res.pitch",                                                                \
       "Pitch",                                                                                    \
       "Pad " #N " Resonator",                                                                     \
       "Hz",                                                                                       \
       S::logarithmic,                                                                             \
       30,                                                                                         \
       8'000,                                                                                      \
       minimal_kit.pads[(N) - 1].pitch,                                                              \
       {},                                                                                         \
       T::live,                                                                                    \
       g},                                                                                         \
      {"knock.pad" #N ".res.tone",                                                                 \
       "Tone",                                                                                     \
       "Pad " #N " Resonator",                                                                     \
       "%",                                                                                        \
       S::linear,                                                                                  \
       0,                                                                                          \
       1,                                                                                          \
       minimal_kit.pads[(N) - 1].tone,                                                               \
       {},                                                                                         \
       T::live,                                                                                    \
       g},                                                                                         \
      {"knock.pad" #N ".res.decay",                                                                \
       "Decay",                                                                                    \
       "Pad " #N " Resonator",                                                                     \
       "s",                                                                                        \
       S::logarithmic,                                                                             \
       0.01,                                                                                       \
       3,                                                                                          \
       minimal_kit.pads[(N) - 1].decay,                                                              \
       {},                                                                                         \
       T::live,                                                                                    \
       g},                                                                                         \
      {"knock.pad" #N ".res.damping",                                                              \
       "Damping",                                                                                  \
       "Pad " #N " Resonator",                                                                     \
       "%",                                                                                        \
       S::linear,                                                                                  \
       0,                                                                                          \
       1,                                                                                          \
       minimal_kit.pads[(N) - 1].damping,                                                            \
       {},                                                                                         \
       T::live,                                                                                    \
       g},                                                                                         \
      {"knock.pad" #N ".res.bend",                                                                 \
       "Bend",                                                                                     \
       "Pad " #N " Resonator",                                                                     \
       "%",                                                                                        \
       S::linear,                                                                                  \
       -1,                                                                                         \
       1,                                                                                          \
       minimal_kit.pads[(N) - 1].bend,                                                               \
       {},                                                                                         \
       T::live,                                                                                    \
       g},                                                                                         \
      {"knock.pad" #N ".couple.material",                                                          \
       "Material",                                                                                 \
       "Pad " #N " Coupled",                                                                       \
       "",                                                                                         \
       S::choice,                                                                                  \
       0,                                                                                          \
       last(materials.size()),                                                                     \
       static_cast<double>(minimal_kit.pads[(N) - 1].couple_material),                               \
       materials,                                                                                  \
       T::live,                                                                                    \
       0,                                                                                          \
       false},                                                                                     \
      {"knock.pad" #N ".couple.tuning",                                                            \
       "Tuning",                                                                                   \
       "Pad " #N " Coupled",                                                                       \
       "",                                                                                         \
       S::logarithmic,                                                                             \
       0.25,                                                                                       \
       4,                                                                                          \
       minimal_kit.pads[(N) - 1].tuning,                                                             \
       {},                                                                                         \
       T::live,                                                                                    \
       g},                                                                                         \
      {"knock.pad" #N ".couple.amount",                                                            \
       "Coupling",                                                                                 \
       "Pad " #N " Coupled",                                                                       \
       "%",                                                                                        \
       S::linear,                                                                                  \
       0,                                                                                          \
       1,                                                                                          \
       minimal_kit.pads[(N) - 1].amount,                                                             \
       {},                                                                                         \
       T::live,                                                                                    \
       g},                                                                                         \
      {"knock.pad" #N ".body.size",  "Size", "Pad " #N " Body", "%", S::linear, 0, 1,              \
       minimal_kit.pads[(N) - 1].size, {},     T::live,           g},                                \
      {"knock.pad" #N ".body.hollow",  "Hollow", "Pad " #N " Body", "%", S::linear, 0, 1,          \
       minimal_kit.pads[(N) - 1].hollow, {},       T::live,           g},                            \
      {"knock.pad" #N ".finish.snap",                                                              \
       "Snap",                                                                                     \
       "Pad " #N " Finish",                                                                        \
       "%",                                                                                        \
       S::linear,                                                                                  \
       0,                                                                                          \
       1,                                                                                          \
       minimal_kit.pads[(N) - 1].snap,                                                               \
       {},                                                                                         \
       T::live,                                                                                    \
       g},                                                                                         \
      {"knock.pad" #N ".finish.length",                                                            \
       "Length",                                                                                   \
       "Pad " #N " Finish",                                                                        \
       "s",                                                                                        \
       S::logarithmic,                                                                             \
       0.015,                                                                                      \
       length_off,                                                                                 \
       minimal_kit.pads[(N) - 1].length,                                                             \
       {},                                                                                         \
       T::live,                                                                                    \
       g},                                                                                         \
      {"knock.pad" #N ".finish.lift",                                                              \
       "Lift",                                                                                     \
       "Pad " #N " Finish",                                                                        \
       "Hz",                                                                                       \
       S::logarithmic,                                                                             \
       lift_off,                                                                                   \
       8'000,                                                                                      \
       minimal_kit.pads[(N) - 1].lift,                                                               \
       {},                                                                                         \
       T::live,                                                                                    \
       g},                                                                                         \
      {"knock.pad" #N ".finish.crush",                                                             \
       "Crush",                                                                                    \
       "Pad " #N " Finish",                                                                        \
       "%",                                                                                        \
       S::linear,                                                                                  \
       0,                                                                                          \
       1,                                                                                          \
       minimal_kit.pads[(N) - 1].crush,                                                              \
       {},                                                                                         \
       T::live,                                                                                    \
       g},                                                                                         \
      {"knock.pad" #N ".hand",       "Variation", "Pad " #N " Hand", "%", S::linear, 0, 1,         \
       minimal_kit.pads[(N) - 1].hand, {},          T::live,           g},                           \
      {"knock.pad" #N ".level",                                                                    \
       "Level",                                                                                    \
       "Pad " #N " Output",                                                                        \
       "dB",                                                                                       \
       S::linear,                                                                                  \
       -60,                                                                                        \
       6,                                                                                          \
       minimal_kit.pads[(N) - 1].level,                                                              \
       {},                                                                                         \
       T::live,                                                                                    \
       g},                                                                                         \
      {"knock.pad" #N ".pan",                                                                      \
       "Pan",                                                                                      \
       "Pad " #N " Output",                                                                        \
       "%",                                                                                        \
       S::linear,                                                                                  \
       -1,                                                                                         \
       1,                                                                                          \
       minimal_kit.pads[(N) - 1].pan,                                                                \
       {},                                                                                         \
       T::live,                                                                                    \
       g},                                                                                         \
      {"knock.pad" #N ".room",       "Room", "Pad " #N " Output", "%", S::linear, 0, 1,            \
       minimal_kit.pads[(N) - 1].room, {},     T::live,             g},                              \
      {"knock.pad" #N ".width.spread",                                                             \
       "Spread",                                                                                   \
       "Pad " #N " Width",                                                                         \
       "%",                                                                                        \
       S::linear,                                                                                  \
       0,                                                                                          \
       1,                                                                                          \
       pad_width(minimal_kit.pads[(N) - 1]).spread,                                                  \
       {},                                                                                         \
       T::live,                                                                                    \
       g},                                                                                         \
      {"knock.pad" #N ".width.motion",                                                             \
       "Motion",                                                                                   \
       "Pad " #N " Width",                                                                         \
       "",                                                                                         \
       S::choice,                                                                                  \
       0,                                                                                          \
       last(motions.size()),                                                                       \
       static_cast<double>(pad_width(minimal_kit.pads[(N) - 1]).motion),                             \
       motions,                                                                                    \
       T::live,                                                                                    \
       0,                                                                                          \
       false},                                                                                     \
      {"knock.pad" #N ".width.depth",                                                              \
       "Depth",                                                                                    \
       "Pad " #N " Width",                                                                         \
       "%",                                                                                        \
       S::linear,                                                                                  \
       0,                                                                                          \
       1,                                                                                          \
       pad_width(minimal_kit.pads[(N) - 1]).depth,                                                   \
       {},                                                                                         \
       T::live,                                                                                    \
       g},                                                                                         \
      {"knock.pad" #N ".width.haas",                                                               \
       "Haas",                                                                                     \
       "Pad " #N " Width",                                                                         \
       "ms",                                                                                       \
       S::linear,                                                                                  \
       0,                                                                                          \
       max_haas_ms,                                                                                \
       pad_width(minimal_kit.pads[(N) - 1]).haas,                                                    \
       {},                                                                                         \
       T::live,                                                                                    \
       g},                                                                                         \
      {"knock.pad" #N ".swing.follow",                                                             \
       "Swing",                                                                                    \
       "Pad " #N " Groove",                                                                        \
       "",                                                                                         \
       S::choice,                                                                                  \
       0,                                                                                          \
       last(swing_follows.size()),                                                                 \
       static_cast<double>(minimal_kit.pads[(N) - 1].swing_follow),                                  \
       swing_follows,                                                                              \
       T::live,                                                                                    \
       0,                                                                                          \
       false},                                                                                     \
  {                                                                                                \
    "knock.pad" #N ".key.follow", "Key", "Pad " #N " Resonator", "", S::choice, 0,                 \
        last(key_follows.size()), static_cast<double>(pad_key_follow(minimal_kit.pads[(N) - 1])),    \
        key_follows, T::live, 0, false                                                             \
  }

inline constexpr std::array<ParameterDescriptor, parameter_count> parameters{{
    KNOCK_PAD(1),
    KNOCK_PAD(2),
    KNOCK_PAD(3),
    KNOCK_PAD(4),
    KNOCK_PAD(5),
    KNOCK_PAD(6),
    KNOCK_PAD(7),
    KNOCK_PAD(8),
    // Decay scales every ring, the bodies' air and (only shortening) every gate.
    {"knock.decay", "Decay", "Kit", "", S::logarithmic, 10, 150, minimal_kit.decay, {}, T::live, g},
    {"knock.room.size",
     "Size",
     "Room",
     "%",
     S::linear,
     0,
     1,
     minimal_kit.room_size,
     {},
     T::live,
     g},
    {"knock.room.level",
     "Level",
     "Room",
     "dB",
     S::linear,
     -60,
     6,
     minimal_kit.room_level,
     {},
     T::live,
     g},
    {"knock.velocity",
     "Velocity",
     "Kit",
     "%",
     S::linear,
     0,
     1,
     minimal_kit.velocity,
     {},
     T::live,
     g},
    {"knock.out.level",
     "Level",
     "Output",
     "dB",
     S::linear,
     -60,
     6,
     minimal_kit.out_level,
     {},
     T::live,
     g},
    // Swing plays each off-beat sixteenth late (knock::swing_delay_beats).
    {"knock.swing",
     "Swing",
     "Groove",
     "%",
     S::linear,
     0,
     max_swing,
     minimal_kit.swing,
     {},
     T::live,
     g},
    {"knock.orbit",
     "Orbit",
     "Width",
     "",
     S::choice,
     0,
     last(orbit_periods.size()),
     static_cast<double>(minimal_kit.orbit),
     orbit_periods,
     T::live,
     0,
     false},
}};

#undef KNOCK_PAD

consteval std::size_t index_of(std::string_view id) {
  return parameter_index(parameters, id);
}

static_assert(index_of("knock.pad1.room") == offset::room);
static_assert(index_of("knock.pad8.striker.type") == 7 * pad_parameters + offset::type);
static_assert(index_of("knock.pad3.finish.length") == 2 * pad_parameters + offset::length);
static_assert(index_of("knock.decay") == global::decay);
static_assert(index_of("knock.out.level") == global::out_level);
static_assert(index_of("knock.pad1.width.spread") == offset::spread);
static_assert(index_of("knock.pad2.width.motion") == pad_parameters + offset::motion);
static_assert(index_of("knock.pad4.width.depth") == 3 * pad_parameters + offset::depth);
static_assert(index_of("knock.pad6.width.haas") == 5 * pad_parameters + offset::haas);
static_assert(index_of("knock.pad8.swing.follow") == 7 * pad_parameters + offset::swing_follow);
static_assert(index_of("knock.pad8.key.follow") == 7 * pad_parameters + offset::key_follow);
static_assert(offset::key_follow + 1 == pad_parameters, "Width, Swing and Key close each pad");
static_assert(index_of("knock.swing") == global::swing);
static_assert(index_of("knock.orbit") == global::orbit);
static_assert(global::orbit + 1 == parameter_count, "the globals close the table");

// A kit as dense values in table order.
constexpr std::array<double, parameter_count> kit_values(const Kit& kit) {
  std::array<double, parameter_count> v{};
  for (std::size_t pad = 0; pad < pad_count; ++pad) {
    const PadSound& s = kit.pads[pad];
    double* p = v.data() + pad * pad_parameters;
    p[offset::type] = static_cast<double>(s.type);
    p[offset::hardness] = s.hardness;
    p[offset::position] = s.position;
    p[offset::click] = s.click;
    p[offset::material] = static_cast<double>(s.material);
    p[offset::pitch] = s.pitch;
    p[offset::tone] = s.tone;
    p[offset::decay] = s.decay;
    p[offset::damping] = s.damping;
    p[offset::bend] = s.bend;
    p[offset::couple_material] = static_cast<double>(s.couple_material);
    p[offset::tuning] = s.tuning;
    p[offset::amount] = s.amount;
    p[offset::size] = s.size;
    p[offset::hollow] = s.hollow;
    p[offset::snap] = s.snap;
    p[offset::length] = s.length;
    p[offset::lift] = s.lift;
    p[offset::crush] = s.crush;
    p[offset::hand] = s.hand;
    p[offset::level] = s.level;
    p[offset::pan] = s.pan;
    p[offset::room] = s.room;
    const PadWidth w = pad_width(s);
    p[offset::spread] = w.spread;
    p[offset::motion] = static_cast<double>(w.motion);
    p[offset::depth] = w.depth;
    p[offset::haas] = w.haas;
    p[offset::swing_follow] = static_cast<double>(s.swing_follow);
    p[offset::key_follow] = static_cast<double>(pad_key_follow(s));
  }
  v[global::decay] = kit.decay;
  v[global::room_size] = kit.room_size;
  v[global::room_level] = kit.room_level;
  v[global::velocity] = kit.velocity;
  v[global::out_level] = kit.out_level;
  v[global::swing] = kit.swing;
  v[global::orbit] = static_cast<double>(kit.orbit);
  return v;
}

} // namespace oscillate::native_dsp::synth::knock_table
// clang-format on
