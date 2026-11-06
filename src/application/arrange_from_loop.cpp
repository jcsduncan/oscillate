// Arrange from a loop (ADR-0080): the rule engine. Pure functions of a
// snapshot and the options; every choice it makes by chance comes from the
// seed, so the same loop, template and seed always give the same plan.
#include "oscillate/application/arrange_from_loop.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace oscillate::application::arrange {
namespace {
using foundation::ErrorCode;
template <typename T> using Result = foundation::Result<T>;

// Engines whose role needs no notes to tell.
constexpr std::string_view anvil_engine = "anvil";
constexpr std::string_view knock_engine = "knock";
constexpr std::string_view crate_engine = "crate";

// Note statistics that pick a melodic role.
constexpr int bass_ceiling_key = 48;   // Below C3 is a bassline.
constexpr std::size_t chord_notes = 2; // Two at once is a chord.
constexpr std::size_t pad_notes = 3;   // Three held together is a pad.
constexpr double pad_beats = 2.0;      // Held at least this long on average.
constexpr int notes_per_octave = 12;
constexpr int octave_of_key_zero = -1; // MIDI 60 is C4.
constexpr std::array<std::string_view, notes_per_octave> note_names{
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

// Variation: above these, fills also land every 32 or every 16 bars, and
// each 16-bar block in a groove may lose a part for 8 bars at this rate.
constexpr double fill_32_threshold = 0.25;
constexpr double fill_16_threshold = 0.75;
constexpr double extra_dropout_rate = 0.35;
constexpr std::int64_t fill_phrase_bars = 32;

// Which words in a name say which role, checked in this order.
struct Keyword {
  std::string_view word;
  Role role;
};
constexpr std::array keywords{
    Keyword{"kick", Role::kick},   Keyword{"bd", Role::kick},     Keyword{"hat", Role::hats},
    Keyword{"hh", Role::hats},     Keyword{"shaker", Role::hats}, Keyword{"ride", Role::hats},
    Keyword{"cymbal", Role::hats}, Keyword{"perc", Role::perc},   Keyword{"clap", Role::perc},
    Keyword{"snare", Role::perc},  Keyword{"rim", Role::perc},    Keyword{"tom", Role::perc},
    Keyword{"conga", Role::perc},  Keyword{"bongo", Role::perc},  Keyword{"drum", Role::perc},
    Keyword{"bass", Role::bass},   Keyword{"sub", Role::bass},    Keyword{"stab", Role::stab},
    Keyword{"chord", Role::stab},  Keyword{"key", Role::stab},    Keyword{"piano", Role::stab},
    Keyword{"lead", Role::lead},   Keyword{"arp", Role::lead},    Keyword{"melod", Role::lead},
    Keyword{"hook", Role::lead},   Keyword{"pluck", Role::lead},  Keyword{"pad", Role::pad},
    Keyword{"string", Role::pad},  Keyword{"atmos", Role::pad},   Keyword{"drone", Role::pad},
    Keyword{"ambient", Role::pad}, Keyword{"vox", Role::vocal},   Keyword{"vocal", Role::vocal},
    Keyword{"voice", Role::vocal}, Keyword{"fx", Role::fx},       Keyword{"riser", Role::fx},
    Keyword{"sweep", Role::fx},    Keyword{"noise", Role::fx},
};

// SplitMix64: the same numbers on every platform, unlike <random>'s
// distributions.
class Dice {
public:
  explicit Dice(std::uint64_t seed) noexcept : state_(seed) {}
  [[nodiscard]] std::uint64_t next() noexcept {
    std::uint64_t z = (state_ += 0x9E37'79B9'7F4A'7C15ULL);
    z = (z ^ (z >> 30U)) * 0xBF58'476D'1CE4'E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D0'49BB'1331'11EBULL;
    return z ^ (z >> 31U);
  }
  // [0, 1)
  [[nodiscard]] double unit() noexcept {
    constexpr int mantissa_bits = 53;
    return static_cast<double>(next() >> (64 - mantissa_bits)) /
           static_cast<double>(std::uint64_t{1} << mantissa_bits);
  }
  [[nodiscard]] std::size_t pick(std::size_t count) noexcept {
    return count == 0 ? 0 : static_cast<std::size_t>(next() % count);
  }

private:
  std::uint64_t state_;
};

[[nodiscard]] std::string lower(std::string_view text) {
  std::string result(text);
  std::ranges::transform(result, result.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return result;
}

[[nodiscard]] std::string key_name(int key) {
  return std::string(note_names[static_cast<std::size_t>(key % notes_per_octave)]) +
         std::to_string(key / notes_per_octave + octave_of_key_zero);
}

[[nodiscard]] std::int64_t floor_to_grid(std::int64_t bars) noexcept {
  return bars / phrase_bars * phrase_bars;
}
[[nodiscard]] std::int64_t round_to_grid(double bars) noexcept {
  return static_cast<std::int64_t>(std::llround(bars / static_cast<double>(phrase_bars))) *
         phrase_bars;
}

[[nodiscard]] Result<timeline::TickPosition> bar_tick(const project::Snapshot& snapshot,
                                                      std::int64_t one_based_bar) {
  return snapshot.meter.tick_at({one_based_bar, 1, 0});
}

// A placement's ticks, through the tempo map when it is placed in samples.
[[nodiscard]] Result<std::pair<std::int64_t, std::int64_t>>
placement_ticks(const project::Snapshot& snapshot, const project::Placement& placement) {
  using R = Result<std::pair<std::int64_t, std::int64_t>>;
  if (const auto* musical = std::get_if<project::MusicalInterval>(&placement))
    return R::success({musical->start.value, musical->end.value});
  const auto& elapsed = std::get<project::ElapsedInterval>(placement);
  auto start = snapshot.tempo.sample_to_tick(elapsed.start, snapshot.sample_rate);
  auto end = snapshot.tempo.sample_to_tick(elapsed.end, snapshot.sample_rate);
  if (!start || !end)
    return R::failure(!start ? start.error() : end.error());
  return R::success({start.value().value, end.value().value});
}

[[nodiscard]] std::string track_label(const project::Track& track, std::size_t row) {
  return track.name.empty() ? "Track " + std::to_string(row + 1) : track.name;
}

[[nodiscard]] std::optional<Role> role_from_words(std::string_view text) {
  // A keyword counts where a word starts with it ("Sub Bass", not "Subtle").
  const auto words = lower(text);
  for (const auto& keyword : keywords)
    for (auto at = words.find(keyword.word); at != std::string::npos;
         at = words.find(keyword.word, at + 1))
      if (at == 0 || std::isalpha(static_cast<unsigned char>(words[at - 1])) == 0)
        return keyword.role;
  return std::nullopt;
}

struct NoteFacts {
  int median_key = 0;
  std::size_t polyphony = 0;
  double mean_beats = 0.0;
};

[[nodiscard]] NoteFacts note_facts(const std::vector<project::MidiNote>& notes) {
  NoteFacts facts;
  std::vector<int> keys;
  keys.reserve(notes.size());
  double total = 0.0;
  // Polyphony: the most notes sounding at one note's start.
  for (const auto& note : notes) {
    keys.push_back(note.key);
    total += static_cast<double>(note.end.value - note.start.value) /
             static_cast<double>(timeline::ticks_per_quarter);
    const auto sounding = std::ranges::count_if(notes, [&](const project::MidiNote& other) {
      return other.start.value <= note.start.value && other.end.value > note.start.value;
    });
    facts.polyphony = std::max(facts.polyphony, static_cast<std::size_t>(sounding));
  }
  std::ranges::sort(keys);
  facts.median_key = keys.empty() ? 0 : keys[keys.size() / 2];
  facts.mean_beats = notes.empty() ? 0.0 : total / static_cast<double>(notes.size());
  return facts;
}

[[nodiscard]] std::pair<Role, std::string>
role_from_notes(const std::vector<project::MidiNote>& notes) {
  const auto facts = note_facts(notes);
  const auto beats = std::to_string(static_cast<int>(std::lround(facts.mean_beats * 10.0)));
  const auto mean =
      beats.size() > 1 ? beats.substr(0, beats.size() - 1) + "." + beats.back() : "0." + beats;
  if (facts.median_key < bass_ceiling_key && facts.polyphony < pad_notes)
    return {Role::bass, "Low notes around " + key_name(facts.median_key) + ", " +
                            (facts.polyphony <= 1 ? "one at a time" : "mostly single")};
  if (facts.polyphony >= pad_notes && facts.mean_beats >= pad_beats)
    return {Role::pad, "Held chords: " + std::to_string(facts.polyphony) + " notes, " + mean +
                           " beats long on average"};
  if (facts.polyphony >= chord_notes)
    return {Role::stab, "Short chords of " + std::to_string(facts.polyphony) + " notes around " +
                            key_name(facts.median_key)};
  return {Role::lead, "A single line around " + key_name(facts.median_key)};
}

[[nodiscard]] std::string engine_of(const project::Track& track) {
  if (const auto* patch = std::get_if<project::NativeInstrumentPatch>(&track.instrument_source))
    return patch->engine;
  return {};
}

// ---- Layout -------------------------------------------------------------------------

[[nodiscard]] const TemplateFacts& facts_of(Template style) {
  return templates[static_cast<std::size_t>(style)];
}

[[nodiscard]] Result<void> valid_sections(const std::vector<Section>& sections,
                                          std::int64_t length) {
  if (sections.empty())
    return Result<void>::failure({ErrorCode::invalid_argument, "No sections"});
  std::int64_t at = 0;
  for (const auto& section : sections) {
    if (section.start_bar != at || section.bars < phrase_bars || section.bars % phrase_bars != 0)
      return Result<void>::failure(
          {ErrorCode::invalid_argument, "Sections must follow each other on the 8-bar grid"});
    at += section.bars;
  }
  if (at != length)
    return Result<void>::failure(
        {ErrorCode::invalid_argument, "The sections must cover the arrangement"});
  return Result<void>::success();
}

// ---- Where each role plays within a section ------------------------------------------

// The bars [first, last) a role plays inside a section of `bars`, relative
// to its start; equal means it sits the section out.
struct Span {
  std::int64_t first = 0;
  std::int64_t last = 0;
};

// Half a section on the grid, at least one phrase in from its start.
[[nodiscard]] std::int64_t half(std::int64_t bars) noexcept {
  return std::clamp(floor_to_grid(bars / 2), std::min(phrase_bars, bars), bars);
}

[[nodiscard]] Span presence(const Section& section, Role role, Template style) {
  const auto n = section.bars;
  const Span all{0, n};
  const Span none{0, 0};
  switch (section.kind) {
  case SectionKind::intro:
    if (role == Role::kick)
      return all;
    if (role == Role::hats)
      return {std::min(phrase_bars, n), n};
    if (role == Role::perc)
      return {half(n), n};
    return none;
  case SectionKind::build:
    if (is_beat(role) || role == Role::bass)
      return all;
    if (role == Role::pad)
      return none;
    return {half(n), n};
  case SectionKind::groove:
    if (role == Role::lead && n >= 2 * min_groove_bars)
      return {half(n), n};
    return all;
  case SectionKind::breakdown:
    if (role == Role::kick || role == Role::bass || role == Role::perc)
      return none;
    if (role == Role::hats)
      return style == Template::peak_time ? Span{n - std::min(phrase_bars, n), n} : none;
    return all;
  case SectionKind::drop:
    return all;
  case SectionKind::groove_2:
    if ((role == Role::lead || role == Role::pad || role == Role::vocal) &&
        n >= 2 * min_groove_bars)
      return {0, n - half(n)};
    return all;
  case SectionKind::outro:
    if (role == Role::kick || role == Role::hats)
      return all;
    if (role == Role::perc)
      return {0, half(n)};
    return none;
  }
  return none;
}

[[nodiscard]] const Section* find_section(const std::vector<Section>& sections, SectionKind kind) {
  const auto found = std::ranges::find(sections, kind, &Section::kind);
  return found == sections.end() ? nullptr : &*found;
}

[[nodiscard]] bool plays_through(const TrackPart& part, std::int64_t from, std::int64_t to) {
  for (auto bar = from; bar < to; ++bar)
    if (!part.plays[static_cast<std::size_t>(bar)])
      return false;
  return true;
}

[[nodiscard]] bool plays_at(const TrackPart& part, std::int64_t bar) {
  return bar >= 0 && bar < static_cast<std::int64_t>(part.plays.size()) &&
         part.plays[static_cast<std::size_t>(bar)];
}

[[nodiscard]] const TrackFinding& finding_of(const Plan& plan, project::TrackId track) {
  return *std::ranges::find(plan.findings, track, &TrackFinding::track);
}

[[nodiscard]] bool is_edge(SectionKind kind) noexcept {
  return kind == SectionKind::intro || kind == SectionKind::outro;
}

// Every bar where something starts or stops: parts, sections and moves.
[[nodiscard]] std::set<std::int64_t> event_bars(const Plan& plan) {
  std::set<std::int64_t> bars;
  for (const auto& part : plan.parts)
    for (const auto bar : part_changes(part))
      bars.insert(bar);
  for (const auto& section : plan.sections)
    bars.insert(section.start_bar);
  for (const auto& sweep : plan.sweeps)
    bars.insert(sweep.start_bar);
  for (const auto& fill : plan.fills)
    bars.insert(fill.bar);
  for (const auto& thrown : plan.throws)
    bars.insert(static_cast<std::int64_t>(std::floor(thrown.start_bar)));
  for (const auto& transition : plan.transitions)
    bars.insert(transition.start_bar);
  return bars;
}

[[nodiscard]] bool has_event(const std::set<std::int64_t>& events, std::int64_t from,
                             std::int64_t to) {
  const auto found = events.lower_bound(from);
  return found != events.end() && *found < to;
}

// The part a fill plays on at a bar: percussion first, then hats, a stab, the bass.
[[nodiscard]] const TrackPart* fill_part(const Plan& plan, std::int64_t bar) {
  constexpr std::array preference{Role::perc, Role::hats, Role::stab, Role::bass};
  for (const auto role : preference)
    for (const auto& part : plan.parts)
      if (part.role == role && plays_at(part, bar))
        return &part;
  return nullptr;
}

void add_fill(Plan& plan, std::int64_t bar, std::string reason) {
  if (std::ranges::find(plan.fills, bar, &Fill::bar) != plan.fills.end())
    return;
  const auto* part = fill_part(plan, bar);
  if (part == nullptr)
    return;
  plan.fills.push_back({part->track, bar, std::move(reason)});
}

// Parts that could sit out [from, to) without breaking a rule: never the
// kick, and only the beat in the intro and outro.
[[nodiscard]] std::vector<TrackPart*> dropout_candidates(Plan& plan, std::int64_t from,
                                                         std::int64_t to) {
  std::vector<TrackPart*> candidates;
  const auto& section = section_at(plan.sections, from);
  const auto inside = [&](double bar) {
    return bar >= static_cast<double>(from) && bar < static_cast<double>(to);
  };
  for (auto& part : plan.parts) {
    if (part.role == Role::kick || !plays_through(part, from, to))
      continue;
    if (is_edge(section.kind) && !is_beat(part.role))
      continue;
    // A part carrying a fill, a throw or a sweep there keeps playing.
    const bool busy =
        std::ranges::any_of(plan.fills,
                            [&](const Fill& fill) {
                              return fill.track == part.track &&
                                     inside(static_cast<double>(fill.bar));
                            }) ||
        std::ranges::any_of(plan.throws,
                            [&](const Throw& thrown) {
                              return thrown.track == part.track && inside(thrown.start_bar);
                            }) ||
        std::ranges::any_of(plan.sweeps, [&](const Sweep& sweep) {
          return sweep.track == part.track && sweep.start_bar < to && sweep.end_bar > from;
        });
    if (!busy)
      candidates.push_back(&part);
  }
  return candidates;
}

void drop_out(Plan& plan, TrackPart& part, std::int64_t from, std::int64_t to, std::string why) {
  for (auto bar = from; bar < to; ++bar)
    part.plays[static_cast<std::size_t>(bar)] = false;
  plan.decisions.push_back({from, finding_of(plan, part.track).name + " sits out " +
                                      std::to_string(to - from) + " bars" + why});
}

void name_changes(Plan& plan) {
  for (const auto& part : plan.parts) {
    const auto& name = finding_of(plan, part.track).name;
    if (plays_at(part, 0))
      plan.decisions.push_back({0, name + " starts the track"});
    for (const auto bar : part_changes(part))
      plan.decisions.push_back({bar, name + (plays_at(part, bar) ? " comes in" : " drops out")});
  }
}

} // namespace

// ---- Loop content -------------------------------------------------------------------

Result<LoopRange> loop_range(const project::Snapshot& snapshot, const Options& options) {
  using R = Result<LoopRange>;
  if (std::ranges::find(loop_choices, options.loop_bars) == loop_choices.end())
    return R::failure({ErrorCode::invalid_argument, "The loop is 8 or 16 bars"});
  if (options.loop_start_bar < 1)
    return R::failure({ErrorCode::invalid_argument, "The loop starts at bar 1 or later"});
  auto start = bar_tick(snapshot, options.loop_start_bar);
  auto end = bar_tick(snapshot, options.loop_start_bar + options.loop_bars);
  if (!start || !end)
    return R::failure(!start ? start.error() : end.error());
  return R::success({start.value(), end.value()});
}

Result<LoopContent> loop_content(const project::Snapshot& snapshot, const project::Track& track,
                                 LoopRange loop) {
  using R = Result<LoopContent>;
  LoopContent content;
  const auto from = loop.start.value;
  const auto to = loop.end.value;
  for (const auto& clip : track.midi_clips) {
    auto span = placement_ticks(snapshot, clip.placement);
    if (!span)
      return R::failure(span.error());
    const auto [start, end] = span.value();
    const auto pattern = clip.pattern_length.value;
    if (pattern <= 0 || end <= from || start >= to)
      continue;
    // The pattern's passes that can reach the loop.
    const auto first_pass = clip.loop ? std::max<std::int64_t>(0, (from - start) / pattern) : 0;
    for (auto pass = first_pass;; ++pass) {
      const auto origin = start + pass * pattern;
      if (origin >= std::min(end, to) || (!clip.loop && pass > 0))
        break;
      for (const auto& note : clip.notes) {
        if (note.start.value >= pattern)
          continue;
        const auto on = origin + note.start.value;
        const auto off = std::min({origin + std::min(note.end.value, pattern), end, to});
        if (on < from || on >= to || off <= on)
          continue;
        content.notes.push_back({note.id, {on - from}, {off - from}, note.key, note.velocity});
      }
    }
  }
  std::ranges::sort(content.notes, [](const auto& a, const auto& b) {
    return std::tie(a.start.value, a.key) < std::tie(b.start.value, b.key);
  });
  for (const auto& clip : track.clips) {
    auto span = placement_ticks(snapshot, clip.placement);
    if (!span)
      return R::failure(span.error());
    const auto [start, end] = span.value();
    if (end <= from || start >= to)
      continue;
    if (start < from || end > to) {
      ++content.crossing_audio;
      continue;
    }
    const auto length = std::holds_alternative<project::MusicalInterval>(clip.placement)
                            ? end - start
                            : std::get<project::ElapsedInterval>(clip.placement).end.value -
                                  std::get<project::ElapsedInterval>(clip.placement).start.value;
    content.audio.push_back({clip, start - from, length});
  }
  return R::success(std::move(content));
}

// ---- Roles --------------------------------------------------------------------------

std::vector<TrackFinding> find_roles(const project::Snapshot& snapshot, const Options& options) {
  std::vector<TrackFinding> findings;
  const auto loop = loop_range(snapshot, options);
  for (std::size_t row = 0; row < snapshot.tracks.size(); ++row) {
    const auto& track = snapshot.tracks[row];
    if (track.kind != project::TrackKind::audio && track.kind != project::TrackKind::instrument)
      continue;
    TrackFinding finding{track.id};
    finding.name = track_label(track, row);
    finding.locked = std::ranges::find(options.locked, track.id) != options.locked.end();
    LoopContent content;
    if (loop) {
      auto read = loop_content(snapshot, track, loop.value());
      if (read)
        content = std::move(read).value();
      else
        finding.unsupported = read.error().message;
    }
    finding.has_content = !content.empty();
    if (track.frozen)
      finding.unsupported = "Frozen: unfreeze it to arrange it";
    else if (content.audio.empty() && content.crossing_audio > 0)
      finding.unsupported = "Its audio crosses the loop's edges: split it at the loop first";

    const auto chosen =
        std::ranges::find(options.roles, track.id, &std::pair<project::TrackId, Role>::first);
    const auto engine = engine_of(track);
    std::string clip_names;
    for (const auto& clip : track.clips)
      clip_names += " " + clip.name;
    if (chosen != options.roles.end()) {
      finding.role = chosen->second;
      finding.detected = false;
      finding.evidence = "Set by you";
    } else if (const auto named = role_from_words(finding.name)) {
      finding.role = *named;
      finding.evidence = "Named “" + finding.name + "”";
    } else if (engine == anvil_engine) {
      finding.role = Role::kick;
      finding.evidence = "Anvil, the kick synth";
    } else if (engine == knock_engine) {
      finding.role = Role::perc;
      finding.evidence = "Knock percussion";
    } else if (engine == crate_engine) {
      finding.role = Role::perc;
      finding.evidence = "Crate chops";
    } else if (!content.notes.empty()) {
      auto [role, why] = role_from_notes(content.notes);
      finding.role = role;
      finding.evidence = std::move(why);
    } else if (const auto clipped = role_from_words(clip_names)) {
      finding.role = *clipped;
      finding.evidence = "Its clips' names";
    } else {
      finding.role = Role::fx;
      finding.evidence = track.kind == project::TrackKind::audio
                             ? "Audio with no name to go by: set its role"
                             : "Nothing to go by: set its role";
    }
    if (!finding.has_content && finding.unsupported.empty())
      finding.evidence += " · nothing in the loop";
    if (content.crossing_audio > 0 && !content.audio.empty())
      finding.evidence += " · " + std::to_string(content.crossing_audio) +
                          " clip(s) across the loop's edges left out";
    findings.push_back(std::move(finding));
  }
  return findings;
}

// ---- Layout -------------------------------------------------------------------------

std::int64_t min_length_bars(Template style, std::int64_t edge_bars) {
  const auto& facts = facts_of(style);
  return 2 * edge_bars + facts.build_bars + facts.break_bars + facts.drop_bars +
         2 * min_groove_bars;
}

std::int64_t default_length_bars(Template style, const project::Snapshot& snapshot) {
  constexpr double seconds_per_minute = 60.0;
  constexpr double quarters_per_whole = 4.0;
  constexpr double fallback_bpm = 120.0;
  const auto& tempo = snapshot.tempo.events();
  const auto& meter = snapshot.meter.events();
  const double bpm = tempo.empty() ? fallback_bpm : tempo.front().bpm;
  const double beats_per_bar =
      meter.empty() ? quarters_per_whole
                    : meter.front().numerator * quarters_per_whole / meter.front().denominator;
  const double bars = facts_of(style).seconds * bpm / seconds_per_minute / beats_per_bar;
  return std::clamp(round_to_grid(bars), min_length_bars(style, default_edge_bars),
                    max_length_bars);
}

Result<std::vector<Section>> layout(Template style, std::int64_t length_bars,
                                    std::int64_t edge_bars, double breakdown) {
  using R = Result<std::vector<Section>>;
  if (std::ranges::find(edge_choices, edge_bars) == edge_choices.end())
    return R::failure({ErrorCode::invalid_argument, "The intro and outro are 16, 32 or 64 bars"});
  if (length_bars % phrase_bars != 0 || length_bars > max_length_bars)
    return R::failure({ErrorCode::invalid_argument, "The length is a multiple of 8 bars"});
  if (length_bars < min_length_bars(style, edge_bars))
    return R::failure(
        {ErrorCode::invalid_argument, "Too short for this template: make it at least " +
                                          std::to_string(min_length_bars(style, edge_bars)) +
                                          " bars or shorten the intro"});
  const auto& facts = facts_of(style);
  const auto rest =
      length_bars - 2 * edge_bars - facts.build_bars - facts.break_bars - facts.drop_bars;
  const auto first =
      std::clamp(round_to_grid(static_cast<double>(rest) * std::clamp(breakdown, 0.0, 1.0)),
                 min_groove_bars, rest - min_groove_bars);
  std::vector<Section> sections;
  std::int64_t at = 0;
  const auto add = [&](SectionKind kind, std::int64_t bars) {
    sections.push_back({kind, at, bars});
    at += bars;
  };
  add(SectionKind::intro, edge_bars);
  add(SectionKind::build, facts.build_bars);
  add(SectionKind::groove, first);
  add(SectionKind::breakdown, facts.break_bars);
  add(SectionKind::drop, facts.drop_bars);
  add(SectionKind::groove_2, rest - first);
  add(SectionKind::outro, edge_bars);
  return R::success(std::move(sections));
}

std::vector<Section> move_edge(std::vector<Section> sections, std::size_t index, std::int64_t bar) {
  if (index == 0 || index >= sections.size())
    return sections;
  auto& previous = sections[index - 1];
  auto& current = sections[index];
  const auto low = previous.start_bar + phrase_bars;
  const auto high = current.start_bar + current.bars - phrase_bars;
  const auto snapped = std::clamp(round_to_grid(static_cast<double>(bar)), low, high);
  const auto end = current.start_bar + current.bars;
  previous.bars = snapped - previous.start_bar;
  current.start_bar = snapped;
  current.bars = end - snapped;
  return sections;
}

const Section& section_at(const std::vector<Section>& sections, std::int64_t bar) {
  for (const auto& section : sections)
    if (bar >= section.start_bar && bar < section.start_bar + section.bars)
      return section;
  return sections.back();
}

std::vector<std::int64_t> part_changes(const TrackPart& part) {
  std::vector<std::int64_t> changes;
  for (std::size_t bar = 1; bar < part.plays.size(); ++bar)
    if (part.plays[bar] != part.plays[bar - 1])
      changes.push_back(static_cast<std::int64_t>(bar));
  return changes;
}

// ---- The plan -----------------------------------------------------------------------

Result<Plan> plan(const project::Snapshot& snapshot, Options options) {
  using R = Result<Plan>;
  auto loop = loop_range(snapshot, options);
  if (!loop)
    return R::failure(loop.error());
  if (options.length_bars == 0)
    options.length_bars = default_length_bars(options.style, snapshot);
  options.breakdown = std::clamp(options.breakdown, 0.0, 1.0);
  options.variation = std::clamp(options.variation, 0.0, 1.0);

  Plan result;
  result.revision = snapshot.revision;
  result.loop = loop.value();
  result.length_bars = options.length_bars;
  if (options.sections.empty()) {
    auto sections =
        layout(options.style, options.length_bars, options.edge_bars, options.breakdown);
    if (!sections)
      return R::failure(sections.error());
    result.sections = std::move(sections).value();
  } else {
    auto valid = valid_sections(options.sections, options.length_bars);
    if (!valid)
      return R::failure(valid.error());
    result.sections = options.sections;
  }
  result.findings = find_roles(snapshot, options);
  result.options = options;
  const auto length = result.length_bars;
  const auto& facts = facts_of(options.style);
  Dice dice(options.seed);

  // Parts: where each role plays, section by section.
  for (const auto& finding : result.findings) {
    if (!finding.arranged())
      continue;
    TrackPart part{finding.track, finding.role,
                   std::vector<bool>(static_cast<std::size_t>(length))};
    for (const auto& section : result.sections) {
      const auto span = presence(section, finding.role, options.style);
      for (auto bar = span.first; bar < span.last; ++bar)
        part.plays[static_cast<std::size_t>(section.start_bar + bar)] = true;
    }
    result.parts.push_back(std::move(part));
  }

  for (const auto& section : result.sections)
    result.decisions.push_back(
        {section.start_bar, std::string(section_name(section.kind)) + ": " +
                                std::to_string(section.bars) + " bars" +
                                (is_edge(section.kind) ? ", beats only for mixing" : "")});

  // Kick-out bars lead out of the build into the groove (and, peak-time,
  // out of the drop into Groove 2). The break has no kick to take out.
  std::vector<const Section*> kicked;
  if (const auto* groove = find_section(result.sections, SectionKind::groove))
    kicked.push_back(groove);
  if (options.style == Template::peak_time)
    if (const auto* second = find_section(result.sections, SectionKind::groove_2))
      kicked.push_back(second);
  for (const auto* section : kicked)
    for (auto& part : result.parts) {
      if (part.role != Role::kick)
        continue;
      const auto from = std::max<std::int64_t>(0, section->start_bar - facts.kick_out_bars);
      for (auto bar = from; bar < section->start_bar; ++bar)
        part.plays[static_cast<std::size_t>(bar)] = false;
    }

  // Sweeps: the build opens a low-pass on the music; the break's last bars
  // raise a high-pass into the drop.
  const auto* build = find_section(result.sections, SectionKind::build);
  const auto* breakdown = find_section(result.sections, SectionKind::breakdown);
  const auto* drop = find_section(result.sections, SectionKind::drop);
  for (const auto& part : result.parts) {
    if (is_beat(part.role))
      continue;
    if (build != nullptr) {
      const auto end = build->start_bar + build->bars;
      auto first = build->start_bar;
      while (first < end && !plays_at(part, first))
        ++first;
      if (first < end && plays_through(part, first, end))
        result.sweeps.push_back({part.track, false, first, end, sweep_closed_hz, sweep_open_hz,
                                 "Opens across the build"});
    }
    if (breakdown != nullptr && drop != nullptr) {
      const auto from = std::max(breakdown->start_bar, drop->start_bar - high_pass_bars);
      if (from < drop->start_bar && plays_through(part, from, drop->start_bar))
        result.sweeps.push_back({part.track, true, from, drop->start_bar, high_pass_start_hz,
                                 high_pass_end_hz, "Thins out into the drop"});
    }
  }

  // Throws: the last half bar before the break and before the drop, into
  // each music part's first send to a return track.
  for (const auto& part : result.parts) {
    if (is_beat(part.role) || part.role == Role::bass)
      continue;
    const auto& track = *std::ranges::find(snapshot.tracks, part.track, &project::Track::id);
    for (const auto& send : track.sends) {
      if (send.purpose != project::SendPurpose::audible || send.gain >= throw_gain)
        continue;
      const auto destination =
          std::ranges::find(snapshot.tracks, send.destination, &project::Track::id);
      if (destination == snapshot.tracks.end() ||
          destination->kind != project::TrackKind::return_bus)
        continue;
      const bool laned = std::ranges::any_of(track.automation, [&](const auto& lane) {
        const auto* target = std::get_if<project::SendGainTarget>(&lane.target);
        return target != nullptr && target->send == send.id;
      });
      if (laned)
        continue;
      for (const auto* section : {breakdown, drop}) {
        if (section == nullptr || !plays_at(part, section->start_bar - 1))
          continue;
        result.throws.push_back(
            {part.track, send.id, static_cast<double>(section->start_bar) - throw_bars, throw_bars,
             throw_gain,
             "Throws the phrase's tail into " +
                 (destination->name.empty() ? std::string("the return") : destination->name)});
      }
      break;
    }
  }

  // Fills: the last bar of each groove, the drop and, with more variation,
  // each 32- or 16-bar phrase inside them.
  for (const auto& section : result.sections) {
    if (section.kind != SectionKind::groove && section.kind != SectionKind::drop &&
        section.kind != SectionKind::groove_2)
      continue;
    const auto end = section.start_bar + section.bars;
    add_fill(result, end - 1,
             "Rolls into " + std::string(section_name(
                                 section_at(result.sections, std::min(end, length - 1)).kind)));
    const auto every = options.variation >= fill_16_threshold   ? change_bars
                       : options.variation >= fill_32_threshold ? fill_phrase_bars
                                                                : std::int64_t{0};
    if (every == 0)
      continue;
    for (auto bar = section.start_bar + every - 1; bar < end - 1; bar += every)
      add_fill(result, bar, "Marks the phrase");
  }

  // Transitions on their own tracks.
  if (options.transitions) {
    if (drop != nullptr && drop->start_bar >= riser_bars)
      result.transitions.push_back(
          {TransitionKind::riser, drop->start_bar - riser_bars, riser_bars, "Rises into the drop"});
    const auto* groove = find_section(result.sections, SectionKind::groove);
    if (groove != nullptr && build != nullptr &&
        (options.style == Template::peak_time || build->bars >= 2 * min_groove_bars))
      result.transitions.push_back({TransitionKind::riser, groove->start_bar - riser_bars,
                                    riser_bars, "Rises out of the build"});
    if (breakdown != nullptr)
      result.transitions.push_back({TransitionKind::downlifter, breakdown->start_bar,
                                    downlifter_bars, "Falls into the break"});
    for (const auto* section : {groove, drop})
      if (section != nullptr)
        result.transitions.push_back(
            {TransitionKind::impact, section->start_bar, impact_bars, "Hits the downbeat"});
    std::ranges::sort(result.transitions, {}, &Transition::start_bar);
  }

  // Variation: in the grooves and the drop, a part may sit out 8 bars of a
  // 16-bar block now and then.
  for (const auto& section : result.sections) {
    if (section.kind != SectionKind::groove && section.kind != SectionKind::groove_2 &&
        section.kind != SectionKind::drop)
      continue;
    for (auto block = section.start_bar; block + change_bars <= section.start_bar + section.bars;
         block += change_bars) {
      if (dice.unit() >= options.variation * extra_dropout_rate)
        continue;
      auto candidates = dropout_candidates(result, block, block + change_bars);
      std::erase_if(candidates, [](const TrackPart* part) { return part->role == Role::bass; });
      if (candidates.empty())
        continue;
      auto* part = candidates[dice.pick(candidates.size())];
      drop_out(result, *part, block + phrase_bars, block + change_bars, " for variation");
    }
  }

  // Something new every 16 bars: where a block has nothing, add a move.
  for (auto block = change_bars; block < length; block += change_bars) {
    const auto end = std::min(block + change_bars, length);
    if (has_event(event_bars(result), block, end)) {
      result.checks.push_back({block, CheckStatus::held, "Changes on its own"});
      continue;
    }
    auto candidates = end - block == change_bars ? dropout_candidates(result, block, end)
                                                 : std::vector<TrackPart*>{};
    if (!candidates.empty()) {
      auto* part = candidates[dice.pick(candidates.size())];
      const auto name = finding_of(result, part->track).name;
      drop_out(result, *part, block + phrase_bars, end, " to keep it moving");
      result.checks.push_back({block, CheckStatus::added, name + " sits out 8 bars"});
      continue;
    }
    const auto fills = result.fills.size();
    add_fill(result, end - 1, "Keeps it moving");
    if (result.fills.size() > fills) {
      result.checks.push_back({block, CheckStatus::added, "A fill on the last bar"});
      continue;
    }
    result.checks.push_back({block, CheckStatus::failed, "Nothing here can change"});
  }

  name_changes(result);
  for (const auto& sweep : result.sweeps)
    result.decisions.push_back({sweep.start_bar, finding_of(result, sweep.track).name + ": " +
                                                     (sweep.high_pass ? "high-pass" : "low-pass") +
                                                     " · " + sweep.reason});
  for (const auto& thrown : result.throws)
    result.decisions.push_back({static_cast<std::int64_t>(std::floor(thrown.start_bar)),
                                finding_of(result, thrown.track).name + ": " + thrown.reason});
  for (const auto& fill : result.fills)
    result.decisions.push_back(
        {fill.bar, finding_of(result, fill.track).name + ": Chop fill · " + fill.reason});
  for (const auto& transition : result.transitions)
    result.decisions.push_back(
        {transition.start_bar,
         std::string(transition_facts[static_cast<std::size_t>(transition.kind)].track_name) +
             " · " + transition.reason});
  std::ranges::stable_sort(result.decisions, {}, &Decision::bar);
  return R::success(std::move(result));
}

std::vector<Rule> rules(const Plan& plan) {
  const auto& facts = facts_of(plan.options.style);
  const auto starts_section = [&](std::int64_t bar) {
    return std::ranges::find(plan.sections, bar, &Section::start_bar) != plan.sections.end();
  };
  bool grid = std::ranges::all_of(
      plan.sections, [](const Section& section) { return section.start_bar % phrase_bars == 0; });
  for (const auto& part : plan.parts)
    for (const auto bar : part_changes(part))
      if (bar % phrase_bars != 0 &&
          !(part.role == Role::kick && starts_section(bar + facts.kick_out_bars)))
        grid = false;

  const bool moving = std::ranges::none_of(
      plan.checks, [](const Check& check) { return check.status == CheckStatus::failed; });

  bool beats_only = true;
  for (const auto& section : plan.sections) {
    if (!is_edge(section.kind))
      continue;
    const auto end = section.start_bar + section.bars;
    for (const auto& part : plan.parts)
      if (!is_beat(part.role))
        for (auto bar = section.start_bar; bar < end; ++bar)
          beats_only = beats_only && !plays_at(part, bar);
    for (const auto& transition : plan.transitions)
      beats_only =
          beats_only && !(transition.start_bar >= section.start_bar && transition.start_bar < end);
  }

  const bool untouched = std::ranges::none_of(plan.findings, [&](const TrackFinding& finding) {
    if (!finding.locked)
      return false;
    const auto touches = [&](const auto& list) {
      return std::ranges::any_of(list,
                                 [&](const auto& item) { return item.track == finding.track; });
    };
    return touches(plan.parts) || touches(plan.sweeps) || touches(plan.throws) ||
           touches(plan.fills);
  });

  return {
      {"Changes land on 8-bar lines", grid},
      {"Something new every 16 bars", moving},
      {"Intro and outro are beats only, for mixing", beats_only},
      {"Locked tracks are left as they are", untouched},
  };
}

} // namespace oscillate::application::arrange
