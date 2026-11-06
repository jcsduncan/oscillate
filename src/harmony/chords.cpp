#include "detail.hpp"

#include <algorithm>
#include <bit>

namespace oscillate::harmony {
namespace {

using detail::bit;
using detail::from_tonic;
using detail::octave;
using detail::pc;

struct QualityRow {
  ChordQuality quality;
  std::array<std::uint8_t, max_chord_tones> semitones;
  std::uint8_t count;
  std::string_view suffix;
};
// The table, in enum order. Suffixes spell as lead sheets do.
const QualityRow quality_rows[] = {
    {ChordQuality::major, {0, 4, 7}, 3, ""},
    {ChordQuality::minor, {0, 3, 7}, 3, "m"},
    {ChordQuality::diminished, {0, 3, 6}, 3, "°"},
    {ChordQuality::sus2, {0, 2, 7}, 3, "sus2"},
    {ChordQuality::add9, {0, 4, 7, 14}, 4, "add9"},
    {ChordQuality::sixth, {0, 4, 7, 9}, 4, "6"},
    {ChordQuality::minor_sixth, {0, 3, 7, 9}, 4, "m6"},
    {ChordQuality::major_seventh, {0, 4, 7, 11}, 4, "maj7"},
    {ChordQuality::minor_seventh, {0, 3, 7, 10}, 4, "m7"},
    {ChordQuality::dominant_seventh, {0, 4, 7, 10}, 4, "7"},
    {ChordQuality::half_diminished, {0, 3, 6, 10}, 4, "m7♭5"},
    {ChordQuality::diminished_seventh, {0, 3, 6, 9}, 4, "°7"},
    {ChordQuality::seventh_sus4, {0, 5, 7, 10}, 4, "7sus4"},
    {ChordQuality::major_ninth, {0, 4, 7, 11, 14}, 5, "maj9"},
    {ChordQuality::minor_ninth, {0, 3, 7, 10, 14}, 5, "m9"},
    {ChordQuality::dominant_ninth, {0, 4, 7, 10, 14}, 5, "9"},
    {ChordQuality::dominant_seventh_flat_ninth, {0, 4, 7, 10, 13}, 5, "7♭9"},
    {ChordQuality::minor_eleventh, {0, 3, 7, 10, 14, 17}, 6, "m11"},
    {ChordQuality::dominant_thirteenth, {0, 4, 10, 14, 21}, 5, "13"},
    {ChordQuality::minor_add9, {0, 3, 7, 14}, 4, "m(add9)"},
    {ChordQuality::sus4, {0, 5, 7}, 3, "sus4"},
    {ChordQuality::power, {0, 7}, 2, "5"},
};
static_assert(std::size(quality_rows) == last_chord_quality);

const QualityRow& row(ChordQuality quality) noexcept {
  const auto index = static_cast<std::size_t>(quality) - 1;
  return quality_rows[std::min(index, std::size(quality_rows) - 1)];
}

// Spellings. Sharp keys spell their scale with sharps, flat keys with flats;
// notes outside the key take the commoner spelling (C♯, E♭, F♯, G♯, B♭) in
// C major / A minor and in sharp keys, flats in flat keys.
constexpr std::array<std::string_view, octave> sharp_names{"C",  "C♯", "D",  "D♯", "E",  "F",
                                                           "F♯", "G",  "G♯", "A",  "A♯", "B"};
constexpr std::array<std::string_view, octave> flat_names{"C",  "D♭", "D",  "E♭", "E",  "F",
                                                          "G♭", "G",  "A♭", "A",  "B♭", "B"};
constexpr std::array<std::string_view, octave> common_names{"C",  "C♯", "D",  "E♭", "E",  "F",
                                                            "F♯", "G",  "G♯", "A",  "B♭", "B"};
// Major-key tonics (the relative major for a minor key) spelled with flats.
constexpr std::uint16_t flat_major_tonics =
    (1U << 5U) | (1U << 10U) | (1U << 3U) | (1U << 8U) | (1U << 1U);
constexpr std::uint8_t relative_major_offset = 3;

enum class Spelling : std::uint8_t { natural, sharp, flat };
Spelling spelling(const MusicalKey& key) noexcept {
  const auto major_tonic =
      key.mode == project::KeyMode::minor ? pc(key.tonic + relative_major_offset) : key.tonic;
  if (major_tonic == 0) {
    return Spelling::natural;
  }
  return (flat_major_tonics & bit(major_tonic)) != 0 ? Spelling::flat : Spelling::sharp;
}

// Roman numerals by semitones from the tonic.
constexpr std::array<std::string_view, octave> minor_numerals{
    "I", "♭II", "II", "III", "♮III", "IV", "♯IV", "V", "VI", "♮VI", "VII", "♮VII"};
constexpr std::array<std::string_view, octave> major_numerals{
    "I", "♭II", "II", "♭III", "III", "IV", "♯IV", "V", "♭VI", "VI", "♭VII", "VII"};

// Tension: a base for each chord's function, then what pulls harder.
struct FunctionTension {
  std::uint8_t from_tonic;
  int tension;
};
constexpr std::array<FunctionTension, 7> minor_function_tension{
    {{0, 8}, {3, 22}, {8, 28}, {5, 40}, {2, 52}, {10, 46}, {7, 50}}};
constexpr std::array<FunctionTension, 7> major_function_tension{
    {{0, 8}, {9, 22}, {4, 28}, {5, 40}, {2, 52}, {7, 50}, {11, 60}}};
constexpr int chromatic_root_tension = 66;
constexpr int per_outside_note = 9;
constexpr int dominant_colour = 12;
constexpr int flat_ninth_bite = 8;
constexpr int diminished_bite = 16;
constexpr int suspended_ease = -5;
constexpr int extension_colour = 3;
constexpr int pedal_under = 8;
constexpr int min_tension = 2;
constexpr int max_tension = 98;

bool is_extended(ChordQuality q) noexcept {
  using enum ChordQuality;
  return q == minor_ninth || q == major_ninth || q == minor_eleventh || q == dominant_ninth ||
         q == dominant_thirteenth || q == add9 || q == minor_add9;
}

} // namespace

namespace detail {

std::uint16_t key_set(const MusicalKey& key) noexcept {
  std::uint16_t set = 0;
  for (const auto degree : key_scale(key)) {
    set = static_cast<std::uint16_t>(set | bit(pc(key.tonic + degree)));
  }
  return set;
}

int count_bits(std::uint16_t bits) noexcept { return std::popcount(bits); }

bool richer(ChordQuality in, ChordQuality& out) noexcept {
  using enum ChordQuality;
  switch (in) {
  case minor_seventh:
    out = minor_ninth;
    return true;
  case minor_ninth:
    out = minor_eleventh;
    return true;
  case major_seventh:
    out = major_ninth;
    return true;
  case dominant_seventh:
    out = dominant_ninth;
    return true;
  case dominant_ninth:
    out = dominant_thirteenth;
    return true;
  case minor:
    out = minor_seventh;
    return true;
  case major:
    out = major_seventh;
    return true;
  case sixth:
    out = major_ninth;
    return true;
  case sus2:
    out = add9;
    return true;
  case sus4:
    out = sus2;
    return true;
  case minor_add9:
    out = minor_ninth;
    return true;
  case seventh_sus4:
    out = dominant_ninth;
    return true;
  default:
    return false;
  }
}

bool simpler(ChordQuality in, ChordQuality& out) noexcept {
  using enum ChordQuality;
  switch (in) {
  case minor_ninth:
    out = minor_seventh;
    return true;
  case minor_eleventh:
    out = minor_ninth;
    return true;
  case major_ninth:
    out = major_seventh;
    return true;
  case dominant_ninth:
    out = dominant_seventh;
    return true;
  case dominant_thirteenth:
    out = dominant_ninth;
    return true;
  case minor_seventh:
    out = minor;
    return true;
  case major_seventh:
    out = major;
    return true;
  case add9:
    out = major;
    return true;
  case minor_add9:
    out = minor;
    return true;
  case dominant_seventh_flat_ninth:
    out = dominant_seventh;
    return true;
  default:
    return false;
  }
}

bool flipped(ChordQuality in, ChordQuality& out) noexcept {
  using enum ChordQuality;
  switch (in) {
  case minor_seventh:
    out = dominant_seventh;
    return true;
  case minor_ninth:
    out = dominant_ninth;
    return true;
  case minor:
    out = major;
    return true;
  case minor_sixth:
    out = sixth;
    return true;
  case major_seventh:
    out = minor_seventh;
    return true;
  case major_ninth:
    out = minor_ninth;
    return true;
  case major:
    out = minor;
    return true;
  case sixth:
    out = minor_sixth;
    return true;
  case dominant_seventh:
    out = minor_seventh;
    return true;
  case dominant_ninth:
    out = minor_ninth;
    return true;
  case dominant_seventh_flat_ninth:
    out = minor_seventh;
    return true;
  case minor_eleventh:
    out = dominant_ninth;
    return true;
  case add9:
    out = minor_add9;
    return true;
  case minor_add9:
    out = add9;
    return true;
  default:
    return false;
  }
}

ChordQuality suspended(ChordQuality in) noexcept {
  using enum ChordQuality;
  // A triad suspends to its fourth; a seventh chord to a 7sus4.
  if (in == major || in == minor || in == add9 || in == minor_add9 || in == sus2)
    return sus4;
  return is_dominant(in) || is_minor(in) ? seventh_sus4 : sus2;
}

} // namespace detail

Intervals intervals(ChordQuality quality) noexcept {
  Intervals out;
  const auto& r = row(quality);
  out.semitones = r.semitones;
  out.count = r.count;
  return out;
}

std::uint16_t pitch_class_set(const ChordSymbol& chord) noexcept {
  std::uint16_t set = 0;
  const auto iv = intervals(chord.quality);
  for (std::uint8_t i = 0; i < iv.count; ++i) {
    set = static_cast<std::uint16_t>(set | bit(pc(chord.root + iv.semitones[i])));
  }
  if (chord.bass) {
    set = static_cast<std::uint16_t>(set | bit(pc(*chord.bass)));
  }
  return set;
}

bool is_dominant(ChordQuality quality) noexcept {
  using enum ChordQuality;
  return quality == dominant_seventh || quality == dominant_ninth ||
         quality == dominant_seventh_flat_ninth || quality == dominant_thirteenth ||
         quality == seventh_sus4;
}

bool is_minor(ChordQuality quality) noexcept {
  using enum ChordQuality;
  return quality == minor || quality == diminished || quality == minor_sixth ||
         quality == minor_seventh || quality == half_diminished || quality == diminished_seventh ||
         quality == minor_ninth || quality == minor_eleventh || quality == minor_add9;
}

std::string_view suffix(ChordQuality quality) noexcept { return row(quality).suffix; }

std::string_view pitch_name(std::uint8_t pitch_class, const MusicalKey& key) noexcept {
  const auto p = pc(pitch_class);
  const bool in_key = (detail::key_set(key) & bit(p)) != 0;
  switch (spelling(key)) {
  case Spelling::flat:
    return flat_names[p];
  case Spelling::sharp:
    return in_key ? sharp_names[p] : common_names[p];
  case Spelling::natural:
    break;
  }
  return common_names[p];
}

std::string name(const ChordSymbol& chord, const MusicalKey& key) {
  std::string out(pitch_name(chord.root, key));
  out += suffix(chord.quality);
  if (chord.bass && pc(*chord.bass) != pc(chord.root)) {
    out += '/';
    out += pitch_name(*chord.bass, key);
  }
  return out;
}

std::string roman(const ChordSymbol& chord, const MusicalKey& key) {
  const auto interval = from_tonic(chord.root, key);
  std::string out(key.mode == project::KeyMode::minor ? minor_numerals[interval]
                                                      : major_numerals[interval]);
  if (is_minor(chord.quality)) {
    for (auto& ch : out) {
      if (ch == 'I' || ch == 'V') {
        ch = static_cast<char>(ch - 'A' + 'a');
      }
    }
  }
  using enum ChordQuality;
  if (chord.quality == half_diminished) {
    out += "ø";
  } else if (chord.quality == diminished_seventh || chord.quality == diminished) {
    out += "°";
  } else if (is_dominant(chord.quality)) {
    out += "⁷";
  }
  return out;
}

Role role(const ChordSymbol& chord, const MusicalKey& key) noexcept {
  const auto interval = from_tonic(chord.root, key);
  const bool minor_key = key.mode == project::KeyMode::minor;
  const bool outside = (pitch_class_set(chord) & ~detail::key_set(key)) != 0;
  const bool the_dominant = interval == detail::dominant_degree && is_dominant(chord.quality);
  if (outside && !(minor_key && the_dominant)) {
    return Role::colour;
  }
  const auto home =
      minor_key ? std::array<std::uint8_t, 3>{0, 3, 8} : std::array<std::uint8_t, 3>{0, 4, 9};
  if (std::ranges::find(home, interval) != home.end()) {
    return Role::home;
  }
  constexpr std::array<std::uint8_t, 2> lift{2, 5};
  if (std::ranges::find(lift, interval) != lift.end()) {
    return Role::lift;
  }
  return Role::pull;
}

int tension(const ChordSymbol& chord, const MusicalKey& key) noexcept {
  const auto interval = from_tonic(chord.root, key);
  const auto& table =
      key.mode == project::KeyMode::minor ? minor_function_tension : major_function_tension;
  int t = chromatic_root_tension;
  for (const auto& f : table) {
    if (f.from_tonic == interval) {
      t = f.tension;
    }
  }
  ChordSymbol rootless = chord;
  rootless.bass.reset();
  t += per_outside_note * detail::count_bits(static_cast<std::uint16_t>(pitch_class_set(rootless) &
                                                                        ~detail::key_set(key)));
  using enum ChordQuality;
  if (is_dominant(chord.quality) && chord.quality != seventh_sus4) {
    t += dominant_colour;
  }
  if (chord.quality == dominant_seventh_flat_ninth) {
    t += flat_ninth_bite;
  }
  if (chord.quality == half_diminished || chord.quality == diminished_seventh ||
      chord.quality == diminished) {
    t += diminished_bite;
  }
  if (chord.quality == seventh_sus4 || chord.quality == sus2) {
    t += suspended_ease;
  }
  if (is_extended(chord.quality)) {
    t += extension_colour;
  }
  if (chord.bass && pc(*chord.bass) != pc(chord.root)) {
    t += pedal_under;
  }
  return std::clamp(t, min_tension, max_tension);
}

} // namespace oscillate::harmony
