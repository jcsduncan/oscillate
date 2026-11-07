#include "oscillate/application/nova_arp_patterns.hpp"
#include "oscillate/foundation/split_mix.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace oscillate::application::nova_arp {

namespace {

namespace synth = native_dsp::synth;
using foundation::Error;
using foundation::ErrorCode;
using nova::ArpChord;
using nova::ArpCondition;
using nova::ArpLane;
using nova::ArpLaneKind;
using nova::ArpMode;
using nova::ArpNoteMode;

// ---- The library ----------------------------------------------------------------------
// Minimal techno and house first: simple notes, displaced, polymetric lanes
// and gaps rather than square on-grid repeats.
constexpr std::array<PatternSpec, 60> library{{
    // Minimal.
    {.name = "Displaced Five",
     .category = "Minimal",
     .mode = ArpMode::euclid,
     .gate = 0.5,
     .swing = 0.12,
     .variation = 4,
     .euclid = {5, 16, 3},
     .note = "0 2 1 3 0 4 2",
     .velocity = "x6869575x6869576",
     .gate_lane = "636638636936",
     .chance = "xxx7xxxxxx6xxxx8",
     .mod = "25374"},
    {.name = "Skip Stone",
     .category = "Minimal",
     .gate = 0.45,
     .swing = 0.15,
     .variation = 8,
     .note = "0 1 0 2 1",
     .octave = "0 0 1 0 0 0 1",
     .velocity = "x7958",
     .gate_lane = "x.x..x.x.x..x.x.",
     .chance = "xxxxxx7x"},
    {.name = "Seven Over Sixteen",
     .category = "Minimal",
     .mode = ArpMode::played,
     .gate = 0.5,
     .swing = 0.1,
     .note = "0 2 1",
     .velocity = "x6x6x7x6x6x7x6x5",
     .gate_lane = "x.x.xx.",
     .mod = "1234567"},
    {.name = "Hollow Steps",
     .category = "Minimal",
     .mode = ArpMode::converge,
     .gate = 0.4,
     .swing = 0.08,
     .octave = "0 0 0 1 0 0 -1 0 0 0 1",
     .gate_lane = "x..x..x.x..x..x.",
     .chance = "xxxx6xxx"},
    {.name = "Ghost Thumb",
     .category = "Minimal",
     .mode = ArpMode::thumb,
     .gate = 0.35,
     .swing = 0.14,
     .velocity = "x3x4x3x5",
     .gate_lane = "x.xxx.xxx.xx",
     .accent = "x......"},
    {.name = "Offset Pulse",
     .category = "Minimal",
     .rate = "1/8",
     .gate = 0.5,
     .swing = 0.2,
     .variation = 4,
     .note = "0 1 2 1 3",
     .gate_lane = ".x.x.xx."},
    {.name = "Three Gaps",
     .category = "Minimal",
     .mode = ArpMode::walk,
     .gate = 0.5,
     .swing = 0.1,
     .velocity = "x777",
     .gate_lane = "xxx.xx.xxx.x.xx."},
    {.name = "Late Bloom",
     .category = "Minimal",
     .mode = ArpMode::up_down,
     .gate = 0.45,
     .swing = 0.12,
     .chance = "2345678xxxxxxxxx",
     .mod = "1122334455667788899xxxxxxxxxxxxx"},
    {.name = "Dust Steps",
     .category = "Minimal",
     .mode = ArpMode::shuffle,
     .gate = 0.4,
     .swing = 0.15,
     .variation = 8,
     .gate_lane = "x.xx.x.xx.x.x.x.",
     .chance = "xx8x7xx9"},
    {.name = "Hypno Cell",
     .category = "Minimal",
     .mode = ArpMode::shape,
     .gate = 0.5,
     .swing = 0.1,
     .variation = 8,
     .note = "0 2 1 0 3 1 2",
     .gate_lane = "x.xx.x.xx.x."},
    // Deep House.
    {.name = "Rhodes Offbeats",
     .category = "Deep House",
     .mode = ArpMode::chord,
     .rate = "1/8",
     .gate = 0.45,
     .swing = 0.2,
     .variation = 8,
     .velocity = ".7.8.7.9",
     .gate_lane = ".7.7.7.x",
     .chance = "xxxxx8xx",
     .mod = "3456654334566543"},
    {.name = "Warm Strum",
     .category = "Deep House",
     .mode = ArpMode::strum,
     .rate = "1/8",
     .gate = 0.7,
     .strum = 0.5,
     .velocity = "x78",
     .gate_lane = "x..x..x."},
    {.name = "Garage Shuffle",
     .category = "Deep House",
     .gate = 0.5,
     .swing = 0.35,
     .gate_lane = "x.x.xx.x.x.xx.x.",
     .accent = "x...x.....x....."},
    {.name = "Soft Ladder",
     .category = "Deep House",
     .gate = 0.55,
     .swing = 0.2,
     .octaves = 2,
     .velocity = "8675",
     .gate_lane = "x.xx.xx."},
    {.name = "Sunday Chords",
     .category = "Deep House",
     .mode = ArpMode::chord,
     .gate = 0.4,
     .swing = 0.25,
     .velocity = "..8....9..7...8.",
     .gate_lane = "..x....x..x...x."},
    {.name = "Chicago Walk",
     .category = "Deep House",
     .mode = ArpMode::walk,
     .gate = 0.5,
     .swing = 0.25,
     .octave = "0 0 0 1",
     .gate_lane = "x.xxx.x.x.xxx.x."},
    {.name = "Velvet Eighths",
     .category = "Deep House",
     .mode = ArpMode::played,
     .rate = "1/8",
     .gate = 0.6,
     .swing = 0.15,
     .velocity = "8696",
     .chord = "- tri - - 7 - - -"},
    // Techno.
    {.name = "Rolling 16ths",
     .category = "Techno",
     .gate = 0.55,
     .octaves = 2,
     .variation = 4,
     .octave = "0 0 1",
     .velocity = "x675",
     .ratchet = "1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 3",
     .accent = "x...",
     .mod = "..11223344556677889999xxxxxxxxxx"},
    {.name = "Warehouse Ratchet",
     .category = "Techno",
     .gate = 0.4,
     .velocity = "x7",
     .ratchet = "1 1 2 1 1 1 1 1 1 1 2 1 1 1 3 1",
     .accent = "x.."},
    {.name = "Steel Ladder",
     .category = "Techno",
     .gate = 0.5,
     .octaves = 3,
     .note = "0 1 2",
     .octave = "0 0 0 0 1 1"},
    {.name = "Dark Drive",
     .category = "Techno",
     .mode = ArpMode::down,
     .gate = 0.6,
     .octaves = 2,
     .gate_lane = "xxxxxxxxxxxxxx.x",
     .accent = "x....",
     .mod = "1357"},
    {.name = "Pressure Up",
     .category = "Techno",
     .gate = 0.45,
     .variation = 8,
     .velocity = "4455667788899xxxxxxxxxxxxxxxxxxx",
     .chance = "5566778899xxxxxxxxxxxxxxxxxxxxxx"},
    {.name = "Concrete Converge",
     .category = "Techno",
     .mode = ArpMode::converge,
     .gate = 0.5,
     .octave = "0 1 0 0 1",
     .accent = "x...x.."},
    {.name = "Night Bus",
     .category = "Techno",
     .mode = ArpMode::down_up,
     .gate = 0.5,
     .swing = 0.1,
     .note = "0 0 1 0 2",
     .gate_lane = "xx.xxx.xx.xxx.x."},
    // Acid.
    {.name = "Squelch Walk",
     .category = "Acid",
     .mode = ArpMode::walk,
     .gate = 0.7,
     .octaves = 2,
     .octave = "0 0 1 0 -1 0 0 0 1 0 0 -1",
     .gate_lane = "77x7.77x77x7.777",
     .tie = "..x....x..x.....",
     .accent = "x..x...",
     .mod = "2457753"},
    {.name = "303 Slides",
     .category = "Acid",
     .gate = 0.6,
     .octaves = 2,
     .octave = "0 0 0 1 0 0 1 0",
     .tie = "x...x..xx...x...",
     .accent = "..x...x..x..x...",
     .mod = "3579"},
    {.name = "Rubber Ladder",
     .category = "Acid",
     .mode = ArpMode::up_down,
     .gate = 0.55,
     .octaves = 2,
     .tie = ".x...x..",
     .accent = "x.....",
     .mod = "123456789"},
    {.name = "Bubbling Under",
     .category = "Acid",
     .mode = ArpMode::random,
     .gate = 0.5,
     .octave = "0 -1 0 0 1 0 -1",
     .gate_lane = "x7x7.7x7x7x.7x7x",
     .tie = "...x....x.......",
     .accent = "x.x...."},
    {.name = "Acid Thumb",
     .category = "Acid",
     .mode = ArpMode::thumb,
     .gate = 0.55,
     .octaves = 2,
     .tie = "....x.......x...",
     .accent = "x..x..",
     .mod = "246864"},
    {.name = "Resonant Steps",
     .category = "Acid",
     .mode = ArpMode::played,
     .gate = 0.5,
     .note = "0 0 2 0 1 0 3",
     .accent = ".x..x..x",
     .mod = "13579x9753"},
    {.name = "Dirty Octaves",
     .category = "Acid",
     .gate = 0.45,
     .octave = "0 1 0 1 0 1 1 0",
     .ratchet = "1 1 1 1 1 1 1 2",
     .accent = "x...x...x.x....."},
    // Dub Chords.
    {.name = "Echo Stab",
     .category = "Dub Chords",
     .mode = ArpMode::chord,
     .gate = 0.3,
     .swing = 0.15,
     .velocity = "8",
     .gate_lane = "..6....6..6...5.",
     .condition = "- - - - - - - - - - - - - - 1:2 -",
     .mod = "4"},
    {.name = "Chord Rain",
     .category = "Dub Chords",
     .mode = ArpMode::strum,
     .gate = 0.35,
     .variation = 8,
     .strum = 0.6,
     .gate_lane = "..x..x....x..x..",
     .chance = "xxx8xx7x"},
    {.name = "Berlin Fog",
     .category = "Dub Chords",
     .mode = ArpMode::chord,
     .rate = "1/8",
     .gate = 0.5,
     .swing = 0.2,
     .gate_lane = ".x...x.x",
     .chance = "xxx6"},
    {.name = "Deep Drift",
     .category = "Dub Chords",
     .mode = ArpMode::chord,
     .gate = 0.35,
     .octave = "0 0 0 0 0 0 0 1",
     .gate_lane = "...x.....x....x.",
     .condition = "- - - 1:2 - - - - - - - 2:2"},
    {.name = "Tape Skank",
     .category = "Dub Chords",
     .mode = ArpMode::chord,
     .rate = "1/8",
     .gate = 0.25,
     .swing = 0.25,
     .velocity = "x7",
     .gate_lane = ".x.x.x.x"},
    {.name = "Wide Dub",
     .category = "Dub Chords",
     .mode = ArpMode::strum,
     .gate = 0.4,
     .strum = 0.8,
     .gate_lane = "x.....x.....x...",
     .condition = "- - - - - - - - - - - - F"},
    {.name = "Late Night Chords",
     .category = "Dub Chords",
     .mode = ArpMode::chord,
     .gate = 0.45,
     .swing = 0.3,
     .variation = 16,
     .gate_lane = "..x..x..x...x...",
     .condition = "- - - - - 1:2 - - - - - - F - - -"},
    // Trance Gate.
    {.name = "Sidechain Gate 16",
     .category = "Trance Gate",
     .mode = ArpMode::chord,
     .gate = 0.9,
     .velocity = "5x89",
     .gate_lane = ".888.888.888.885"},
    {.name = "Stutter Gate",
     .category = "Trance Gate",
     .mode = ArpMode::chord,
     .gate = 0.7,
     .gate_lane = "x.x.xxx.x.x.xx.x",
     .ratchet = "1 1 1 1 1 1 1 1 1 1 1 1 1 1 2 4"},
    {.name = "Breathing Gate",
     .category = "Trance Gate",
     .mode = ArpMode::chord,
     .gate = 0.85,
     .velocity = "45678899xx998876544567889xx99887",
     .gate_lane = "x.xxx.xxx.xxx.xx"},
    {.name = "Triplet Gate",
     .category = "Trance Gate",
     .mode = ArpMode::chord,
     .rate = "1/16 T",
     .gate = 0.8,
     .gate_lane = "xx.xx.xx.xx."},
    {.name = "Half Gate",
     .category = "Trance Gate",
     .mode = ArpMode::chord,
     .rate = "1/8",
     .gate = 0.9,
     .gate_lane = "x.x.xx.x"},
    {.name = "Lift Gate",
     .category = "Trance Gate",
     .mode = ArpMode::chord,
     .gate = 0.8,
     .velocity = "55556666777788889999xxxxxxxxxxxx",
     .gate_lane = "x.x.x.x.x.x.x.x.xxxxxxxxxxxxxxxx"},
    {.name = "Pump Chords",
     .category = "Trance Gate",
     .mode = ArpMode::chord,
     .gate = 0.9,
     .velocity = "4x9x",
     .gate_lane = ".7xx.7xx.7xx.7xx"},
    // Classic.
    {.name = "Munich Bounce",
     .category = "Classic",
     .mode = ArpMode::played,
     .gate = 0.45,
     .note = "0 0 2 0",
     .octave = "0 1",
     .velocity = "x7",
     .mod = "2333444455556666"},
    {.name = "Lyon Arpeggio",
     .category = "Classic",
     .mode = ArpMode::up_down,
     .rate = "1/16 T",
     .gate = 0.55,
     .octaves = 2},
    {.name = "Oxygen Ladder", .category = "Classic", .gate = 0.5, .octaves = 3, .velocity = "x787"},
    {.name = "Disco Octaves", .category = "Classic", .gate = 0.4, .octave = "0 1", .accent = "x."},
    {.name = "Berlin School Run",
     .category = "Classic",
     .gate = 0.6,
     .octaves = 2,
     .note = "0 1 2 1",
     .mod = "123456787654321"},
    {.name = "Sequencer 1979",
     .category = "Classic",
     .mode = ArpMode::pinky,
     .gate = 0.5,
     .velocity = "x6"},
    {.name = "Cascades",
     .category = "Classic",
     .mode = ArpMode::down,
     .rate = "1/16 T",
     .gate = 0.45,
     .octaves = 3,
     .velocity = "x765"},
    // Polymetric.
    {.name = "16 · 12 · 7",
     .category = "Polymetric",
     .gate = 0.55,
     .octaves = 2,
     .note = "0 1 0 2 0 3 1",
     .velocity = "x5759575x5757576",
     .gate_lane = "7.575.7.5.75",
     .accent = "x...."},
    {.name = "Five Against Four",
     .category = "Polymetric",
     .gate = 0.5,
     .note = "0 1 2 3 1",
     .velocity = "x786",
     .accent = "x..."},
    {.name = "Euclid 7 of 16",
     .category = "Polymetric",
     .mode = ArpMode::euclid,
     .gate = 0.5,
     .euclid = {7, 16, 0},
     .note = "0 2 1 3",
     .octave = "0 0 1 0 0"},
    {.name = "Euclid 5 of 12",
     .category = "Polymetric",
     .mode = ArpMode::euclid,
     .gate = 0.5,
     .euclid = {5, 12, 2},
     .note = "0 1 2",
     .mod = "2468"},
    {.name = "Spiral Threes",
     .category = "Polymetric",
     .gate = 0.5,
     .octaves = 2,
     .note = "0 1 2",
     .octave = "0 0 0 1 1",
     .gate_lane = "x.xx.xx"},
    {.name = "Drifting Thirds",
     .category = "Polymetric",
     .gate = 0.5,
     .note_mode = ArpNoteMode::scale,
     .note = "0 2 4 2 0 -1",
     .gate_lane = "x.x.x.xx.x.x.x."},
    {.name = "Phase Pair",
     .category = "Polymetric",
     .mode = ArpMode::played,
     .gate = 0.45,
     .velocity = "x7x7x7x7x7x7x7x7",
     .gate_lane = "x.xx.x.xx.x.x",
     .accent = "x......."},
    {.name = "Prime Lanes",
     .category = "Polymetric",
     .gate = 0.5,
     .note = "0 2 1 3 2 4 1",
     .octave = "0 0 1 0 -1",
     .velocity = "x7958697x68",
     .gate_lane = "x7.x6x.7x.x6x",
     .accent = "x..",
     .mod = "13579x97531357975"},
}};

// ---- Mutation's reach -----------------------------------------------------------------
// How far a level moves (of its range, times the amount), and the odds a
// mutated step flips a toggle, turns a step into a rest or back, gets a
// ratchet, a condition or a chord; and the odds a lane's length moves.
constexpr double level_reach = 0.5;
constexpr double toggle_odds = 0.35;
constexpr double rest_odds = 0.25;
constexpr double ratchet_odds = 0.2;
constexpr double condition_odds = 0.15;
constexpr double chord_odds = 0.15;
constexpr double leap_odds = 0.25;   // A note moves two places, not one.
constexpr double length_odds = 0.3;  // Times the amount.
constexpr double shorter_odds = 0.5; // A moved length gets shorter.
constexpr double rest_return_floor = 0.6;
constexpr std::uint64_t mutation_salt = 0x6d757461746564ULL;
constexpr std::array<ArpCondition, 5> mutated_conditions{
    ArpCondition::one_of_two, ArpCondition::two_of_two, ArpCondition::three_of_four,
    ArpCondition::fill, ArpCondition::not_first};
constexpr std::array<ArpChord, 4> mutated_chords{ArpChord::octave, ArpChord::fifth, ArpChord::triad,
                                                 ArpChord::sus};

// SplitMix64 with a pick from a fixed table.
class Random {
public:
  explicit Random(std::uint64_t seed) noexcept : generator_(seed) {}
  std::uint64_t next() noexcept { return generator_.next(); }
  double unit() noexcept { return generator_.unit(); }
  template <typename T, std::size_t N> const T& pick(const std::array<T, N>& from) noexcept {
    return from[next() % N];
  }

private:
  foundation::SplitMix64 generator_;
};

// The arp's parameters by id, resolved against a table once per call.
class Ids {
public:
  explicit Ids(const synth::EngineDescriptor& table) : table_(table) {
    for (std::size_t i = 0; i < table.parameters.size(); ++i) {
      const auto id = table.parameters[i].id;
      if (id.starts_with(arp_prefix))
        index_.emplace(id, i);
    }
  }
  [[nodiscard]] std::optional<std::size_t> find(std::string_view id) const {
    const auto found = index_.find(id);
    return found == index_.end() ? std::nullopt : std::optional(found->second);
  }
  [[nodiscard]] const synth::EngineDescriptor& table() const noexcept { return table_; }

private:
  const synth::EngineDescriptor& table_;
  std::unordered_map<std::string_view, std::size_t> index_;
};

// Writes ids into a copy of the values, clamped to each parameter.
class Writer {
public:
  Writer(const Ids& ids, std::vector<double>& values) : ids_(ids), values_(values) {}
  void set(std::string_view id, double value) {
    if (const auto index = ids_.find(id)) {
      values_[*index] = synth::sanitise(ids_.table().parameters[*index], value);
    }
  }
  [[nodiscard]] double get(std::string_view id, double fallback) const {
    const auto index = ids_.find(id);
    return index ? values_[*index] : fallback;
  }

private:
  const Ids& ids_;
  std::vector<double>& values_;
};

[[nodiscard]] std::string lane_id(ArpLane lane, std::string_view what) {
  return std::string(lane_prefix) + std::string(nova::arp_lane_spec(lane).id) + "." +
         std::string(what);
}

[[nodiscard]] std::string step_id(ArpLane lane, std::size_t step) {
  return lane_id(lane, std::to_string(step + 1));
}

[[nodiscard]] std::string_view lane_text(const PatternSpec& pattern, ArpLane lane) noexcept {
  switch (lane) {
  case ArpLane::note:
    return pattern.note;
  case ArpLane::octave:
    return pattern.octave;
  case ArpLane::velocity:
    return pattern.velocity;
  case ArpLane::gate:
    return pattern.gate_lane;
  case ArpLane::tie:
    return pattern.tie;
  case ArpLane::ratchet:
    return pattern.ratchet;
  case ArpLane::chance:
    return pattern.chance;
  case ArpLane::condition:
    return pattern.condition;
  case ArpLane::accent:
    return pattern.accent;
  case ArpLane::mod:
    return pattern.mod;
  case ArpLane::chord:
    return pattern.chord;
  }
  return {};
}

[[nodiscard]] std::vector<std::string_view> words(std::string_view text) {
  std::vector<std::string_view> out;
  std::size_t at = 0;
  while (at < text.size()) {
    while (at < text.size() && text[at] == ' ')
      ++at;
    const std::size_t end = std::min(text.find(' ', at), text.size());
    if (end > at)
      out.push_back(text.substr(at, end - at));
    at = end;
  }
  return out;
}

constexpr double level_digit_step = 0.1;

[[nodiscard]] std::optional<double> character_value(ArpLaneKind kind, char c) noexcept {
  if (c == '.')
    return 0.0;
  if (c == 'x')
    return 1.0;
  if (kind == ArpLaneKind::level && c >= '1' && c <= '9')
    return level_digit_step * (c - '0');
  return std::nullopt;
}

[[nodiscard]] std::optional<double> word_value(ArpLane lane, std::string_view word) noexcept {
  if (lane == ArpLane::condition) {
    static constexpr std::array<std::pair<std::string_view, ArpCondition>, 14> names{{
        {"-", ArpCondition::always},
        {"1:2", ArpCondition::one_of_two},
        {"2:2", ArpCondition::two_of_two},
        {"1:3", ArpCondition::one_of_three},
        {"2:3", ArpCondition::two_of_three},
        {"3:3", ArpCondition::three_of_three},
        {"1:4", ArpCondition::one_of_four},
        {"2:4", ArpCondition::two_of_four},
        {"3:4", ArpCondition::three_of_four},
        {"4:4", ArpCondition::four_of_four},
        {"F", ArpCondition::fill},
        {"!F", ArpCondition::not_fill},
        {"1st", ArpCondition::first},
        {"!1st", ArpCondition::not_first},
    }};
    for (const auto& [name, condition] : names) {
      if (name == word)
        return static_cast<double>(condition);
    }
    return std::nullopt;
  }
  if (lane == ArpLane::chord) {
    static constexpr std::array<std::pair<std::string_view, ArpChord>, 6> names{{
        {"-", ArpChord::off},
        {"oct", ArpChord::octave},
        {"5", ArpChord::fifth},
        {"tri", ArpChord::triad},
        {"7", ArpChord::seventh},
        {"sus", ArpChord::sus},
    }};
    for (const auto& [name, chord] : names) {
      if (name == word)
        return static_cast<double>(chord);
    }
    return std::nullopt;
  }
  int value = 0;
  const auto* first = word.data();
  const auto* last = word.data() + word.size();
  const auto [end, error] = std::from_chars(first, last, value);
  if (error != std::errc{} || end != last)
    return std::nullopt;
  return static_cast<double>(value);
}

[[nodiscard]] Error invalid(const PatternSpec& pattern, std::string_view why) {
  return {ErrorCode::invalid_argument,
          "Arp pattern '" + std::string(pattern.name) + "': " + std::string(why)};
}

[[nodiscard]] std::optional<std::size_t> choice_index(const synth::ParameterDescriptor& parameter,
                                                      std::string_view name) noexcept {
  for (std::size_t i = 0; i < parameter.choices.size(); ++i) {
    if (parameter.choices[i] == name)
      return i;
  }
  return std::nullopt;
}

} // namespace

std::span<const PatternSpec> factory_patterns() noexcept { return library; }

foundation::Result<std::array<nova::ArpLaneValues, nova::arp_lane_count>>
lanes_of(const PatternSpec& pattern) {
  using R = foundation::Result<std::array<nova::ArpLaneValues, nova::arp_lane_count>>;
  auto lanes = nova::default_arp_lanes();
  for (std::size_t l = 0; l < nova::arp_lane_count; ++l) {
    const auto lane = static_cast<ArpLane>(l);
    const auto& spec = nova::arp_lane_specs[l];
    const auto text = lane_text(pattern, lane);
    if (text.empty())
      continue;
    std::vector<double> values;
    if (spec.kind == ArpLaneKind::level || spec.kind == ArpLaneKind::toggle) {
      for (const char c : text) {
        const auto value = character_value(spec.kind, c);
        if (!value)
          return R::failure(invalid(pattern, "bad step '" + std::string(1, c) + "' in " +
                                                 std::string(spec.name)));
        values.push_back(*value);
      }
    } else {
      for (const auto word : words(text)) {
        const auto value = word_value(lane, word);
        if (!value || *value < spec.minimum || *value > spec.maximum)
          return R::failure(invalid(pattern, "bad step '" + std::string(word) + "' in " +
                                                 std::string(spec.name)));
        values.push_back(*value);
      }
    }
    if (values.empty() || values.size() > nova::arp_max_steps)
      return R::failure(invalid(pattern, std::string(spec.name) + " has no steps or too many"));
    lanes[l].length = static_cast<std::uint32_t>(values.size());
    for (std::size_t n = 0; n < nova::arp_max_steps; ++n)
      lanes[l].values[n] = values[n % values.size()];
  }
  return R::success(lanes);
}

foundation::Result<std::vector<double>> apply(const synth::EngineDescriptor& table,
                                              std::span<const double> current,
                                              const PatternSpec& pattern, const LaneLocks& locks) {
  using R = foundation::Result<std::vector<double>>;
  if (current.size() != table.parameters.size())
    return R::failure({ErrorCode::invalid_argument, "The values do not fit the synth"});
  auto lanes = lanes_of(pattern);
  if (!lanes)
    return R::failure(lanes.error());
  const Ids ids(table);
  const auto rate = ids.find("nova.arp.rate");
  const auto rate_index =
      rate ? choice_index(table.parameters[*rate], pattern.rate) : std::optional<std::size_t>{};
  const auto variation = std::ranges::find(nova::arp_variation_bars, pattern.variation);
  if (!rate_index || variation == nova::arp_variation_bars.end())
    return R::failure(invalid(pattern, "an unknown rate or variation"));

  std::vector<double> values(current.begin(), current.end());
  Writer write(ids, values);
  write.set("nova.arp.on", 1.0);
  write.set("nova.arp.mode", static_cast<double>(pattern.mode));
  write.set("nova.arp.rate", static_cast<double>(*rate_index));
  write.set("nova.arp.gate", pattern.gate);
  write.set("nova.arp.swing", pattern.swing);
  write.set("nova.arp.octaves", pattern.octaves);
  write.set("nova.arp.note_mode", static_cast<double>(pattern.note_mode));
  write.set("nova.arp.passing", pattern.passing ? 1.0 : 0.0);
  write.set("nova.arp.strum", pattern.strum);
  write.set("nova.arp.euclid.steps", pattern.euclid.steps);
  write.set("nova.arp.euclid.hits", pattern.euclid.hits);
  write.set("nova.arp.euclid.rotate", pattern.euclid.rotate);
  write.set("nova.arp.variation",
            static_cast<double>(std::distance(nova::arp_variation_bars.begin(), variation)));
  write.set("nova.arp.fill", 0.0);
  write.set("nova.arp.seed", 0.0);
  for (std::size_t l = 0; l < nova::arp_lane_count; ++l) {
    if (locks[l])
      continue;
    const auto lane = static_cast<ArpLane>(l);
    write.set(lane_id(lane, "length"), lanes.value()[l].length);
    for (std::size_t n = 0; n < nova::arp_max_steps; ++n)
      write.set(step_id(lane, n), lanes.value()[l].values[n]);
  }
  return R::success(std::move(values));
}

std::vector<double> mutate(const synth::EngineDescriptor& table, std::span<const double> base,
                           std::uint32_t seed, double amount, const LaneLocks& locks) {
  std::vector<double> values(base.begin(), base.end());
  if (seed == 0 || base.size() != table.parameters.size())
    return values;
  amount = std::isfinite(amount) ? std::clamp(amount, 0.0, 1.0) : default_mutation_amount;
  const Ids ids(table);
  Writer write(ids, values);
  Random random(static_cast<std::uint64_t>(seed) ^ mutation_salt);
  for (std::size_t l = 0; l < nova::arp_lane_count; ++l) {
    if (locks[l])
      continue;
    const auto lane = static_cast<ArpLane>(l);
    const auto& spec = nova::arp_lane_specs[l];
    // The length first, so the same seed moves the same lanes.
    const auto length_id = lane_id(lane, "length");
    if (random.unit() < amount * length_odds) {
      const double length = write.get(length_id, nova::arp_default_length);
      const double by = random.unit() < leap_odds ? 2.0 : 1.0;
      write.set(length_id, length + (random.unit() < shorter_odds ? -by : by));
    }
    for (std::size_t n = 0; n < nova::arp_max_steps; ++n) {
      const auto id = step_id(lane, n);
      const double was = write.get(id, spec.fallback);
      const double roll = random.unit();
      const double detail = random.unit();
      const double sign = random.unit() < shorter_odds ? -1.0 : 1.0;
      if (roll >= amount)
        continue;
      double now = was;
      switch (spec.kind) {
      case ArpLaneKind::level:
        if (lane == ArpLane::gate && detail < rest_odds)
          now = was > 0.0 ? 0.0 : rest_return_floor + (1.0 - rest_return_floor) * detail;
        else
          now = was + sign * detail * amount * level_reach * (spec.maximum - spec.minimum);
        break;
      case ArpLaneKind::toggle:
        if (detail < toggle_odds)
          now = was >= 0.5 ? 0.0 : 1.0;
        break;
      case ArpLaneKind::integer:
        if (lane == ArpLane::ratchet)
          now = detail < ratchet_odds ? 2.0 : spec.fallback;
        else
          now = was + sign * (detail < leap_odds ? 2.0 : 1.0);
        break;
      case ArpLaneKind::choice:
        if (lane == ArpLane::condition && detail < condition_odds)
          now = static_cast<double>(random.pick(mutated_conditions));
        else if (lane == ArpLane::chord && detail < chord_odds)
          now = static_cast<double>(random.pick(mutated_chords));
        break;
      }
      write.set(id, now);
    }
  }
  write.set("nova.arp.seed", seed);
  return values;
}

foundation::Result<std::vector<double>> apply_groove(const synth::EngineDescriptor& table,
                                                     std::span<const double> current,
                                                     const persistence::GrooveTemplate& groove) {
  using R = foundation::Result<std::vector<double>>;
  if (current.size() != table.parameters.size())
    return R::failure({ErrorCode::invalid_argument, "The values do not fit the synth"});
  if (auto valid = persistence::validate_groove(groove); !valid)
    return R::failure(valid.error());
  const auto grid = std::ranges::find(nova::arp_groove_grids, groove.steps_per_quarter);
  if (grid == nova::arp_groove_grids.end())
    return R::failure({ErrorCode::invalid_argument, "The arp has no groove grid of " +
                                                        std::to_string(groove.steps_per_quarter) +
                                                        " steps a quarter"});
  std::vector<double> values = clear_groove(table, current);
  const Ids ids(table);
  Writer write(ids, values);
  const std::size_t steps = std::min(groove.steps.size(), nova::arp_groove_steps);
  write.set("nova.arp.groove.steps", static_cast<double>(steps));
  write.set("nova.arp.groove.grid",
            static_cast<double>(std::distance(nova::arp_groove_grids.begin(), grid)));
  for (std::size_t n = 0; n < steps; ++n) {
    const auto p = "nova.arp.groove." + std::to_string(n + 1) + ".";
    write.set(p + "timing", groove.steps[n].timing);
    write.set(p + "velocity", groove.steps[n].velocity);
  }
  return R::success(std::move(values));
}

std::vector<double> clear_groove(const synth::EngineDescriptor& table,
                                 std::span<const double> current) {
  std::vector<double> values(current.begin(), current.end());
  if (current.size() != table.parameters.size())
    return values;
  for (std::size_t i = 0; i < table.parameters.size(); ++i) {
    if (table.parameters[i].id.starts_with("nova.arp.groove."))
      values[i] = table.parameters[i].default_value;
  }
  return values;
}

nova::ArpSettings settings_of(const synth::EngineDescriptor& table,
                              std::span<const double> values) {
  nova::ArpSettings arp;
  if (values.size() != table.parameters.size())
    return arp;
  const Ids ids(table);
  const auto get = [&](std::string_view id, double fallback) {
    const auto index = ids.find(id);
    return index ? values[*index] : fallback;
  };
  const auto whole = [&](std::string_view id, double fallback) {
    return static_cast<std::uint32_t>(std::max(0L, std::lround(get(id, fallback))));
  };
  arp.on = get("nova.arp.on", 0.0) >= 0.5;
  arp.mode = static_cast<ArpMode>(whole("nova.arp.mode", 0.0));
  arp.rate = whole("nova.arp.rate", nova::arp_default_rate);
  arp.gate = get("nova.arp.gate", arp.gate);
  arp.swing = get("nova.arp.swing", arp.swing);
  arp.octaves = whole("nova.arp.octaves", arp.octaves);
  arp.note_mode = static_cast<ArpNoteMode>(whole("nova.arp.note_mode", 0.0));
  arp.scale = whole("nova.arp.scale", 0.0);
  arp.root = whole("nova.arp.root", 0.0);
  arp.passing = get("nova.arp.passing", 0.0) >= 0.5;
  arp.strum = get("nova.arp.strum", arp.strum);
  arp.euclid_hits = whole("nova.arp.euclid.hits", arp.euclid_hits);
  arp.euclid_steps = whole("nova.arp.euclid.steps", arp.euclid_steps);
  arp.euclid_rotate = whole("nova.arp.euclid.rotate", arp.euclid_rotate);
  arp.fill = get("nova.arp.fill", 0.0) >= 0.5;
  arp.variation = nova::arp_variation_bars[std::min<std::size_t>(
      whole("nova.arp.variation", 0.0), nova::arp_variation_bars.size() - 1)];
  arp.seed = whole("nova.arp.seed", 0.0);
  arp.groove.amount = get("nova.arp.groove_amount", arp.groove.amount);
  arp.groove.steps = whole("nova.arp.groove.steps", 0.0);
  arp.groove.grid = whole("nova.arp.groove.grid", nova::arp_default_groove_grid);
  for (std::size_t n = 0; n < nova::arp_groove_steps; ++n) {
    const auto p = "nova.arp.groove." + std::to_string(n + 1) + ".";
    arp.groove.timing[n] = get(p + "timing", 0.0);
    arp.groove.velocity[n] = get(p + "velocity", 1.0);
  }
  for (std::size_t l = 0; l < nova::arp_lane_count; ++l) {
    const auto lane = static_cast<ArpLane>(l);
    arp.lanes[l].length = whole(lane_id(lane, "length"), nova::arp_default_length);
    for (std::size_t n = 0; n < nova::arp_max_steps; ++n)
      arp.lanes[l].values[n] = get(step_id(lane, n), nova::arp_lane_specs[l].fallback);
  }
  return arp;
}

} // namespace oscillate::application::nova_arp
