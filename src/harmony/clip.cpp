#include "oscillate/harmony/clip.hpp"
#include "oscillate/foundation/midi.hpp"

#include "detail.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <limits>

// Chords as notes: reading a clip's chords from its notes on a 16th grid, and
// writing chord edits back into the notes in their rhythm. Recognition sweeps
// sorted note edges (one reading per change of the sounding set, not per
// step), and re-reads each region from an active-note sweep, so a clip costs
// about its note count times its polyphony.
namespace oscillate::harmony {
namespace {

using detail::bit;
using detail::octave;
using detail::pc;

constexpr std::int64_t step_ticks = recognition_step_ticks;
constexpr std::size_t midi_keys = 128;
constexpr int perfect_fifth = 7;
// The quality's second tone is its third (or the tone a sus chord has instead).
constexpr std::size_t third_index = 1;

// Template scoring. A matched tone outweighs a missing one; a missing fifth
// costs more than the root-in-the-bass bonus, so a complete inversion (Am/C)
// beats an incomplete root-position reading (C6 without its fifth); a missing
// third costs most, so G/A reads as G over A rather than an A11 without its
// third. Tones outside the chord cost more than missing ones.
constexpr double matched_tone = 2.0;
constexpr double missing_fifth = 2.0;
// A missing seventh or extension costs enough that a complete slash chord
// (Cadd9/A, a pedal under C) beats a root-position reading missing its ninth
// (Am11).
constexpr double missing_tone = 3.5;
constexpr double missing_third = 4.5;
constexpr double outside_tone = 3.0;
constexpr double root_in_bass = 1.5;
constexpr double root_silent = 1.0;
// Simpler chords win ties against richer ones.
constexpr double per_template_tone = 0.05;
// Breaks exact ties (Am7/G against C6/G) towards the chord before; smaller
// than any other difference in score.
constexpr double same_root_as_before = 0.01;
constexpr int min_pitch_classes = 2;
// A step with fewer pitch classes that fit the chord before inherits it.
constexpr int min_step_pitch_classes = 3;
// A note sounding less than this share of the region's longest is passing.
constexpr std::int64_t passing_share = 4;

// The voicing cleanliness rules voice() applies (voicing.cpp), checked again
// when a slash chord's root joins the voicing.
constexpr int semitone = 1;
constexpr int minor_ninth = 13;
constexpr int fifth_floor = 48;
constexpr int third_floor = 53;
constexpr int minor_third = 3;
// Cost of the added root becoming the top voice, against each semitone it
// sits from the voicing's middle.
constexpr int root_on_top_cost = octave * 2;

struct Match {
  ChordSymbol chord;
  double score = -std::numeric_limits<double>::infinity();
  bool complete = false; // Every tone but the fifth present, nothing outside.
};

std::uint16_t template_set(std::uint8_t root, ChordQuality quality) noexcept {
  const auto iv = intervals(quality);
  std::uint16_t set = 0;
  for (std::uint8_t i = 0; i < iv.count; ++i)
    set = static_cast<std::uint16_t>(set | bit(pc(root + iv.semitones[i])));
  return set;
}

int count(std::uint16_t bits) noexcept { return std::popcount(bits); }

Match best_match(std::uint16_t pcs, std::uint8_t bass,
                 std::optional<std::uint8_t> previous_root) noexcept {
  Match best;
  for (std::uint8_t root = 0; root < pitch_classes; ++root) {
    for (std::uint8_t q = 1; q <= last_chord_quality; ++q) {
      const auto quality = static_cast<ChordQuality>(q);
      const auto tones = template_set(root, quality);
      const auto fifth = static_cast<std::uint16_t>(tones & bit(pc(root + perfect_fifth)));
      const auto third = bit(pc(root + intervals(quality).semitones[third_index]));
      const auto missing = static_cast<std::uint16_t>(tones & ~pcs);
      const auto outside = static_cast<std::uint16_t>(pcs & ~tones & ~bit(bass));
      double score = matched_tone * count(static_cast<std::uint16_t>(tones & pcs)) -
                     missing_fifth * count(static_cast<std::uint16_t>(missing & fifth)) -
                     missing_third * count(static_cast<std::uint16_t>(missing & third)) -
                     missing_tone * count(static_cast<std::uint16_t>(missing & ~fifth & ~third)) -
                     outside_tone * count(outside) - per_template_tone * count(tones);
      if (root == bass)
        score += root_in_bass;
      if ((pcs & bit(root)) == 0)
        score -= root_silent;
      if (previous_root && *previous_root == root)
        score += same_root_as_before;
      if (score > best.score) {
        best.score = score;
        best.chord = {root, quality,
                      bass != root ? std::optional<std::uint8_t>{bass} : std::nullopt};
        best.complete = (missing & ~fifth) == 0 && outside == 0;
      }
    }
  }
  return best;
}

struct Reading {
  ChordSymbol chord;
  std::optional<std::uint8_t> top; // A drone outside the chord.
  bool operator==(const Reading&) const = default;
};

// The chord of a pitch-class set over `bass`. `drone` is the highest note's
// pitch class when it may be a drone: it is read as one when the chord is
// complete without it and not with it.
std::optional<Reading> read_set(std::uint16_t pcs, std::uint8_t bass,
                                std::optional<std::uint8_t> drone,
                                std::optional<std::uint8_t> previous_root) noexcept {
  if (count(pcs) < min_pitch_classes)
    return std::nullopt;
  const auto with = best_match(pcs, bass, previous_root);
  if (drone && *drone != bass && !with.complete) {
    const auto rest = static_cast<std::uint16_t>(pcs & ~bit(*drone));
    if (count(rest) >= min_pitch_classes) {
      const auto without = best_match(rest, bass, previous_root);
      if (without.complete && (pitch_class_set(without.chord) & bit(*drone)) == 0)
        return Reading{without.chord, drone};
    }
  }
  return Reading{with.chord, std::nullopt};
}

// The drone a region asks for, if it is one: a top that is a chord tone is
// just the chord's top voice.
std::optional<std::uint8_t> drone_of(const ChordSymbol& chord,
                                     std::optional<std::uint8_t> top) noexcept {
  if (top && (pitch_class_set(chord) & bit(pc(*top))) == 0)
    return pc(*top);
  return std::nullopt;
}

// ---- the grid ----
std::int64_t to_step(std::int64_t tick) noexcept {
  return (std::max<std::int64_t>(tick, 0) + step_ticks / 2) / step_ticks;
}
std::int64_t steps_in(std::int64_t length) noexcept {
  return (length + step_ticks - 1) / step_ticks;
}

struct Sounding {
  std::int64_t from = 0; // Steps, [from, to).
  std::int64_t to = 0;
  std::uint8_t key = 0;
};

// The notes that sound on at least one grid step, sorted by first step.
std::vector<Sounding> quantise(std::span<const project::MidiNote> notes, std::int64_t steps) {
  std::vector<Sounding> out;
  out.reserve(notes.size());
  for (const auto& n : notes) {
    if (!foundation::valid_midi_key(n.key))
      continue;
    const auto from = std::min(to_step(n.start.value), steps);
    const auto to = std::min(to_step(n.end.value), steps);
    if (to > from)
      out.push_back({from, to, n.key});
  }
  std::sort(out.begin(), out.end(),
            [](const Sounding& x, const Sounding& y) { return x.from < y.from; });
  return out;
}

struct StepSpan {
  std::int64_t from = 0;
  std::int64_t to = 0;
};

// Reads each span (sorted, non-overlapping) from the notes sounding in it,
// leaving passing notes out. nullopt where fewer than two pitch classes sound.
std::vector<std::optional<Reading>> read_spans(std::span<const Sounding> notes,
                                               std::span<const StepSpan> spans) {
  std::vector<std::optional<Reading>> out;
  out.reserve(spans.size());
  std::vector<std::size_t> active;
  std::size_t next = 0;
  std::optional<std::uint8_t> previous_root;
  std::array<std::int64_t, midi_keys> weight{};
  for (const auto& span : spans) {
    while (next < notes.size() && notes[next].from < span.to)
      active.push_back(next++);
    weight.fill(0);
    std::int64_t longest = 0;
    for (const auto i : active) {
      const auto& n = notes[i];
      const auto overlap = std::min(n.to, span.to) - std::max(n.from, span.from);
      if (overlap > 0) {
        weight[n.key] += overlap;
        longest = std::max(longest, weight[n.key]);
      }
    }
    std::erase_if(active, [&](std::size_t i) { return notes[i].to <= span.to; });
    std::uint16_t pcs = 0;
    int lowest = -1;
    int highest = -1;
    for (std::size_t key = 0; key < midi_keys; ++key) {
      if (weight[key] > 0 && weight[key] * passing_share >= longest) {
        pcs = static_cast<std::uint16_t>(pcs | bit(pc(static_cast<int>(key))));
        if (lowest < 0)
          lowest = static_cast<int>(key);
        highest = static_cast<int>(key);
      }
    }
    std::optional<Reading> reading;
    if (lowest >= 0) {
      const bool held = weight[static_cast<std::size_t>(highest)] >= span.to - span.from;
      reading =
          read_set(pcs, pc(lowest), held ? std::optional<std::uint8_t>{pc(highest)} : std::nullopt,
                   previous_root);
    }
    if (reading)
      previous_root = reading->chord.root;
    out.push_back(reading);
  }
  return out;
}

struct Run {
  std::int64_t from = 0;
  std::int64_t to = 0;
  std::optional<Reading> reading;
  // A lone bass that is not the chord's bass: it may lead into the next chord.
  std::optional<std::uint8_t> lone_bass;
};

std::uint8_t bass_of(const Reading& r) noexcept { return r.chord.bass.value_or(r.chord.root); }

void merge_equal(std::vector<Run>& runs) {
  std::vector<Run> out;
  out.reserve(runs.size());
  for (auto& r : runs) {
    if (!out.empty() && out.back().reading == r.reading)
      out.back().to = r.to;
    else
      out.push_back(std::move(r));
  }
  runs = std::move(out);
}

// Readings per grid step, as runs: each change of the sounding set read once.
std::vector<Run> step_runs(std::span<const Sounding> notes, std::int64_t steps) {
  struct Edge {
    std::int64_t step;
    std::uint8_t key;
    bool on;
  };
  std::vector<Edge> edges;
  edges.reserve(notes.size() * 2);
  for (const auto& n : notes) {
    edges.push_back({n.from, n.key, true});
    edges.push_back({n.to, n.key, false});
  }
  std::sort(edges.begin(), edges.end(),
            [](const Edge& x, const Edge& y) { return x.step < y.step; });
  std::array<std::uint32_t, midi_keys> sounding{};
  std::array<std::int64_t, midi_keys> struck_at{};
  struck_at.fill(-1);
  std::vector<Run> runs;
  std::optional<Reading> previous;
  std::size_t e = 0;
  std::int64_t at = 0;
  while (at < steps) {
    for (; e < edges.size() && edges[e].step <= at; ++e) {
      auto& c = sounding[edges[e].key];
      c = edges[e].on ? c + 1 : c - 1;
      if (edges[e].on)
        struck_at[edges[e].key] = at;
    }
    const auto until = e < edges.size() ? std::min(edges[e].step, steps) : steps;
    std::uint16_t pcs = 0;
    int lowest = -1;
    int highest = -1;
    for (std::size_t key = 0; key < midi_keys; ++key) {
      if (sounding[key] > 0) {
        pcs = static_cast<std::uint16_t>(pcs | bit(pc(static_cast<int>(key))));
        if (lowest < 0)
          lowest = static_cast<int>(key);
        highest = static_cast<int>(key);
      }
    }
    std::optional<Reading> reading;
    const int distinct = count(pcs);
    const auto previous_root =
        previous ? std::optional<std::uint8_t>{previous->chord.root} : std::nullopt;
    const auto read = [&] { return read_set(pcs, pc(lowest), pc(highest), previous_root); };
    // A lone bass struck here, or other than the chord's bass, may lead in.
    const bool lone = distinct == 1 && (!previous || bass_of(*previous) != pc(lowest) ||
                                        struck_at[static_cast<std::size_t>(lowest)] == at);
    if (distinct >= min_step_pitch_classes) {
      reading = read();
    } else if (previous && !lone) {
      auto fits = pitch_class_set(previous->chord);
      if (previous->top)
        fits = static_cast<std::uint16_t>(fits | bit(*previous->top));
      if ((pcs & ~fits) == 0)
        reading = previous;
      else if (distinct >= min_pitch_classes)
        reading = read();
    } else if (distinct >= min_pitch_classes) {
      reading = read();
    }
    std::optional<std::uint8_t> lone_bass;
    if (!reading) {
      reading = previous;
      if (lone)
        lone_bass = pc(lowest);
    }
    if (!runs.empty() && runs.back().reading == reading && runs.back().lone_bass == lone_bass)
      runs.back().to = until;
    else
      runs.push_back({at, until, reading, lone_bass});
    if (reading)
      previous = reading;
    at = until;
  }
  // A bass played ahead of its chord (on the beat, the chord on the off-beat)
  // belongs to the chord it is the bass of; otherwise it inherits.
  std::optional<Reading> next;
  for (auto r = runs.rbegin(); r != runs.rend(); ++r) {
    if (r->lone_bass && next && bass_of(*next) == *r->lone_bass)
      r->reading = next;
    r->lone_bass.reset();
    if (r->reading)
      next = r->reading;
  }
  return runs;
}

// Runs shorter than min_region_steps join the run before (the first joins the
// run after).
void absorb_short(std::vector<Run>& runs) {
  std::vector<Run> kept;
  kept.reserve(runs.size());
  for (std::size_t i = runs.size(); i-- > 0;) {
    if (i > 0 && runs[i].to - runs[i].from < min_region_steps)
      runs[i - 1].to = runs[i].to;
    else
      kept.push_back(runs[i]);
  }
  std::reverse(kept.begin(), kept.end());
  if (kept.size() > 1 && kept.front().to - kept.front().from < min_region_steps) {
    kept[1].from = kept.front().from;
    kept.erase(kept.begin());
  }
  runs = std::move(kept);
}

std::vector<StepSpan> spans_of(std::span<const Run> runs) {
  std::vector<StepSpan> out;
  out.reserve(runs.size());
  for (const auto& r : runs)
    out.push_back({r.from, r.to});
  return out;
}

// ---- voicing ----
bool clean(const Voicing& v, bool flat_ninth_allowed) noexcept {
  for (std::uint8_t i = 1; i < v.count; ++i) {
    const int low = v.notes[i - 1];
    const int gap = v.notes[i] - low;
    if (gap == semitone || (low < fifth_floor && gap < perfect_fifth) ||
        (low < third_floor && gap < minor_third))
      return false;
  }
  if (!flat_ninth_allowed)
    for (std::uint8_t i = 0; i < v.count; ++i)
      for (std::uint8_t j = i + 1; j < v.count; ++j)
        if ((v.notes[j] - v.notes[i]) % octave == semitone &&
            v.notes[j] - v.notes[i] >= minor_ninth)
          return false;
  return true;
}

// The options with the chord kept above the bass's octave: below `lowest`
// is the bass's role (the left hand), from it up the chord's.
VoicingOptions clip_options(const VoicingOptions& options) noexcept {
  auto o = options;
  o.lowest = std::max<std::uint8_t>(o.lowest, static_cast<std::uint8_t>(o.bass_lowest + octave));
  return o;
}

// voice(), plus the root of a slash chord whose voicing leaves it to the bass
// (without it, Am9/G would read as Cmaj7/G): placed where it sounds clean,
// below the top voice when it can be, nearest the voicing's middle.
Voicing rooted_voicing(const ChordRegion& region, const VoicingOptions& o,
                       const Voicing* previous) {
  auto v = voice(region.chord, region.top, o, previous);
  const auto& chord = region.chord;
  if (!chord.bass || pc(*chord.bass) == chord.root || v.count == 0 || v.count >= max_voices)
    return v;
  for (std::uint8_t i = 0; i < v.count; ++i)
    if (pc(v.notes[i]) == chord.root)
      return v;
  int sum = 0;
  for (std::uint8_t i = 0; i < v.count; ++i)
    sum += v.notes[i];
  const int middle = sum / v.count;
  const int top = v.notes[v.count - 1];
  const bool flat_ninth = chord.quality == ChordQuality::dominant_seventh_flat_ninth;
  std::optional<Voicing> best;
  int best_cost = std::numeric_limits<int>::max();
  for (int m = o.lowest; m <= o.highest; ++m) {
    if (pc(m) != chord.root)
      continue;
    Voicing w = v;
    std::uint8_t at = w.count;
    while (at > 0 && w.notes[at - 1] > m) {
      w.notes[at] = w.notes[at - 1];
      --at;
    }
    w.notes[at] = static_cast<std::uint8_t>(m);
    ++w.count;
    if (!clean(w, flat_ninth))
      continue;
    const int cost = std::abs(m - middle) + (m > top ? root_on_top_cost : 0);
    if (cost < best_cost) {
      best_cost = cost;
      best = w;
    }
  }
  return best ? *best : v;
}

project::MidiNote made(std::int64_t start, std::int64_t end, std::uint8_t key,
                       std::uint8_t velocity) {
  return {unassigned_note_id(), {start}, {end}, key, velocity};
}

void sort_and_dedupe(std::vector<project::MidiNote>& notes) {
  std::stable_sort(notes.begin(), notes.end(), [](const auto& x, const auto& y) {
    return x.start != y.start ? x.start < y.start : x.key < y.key;
  });
  notes.erase(std::unique(notes.begin(), notes.end(),
                          [](const auto& x, const auto& y) {
                            return x.start == y.start && x.key == y.key;
                          }),
              notes.end());
}

// The bass `pitch_class` in the octave nearest `old`, kept in the bass role.
std::uint8_t bass_near(std::uint8_t old, std::uint8_t pitch_class, std::uint8_t split) noexcept {
  const int below = old - pc(static_cast<int>(old) - pitch_class);
  int key = old - below <= below + octave - old ? below : below + octave;
  while (key >= split)
    key -= octave;
  while (key < 0)
    key += octave;
  return static_cast<std::uint8_t>(key);
}

// The upper voices of a region's fullest onset group (the earliest on a tie).
std::vector<std::uint8_t> fullest_group(std::span<const project::MidiNote> notes,
                                        std::uint8_t split) {
  std::vector<std::uint8_t> best;
  std::size_t i = 0;
  while (i < notes.size()) {
    std::vector<std::uint8_t> group;
    std::size_t j = i;
    for (; j < notes.size() && notes[j].start == notes[i].start; ++j)
      if (notes[j].key >= split && (group.empty() || group.back() != notes[j].key))
        group.push_back(notes[j].key);
    if (group.size() > best.size())
      best = std::move(group);
    i = j;
  }
  return best;
}

// Without the bass, the chord's bass pitch class goes under the voicing:
// the nearest below its lowest note when that stays out of the bass's
// register, otherwise from `lowest` with the notes under it an octave up.
Voicing without_bass(Voicing v, const ChordSymbol& chord, const VoicingOptions& o) {
  if (o.bass || v.count == 0)
    return v;
  const auto wanted = pc(chord.bass.value_or(chord.root));
  if (pc(v.notes[0]) == wanted)
    return v;
  const auto below = static_cast<int>(v.notes[0]) - pc(v.notes[0] - static_cast<int>(wanted));
  const auto low =
      below >= o.lowest ? static_cast<std::uint8_t>(below) : bass_note(chord, o.lowest);
  auto count = std::min<std::size_t>(v.count, max_voices);
  for (std::size_t i = 0; i < count; ++i)
    while (v.notes[i] < low)
      v.notes[i] = static_cast<std::uint8_t>(v.notes[i] + octave);
  // A full voicing gives up its lowest note for it.
  std::sort(v.notes.begin(), v.notes.begin() + static_cast<std::ptrdiff_t>(count));
  if (count < max_voices)
    v.notes[count++] = low;
  else
    v.notes[0] = low;
  const auto used = v.notes.begin() + static_cast<std::ptrdiff_t>(count);
  std::sort(v.notes.begin(), used);
  v.count = static_cast<std::uint8_t>(std::unique(v.notes.begin(), used) - v.notes.begin());
  return v;
}

// True when `bass` sounds cleanly under the voicing: no semitone to the
// voice above it, wide enough low down, and no minor ninth against any voice
// (unless the chord is a 7♭9).
bool sits_under(int bass, const Voicing& v, bool flat_ninth_allowed) noexcept {
  const int gap = v.notes[0] - bass;
  if (gap <= 0 || gap == semitone || (bass < fifth_floor && gap < perfect_fifth) ||
      (bass < third_floor && gap < minor_third))
    return false;
  if (!flat_ninth_allowed)
    for (std::uint8_t i = 0; i < v.count; ++i)
      if ((v.notes[i] - bass) % octave == semitone && v.notes[i] - bass >= minor_ninth)
        return false;
  return true;
}

// Without the bass, the chord's own bass is its lowest note. The voicing is
// chosen with room for it: voiced from a floor raised a semitone at a time
// (up to an octave) until the bass pitch class fits just under it, in range
// and clean, so the voicing keeps the shape it has over a bass rather than
// folding its low notes up into a cluster.
Voicing chord_voicing(const ChordRegion& region, const VoicingOptions& o, const Voicing* previous) {
  if (o.bass)
    return rooted_voicing(region, o, previous);
  // Open keeps the voicing it has over the bass, the bass's octave its
  // lowest voice.
  if (o.style == VoicingStyle::open) {
    auto over = o;
    over.bass = true;
    over.bass_octave_lowest = true;
    const auto v = rooted_voicing(region, over, previous);
    const int bass = bass_note(region.chord, o.bass_lowest);
    if (v.count > 0 && v.notes[0] == bass + octave)
      return v;
  }
  const auto& chord = region.chord;
  const int wanted = pc(chord.bass.value_or(chord.root));
  const bool flat_ninth = chord.quality == ChordQuality::dominant_seventh_flat_ninth;
  for (int raise = 0; raise < octave && o.lowest + raise <= o.highest; ++raise) {
    auto lifted = o;
    lifted.lowest = static_cast<std::uint8_t>(o.lowest + raise);
    auto v = rooted_voicing(region, lifted, previous);
    if (v.count == 0)
      continue;
    if (pc(v.notes[0]) == wanted)
      return v;
    const int bass = v.notes[0] - pc(v.notes[0] - wanted);
    if (v.count >= max_voices || bass < o.lowest || !sits_under(bass, v, flat_ninth))
      continue;
    const auto used = v.notes.begin() + v.count;
    std::copy_backward(v.notes.begin(), used, used + 1);
    v.notes[0] = static_cast<std::uint8_t>(bass);
    ++v.count;
    return v;
  }
  return without_bass(rooted_voicing(region, o, previous), chord, o);
}

Voicing as_voicing(std::span<const std::uint8_t> keys) noexcept {
  Voicing v;
  const auto skip = keys.size() > max_voices ? keys.size() - max_voices : 0;
  for (std::size_t i = skip; i < keys.size(); ++i)
    v.notes[v.count++] = keys[i];
  return v;
}

void append_held(std::vector<project::MidiNote>& out, const ChordRegion& region, const Voicing& v,
                 bool with_bass, std::uint8_t velocity, const VoicingOptions& o) {
  const auto a = region.position.value;
  const auto b = detail::end_of(region);
  if (with_bass && o.bass)
    out.push_back(made(a, b, bass_note(region.chord, o.bass_lowest), velocity));
  for (std::uint8_t i = 0; i < v.count; ++i)
    out.push_back(made(a, b, v.notes[i], velocity));
}

// Re-voices a region's notes (sorted by start then key, all starting inside
// it) onto `v` in their rhythm.
void revoice_notes(std::vector<project::MidiNote>& out, std::vector<project::MidiNote> notes,
                   const ChordRegion& region, const Voicing& v, const VoicingOptions& o) {
  if (!o.bass)
    std::erase_if(notes, [&](const project::MidiNote& n) { return n.key < o.lowest; });
  if (notes.empty()) {
    append_held(out, region, v, true, written_velocity, o);
    return;
  }
  const auto& chord = region.chord;
  const auto bass_pc = pc(chord.bass.value_or(chord.root));
  const auto upper = fullest_group(notes, o.lowest);
  bool has_bass = false;
  for (auto& n : notes) {
    if (n.key < o.lowest) {
      n.key = bass_near(n.key, bass_pc, o.lowest);
      has_bass = true;
    }
  }
  if (upper.empty()) {
    append_held(out, region, v, false, notes.front().velocity, o);
  } else {
    const auto voices = static_cast<std::size_t>(v.count);
    // Rank j of the fullest group maps to voice f(j): top to top, bottom to bottom.
    const auto target = [&](std::size_t j) -> std::size_t {
      if (upper.size() == 1)
        return voices - 1;
      const auto span = upper.size() - 1;
      return (2 * j * (voices - 1) + span) / (2 * span);
    };
    const auto rank = [&](std::uint8_t key) {
      std::size_t best = 0;
      for (std::size_t r = 1; r < upper.size(); ++r)
        if (std::abs(upper[r] - key) < std::abs(upper[best] - key))
          best = r;
      return best;
    };
    std::vector<project::MidiNote> joined;
    std::size_t i = 0;
    while (i < notes.size()) {
      std::size_t j = i;
      while (j < notes.size() && notes[j].start == notes[i].start)
        ++j;
      // The group's upper notes by rank; a note sharing a rank doubles it.
      std::vector<std::optional<std::size_t>> by_rank(upper.size());
      for (std::size_t k = i; k < j; ++k) {
        auto& n = notes[k];
        if (n.key < o.lowest)
          continue;
        const auto r = rank(n.key);
        if (!by_rank[r])
          by_rank[r] = k;
        n.key = v.notes[target(r)];
      }
      for (std::size_t r = 0; r + 1 < upper.size(); ++r) {
        if (!by_rank[r] || !by_rank[r + 1])
          continue;
        const auto& from = notes[*by_rank[r]];
        for (auto x = target(r) + 1; x < target(r + 1); ++x)
          joined.push_back({unassigned_note_id(), from.start, from.end, v.notes[x], from.velocity});
      }
      i = j;
    }
    out.insert(out.end(), joined.begin(), joined.end());
  }
  // With the bass on, a region played without one gets it held.
  if (o.bass && !has_bass) {
    out.push_back(made(region.position.value, detail::end_of(region),
                       bass_note(chord, o.bass_lowest), notes.front().velocity));
  }
  out.insert(out.end(), notes.begin(), notes.end());
}

} // namespace

project::NoteId unassigned_note_id() {
  constexpr auto all_ones = std::numeric_limits<std::uint64_t>::max();
  return project::NoteId::from_bits({all_ones, all_ones}).value();
}

std::optional<ChordSymbol> identify(std::span<const std::uint8_t> keys) {
  std::uint16_t pcs = 0;
  int lowest = std::numeric_limits<int>::max();
  for (const auto key : keys) {
    pcs = static_cast<std::uint16_t>(pcs | bit(pc(key)));
    lowest = std::min<int>(lowest, key);
  }
  if (count(pcs) < min_pitch_classes)
    return std::nullopt;
  return best_match(pcs, pc(lowest), std::nullopt).chord;
}

std::vector<ChordRegion> recognise(std::span<const project::MidiNote> notes,
                                   timeline::TickPosition length) {
  if (length.value <= 0)
    return {};
  const auto steps = steps_in(length.value);
  const auto sounding = quantise(notes, steps);
  auto runs = step_runs(sounding, steps);
  const auto first =
      std::find_if(runs.begin(), runs.end(), [](const Run& r) { return r.reading.has_value(); });
  if (first == runs.end())
    return {};
  const auto lead = first->reading;
  for (auto r = runs.begin(); r != first; ++r)
    r->reading = lead;
  merge_equal(runs);
  absorb_short(runs);
  merge_equal(runs);
  // Re-read each region from all its notes until no neighbours agree.
  for (;;) {
    const auto spans = spans_of(runs);
    const auto readings = read_spans(sounding, spans);
    for (std::size_t i = 0; i < runs.size(); ++i)
      if (readings[i])
        runs[i].reading = readings[i];
    const auto before = runs.size();
    merge_equal(runs);
    if (runs.size() == before)
      break;
  }
  std::vector<ChordRegion> out;
  out.reserve(runs.size());
  for (const auto& r : runs) {
    const auto start = r.from * step_ticks;
    const auto end = std::min(r.to * step_ticks, length.value);
    out.push_back({{start}, end - start, r.reading->chord, r.reading->top, ChordDevice::none});
  }
  return out;
}

std::vector<project::MidiNote> rewrite(std::span<const project::MidiNote> notes,
                                       std::span<const ChordRegion> lane,
                                       timeline::TickPosition length, const VoicingOptions& options,
                                       std::optional<std::size_t> revoice) {
  const auto o = clip_options(options);
  std::vector<project::MidiNote> input;
  input.reserve(notes.size());
  for (auto n : notes) {
    n.start.value = std::max<std::int64_t>(n.start.value, 0);
    n.end.value = std::min(n.end.value, length.value);
    if (n.start.value < length.value && n.end.value > n.start.value)
      input.push_back(n);
  }
  sort_and_dedupe(input);
  std::vector<ChordRegion> regions;
  std::vector<std::size_t> lane_index;
  for (std::size_t i = 0; i < lane.size(); ++i) {
    auto r = lane[i];
    const auto start = std::max<std::int64_t>(r.position.value, 0);
    const auto end = std::min(detail::end_of(r), length.value);
    if (end <= start)
      continue;
    r.position.value = start;
    r.length_ticks = end - start;
    regions.push_back(r);
    lane_index.push_back(i);
  }
  if (regions.empty())
    return input;

  // What each region reads as now, silence inheriting the chord before.
  const auto steps = steps_in(length.value);
  const auto sounding = quantise(input, steps);
  std::vector<StepSpan> spans;
  spans.reserve(regions.size());
  for (const auto& r : regions) {
    const auto from = to_step(r.position.value);
    const auto to = detail::end_of(r) >= length.value ? steps : to_step(detail::end_of(r));
    spans.push_back({from, std::max(from, to)});
  }
  const auto readings = read_spans(sounding, spans);
  std::vector<std::optional<Reading>> current(readings.size());
  const auto first =
      std::find_if(readings.begin(), readings.end(), [](const auto& r) { return r.has_value(); });
  std::optional<Reading> inherited = first != readings.end() ? *first : std::nullopt;
  // Bass-role notes by start (input is sorted by start), with the latest end
  // so far: a region overlaps a bass note when one starting before its end
  // ends after its start. One pass here, a binary search per region.
  std::vector<std::int64_t> bass_starts;
  std::vector<std::int64_t> bass_latest_end;
  for (const auto& n : input) {
    if (n.key >= o.lowest)
      continue;
    bass_starts.push_back(n.start.value);
    bass_latest_end.push_back(
        std::max(n.end.value, bass_latest_end.empty() ? n.end.value : bass_latest_end.back()));
  }
  std::vector<bool> rewritten(regions.size());
  std::vector<std::int64_t> cuts;
  for (std::size_t i = 0; i < regions.size(); ++i) {
    if (readings[i])
      inherited = readings[i];
    const auto& r = regions[i];
    const bool same =
        inherited && inherited->chord == r.chord && inherited->top == drone_of(r.chord, r.top);
    // A region whose bass the switch turned on or off is rewritten too.
    const auto before_end = static_cast<std::size_t>(
        std::lower_bound(bass_starts.begin(), bass_starts.end(), detail::end_of(r)) -
        bass_starts.begin());
    const bool has_bass = before_end != 0 && bass_latest_end[before_end - 1] > r.position.value;
    rewritten[i] = !same || (revoice && *revoice == lane_index[i]) || has_bass != o.bass;
    if (rewritten[i]) {
      cuts.push_back(r.position.value);
      cuts.push_back(detail::end_of(r));
    }
  }
  std::sort(cuts.begin(), cuts.end());

  // Cut notes held across a rewritten region's edges; later pieces are new notes.
  std::vector<project::MidiNote> pieces;
  pieces.reserve(input.size());
  for (const auto& n : input) {
    auto rest = n;
    for (auto c = std::upper_bound(cuts.begin(), cuts.end(), rest.start.value);
         c != cuts.end() && *c < rest.end.value; ++c) {
      auto head = rest;
      head.end.value = *c;
      pieces.push_back(head);
      rest.start.value = *c;
      rest.id = unassigned_note_id();
    }
    pieces.push_back(rest);
  }
  sort_and_dedupe(pieces);

  std::vector<std::vector<project::MidiNote>> inside(regions.size());
  std::vector<project::MidiNote> out;
  out.reserve(pieces.size());
  for (const auto& n : pieces) {
    const auto after =
        std::upper_bound(regions.begin(), regions.end(), n.start.value,
                         [](std::int64_t t, const ChordRegion& r) { return t < r.position.value; });
    if (after != regions.begin()) {
      const auto i = static_cast<std::size_t>(after - regions.begin()) - 1;
      if (n.start.value < detail::end_of(regions[i])) {
        inside[i].push_back(n);
        continue;
      }
    }
    out.push_back(n);
  }

  std::optional<Voicing> previous;
  for (std::size_t i = 0; i < regions.size(); ++i) {
    if (!rewritten[i]) {
      const auto upper = fullest_group(inside[i], o.lowest);
      if (!upper.empty())
        previous = as_voicing(upper);
      out.insert(out.end(), inside[i].begin(), inside[i].end());
      continue;
    }
    // A region re-voiced on request is voiced afresh in the options' style;
    // a changed chord is led from the voicing before.
    const bool asked = revoice && *revoice == lane_index[i];
    const auto v = chord_voicing(regions[i], o, previous && !asked ? &*previous : nullptr);
    revoice_notes(out, std::move(inside[i]), regions[i], v, o);
    previous = v;
  }
  sort_and_dedupe(out);
  return out;
}

std::int64_t anticipation_sixteenths(Anticipation anticipation, std::size_t change) noexcept {
  switch (anticipation) {
  case Anticipation::none:
    return 0;
  case Anticipation::sixteenth:
    return 1;
  case Anticipation::eighth:
    return 2;
  case Anticipation::dotted_eighth:
    return 3;
  case Anticipation::varied:
    return varied_anticipation[change % varied_anticipation.size()];
  }
  return 0;
}

std::vector<ChordRegion> anticipate(std::span<const ChordRegion> lane, Anticipation anticipation) {
  std::vector<ChordRegion> out(lane.begin(), lane.end());
  for (std::size_t i = 1; i < out.size(); ++i) {
    // Never so early that the chord before loses its whole length.
    const auto early = std::min(anticipation_sixteenths(anticipation, i) * step_ticks,
                                out[i - 1].length_ticks - step_ticks);
    if (early <= 0)
      continue;
    out[i - 1].length_ticks -= early;
    out[i].position.value -= early;
    out[i].length_ticks += early;
  }
  return out;
}

std::vector<project::MidiNote> push_and_land(std::span<const ChordRegion> lane,
                                             const VoicingOptions& options,
                                             Anticipation anticipation, std::uint8_t velocity) {
  auto notes = write(lane, options, velocity);
  std::vector<project::MidiNote> touches;
  for (std::size_t i = 1; i < lane.size(); ++i) {
    const auto change = lane[i].position.value;
    // A touch needs room for itself and a breath before the landing.
    const auto lead = std::max(anticipation == Anticipation::none
                                   ? anticipation_sixteenths(Anticipation::dotted_eighth, i)
                                   : anticipation_sixteenths(anticipation, i),
                               touch_sixteenths) *
                      step_ticks;
    const auto at = change - std::min(lead, lane[i - 1].length_ticks - step_ticks);
    // No room (a chord before shorter than a 16th puts `at` past the change).
    if (at <= lane[i - 1].position.value || at >= change)
      continue;
    // The chord before stops for the touch.
    for (auto& n : notes)
      if (n.start.value < at && n.end.value > at)
        n.end.value = at;
    const auto until = std::min(at + touch_sixteenths * step_ticks, change);
    for (const auto& n : notes)
      if (n.start.value == change)
        touches.push_back(made(at, until, n.key, touch_velocity));
  }
  notes.insert(notes.end(), touches.begin(), touches.end());
  sort_and_dedupe(notes);
  return notes;
}

std::vector<project::MidiNote> pad_layer(std::span<const ChordRegion> lane, std::uint8_t velocity) {
  auto o = open_voicing();
  o.lowest = static_cast<std::uint8_t>(o.lowest + pad_octave_up);
  o.highest = static_cast<std::uint8_t>(o.highest + pad_octave_up);
  o.bass_lowest = static_cast<std::uint8_t>(o.lowest - octave);
  auto notes = write(lane, o, velocity);
  std::erase_if(notes, [&](const project::MidiNote& n) { return n.key < o.lowest; });
  return notes;
}

std::vector<project::MidiNote> pluck_layer(std::span<const ChordRegion> lane,
                                           std::uint8_t velocity) {
  VoicingOptions o{.style = VoicingStyle::close,
                   .lowest = pluck_lowest,
                   .highest = pluck_highest,
                   .bass_lowest = static_cast<std::uint8_t>(pluck_lowest - octave)};
  auto notes = write(lane, o, velocity);
  std::erase_if(notes, [&](const project::MidiNote& n) { return n.key < o.lowest; });
  return notes;
}

std::vector<project::MidiNote> running_tonic(timeline::TickPosition length, std::uint8_t tonic,
                                             Pulse pulse, std::uint8_t lowest) {
  std::vector<project::MidiNote> notes;
  const auto every = pulse_sixteenths[static_cast<std::size_t>(pulse)] * step_ticks;
  // Half the pulse long: it pulses rather than drones.
  const auto gate = std::max<std::int64_t>(every / 2, step_ticks / 2);
  const auto key = static_cast<std::uint8_t>(lowest + pc(static_cast<int>(tonic) - lowest));
  constexpr std::int64_t beat = 4 * recognition_step_ticks;
  for (std::int64_t at = 0; at < length.value; at += every) {
    const bool on_beat = at % beat == 0;
    notes.push_back(made(at, std::min(at + gate, length.value), key,
                         on_beat ? accent_velocity : pulse_velocity));
  }
  return notes;
}

std::vector<project::MidiNote> bass_line(std::span<const ChordRegion> lane, BassStyle style,
                                         std::uint8_t lowest, std::uint8_t velocity) {
  std::vector<project::MidiNote> notes;
  constexpr std::int64_t steps_per_beat = 4;
  for (const auto& region : lane) {
    const auto key = bass_note(region.chord, lowest);
    const auto a = region.position.value;
    const auto b = detail::end_of(region);
    if (style == BassStyle::held) {
      notes.push_back(made(a, b, key, velocity));
      continue;
    }
    // The steps of the region on the song's 16th grid.
    for (auto step = (a + step_ticks - 1) / step_ticks; step * step_ticks < b; ++step) {
      const auto in_beat = step % steps_per_beat;
      const bool hit = style == BassStyle::offbeat ? in_beat == 2 : in_beat != 0;
      if (!hit)
        continue;
      const auto at = step * step_ticks;
      const auto span = style == BassStyle::offbeat ? 2 * step_ticks : step_ticks;
      notes.push_back(made(at, std::min(at + span, b), key, velocity));
    }
  }
  return notes;
}

std::vector<project::MidiNote> write(std::span<const ChordRegion> lane,
                                     const VoicingOptions& options, std::uint8_t velocity) {
  const auto o = clip_options(options);
  std::vector<project::MidiNote> out;
  out.reserve(lane.size() * (max_voices + 1));
  std::optional<Voicing> previous;
  for (const auto& region : lane) {
    if (region.length_ticks <= 0)
      continue;
    const auto v = chord_voicing(region, o, previous ? &*previous : nullptr);
    append_held(out, region, v, true, velocity, o);
    previous = v;
  }
  sort_and_dedupe(out);
  return out;
}

} // namespace oscillate::harmony
