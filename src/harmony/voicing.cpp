#include "detail.hpp"
#include "oscillate/foundation/midi.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

// Keyboard voicing: the bass plays the root (or the inversion's bass), the
// right hand plays an upper structure of three or four chord tones chosen per
// quality, so a ninth chord is its 3rd, 5th, 7th and 9th over the root in the
// bass. Candidates are every inversion of that structure in every octave, in
// the chosen style; ones that clash or muddy are refused, and the rest are
// scored by how far the voices move, how far from the register centre they
// sit and how well the top voice follows its line.
namespace oscillate::harmony {
namespace {

using detail::octave;
using detail::pc;

// The upper voices' average sits near this note (E4) plus the register offset.
constexpr double default_centre = 64.0;
// Cost of each semitone the average sits from the centre, against each
// semitone a voice moves.
constexpr double register_weight = 0.6;
// Cost of each semitone the top voice misses its line by.
constexpr double top_line_weight = 3.0;
// Fall and rise aim the top voice this far from the last one.
constexpr int top_line_step = 2;
// Cost when a drone that is a chord tone is not the top voice.
constexpr double drone_not_on_top = 6.0;
// Cost of an interval wider than an octave between neighbouring upper voices.
constexpr double wide_gap_cost = 4.0;

// Cost of an add9 chord's ninth as its lowest upper voice.
constexpr double ninth_at_bottom = 24.0;
constexpr int ninth_above_root = 2;

// Intervals that never sit next to each other in a voicing.
constexpr int semitone = 1;
constexpr int minor_ninth = 13;
// Low-interval limits: below the first note, neighbouring voices are at
// least a fifth apart; below the second, at least a minor third.
constexpr int fifth_floor = 48; // C3
constexpr int perfect_fifth = 7;
constexpr int diminished_fifth = 6;
constexpr int augmented_fifth = 8;
constexpr int third_floor = 53; // F3
constexpr int minor_third = 3;
// A drone may sit this far above the upper range.
constexpr int drone_headroom = 3;
// Drop voicings need four voices.
constexpr std::uint8_t drop_voices = 4;
constexpr std::size_t max_upper = 4;

struct Structure {
  std::array<std::uint8_t, max_upper> pcs{};
  std::uint8_t count = 0;
};

// The upper structure's intervals above the root, per quality. Rootless
// voicings swap the root of a four-note seventh chord for its ninth.
struct Upper {
  std::array<std::uint8_t, max_upper> semitones;
  std::uint8_t count;
};
constexpr Upper upper_of(ChordQuality quality) noexcept {
  using Q = ChordQuality;
  switch (quality) {
  case Q::major:
    return {{0, 4, 7}, 3};
  case Q::minor:
    return {{0, 3, 7}, 3};
  case Q::diminished:
    return {{0, 3, 6}, 3};
  case Q::sus2:
    return {{0, 2, 7}, 3};
  case Q::add9:
    return {{0, 4, 7, 2}, 4};
  case Q::sixth:
    return {{0, 4, 7, 9}, 4};
  case Q::minor_sixth:
    return {{0, 3, 7, 9}, 4};
  case Q::major_seventh:
    return {{0, 4, 7, 11}, 4};
  case Q::minor_seventh:
    return {{0, 3, 7, 10}, 4};
  case Q::dominant_seventh:
    return {{0, 4, 7, 10}, 4};
  case Q::half_diminished:
    return {{0, 3, 6, 10}, 4};
  case Q::diminished_seventh:
    return {{0, 3, 6, 9}, 4};
  case Q::seventh_sus4:
    return {{0, 5, 7, 10}, 4};
  case Q::major_ninth:
    return {{4, 7, 11, 2}, 4};
  case Q::minor_ninth:
    return {{3, 7, 10, 2}, 4};
  case Q::dominant_ninth:
    return {{4, 7, 10, 2}, 4};
  case Q::dominant_seventh_flat_ninth:
    return {{4, 7, 10, 1}, 4};
  case Q::minor_eleventh:
    return {{3, 10, 2, 5}, 4};
  case Q::dominant_thirteenth:
    return {{4, 10, 2, 9}, 4};
  case Q::minor_add9:
    return {{0, 3, 7, 2}, 4};
  case Q::sus4:
    return {{0, 5, 7}, 3};
  case Q::power:
    return {{0, 7, 12}, 3};
  }
  return {{0, 4, 7}, 3};
}

constexpr bool added_ninth(ChordQuality quality) noexcept {
  return quality == ChordQuality::add9 || quality == ChordQuality::minor_add9;
}

Structure structure_of(const ChordSymbol& chord, VoicingStyle style) noexcept {
  auto upper = upper_of(chord.quality);
  const bool seventh =
      upper.count == drop_voices && upper.semitones[0] == 0 &&
      chord.quality != ChordQuality::sixth && chord.quality != ChordQuality::minor_sixth &&
      chord.quality != ChordQuality::add9 && chord.quality != ChordQuality::minor_add9 &&
      chord.quality != ChordQuality::diminished_seventh;
  if (style == VoicingStyle::rootless && seventh) {
    // The ninth, or the eleventh over a half-diminished chord (whose ninth
    // would be a minor ninth over the root).
    constexpr std::uint8_t ninth = 2;
    constexpr std::uint8_t eleventh = 5;
    upper.semitones[0] = chord.quality == ChordQuality::half_diminished ? eleventh : ninth;
  }
  Structure out;
  for (std::uint8_t i = 0; i < upper.count; ++i)
    out.pcs[out.count++] = pc(chord.root + upper.semitones[i]);
  return out;
}

double move_cost(const Voicing& a, const Voicing& b) noexcept {
  if (a.count == 0 || b.count == 0)
    return 0.0;
  if (a.count == b.count) {
    double sum = 0.0;
    for (std::uint8_t i = 0; i < a.count; ++i)
      sum += std::abs(static_cast<int>(a.notes[i]) - static_cast<int>(b.notes[i]));
    return sum;
  }
  const auto nearest = [](const Voicing& x, const Voicing& y) {
    double sum = 0.0;
    for (std::uint8_t i = 0; i < x.count; ++i) {
      int best = std::numeric_limits<int>::max();
      for (std::uint8_t j = 0; j < y.count; ++j)
        best =
            std::min(best, std::abs(static_cast<int>(x.notes[i]) - static_cast<int>(y.notes[j])));
      sum += best;
    }
    return sum;
  };
  return (nearest(a, b) + nearest(b, a)) / 2.0;
}

// Insertion sort: at most max_voices notes.
void sort_notes(Voicing& v) noexcept {
  for (std::size_t i = 1; i < v.count && i < max_voices; ++i) {
    const auto note = v.notes[i];
    std::size_t j = i;
    for (; j > 0 && v.notes[j - 1] > note; --j)
      v.notes[j] = v.notes[j - 1];
    v.notes[j] = note;
  }
}

// True when the voicing sounds clean: no neighbouring semitones, no minor
// ninth between any two voices (unless the chord is a 7♭9), and wide enough
// intervals low down.
// `rub`: a chord whose ninth may sit a semitone under its minor third (the
// Open add9), above third_floor.
bool clean(const Voicing& v, bool flat_ninth_allowed, const ChordSymbol* rub = nullptr) noexcept {
  for (std::uint8_t i = 1; i < v.count; ++i) {
    const int low = v.notes[i - 1];
    const int gap = v.notes[i] - low;
    const bool rubs = rub != nullptr && gap == semitone && low >= third_floor &&
                      pc(low) == pc(rub->root + ninth_above_root) &&
                      pc(v.notes[i]) == pc(rub->root + minor_third);
    if (gap == semitone && !rubs)
      return false;
    if (low < fifth_floor && gap < perfect_fifth)
      return false;
    if (low < third_floor && gap < minor_third)
      return false;
  }
  if (!flat_ninth_allowed) {
    for (std::uint8_t i = 0; i < v.count; ++i)
      for (std::uint8_t j = i + 1; j < v.count; ++j)
        if ((v.notes[j] - v.notes[i]) % octave == semitone &&
            v.notes[j] - v.notes[i] >= minor_ninth &&
            !(rub != nullptr && pc(v.notes[i]) == pc(rub->root + ninth_above_root)))
          return false;
  }
  return true;
}

// Builds the close stack of `s` from inversion `k` with its lowest note in
// octave `o`, then applies the style's drop.
Voicing stack(const Structure& s, std::uint8_t k, int o, VoicingStyle style) noexcept {
  Voicing v;
  int last = s.pcs[k] + octave * o;
  v.notes[v.count++] = static_cast<std::uint8_t>(last);
  for (std::uint8_t j = 1; j < s.count; ++j) {
    int m = s.pcs[static_cast<std::size_t>((k + j) % s.count)] + octave * (last / octave);
    while (m <= last)
      m += octave;
    v.notes[v.count++] = static_cast<std::uint8_t>(m);
    last = m;
  }
  const auto n = v.count;
  if ((style == VoicingStyle::drop2 || style == VoicingStyle::rootless) && n >= drop_voices) {
    v.notes[n - 2] = static_cast<std::uint8_t>(v.notes[n - 2] - octave);
  } else if (style == VoicingStyle::spread) {
    if (n >= drop_voices) {
      v.notes[n - 2] = static_cast<std::uint8_t>(v.notes[n - 2] - octave);
      v.notes[n - 4] = static_cast<std::uint8_t>(v.notes[n - 4] - octave);
    } else if (n > 1) {
      v.notes[1] = static_cast<std::uint8_t>(v.notes[1] + octave);
    }
  }
  sort_notes(v);
  return v;
}

// Places a drone pitch class on top: it is already the top voice, it
// replaces a top voice a semitone or tone below it when that voice is only
// the root or fifth (never the third or seventh), or it is added above.
// False when it cannot sit cleanly within reach.
bool add_drone(Voicing& v, std::uint8_t drone, std::uint8_t highest,
               const ChordSymbol& chord) noexcept {
  const int top = v.notes[v.count - 1];
  if (pc(top) == drone)
    return true;
  int m = top + 1;
  while (pc(m) != drone)
    ++m;
  if (m > highest + drone_headroom)
    return false;
  constexpr int tone = 2;
  constexpr int fifth = 7;
  // A power chord's fifth is all it has besides its root: it stays.
  const bool expendable = pc(top) == chord.root || (pc(top) == pc(chord.root + fifth) &&
                                                    chord.quality != ChordQuality::power);
  if (m - top <= tone) {
    if (!expendable)
      return false;
    v.notes[v.count - 1] = static_cast<std::uint8_t>(m);
  } else if (v.count < max_voices)
    v.notes[v.count++] = static_cast<std::uint8_t>(m);
  return true;
}

// ---- Open (ADR-0084; refit to the records 2026-11-03) ----
// How the records (PML's Iconic Chord Progressions: 52 progressions, 718
// chords, measured) voice a chord over its bass. The low register holds
// octaves and fifths only: the first voice above the bass is its octave in
// half the chords and its fifth in a third. The third sits high, most often
// on top; colour tones (sevenths, ninths, sixths) sit above the doubled root,
// near the top. Seconds appear only up from about C4, a semitone between
// neighbours almost never. Three or four voices over a bass near F2, the
// lowest near D3 and the top near E4. Voicings are searched under those rules
// and scored by how they sit and how they move.

enum class ToneRole : std::uint8_t { root, third, fifth, colour };

struct OpenTone {
  std::uint8_t pc = 0;
  ToneRole role = ToneRole::root;
  bool required = false;
  std::uint8_t most = 1; // How many voices may sound it.
  // How far over the bass it may sit at least (thirds and colour only).
  std::uint8_t over_bass = 0;
};

// Thirds and colour sit at least a ninth over the bass (the records' tenth,
// or a ninth's ninth) and never under E3; under it neighbours are at least a
// fourth apart.
constexpr int open_colour_floor = 52;
constexpr int open_colour_over_bass = 14;
// A seventh may sit a seventh over the bass (the shell voicing the records
// use under a thirteenth's thirteenth).
constexpr int open_seventh_over_bass = 10;
constexpr int minor_seventh_semitones = 10;
constexpr int major_seventh_semitones = 11;
constexpr int open_low_gap = 5;
// Seconds between neighbours only from here (C4), and one at most.
constexpr int open_second_floor = 60;
constexpr int whole_tone = 2;
// Neighbouring upper voices at most an octave apart; the bass to the first
// voice at most an octave and a fifth, at least a fifth.
constexpr int open_max_gap = octave;
constexpr int open_bass_gap_most = 19;
constexpr int open_bass_gap_least = perfect_fifth;
// A colour tone may sit this far over the range's top (a minor ninth
// chord's ninth must clear its third: Cm9's D over E♭4).
constexpr int open_colour_headroom = 3;
constexpr std::uint8_t open_fewest = 3;
constexpr std::uint8_t open_most = 5;
constexpr std::uint8_t open_usual = 4;
// The records' medians: the lowest voice and the top voice.
constexpr double open_lowest_target = 50.5;
constexpr double open_top_target = 64.5;
// Costs, in semitones of voice movement.
// When no voicing holds every colour tone (a thirteenth's seventh and
// thirteenth cannot both clear each other in range), the next search may
// leave colour out at this cost each.
constexpr double open_missing_colour_cost = 3.0;
constexpr double open_register_weight = 0.35;
constexpr double open_voice_count_cost = 1.5;
constexpr double open_first_fifth_cost = 0.5; // The fifth over the bass rather than its octave.
constexpr double open_first_other_cost = 2.5; // Anything else there.
constexpr double open_second_cost = 1.5;
constexpr double open_move_weight = 0.6;
// The top voice, by how often the records put each role there.
constexpr std::array<double, 4> open_top_cost{1.2, 0.0, 0.6, 0.3}; // root, third, fifth, colour
// A drone rings on top, at one pitch: costs when it is not the top voice, or
// moves from where it rang.
constexpr double open_drone_under_cost = 3.0;
// The drone rings up top: from C4.
constexpr int open_drone_floor = 60;
constexpr double open_drone_moves_cost = 4.0;

constexpr bool suspended_quality(ChordQuality q) noexcept {
  return q == ChordQuality::sus2 || q == ChordQuality::sus4 || q == ChordQuality::seventh_sus4;
}

// The chord's tones with their roles; a drone that is not a chord tone joins
// as a colour that must sound on top.
std::uint8_t open_tones(const ChordSymbol& chord, std::optional<std::uint8_t> drone,
                        std::array<OpenTone, max_voices>& out) noexcept {
  const auto iv = intervals(chord.quality);
  const bool sus = suspended_quality(chord.quality);
  const bool slash = chord.bass && pc(*chord.bass) != chord.root;
  std::uint8_t n = 0;
  for (std::uint8_t i = 0; i < iv.count && n < max_voices; ++i) {
    const int s = iv.semitones[i];
    const auto p = pc(chord.root + s);
    bool known = false;
    for (std::uint8_t j = 0; j < n; ++j)
      known = known || out[j].pc == p;
    if (known)
      continue;
    OpenTone t{p};
    if (s == 0) {
      t = {p, ToneRole::root, slash, 2};
    } else if (s == 3 || s == 4 || (sus && i == 1 && s < perfect_fifth)) {
      t = {p, ToneRole::third, true, 1, open_colour_over_bass};
    } else if (s >= diminished_fifth && s <= augmented_fifth && i == 2) {
      // Some chords are not themselves without their fifth: a sixth chord
      // less its fifth is its relative minor seventh (C6 less G is Am/C), a
      // sus chord less its fifth an open fourth or second (the records
      // voice sus4 as 7-12-17-19), a power chord nothing at all.
      // A triad less its fifth is a bare third. Chords with a seventh or
      // more may leave it out.
      constexpr std::uint8_t triad_tones = 3;
      const bool needed =
          chord.quality == ChordQuality::power || chord.quality == ChordQuality::sixth ||
          chord.quality == ChordQuality::minor_sixth || sus || iv.count <= triad_tones ||
          s != perfect_fifth; // A diminished fifth is the chord's own (m7♭5).
      t = {p, ToneRole::fifth, needed, 2};
    } else if (s == octave) {
      continue; // The power chord's octave is its root again.
    } else {
      const bool seventh = s == minor_seventh_semitones || s == major_seventh_semitones;
      t = {p, ToneRole::colour, true, 1,
           static_cast<std::uint8_t>(seventh ? open_seventh_over_bass : open_colour_over_bass)};
    }
    out[n++] = t;
  }
  if (drone) {
    bool known = false;
    for (std::uint8_t j = 0; j < n; ++j) {
      if (out[j].pc == pc(*drone)) {
        out[j].required = true;
        known = true;
      }
    }
    if (!known && n < max_voices)
      out[n++] = {pc(*drone), ToneRole::colour, true, 1, open_colour_over_bass};
  }
  return n;
}

struct OpenSearch {
  const ChordSymbol* chord = nullptr;
  std::array<OpenTone, max_voices> tones{};
  std::uint8_t tone_count = 0;
  std::uint8_t required = 0;
  std::optional<std::uint8_t> drone;
  int bass = -1; // The bass under the voicing, or -1.
  bool flat_ninth = false;
  const VoicingOptions* options = nullptr;
  const Voicing* previous = nullptr;
  double lowest_target = open_lowest_target;
  double top_target = open_top_target;
  std::uint8_t usual = open_usual;
  bool strict = true; // Every colour tone sounds.
  // Candidate notes, ascending, and which tone each sounds.
  std::array<std::uint8_t, 2 * octave * 2> notes{};
  std::array<std::uint8_t, 2 * octave * 2> tone_of{};
  std::uint8_t note_count = 0;
  [[nodiscard]] std::uint8_t uses_in(const Voicing& v, std::uint8_t t) const noexcept {
    std::uint8_t n = 0;
    for (std::uint8_t i = 0; i < v.count; ++i)
      n = static_cast<std::uint8_t>(n + (pc(v.notes[i]) == tones[t].pc ? 1 : 0));
    return n;
  }
  // The search state.
  Voicing chosen;
  std::array<std::uint8_t, max_voices> uses{};
  std::uint8_t seconds = 0;
  Voicing best;
  double best_cost = std::numeric_limits<double>::infinity();
};

double open_cost(const OpenSearch& s, const Voicing& v) noexcept {
  const auto& o = *s.options;
  double cost = open_voice_count_cost * std::abs(static_cast<int>(v.count) - s.usual);
  const int low = v.notes[0];
  const int top = v.notes[v.count - 1];
  cost += open_register_weight * (std::abs(low - s.lowest_target) + std::abs(top - s.top_target));
  if (s.bass >= 0) {
    const int first = low - s.bass;
    cost += first == octave          ? 0.0
            : first == perfect_fifth ? open_first_fifth_cost
                                     : open_first_other_cost;
  }
  const auto role_of = [&](int note) {
    for (std::uint8_t t = 0; t < s.tone_count; ++t)
      if (s.tones[t].pc == pc(note))
        return s.tones[t].role;
    return ToneRole::colour;
  };
  cost += open_top_cost[static_cast<std::size_t>(role_of(top))];
  cost += open_second_cost * s.seconds;
  if (!s.strict)
    for (std::uint8_t t = 0; t < s.tone_count; ++t)
      if (s.tones[t].required && s.uses_in(v, t) == 0)
        cost += open_missing_colour_cost;
  if (s.drone) {
    if (pc(top) != pc(*s.drone))
      cost += open_drone_under_cost;
    if (s.previous != nullptr) {
      // Where it rang: its highest note.
      int rang = -1;
      for (std::uint8_t i = 0; i < s.previous->count; ++i)
        if (pc(s.previous->notes[i]) == pc(*s.drone))
          rang = s.previous->notes[i];
      int rings = -1;
      for (std::uint8_t i = 0; i < v.count; ++i)
        if (pc(v.notes[i]) == pc(*s.drone))
          rings = v.notes[i];
      const bool stays = rang < 0 || rings == rang;
      if (!stays)
        cost += open_drone_moves_cost;
    }
  }
  if (s.previous != nullptr && s.previous->count > 0) {
    cost += open_move_weight * move_cost(*s.previous, v);
    const int was = s.previous->notes[s.previous->count - 1];
    switch (o.top_line) {
    case TopLine::free:
      break;
    case TopLine::hold:
      cost += top_line_weight * std::abs(top - was);
      break;
    case TopLine::fall:
      cost += top_line_weight * std::abs(top - (was - top_line_step));
      break;
    case TopLine::rise:
      cost += top_line_weight * std::abs(top - (was + top_line_step));
      break;
    }
  }
  return cost;
}

void open_search(OpenSearch& s, std::uint8_t from) noexcept {
  if (s.chosen.count >= open_fewest) {
    bool complete = true;
    for (std::uint8_t t = 0; t < s.tone_count; ++t) {
      const bool optional_colour = !s.strict && s.tones[t].role == ToneRole::colour &&
                                   !(s.drone && s.tones[t].pc == pc(*s.drone));
      complete = complete && (!s.tones[t].required || optional_colour || s.uses[t] > 0);
    }
    if (complete && s.drone) {
      bool rings = false;
      for (std::uint8_t i = 0; i < s.chosen.count; ++i)
        rings = rings ||
                (pc(s.chosen.notes[i]) == pc(*s.drone) && s.chosen.notes[i] >= open_drone_floor);
      // A drone outside the chord rings on top, where it reads as a drone
      // rather than a tone of another chord.
      const bool on_top = pc(s.chosen.notes[s.chosen.count - 1]) == pc(*s.drone);
      complete = rings && (on_top || (pitch_class_set(*s.chord) & detail::bit(pc(*s.drone))) != 0);
    }
    if (complete) {
      const double cost = open_cost(s, s.chosen);
      if (cost < s.best_cost) {
        s.best_cost = cost;
        s.best = s.chosen;
      }
    }
  }
  if (s.chosen.count >= open_most)
    return;
  for (std::uint8_t i = from; i < s.note_count; ++i) {
    const int m = s.notes[i];
    const auto t = s.tone_of[i];
    if (s.uses[t] >= s.tones[t].most)
      continue;
    std::uint8_t seconds = s.seconds;
    if (s.chosen.count == 0) {
      if (s.bass >= 0 && (m - s.bass < open_bass_gap_least || m - s.bass > open_bass_gap_most))
        continue;
      if (s.options->bass_octave_lowest && s.bass >= 0 && m != s.bass + octave)
        continue;
    } else {
      const int last = s.chosen.notes[s.chosen.count - 1];
      const int gap = m - last;
      if (gap > open_max_gap)
        break; // Every later note is further still.
      if (gap == semitone || (last < open_colour_floor && gap < open_low_gap))
        continue;
      if (gap == whole_tone) {
        if (last < open_second_floor || seconds > 0)
          continue;
        ++seconds;
      }
    }
    bool clashes = false;
    if (!s.flat_ninth) {
      for (std::uint8_t j = 0; j < s.chosen.count && !clashes; ++j) {
        const int span = m - s.chosen.notes[j];
        clashes = span >= minor_ninth && span % octave == semitone;
      }
      const int over_bass = m - s.bass;
      clashes =
          clashes || (s.bass >= 0 && over_bass >= minor_ninth && over_bass % octave == semitone);
    }
    if (clashes)
      continue;
    s.chosen.notes[s.chosen.count++] = static_cast<std::uint8_t>(m);
    ++s.uses[t];
    const auto kept = s.seconds;
    s.seconds = seconds;
    open_search(s, static_cast<std::uint8_t>(i + 1));
    s.seconds = kept;
    --s.uses[t];
    --s.chosen.count;
  }
}

Voicing voice_open(const ChordSymbol& chord, std::optional<std::uint8_t> top,
                   const VoicingOptions& options, const Voicing* previous,
                   bool strict = true) noexcept {
  OpenSearch s;
  s.strict = strict;
  s.chord = &chord;
  s.options = &options;
  s.previous = previous;
  s.drone = top;
  s.flat_ninth = chord.quality == ChordQuality::dominant_seventh_flat_ninth;
  s.tone_count = open_tones(chord, top, s.tones);
  for (std::uint8_t t = 0; t < s.tone_count; ++t)
    s.required = static_cast<std::uint8_t>(s.required + (s.tones[t].required ? 1 : 0));
  // Chords with more to say take a voice more.
  s.usual = s.required >= open_usual ? open_most : open_usual;
  if (options.bass) {
    const auto wanted = pc(chord.bass.value_or(chord.root));
    s.bass =
        options.bass_lowest + pc(static_cast<int>(wanted) - static_cast<int>(options.bass_lowest));
  }
  // With the bass off its octave is the lowest voice, whatever role it has
  // (an inversion's third, a pedal under the chord).
  const int bass_octave = options.bass_octave_lowest && s.bass >= 0 ? s.bass + octave : -1;
  if (bass_octave >= 0) {
    bool known = false;
    for (std::uint8_t t = 0; t < s.tone_count; ++t)
      known = known || s.tones[t].pc == pc(bass_octave);
    if (!known && s.tone_count < s.tones.size())
      s.tones[s.tone_count++] = {pc(bass_octave), ToneRole::root, true, 1};
  }
  s.lowest_target = open_lowest_target + options.register_offset;
  s.top_target = open_top_target + options.register_offset;
  const int highest = options.highest + std::max(open_colour_headroom, top ? drone_headroom : 0);
  for (int m = options.lowest; m <= highest && s.note_count < s.notes.size(); ++m) {
    for (std::uint8_t t = 0; t < s.tone_count; ++t) {
      if (s.tones[t].pc != pc(m))
        continue;
      const bool low_ok = s.tones[t].role == ToneRole::root || s.tones[t].role == ToneRole::fifth;
      // Over its root the chord's thirds and colour clear the bass by a
      // ninth or so; over a pedal or an inversion only the floor holds.
      const bool over_root = s.bass >= 0 && pc(s.bass) == chord.root;
      if (!low_ok && m != bass_octave &&
          (m < open_colour_floor || (over_root && m - s.bass < s.tones[t].over_bass)))
        break;
      // Only colour (and the drone) climbs past the range.
      if (m > options.highest && s.tones[t].role != ToneRole::colour)
        break;
      s.notes[s.note_count] = static_cast<std::uint8_t>(m);
      s.tone_of[s.note_count++] = t;
      break;
    }
  }
  open_search(s, 0);
  if (s.best.count == 0 && strict)
    return voice_open(chord, top, options, previous, false);
  if (s.best.count == 0 && top)
    return voice_open(chord, std::nullopt, options, previous);
  return s.best;
}

} // namespace

Voicing voice(const ChordSymbol& chord, std::optional<std::uint8_t> top,
              const VoicingOptions& options, const Voicing* previous) noexcept {
  if (options.style == VoicingStyle::open) {
    if (auto open = voice_open(chord, top, options, previous); open.count > 0)
      return open;
  }
  const bool flat_ninth = chord.quality == ChordQuality::dominant_seventh_flat_ninth;
  const double centre = default_centre + options.register_offset;
  Voicing best;
  double best_cost = std::numeric_limits<double>::infinity();
  // Rootless voicing tries both its rootless and its rooted structure, so a
  // chord without a ninth still voices.
  for (const auto style : {options.style, VoicingStyle::close, VoicingStyle::drop2}) {
    const auto s = structure_of(chord, style);
    const double style_cost = style == options.style ? 0.0 : wide_gap_cost;
    for (std::uint8_t k = 0; k < s.count; ++k) {
      for (int o = options.lowest / octave - 1; o <= options.highest / octave; ++o) {
        auto v = stack(s, k, o, style);
        if (v.notes[0] < options.lowest || v.notes[v.count - 1] > options.highest)
          continue;
        const bool drone_in_chord = top && (pitch_class_set(chord) & detail::bit(pc(*top))) != 0;
        double cost = style_cost;
        if (top) {
          if (drone_in_chord && pc(v.notes[v.count - 1]) != pc(*top))
            cost += drone_not_on_top;
          if (!add_drone(v, pc(*top), options.highest, chord))
            continue;
          sort_notes(v);
        }
        if (!clean(v, flat_ninth))
          continue;
        double mean = 0.0;
        for (std::uint8_t i = 0; i < v.count; ++i)
          mean += v.notes[i];
        mean /= v.count;
        cost += std::abs(mean - centre) * register_weight;
        // An added ninth belongs above the triad: at the bottom of the hand it
        // muddies (melodic house keeps it on top).
        if (added_ninth(chord.quality) && pc(v.notes[0]) == pc(chord.root + ninth_above_root))
          cost += ninth_at_bottom;
        for (std::uint8_t i = 1; i < v.count; ++i)
          if (v.notes[i] - v.notes[i - 1] > octave)
            cost += wide_gap_cost;
        if (previous != nullptr && previous->count > 0) {
          cost += move_cost(*previous, v);
          const int was = previous->notes[previous->count - 1];
          const int now = v.notes[v.count - 1];
          switch (options.top_line) {
          case TopLine::free:
            break;
          case TopLine::hold:
            cost += top_line_weight * std::abs(now - was);
            break;
          case TopLine::fall:
            cost += top_line_weight * std::abs(now - (was - top_line_step));
            break;
          case TopLine::rise:
            cost += top_line_weight * std::abs(now - (was + top_line_step));
            break;
          }
        }
        if (cost < best_cost) {
          best_cost = cost;
          best = v;
        }
      }
    }
  }
  if (best.count == 0 && top) {
    // The drone cannot sit cleanly over this chord: voice it without one.
    return voice(chord, std::nullopt, options, previous);
  }
  if (best.count == 0) {
    // Nothing qualified (a range too narrow): the close root position
    // nearest the centre, in the octave nearest it that keeps every note a
    // MIDI key (the range is not met, but the notes stay valid).
    const auto s = structure_of(chord, VoicingStyle::close);
    const auto lowest_stack = stack(s, 0, 0, VoicingStyle::close);
    const int highest_octave =
        (foundation::max_midi_key - lowest_stack.notes[lowest_stack.count - 1]) / octave;
    const int nearest = std::clamp(static_cast<int>(centre) / octave - 1, 0, highest_octave);
    best = stack(s, 0, nearest, VoicingStyle::close);
  }
  return best;
}

int movement(const Voicing& from, const Voicing& to) noexcept {
  return static_cast<int>(std::lround(move_cost(from, to)));
}

std::uint8_t bass_note(const ChordSymbol& chord, std::uint8_t lowest) noexcept {
  const auto target = pc(chord.bass.value_or(chord.root));
  return static_cast<std::uint8_t>(lowest +
                                   pc(static_cast<int>(target) - static_cast<int>(lowest)));
}

std::vector<Voicing> voice_lane(std::span<const ChordRegion> lane, const VoicingOptions& options) {
  std::vector<Voicing> out;
  out.reserve(lane.size());
  for (const auto& region : lane)
    out.push_back(voice(region.chord, region.top, options, out.empty() ? nullptr : &out.back()));
  return out;
}

} // namespace oscillate::harmony
