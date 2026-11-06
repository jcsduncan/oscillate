#include "detail.hpp"

#include <algorithm>

namespace oscillate::harmony {
namespace {

using detail::bit;
using detail::end_of;
using detail::from_tonic;
using detail::pc;

struct PaletteEntry {
  std::uint8_t from_tonic;
  ChordQuality quality;
};
using enum ChordQuality;
// The chords "what comes next" weighs, by semitones from the tonic: the
// key's own, its dominants, and the usual borrowed and chromatic colours.
constexpr std::array<PaletteEntry, 16> minor_palette{{
    {0, minor_ninth},
    {2, half_diminished},
    {3, major_ninth},
    {5, minor_ninth},
    {7, minor_seventh},
    {8, major_ninth},
    {10, sixth},
    {7, seventh_sus4},
    {7, dominant_seventh_flat_ninth},
    {5, dominant_ninth},
    {1, major_seventh},
    {10, dominant_thirteenth},
    {9, half_diminished},
    {11, diminished_seventh},
    {8, minor_sixth},
    {3, add9},
}};
constexpr std::array<PaletteEntry, 16> major_palette{{
    {0, major_ninth},
    {2, minor_ninth},
    {4, minor_seventh},
    {5, major_ninth},
    {7, dominant_ninth},
    {9, minor_ninth},
    {11, half_diminished},
    {7, seventh_sus4},
    {9, dominant_seventh},
    {4, dominant_seventh},
    {10, major_seventh},
    {8, major_seventh},
    {5, minor_sixth},
    {2, dominant_seventh},
    {1, dominant_seventh},
    {0, add9},
}};
// How common each root motion is (semitones up from the current root); a
// falling fifth (up a fourth) is strongest.
constexpr std::array<double, detail::octave> motion_weight{0.2, 0.35, 0.65, 0.55, 0.45, 1.0,
                                                           0.3, 0.55, 0.7,  0.75, 0.7,  0.45};
constexpr double outside_penalty = 0.18;
constexpr double shared_bonus = 0.08;
constexpr double dominant_resolves = 1.7;
// A diminished seventh's pull up a semitone outweighs any other motion.
constexpr double diminished_rises = 2.4;
constexpr double suspension_resolves = 2.4;
constexpr int semitone = 1;
constexpr int tritone = 6;
constexpr int perfect_fifth = 7;
constexpr std::uint8_t third_index = 1;
constexpr std::uint8_t fifth_index = 2;

// Edits cut regions on the beat; a region gives up time only when this long.
constexpr std::int64_t beat = timeline::ticks_per_quarter;
constexpr std::int64_t min_split = 2 * beat;
constexpr std::int64_t lead_in_max = 2 * beat;
// A passing chord is suggested after a root jump of at least this.
constexpr int passing_jump = 3;
// An inversion is suggested when the bass could step down this far or less.
constexpr int inversion_step = 2;
// Inversions are suggested only in lanes with more chords than this.
constexpr std::size_t inversion_min_lane = 2;

bool contiguous_before(std::span<const ChordRegion> lane, std::size_t index) noexcept {
  return index > 0 && end_of(lane[index - 1]) == lane[index].position.value;
}

ChordSymbol without_bass(ChordSymbol c) noexcept {
  c.bass.reset();
  return c;
}

std::vector<ChordRegion> copy_of(std::span<const ChordRegion> lane) {
  return {lane.begin(), lane.end()};
}

// Inserts `chord` for `length` ticks before region `index`, taken from the
// end of the region before it.
std::vector<ChordRegion> insert_before(std::span<const ChordRegion> lane, std::size_t index,
                                       ChordSymbol chord, std::int64_t length, ChordDevice device) {
  auto out = copy_of(lane);
  out[index - 1].length_ticks -= length;
  ChordRegion added{{lane[index].position.value - length}, length, chord, lane[index].top, device};
  out.insert(out.begin() + static_cast<std::ptrdiff_t>(index), added);
  return out;
}

LaneEdit edit(EditKind kind, std::string title, std::string preview, std::vector<ChordRegion> lane,
              std::size_t selected) {
  return {kind, std::move(title), std::move(preview), {}, std::move(lane), selected, false};
}

} // namespace

std::vector<NextCandidate> next_candidates(const ChordSymbol& current, const MusicalKey& key) {
  const auto& palette = key.mode == project::KeyMode::minor ? minor_palette : major_palette;
  const auto in_key = detail::key_set(key);
  const auto current_set = pitch_class_set(without_bass(current));
  const auto current_degree = from_tonic(current.root, key);
  std::vector<NextCandidate> out;
  for (const auto& entry : palette) {
    const ChordSymbol c{pc(key.tonic + entry.from_tonic), entry.quality, std::nullopt};
    if (c.root == current.root && c.quality == current.quality) {
      continue;
    }
    const auto set = pitch_class_set(c);
    const auto motion = pc(static_cast<int>(c.root) - static_cast<int>(current.root));
    const int shared = detail::count_bits(static_cast<std::uint16_t>(set & current_set));
    const int outside = detail::count_bits(static_cast<std::uint16_t>(set & ~in_key));
    double w =
        motion_weight[motion] * (1.0 - outside_penalty * outside) * (1.0 + shared_bonus * shared);
    if (is_dominant(current.quality) && current.quality != seventh_sus4 &&
        current_degree == detail::dominant_degree && entry.from_tonic == 0) {
      w *= dominant_resolves;
    }
    if (current.quality == diminished_seventh && motion == semitone) {
      w *= diminished_rises;
    }
    if (current.quality == seventh_sus4 && motion == 0 &&
        (c.quality == dominant_seventh || c.quality == dominant_seventh_flat_ninth)) {
      w *= suspension_resolves;
    }
    out.push_back({c, w, tension(c, key), shared});
  }
  std::ranges::stable_sort(out, std::greater<>{}, &NextCandidate::likelihood);
  if (out.size() > next_candidate_limit) {
    out.resize(next_candidate_limit);
  }
  if (!out.empty() && out.front().likelihood > 0.0) {
    const auto best = out.front().likelihood;
    for (auto& c : out) {
      c.likelihood /= best;
    }
  }
  return out;
}

std::vector<LaneEdit> substitutions(std::span<const ChordRegion> lane, std::size_t index,
                                    const MusicalKey& key) {
  std::vector<LaneEdit> out;
  if (index >= lane.size()) {
    return out;
  }
  const auto& r = lane[index];
  const auto with = [&](ChordSymbol chord) {
    auto l = copy_of(lane);
    l[index].chord = chord;
    return l;
  };
  ChordQuality q{};
  if (detail::richer(r.chord.quality, q)) {
    auto c = r.chord;
    c.quality = q;
    out.push_back(edit(EditKind::richer, "Richer", name(c, key), with(c), index));
  }
  if (detail::simpler(r.chord.quality, q)) {
    auto c = r.chord;
    c.quality = q;
    out.push_back(edit(EditKind::simpler, "Simpler", name(c, key), with(c), index));
  }
  if (detail::flipped(r.chord.quality, q)) {
    auto c = r.chord;
    c.quality = q;
    out.push_back(edit(EditKind::borrowed, "Borrowed", name(c, key), with(c), index));
  }
  if (const auto sus = detail::suspended(r.chord.quality); sus != r.chord.quality) {
    auto c = r.chord;
    c.quality = sus;
    out.push_back(edit(EditKind::sus, "Sus", name(c, key), with(c), index));
  }
  if (is_dominant(r.chord.quality)) {
    const ChordSymbol c{pc(r.chord.root + tritone), dominant_seventh, r.chord.bass};
    out.push_back(edit(EditKind::tritone, "Tritone sub", name(c, key), with(c), index));
  }
  if (contiguous_before(lane, index) && lane[index - 1].length_ticks >= min_split) {
    const auto length = std::min(lead_in_max, lane[index - 1].length_ticks / 2);
    const ChordSymbol v{pc(r.chord.root + perfect_fifth), dominant_seventh, std::nullopt};
    out.push_back(edit(EditKind::lead_in_dominant, "Lead-in dominant",
                       name(v, key) + " → " + name(r.chord, key),
                       insert_before(lane, index, v, length, ChordDevice::pull_home), index + 1));
    const ChordSymbol dim{pc(r.chord.root - semitone), diminished_seventh, std::nullopt};
    out.push_back(edit(EditKind::passing_chord, "Passing chord",
                       name(dim, key) + " → " + name(r.chord, key),
                       insert_before(lane, index, dim, beat, ChordDevice::passing), index + 1));
  }
  if (r.chord.bass) {
    out.push_back(edit(EditKind::release_pedal, "Release pedal", name(without_bass(r.chord), key),
                       with(without_bass(r.chord)), index));
  } else if (index > 0 && lane[index - 1].chord.root != r.chord.root) {
    auto c = r.chord;
    c.bass = lane[index - 1].chord.root;
    out.push_back(edit(EditKind::pedal_bass, "Pedal bass", name(c, key), with(c), index));
  }
  return out;
}

std::vector<LaneEdit> hints(std::span<const ChordRegion> lane, std::size_t index,
                            const MusicalKey& key) {
  std::vector<LaneEdit> out;
  if (index >= lane.size()) {
    return out;
  }
  const auto n = lane.size();
  const auto& r = lane[index];
  const auto& c = r.chord;
  const auto tonic = pc(key.tonic);
  const auto add = [&](EditKind kind, std::string title, std::string why,
                       std::vector<ChordRegion> l, std::size_t selected, bool already = false) {
    LaneEdit e = edit(kind, std::move(title), "", std::move(l), selected);
    e.preview = name(e.lane[selected].chord, key);
    e.why = std::move(why);
    e.already = already;
    out.push_back(std::move(e));
  };

  if (c.root != tonic) {
    auto l = copy_of(lane);
    l[index].chord.bass = tonic;
    l[index].device = ChordDevice::pedal;
    add(EditKind::pedal_bass, "Pedal tone",
        "Keep " + std::string(pitch_name(tonic, key)) + " in the bass under " +
            name(without_bass(c), key) + ": the chord moves, the floor doesn't.",
        std::move(l), index, c.bass && *c.bass == tonic);
  }

  if (!r.top && n > 1) {
    auto shared = pitch_class_set(without_bass(c));
    if (index > 0) {
      shared &= pitch_class_set(without_bass(lane[index - 1].chord));
    }
    if (index + 1 < n) {
      shared &= pitch_class_set(without_bass(lane[index + 1].chord));
    }
    // The highest chord tone they share sits best on top.
    const auto iv = intervals(c.quality);
    for (int i = iv.count - 1; i >= 0; --i) {
      const auto p = pc(c.root + iv.semitones[static_cast<std::size_t>(i)]);
      if ((shared & bit(p)) == 0) {
        continue;
      }
      auto l = copy_of(lane);
      for (auto& x : l) {
        if ((pitch_class_set(without_bass(x.chord)) & bit(p)) != 0) {
          x.top = p;
        }
      }
      add(EditKind::top_drone, "Top drone",
          std::string(pitch_name(p, key)) +
              " is in this chord and its neighbours. Hold it on top to tie them together.",
          std::move(l), index);
      break;
    }
  }

  if (contiguous_before(lane, index) && lane[index - 1].length_ticks >= min_split &&
      detail::step_between(c.root, lane[index - 1].chord.root) >= passing_jump) {
    const ChordSymbol dim{pc(c.root - semitone), diminished_seventh, std::nullopt};
    add(EditKind::passing_chord, "Passing chord",
        name(lane[index - 1].chord, key) + " → " + name(c, key) + " is a jump. A " +
            name(dim, key) + " on the beat before smooths it.",
        insert_before(lane, index, dim, beat, ChordDevice::passing), index);
  }

  if (r.length_ticks >= min_split && c.quality != seventh_sus4 && c.quality != sus2 &&
      (from_tonic(c.root, key) == detail::dominant_degree || index + 1 == n)) {
    auto l = copy_of(lane);
    const auto half = (r.length_ticks / 2 / beat) * beat;
    auto sus = r;
    sus.chord.quality = detail::suspended(c.quality);
    sus.length_ticks = half;
    sus.device = ChordDevice::suspension;
    l[index].position = {r.position.value + half};
    l[index].length_ticks -= half;
    l.insert(l.begin() + static_cast<std::ptrdiff_t>(index), sus);
    add(EditKind::suspension, "Suspension",
        "Let " + name(c, key) +
            " arrive suspended and resolve halfway. It delays the pull without losing it.",
        std::move(l), index);
  }

  const ChordSymbol home_v{pc(lane.front().chord.root + perfect_fifth), dominant_seventh,
                           std::nullopt};
  if (index + 1 == n && r.length_ticks >= min_split &&
      !(is_dominant(c.quality) && c.root == home_v.root)) {
    auto l = copy_of(lane);
    l[index].length_ticks -= beat;
    l.push_back({{end_of(r) - beat}, beat, home_v, r.top, ChordDevice::pull_home});
    add(EditKind::pull_home, "Pull home",
        "End on " + name(home_v, key) + " for the last beat so the loop falls back into " +
            name(lane.front().chord, key) + ".",
        std::move(l), index + 1);
  }

  ChordQuality flip{};
  if (detail::flipped(c.quality, flip) && role(c, key) != Role::home) {
    auto l = copy_of(lane);
    l[index].chord.quality = flip;
    l[index].device = ChordDevice::borrowed;
    auto why = name(l[index].chord, key) + " instead: the same root from the opposite mode, " +
               (is_minor(c.quality) ? "a lift." : "a shadow.");
    add(EditKind::borrowed, "Borrowed chord", std::move(why), std::move(l), index);
  }

  if (index > 0 && !c.bass && n > inversion_min_lane) {
    const auto previous_bass = lane[index - 1].chord.bass.value_or(lane[index - 1].chord.root);
    const auto iv = intervals(c.quality);
    for (const auto i : {third_index, fifth_index}) {
      const auto t = pc(c.root + iv.semitones[i]);
      const int down = pc(previous_bass - t);
      if (down > 0 && down <= inversion_step) {
        auto l = copy_of(lane);
        l[index].chord.bass = t;
        add(EditKind::inversion, "Inversion",
            "Put " + std::string(pitch_name(t, key)) + " in the bass: it's a step from " +
                std::string(pitch_name(previous_bass, key)) +
                ", so the bass walks instead of jumping.",
            std::move(l), index);
        break;
      }
    }
  }

  if (out.size() > hint_limit) {
    out.resize(hint_limit);
  }
  return out;
}

} // namespace oscillate::harmony
