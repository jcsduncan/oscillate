#include "detail.hpp"

#include <algorithm>
#include <cmath>
#include <optional>

namespace oscillate::harmony {
namespace {

using detail::bit;
using detail::from_tonic;
using detail::pc;
using detail::Scale;

constexpr std::array<Scale, mode_count> mode_scales{{
    {0, 1, 3, 5, 7, 8, 10}, // Phrygian
    {0, 2, 3, 5, 7, 8, 10}, // Aeolian
    {0, 2, 3, 5, 7, 9, 10}, // Dorian
    {0, 2, 4, 5, 7, 9, 10}, // Mixolydian
    {0, 2, 4, 5, 7, 9, 11}, // Ionian
    {0, 2, 4, 6, 7, 9, 11}, // Lydian
}};
constexpr std::array<std::string_view, mode_count> mode_names{"Phrygian",   "Aeolian", "Dorian",
                                                              "Mixolydian", "Ionian",  "Lydian"};

struct ShapeRow {
  std::string_view name;
  std::string_view summary;
  std::array<std::uint8_t, max_shape_chords> degrees;
  std::uint8_t count;
};
constexpr std::array<ShapeRow, shape_count> shape_rows{{
    {"Still", "one chord, its colour shifts", {0, 0}, 2},
    // The shapes melodic techno, progressive house and trance lean on, as
    // degrees of the mode: i–VI, i–VI–III–VII, i–iv–VII–III, i–VII–VI–v,
    // iv–v–VI–VII, VI–VII–i–i and VI–III–VII–i in a minor key.
    {"Vamp", "two chords, hypnotic", {0, 5}, 2},
    {"Four-chord loop", "the melodic techno staple", {0, 5, 2, 6}, 4},
    {"Circle", "falls by fourths, never lands", {0, 3, 6, 2}, 4},
    {"Falling line", "the bass steps down", {0, 6, 5, 4}, 4},
    {"Climb", "the bass steps up to the drop", {3, 4, 5, 6}, 4},
    {"Anthem", "six, seven, home and stay", {5, 6, 0, 0}, 4},
    {"Bittersweet", "the sad side of the four chords", {5, 2, 6, 0}, 4},
    // VI–VII, i–VII–VI–VII, i–iv, i–III–v–IV (Dorian's major IV when the
    // mood is bright), i–VI–VII over four bars, an eight-chord trance phrase
    // and iv–VI–VII–i settling back through VI–v–iv–i.
    {"Pendulum", "six and seven, never home", {5, 6}, 2},
    {"Shuffle", "rocks back up instead of falling", {0, 6, 5, 6}, 4},
    {"Sway", "two chords, softer than the vamp", {0, 3}, 2},
    {"Hope", "the Böhmer lift, brightest in Dorian", {0, 2, 4, 3}, 4},
    {"Odd loop", "three chords against four bars", {0, 5, 6}, 3},
    {"Long phrase", "an eight-chord trance arc", {0, 5, 2, 6, 0, 5, 3, 4}, 8},
    {"Rise and fall", "builds home, then settles back", {3, 5, 6, 0, 5, 4, 3, 0}, 8},
}};

struct DeviceRow {
  std::string_view name;
  std::string_view hint;
};
constexpr std::array<DeviceRow, device_count> device_rows{{
    {"Pedal tone", "The bass holds the home note while the chords move above it. Tension that "
                   "never leaves home."},
    {"Top drone", "One note rings on top of every chord. It glues the changes together, the deep "
                  "house pad trick."},
    {"Walking bass", "Inversions, so the bass steps down to each chord instead of jumping."},
    {"Suspension", "The last chord arrives suspended and resolves halfway through."},
    {"Passing chord", "The scale's chord a step below slips in before the biggest jump."},
    {"Borrowed chord",
     "One chord from the opposite mode: a lift in the dark, a shadow in the bright."},
    {"Pull home", "A major V on the last beat lifts back to bar 1, the trance way."},
    {"Chromatic slide", "Slides into bar 1 from a semitone above."},
}};

struct Feel {
  double brightness;
  double restlessness;
  std::string_view name;
};
constexpr std::array<Feel, 7> feels{{{0.0, 0.0, "Hypnotic"},
                                     {0.0, 1.0, "Tense"},
                                     {1.0, 0.0, "Dreamy"},
                                     {1.0, 1.0, "Uplifting"},
                                     {0.5, 0.5, "Bittersweet"},
                                     {0.25, 0.5, "Brooding"},
                                     {0.75, 0.5, "Warm"}}};

// Restlessness thresholds for the v chord: still suspends it, restless
// makes it a major V (the lift trance uses; a V7 with seventh colours).
constexpr double still_below = 0.22;
constexpr double restless_above = 0.55;
// Above this the label notes the major V.
constexpr double pull_bites_above = 0.7;

// Chord tones stacked in thirds for each colour.
constexpr std::uint8_t plain_tones = 3;
constexpr std::uint8_t seventh_tones = 4;
constexpr std::uint8_t lush_tones = 5;

// Devices cut regions on the beat; a region splits only when this long.
constexpr std::int64_t beat = timeline::ticks_per_quarter;
constexpr std::int64_t min_split = 2 * beat;
// Passing chords go before a root jump of more than this many semitones.
constexpr int passing_jump_above = 2;
// Walking bass steps down at most this far to the next chord's bass.
constexpr int walk_step_limit = 4;
// The drone prefers, in order: the fifth, the tonic, the third, the
// seventh, the ninth (semitones from the tonic, minor third first).
constexpr std::array<std::uint8_t, 5> drone_preference{7, 0, 3, 10, 2};
// Functional twins the Swap variant exchanges (scale degrees).
constexpr std::array<std::pair<std::uint8_t, std::uint8_t>, 6> twins{
    {{1, 3}, {3, 1}, {5, 2}, {2, 5}, {6, 4}, {4, 6}}};
// Seeds each variant draws from, so one variant's choice never shifts another's.
constexpr std::uint64_t swap_salt = 0x5157'4150ULL;
constexpr std::uint64_t rotate_salt = 0x524F'5441ULL;
constexpr std::uint8_t triad_third = 1;
constexpr std::uint8_t triad_fifth = 2;
constexpr std::uint8_t seventh_index = 3;
constexpr std::uint8_t ninth_index = 4;
constexpr int minor_third_semitones = 3;
constexpr int major_third_semitones = 4;
constexpr int perfect_fifth = 7;
constexpr int diminished_fifth_semitones = 6;
constexpr int diminished_seventh_semitones = 9;
constexpr int major_seventh_semitones = 11;
constexpr int flat_ninth_semitones = 13;
constexpr int ninth_semitones = 14;

ChordQuality quality_of(std::span<const int> iv) noexcept {
  using enum ChordQuality;
  const auto n = iv.size();
  if (iv[triad_fifth] == diminished_fifth_semitones) {
    if (n >= seventh_tones) {
      return iv[seventh_index] == diminished_seventh_semitones ? diminished_seventh
                                                               : half_diminished;
    }
    return diminished;
  }
  if (iv[triad_third] == minor_third_semitones) {
    if (n == plain_tones) {
      return minor;
    }
    if (n == seventh_tones) {
      return minor_seventh;
    }
    return iv[ninth_index] == ninth_semitones ? minor_ninth : minor_seventh;
  }
  if (n == plain_tones) {
    return major;
  }
  if (n == seventh_tones) {
    return iv[seventh_index] == major_seventh_semitones ? major_seventh : dominant_seventh;
  }
  if (iv[seventh_index] == major_seventh_semitones) {
    return major_ninth;
  }
  return iv[ninth_index] == flat_ninth_semitones ? dominant_seventh_flat_ninth
         : iv[ninth_index] == ninth_semitones    ? dominant_ninth
                                                 : dominant_seventh;
}

ChordSymbol chord_on(const Scale& scale, std::uint8_t tonic, std::uint8_t degree,
                     std::uint8_t tones) noexcept {
  std::array<int, lush_tones> iv{};
  for (std::uint8_t k = 0; k < tones; ++k) {
    const auto i = static_cast<std::size_t>(degree + 2 * k);
    iv[k] = scale[i % detail::scale_size] +
            detail::octave * static_cast<int>(i / detail::scale_size) - scale[degree];
  }
  return {pc(tonic + scale[degree]), quality_of(std::span<const int>(iv.data(), tones)),
          std::nullopt};
}

std::uint8_t tones_of(Colour colour) noexcept {
  return colour == Colour::sevenths ? seventh_tones
         : colour == Colour::lush   ? lush_tones
                                    : plain_tones;
}

// A triad in the colour: its ninth added, or its third suspended to the
// second. Diminished triads and chords with sevenths stay as they are.
ChordQuality coloured(ChordQuality q, Colour colour) noexcept {
  using enum ChordQuality;
  if (colour == Colour::add9)
    return q == major ? add9 : q == minor ? minor_add9 : q;
  if (colour == Colour::sus2)
    return q == major || q == minor ? sus2 : q;
  return q;
}

// A major V in the colour: a triad, or a V7. Its ninth would be the
// raised sixth, outside every minor mode the Lab writes in.
ChordQuality major_v(Colour colour) noexcept {
  return colour == Colour::sevenths || colour == Colour::lush ? ChordQuality::dominant_seventh
                                                              : ChordQuality::major;
}

// The chord with its colour kept inside the mode: a ninth (or an eleventh
// or thirteenth) the scale does not hold is dropped, the chord keeping its
// seventh where the scale has it, so Phrygian's i is Am7, not Am(add9) with
// a B♮, and Aeolian's v is Em7, not Em(add9) with an F♯.
ChordSymbol in_mode(ChordSymbol chord, const Scale& scale, std::uint8_t tonic) noexcept {
  using enum ChordQuality;
  const auto holds = [&](int semitones) {
    const auto p = from_tonic(pc(chord.root + semitones), {tonic, project::KeyMode::minor});
    return std::ranges::find(scale, p) != scale.end();
  };
  const auto iv = intervals(chord.quality);
  bool outside = false;
  for (std::uint8_t i = 0; i < iv.count; ++i)
    outside = outside || (iv.semitones[i] > detail::octave && !holds(iv.semitones[i]));
  if (!outside)
    return chord;
  constexpr int minor_seventh_semitones = 10;
  switch (chord.quality) {
  case add9:
    chord.quality = major;
    break;
  case minor_add9:
  case minor_eleventh:
    chord.quality = holds(minor_seventh_semitones) ? minor_seventh : minor;
    break;
  case minor_ninth:
    chord.quality = minor_seventh;
    break;
  case major_ninth:
    chord.quality = major_seventh;
    break;
  case dominant_ninth:
  case dominant_thirteenth:
    chord.quality = dominant_seventh;
    break;
  default:
    break;
  }
  return chord;
}

std::uint8_t neighbour_mode(std::size_t mode) noexcept {
  constexpr std::size_t middle = mode_count / 2;
  return static_cast<std::uint8_t>(mode < middle ? mode + 1 : mode - 1);
}

ChordQuality still_colour(ChordQuality q) noexcept {
  using enum ChordQuality;
  switch (q) {
  case minor:
    return sus2;
  case minor_add9:
    return minor_ninth;
  case add9:
    return sus2;
  case minor_seventh:
    return minor_ninth;
  case minor_ninth:
    return minor_eleventh;
  case major:
    return sus2;
  case major_seventh:
    return major_ninth;
  case major_ninth:
    return sixth;
  case dominant_seventh:
    return dominant_ninth;
  case dominant_ninth:
    return dominant_thirteenth;
  case half_diminished:
    return minor_seventh;
  case diminished:
    return minor;
  default:
    return q;
  }
}

// ---- Natural colour: the records' habits by degree ----
// Measured over PML's Iconic Chord Progressions (430 chords of a beat or
// more): 63 % plain triads, add9 on 8. Each degree's colours, weighted by
// how often the records use them there; the first is the plain chord.
struct Weighted {
  ChordQuality quality;
  double weight;
};
struct DegreeColours {
  std::array<Weighted, 5> choices;
  std::uint8_t count;
};
constexpr DegreeColours natural_colours(std::uint8_t degree, bool minor_chord) noexcept {
  using enum ChordQuality;
  switch (degree) {
  case 0:
    return minor_chord
               ? DegreeColours{{{{minor, 75}, {sus2, 18}, {minor_seventh, 5}}}, 3}
               : DegreeColours{{{{major, 70}, {sus2, 15}, {add9, 10}, {major_seventh, 5}}}, 4};
  case 1:
    return minor_chord ? DegreeColours{{{{minor, 4}, {minor_seventh, 2}}}, 2}
                       : DegreeColours{{{{major, 1}}}, 1};
  case 2:
    return minor_chord ? DegreeColours{{{{minor, 7}, {minor_seventh, 3}}}, 2}
                       : DegreeColours{{{{major, 20}, {sus4, 3}, {sus2, 2}, {sixth, 2}}}, 4};
  case 3:
    return minor_chord ? DegreeColours{{{{minor, 36}, {minor_seventh, 12}, {sus2, 3}}}, 3}
                       : DegreeColours{{{{major, 11}, {sus2, 2}, {major_seventh, 2}}}, 3};
  case 4:
    return minor_chord ? DegreeColours{{{{minor, 13}, {sus4, 22}, {minor_seventh, 5}}}, 3}
                       : DegreeColours{{{{major, 50}, {sus4, 30}, {seventh_sus4, 10}}}, 3};
  case 5:
    return minor_chord
               ? DegreeColours{{{{minor, 6}, {minor_seventh, 3}}}, 2}
               : DegreeColours{{{{major, 75}, {major_seventh, 20}, {sixth, 2}, {add9, 2}}}, 4};
  default:
    return minor_chord ? DegreeColours{{{{minor, 1}}}, 1}
                       : DegreeColours{{{{major, 33}, {sixth, 8}, {sus4, 8}, {sus2, 4}}}, 4};
  }
}
constexpr std::uint64_t natural_salt = 0x4E41'5455ULL;

bool inside(const ChordSymbol& chord, const Scale& scale, std::uint8_t tonic) noexcept {
  const auto iv = intervals(chord.quality);
  for (std::uint8_t i = 0; i < iv.count; ++i) {
    const auto p = from_tonic(pc(chord.root + iv.semitones[i]), {tonic, project::KeyMode::minor});
    if (std::ranges::find(scale, p) == scale.end())
      return false;
  }
  return true;
}

// Colours the plain chords of a loop as the records would: each chord's
// colour drawn by its degree's weights, then at least one chord coloured
// (the likeliest colour there is) and no more than half, and never two
// neighbours alike in colour. The same seed gives the same colours.
void colour_naturally(std::vector<ChordRegion>& ch, std::span<const std::uint8_t> degrees,
                      std::span<const std::size_t> modes, std::uint8_t tonic, std::uint64_t seed) {
  detail::SplitMix64 rng(seed ^ natural_salt);
  const auto n = ch.size();
  std::vector<bool> coloured(n, false);
  std::size_t count = 0;
  const auto choices_of = [&](std::size_t i) {
    const bool minor = is_minor(ch[i].chord.quality);
    return natural_colours(degrees[i], minor);
  };
  const auto allowed = [&](std::size_t i, ChordQuality q) {
    ChordSymbol c = ch[i].chord;
    c.quality = q;
    return inside(c, mode_scales[modes[i]], tonic);
  };
  for (std::size_t i = 0; i < n; ++i) {
    const auto table = choices_of(i);
    double total = 0.0;
    for (std::uint8_t k = 0; k < table.count; ++k)
      total += allowed(i, table.choices[k].quality) ? table.choices[k].weight : 0.0;
    double pick = rng.unit() * total;
    for (std::uint8_t k = 0; k < table.count; ++k) {
      if (!allowed(i, table.choices[k].quality))
        continue;
      pick -= table.choices[k].weight;
      if (pick < 0.0) {
        if (k > 0 && count < (n + 1) / 2 &&
            (i == 0 || ch[i - 1].chord.quality != table.choices[k].quality)) {
          ch[i].chord.quality = table.choices[k].quality;
          coloured[i] = true;
          ++count;
        }
        break;
      }
    }
  }
  if (count > 0 || n < 2)
    return;
  // None drew a colour: the chord whose colour the records use most.
  std::size_t best = n;
  double best_share = 0.0;
  ChordQuality best_quality = ChordQuality::major;
  for (std::size_t i = 0; i < n; ++i) {
    const auto table = choices_of(i);
    double total = 0.0;
    for (std::uint8_t k = 0; k < table.count; ++k)
      total += table.choices[k].weight;
    for (std::uint8_t k = 1; k < table.count; ++k) {
      const double share = table.choices[k].weight / total;
      if (allowed(i, table.choices[k].quality) && share > best_share) {
        best_share = share;
        best = i;
        best_quality = table.choices[k].quality;
      }
    }
  }
  if (best < n)
    ch[best].chord.quality = best_quality;
}

ChordRegion region(ChordSymbol chord, std::int64_t length, ChordDevice device = ChordDevice::none) {
  return {{0}, length, chord, std::nullopt, device};
}

void place(std::vector<ChordRegion>& lane) noexcept {
  std::int64_t at = 0;
  for (auto& r : lane) {
    r.position = {at};
    at += r.length_ticks;
  }
}

// Half a region, on the beat grid when the region is at least two beats.
std::int64_t half_of(std::int64_t length) noexcept {
  const auto half = length / 2;
  return half >= beat ? (half / beat) * beat : half;
}

void apply_devices(std::vector<ChordRegion>& ch, const LabOptions& o, const Scale& scale) {
  const auto has = [&](Device d) { return (o.devices & device_bit(d)) != 0; };
  const auto tonic = pc(o.tonic);
  if (has(Device::borrowed)) {
    auto& c = ch[ch.size() > 2 ? ch.size() - 2 : 1];
    ChordQuality q{};
    if (detail::flipped(c.chord.quality, q)) {
      c.chord.quality = q;
      c.device = ChordDevice::borrowed;
    }
  }
  if (has(Device::suspension) && ch.back().length_ticks >= min_split) {
    auto& last = ch.back();
    const auto h = half_of(last.length_ticks);
    auto sus = last;
    sus.chord.quality = detail::suspended(last.chord.quality);
    sus.length_ticks = h;
    sus.device = ChordDevice::suspension;
    last.length_ticks -= h;
    ch.insert(ch.end() - 1, sus);
  }
  if (has(Device::passing)) {
    std::size_t at = 0;
    int biggest = passing_jump_above;
    for (std::size_t i = 1; i < ch.size(); ++i) {
      const int d = detail::step_between(ch[i].chord.root, ch[i - 1].chord.root);
      if (d > biggest && ch[i - 1].length_ticks >= min_split) {
        biggest = d;
        at = i;
      }
    }
    if (at > 0) {
      ch[at - 1].length_ticks -= beat;
      // The scale's chord a step below the one it leads into.
      std::uint8_t degree = 0;
      for (std::uint8_t d = 0; d < detail::scale_size; ++d)
        if (pc(o.tonic + scale[d]) == ch[at].chord.root)
          degree = d;
      const auto below =
          static_cast<std::uint8_t>((degree + detail::scale_size - 1) % detail::scale_size);
      auto passing = chord_on(scale, o.tonic, below, tones_of(o.colour));
      passing.quality = coloured(passing.quality, o.colour);
      ch.insert(ch.begin() + static_cast<std::ptrdiff_t>(at),
                region(passing, beat, ChordDevice::passing));
    }
  }
  const bool pull = has(Device::pull_home) && ch.back().length_ticks >= min_split;
  if (pull) {
    ch.back().length_ticks -= beat;
    const ChordSymbol v{pc(ch.front().chord.root + perfect_fifth), major_v(o.colour), std::nullopt};
    ch.push_back(region(v, beat, ChordDevice::pull_home));
  }
  if (has(Device::slide) && !pull && ch.back().length_ticks >= min_split) {
    ch.back().length_ticks -= beat;
    const ChordSymbol above{pc(ch.front().chord.root + 1), ch.front().chord.quality, std::nullopt};
    ch.push_back(region(above, beat, ChordDevice::slide));
  }
  if (has(Device::walk)) {
    auto previous_bass = ch.front().chord.root;
    for (std::size_t i = 1; i < ch.size(); ++i) {
      auto& c = ch[i];
      const auto iv = intervals(c.chord.quality);
      int best_step = walk_step_limit + 1;
      std::uint8_t best = c.chord.root;
      for (const auto t : {std::uint8_t{0}, iv.semitones[triad_third], iv.semitones[triad_fifth]}) {
        const auto p = pc(c.chord.root + t);
        const int down = pc(previous_bass - p);
        if (down > 0 && down < best_step) {
          best_step = down;
          best = p;
        }
      }
      if (best != c.chord.root) {
        c.chord.bass = best;
        if (c.device == ChordDevice::none) {
          c.device = ChordDevice::walk;
        }
      }
      previous_bass = c.chord.bass.value_or(c.chord.root);
    }
  }
  if (has(Device::pedal)) {
    for (auto& c : ch) {
      if (c.chord.root != tonic) {
        c.chord.bass = tonic;
        if (c.device == ChordDevice::none || c.device == ChordDevice::walk) {
          c.device = ChordDevice::pedal;
        }
      }
    }
  }
  if (has(Device::drone)) {
    std::uint8_t best = pc(tonic + drone_preference.front());
    int best_count = -1;
    for (const auto offset : drone_preference) {
      const auto p = pc(tonic + offset);
      int count = 0;
      for (const auto& c : ch) {
        ChordSymbol rootless = c.chord;
        rootless.bass.reset();
        count += (pitch_class_set(rootless) & bit(p)) != 0 ? 1 : 0;
      }
      if (count > best_count) {
        best_count = count;
        best = p;
      }
    }
    for (auto& c : ch) {
      c.top = best;
      // A drone on the third a sus chord suspends is that third: the
      // chord is its triad under it.
      const int third = pc(static_cast<int>(best) - static_cast<int>(c.chord.root));
      const bool sus = c.chord.quality == ChordQuality::sus2 ||
                       c.chord.quality == ChordQuality::sus4 ||
                       c.chord.quality == ChordQuality::seventh_sus4;
      if (sus && (third == major_third_semitones || third == minor_third_semitones)) {
        c.chord.quality =
            c.chord.quality == ChordQuality::seventh_sus4 && third == major_third_semitones
                ? ChordQuality::dominant_seventh
            : third == major_third_semitones ? ChordQuality::major
                                             : ChordQuality::minor;
      }
    }
  }
}

// The chord on `degree` of `mode` in the Lab's colour. These styles never
// use a diminished triad: its degree comes from the nearest mode with a
// plain chord there (A Dorian's VI is F, not F♯°).
ChordSymbol scale_chord(const LabOptions& o, std::size_t mode, std::uint8_t degree) {
  const auto tones = tones_of(o.colour);
  auto chord = chord_on(mode_scales[mode], o.tonic, degree, tones);
  if (tones == plain_tones && chord.quality == ChordQuality::diminished) {
    for (std::size_t step = 1; step < mode_count; ++step) {
      const auto lower = mode >= step ? std::optional{mode - step} : std::nullopt;
      const auto upper = mode + step < mode_count ? std::optional{mode + step} : std::nullopt;
      for (const auto other : {lower, upper}) {
        if (!other || chord.quality != ChordQuality::diminished)
          continue;
        const auto borrowed = chord_on(mode_scales[*other], o.tonic, degree, tones);
        if (borrowed.quality != ChordQuality::diminished)
          chord = borrowed;
      }
    }
  }
  chord.quality = coloured(chord.quality, o.colour);
  return chord;
}

Suggestion generate(const LabOptions& o, Variant variant) {
  const auto mi = static_cast<std::size_t>(mode_at(o.mood.brightness, o.key_mode));
  const auto& shape = shape_rows[static_cast<std::size_t>(o.shape)];
  std::vector<std::uint8_t> degrees(shape.degrees.begin(), shape.degrees.begin() + shape.count);
  std::string detail_label;
  if (variant == Variant::swap && degrees.size() > 2) {
    detail::SplitMix64 rng(o.seed ^ swap_salt);
    const auto i =
        1 + static_cast<std::size_t>(rng.unit() * static_cast<double>(degrees.size() - 1));
    for (const auto& [from, to] : twins) {
      if (degrees[i] == from) {
        degrees[i] = to;
        detail_label = "swaps chord " + std::to_string(i + 1);
        break;
      }
    }
  }
  if (variant == Variant::rotate && degrees.size() > 2) {
    detail::SplitMix64 rng(o.seed ^ rotate_salt);
    const auto r =
        1 + static_cast<std::size_t>(rng.unit() * static_cast<double>(degrees.size() - 1));
    std::rotate(degrees.begin(), degrees.begin() + static_cast<std::ptrdiff_t>(r), degrees.end());
    detail_label = "starts on chord " + std::to_string(r + 1);
  }
  const auto borrowed_at = degrees.size() - 2;
  std::vector<ChordRegion> ch;
  std::vector<std::size_t> modes;
  for (std::size_t i = 0; i < degrees.size(); ++i) {
    auto mode = mi;
    if (variant == Variant::borrow && i == borrowed_at) {
      mode = neighbour_mode(mi);
    }
    modes.push_back(mode);
    ch.push_back(region(scale_chord(o, mode, degrees[i]), o.chord_ticks));
  }
  // Still's colour is its own: the chord, then the chord recoloured.
  if (o.colour == Colour::natural && o.shape != Shape::still)
    colour_naturally(ch, degrees, modes, o.tonic, o.seed);
  if (variant == Variant::borrow) {
    detail_label = "borrows from " + std::string(mode_names[neighbour_mode(mi)]);
  }
  if (detail_label.empty()) {
    detail_label = "as the shape";
  }
  if (o.shape == Shape::still) {
    ch[1].chord.quality = still_colour(ch[1].chord.quality);
  }
  // Colour stays in each chord's mode (the borrowed one's own).
  for (std::size_t i = 0; i < ch.size(); ++i) {
    ch[i].chord = in_mode(ch[i].chord, mode_scales[modes[i]], o.tonic);
  }
  for (auto& c : ch) {
    if (from_tonic(c.chord.root, {o.tonic, project::KeyMode::minor}) != detail::dominant_degree) {
      continue;
    }
    if (o.mood.restlessness > restless_above && is_minor(c.chord.quality)) {
      c.chord.quality = major_v(o.colour);
    } else if (o.mood.restlessness < still_below) {
      c.chord.quality = detail::suspended(c.chord.quality);
    }
  }
  std::string label = std::string(mode_names[mi]) + " · " + detail_label;
  if (o.mood.restlessness > pull_bites_above && (o.devices & device_bit(Device::pull_home)) == 0) {
    label += " · restless: a major V";
  }
  apply_devices(ch, o, mode_scales[mi]);
  place(ch);
  return {variant, std::move(ch), std::move(label)};
}

} // namespace

std::span<const Mode> pad_modes(std::optional<project::KeyMode> key) noexcept {
  static constexpr std::array all{Mode::phrygian,   Mode::aeolian, Mode::dorian,
                                  Mode::mixolydian, Mode::ionian,  Mode::lydian};
  static_assert(all.size() == mode_count);
  static constexpr std::array minor{Mode::phrygian, Mode::aeolian, Mode::dorian};
  static constexpr std::array major{Mode::mixolydian, Mode::ionian, Mode::lydian};
  if (!key)
    return all;
  return *key == project::KeyMode::minor ? std::span<const Mode>(minor)
                                         : std::span<const Mode>(major);
}
Mode mode_at(double brightness, std::optional<project::KeyMode> key) noexcept {
  const auto modes = pad_modes(key);
  const auto index =
      std::floor(std::clamp(brightness, 0.0, 1.0) * static_cast<double>(modes.size()));
  return modes[std::min(static_cast<std::size_t>(index), modes.size() - 1)];
}
double home_brightness(project::KeyMode key) noexcept {
  const auto modes = pad_modes(key);
  const auto home = key == project::KeyMode::minor ? Mode::aeolian : Mode::ionian;
  const auto index = static_cast<std::size_t>(std::ranges::find(modes, home) - modes.begin());
  constexpr double middle = 0.5;
  return (static_cast<double>(index) + middle) / static_cast<double>(modes.size());
}
std::string_view mode_name(Mode mode) noexcept {
  return mode_names[static_cast<std::size_t>(mode)];
}
std::string_view feel_at(MoodPoint mood) noexcept {
  const auto distance = [&](const Feel& f) {
    return std::hypot(f.brightness - mood.brightness, f.restlessness - mood.restlessness);
  };
  return std::ranges::min(feels, {}, distance).name;
}
std::string_view shape_name(Shape shape) noexcept {
  return shape_rows[static_cast<std::size_t>(shape)].name;
}
std::string_view shape_summary(Shape shape) noexcept {
  return shape_rows[static_cast<std::size_t>(shape)].summary;
}
std::span<const std::uint8_t> shape_degrees(Shape shape) noexcept {
  const auto& row = shape_rows[static_cast<std::size_t>(shape)];
  return {row.degrees.data(), row.count};
}
std::string_view device_name(Device device) noexcept {
  return device_rows[static_cast<std::size_t>(device)].name;
}
std::string_view device_hint(Device device) noexcept {
  return device_rows[static_cast<std::size_t>(device)].hint;
}

std::array<Suggestion, variant_count> suggest(const LabOptions& options) {
  return {generate(options, Variant::as_shape), generate(options, Variant::swap),
          generate(options, Variant::rotate), generate(options, Variant::borrow)};
}

std::vector<ChordRegion> tile(std::span<const ChordRegion> progression,
                              timeline::TickPosition start, timeline::TickPosition end) {
  std::vector<ChordRegion> out;
  if (progression.empty() || end.value <= start.value) {
    return out;
  }
  const auto total = detail::end_of(progression.back());
  if (total <= 0) {
    return out;
  }
  for (auto base = start.value; base < end.value; base += total) {
    for (const auto& r : progression) {
      const auto at = base + r.position.value;
      if (at >= end.value) {
        return out;
      }
      auto copy = r;
      copy.position = {at};
      copy.length_ticks = std::min(r.length_ticks, end.value - at);
      out.push_back(copy);
    }
  }
  return out;
}

std::vector<ChordRegion> replace_range(std::span<const ChordRegion> lane,
                                       timeline::TickPosition start, timeline::TickPosition end,
                                       std::span<const ChordRegion> regions) {
  std::vector<ChordRegion> out;
  for (const auto& r : lane) {
    const auto r_end = detail::end_of(r);
    if (r_end <= start.value || r.position.value >= end.value) {
      out.push_back(r);
      continue;
    }
    if (r.position.value < start.value) {
      auto head = r;
      head.length_ticks = start.value - r.position.value;
      out.push_back(head);
    }
    if (r_end > end.value) {
      auto tail = r;
      tail.position = end;
      tail.length_ticks = r_end - end.value;
      out.push_back(tail);
    }
  }
  for (const auto& r : regions) {
    const auto from = std::max(r.position.value, start.value);
    const auto to = std::min(detail::end_of(r), end.value);
    if (to <= from) {
      continue;
    }
    auto copy = r;
    copy.position = {from};
    copy.length_ticks = to - from;
    out.push_back(copy);
  }
  std::ranges::sort(out, {}, [](const ChordRegion& r) { return r.position.value; });
  return out;
}

} // namespace oscillate::harmony
