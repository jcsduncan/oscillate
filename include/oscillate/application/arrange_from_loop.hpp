#pragma once

#include "oscillate/foundation/error.hpp"
#include "oscillate/project/model.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// Arrange from a loop (ADR-0080, docs/ui/creative-tools-mockup.html#arrange):
// an 8- or 16-bar loop becomes a whole DJ-friendly track. A deterministic
// rule engine reads each track's role from its name, instrument and notes,
// lays out sections on 8-bar lines, decides where each part plays, and adds
// the moves that make a long track breathe: filter sweeps, a high-pass
// before the drop, send throws, Chop fills, kick-out bars and, optionally,
// risers, downlifters and impacts on their own tracks. Everything here is a
// pure function of the snapshot and the options; ProjectSession::
// apply_arrangement writes a plan as one undo step. Positions in a plan are
// zero-based bars from the arrangement's start (bar 1 of the project).
namespace oscillate::application::arrange {

// ---- Roles --------------------------------------------------------------------------

enum class Role : std::uint8_t { kick, hats, perc, bass, stab, lead, pad, fx, vocal };
inline constexpr std::size_t role_count = 9;
inline constexpr std::array<std::string_view, role_count> role_names{
    "Kick", "Hats", "Perc", "Bass", "Stab", "Lead", "Pad", "Fx", "Vocal"};
[[nodiscard]] constexpr std::string_view role_name(Role role) noexcept {
  return role_names[static_cast<std::size_t>(role)];
}
// The beat (kick, hats, perc) plays the intro and outro; everything else is
// music and waits for the build.
[[nodiscard]] constexpr bool is_beat(Role role) noexcept {
  return role == Role::kick || role == Role::hats || role == Role::perc;
}

// ---- Templates and sections ---------------------------------------------------------

enum class Template : std::uint8_t { dj_tool, hypnotic, peak_time };
inline constexpr std::size_t template_count = 3;

struct TemplateFacts {
  std::string_view name;
  // The length it aims for, turned into bars at the project's tempo.
  double seconds;
  std::int64_t build_bars;
  std::int64_t break_bars;
  std::int64_t drop_bars;
  // Bars the kick sits out at the end of the build, before the groove.
  std::int64_t kick_out_bars;
};
inline constexpr std::array<TemplateFacts, template_count> templates{{
    {"DJ Tool", 390.0, 16, 16, 16, 1},
    {"Hypnotic", 420.0, 32, 32, 16, 1},
    {"Peak-time build", 360.0, 16, 16, 32, 2},
}};

enum class SectionKind : std::uint8_t { intro, build, groove, breakdown, drop, groove_2, outro };
inline constexpr std::size_t section_kind_count = 7;
inline constexpr std::array<std::string_view, section_kind_count> section_names{
    "Intro", "Build", "Groove", "Break", "Drop", "Groove 2", "Outro"};
[[nodiscard]] constexpr std::string_view section_name(SectionKind kind) noexcept {
  return section_names[static_cast<std::size_t>(kind)];
}

struct Section {
  SectionKind kind = SectionKind::intro;
  std::int64_t start_bar = 0;
  std::int64_t bars = 0;
  bool operator==(const Section&) const = default;
};

// Every section edge and every part entering or leaving lands on this grid
// (fills and kick-outs excepted: they lead into a line).
inline constexpr std::int64_t phrase_bars = 8;
// Something new happens at least this often.
inline constexpr std::int64_t change_bars = 16;
// A groove section is never shorter than this.
inline constexpr std::int64_t min_groove_bars = 16;
inline constexpr std::array<std::int64_t, 2> loop_choices{8, 16};
inline constexpr std::array<std::int64_t, 3> edge_choices{16, 32, 64};
inline constexpr std::int64_t default_edge_bars = 32;
inline constexpr double default_breakdown = 0.55;
inline constexpr double default_variation = 0.5;
inline constexpr std::int64_t max_length_bars = 512;

// ---- Options ------------------------------------------------------------------------

struct Options {
  // The loop: one-based project bars [loop_start_bar, loop_start_bar + loop_bars).
  std::int64_t loop_start_bar = 1;
  std::int64_t loop_bars = 16;
  Template style = Template::dj_tool;
  // 0: the template's length at the project's tempo.
  std::int64_t length_bars = 0;
  std::int64_t edge_bars = default_edge_bars;
  // Where the break sits in what lies between the build and the outro, 0..1.
  double breakdown = default_breakdown;
  // How often the planner adds a move of its own (fills, drop-outs), 0..1.
  double variation = default_variation;
  std::uint64_t seed = 1;
  // Add risers, downlifters and impacts on new tracks.
  bool transitions = true;
  // Left exactly as they are: no clips added, moved or removed, no automation.
  std::vector<project::TrackId> locked;
  // The user's corrections to what was found.
  std::vector<std::pair<project::TrackId, Role>> roles;
  // A layout dragged by hand (contiguous, on the 8-bar grid, covering the
  // length); empty takes the template's.
  std::vector<Section> sections;
};

// ---- What the loop holds ------------------------------------------------------------

// One track's part of the loop: notes relative to the loop's start, and the
// audio clips that lie wholly inside it with their offset from the loop's
// start. A clip's own automation stays with the clip; a MIDI clip's
// automation and pedals are not carried into the arrangement.
struct LoopAudioClip {
  project::Clip clip;
  std::int64_t offset_ticks = 0;
  // Its length in ticks (musical) or samples (elapsed placement).
  std::int64_t length = 0;
};
struct LoopContent {
  std::vector<project::MidiNote> notes;
  std::vector<LoopAudioClip> audio;
  // Audio clips that cross the loop's edges and are left out.
  std::size_t crossing_audio = 0;
  [[nodiscard]] bool empty() const noexcept { return notes.empty() && audio.empty(); }
};

struct LoopRange {
  timeline::TickPosition start;
  timeline::TickPosition end;
  [[nodiscard]] std::int64_t ticks() const noexcept { return end.value - start.value; }
};

// The loop's ticks, or why the options do not name a loop.
[[nodiscard]] foundation::Result<LoopRange> loop_range(const project::Snapshot& snapshot,
                                                       const Options& options);
[[nodiscard]] foundation::Result<LoopContent>
loop_content(const project::Snapshot& snapshot, const project::Track& track, LoopRange loop);

// ---- Roles found --------------------------------------------------------------------

struct TrackFinding {
  project::TrackId track;
  std::string name{};
  Role role = Role::fx;
  // False when the user set the role.
  bool detected = true;
  // Why this role, in plain words.
  std::string evidence{};
  bool locked = false;
  // The loop holds something on this track.
  bool has_content = false;
  // Why Arrange leaves it alone even unlocked (frozen, audio across the
  // loop's edges), or empty.
  std::string unsupported{};
  [[nodiscard]] bool arranged() const noexcept {
    return !locked && has_content && unsupported.empty();
  }
};

// Every audio and instrument track, in track order.
[[nodiscard]] std::vector<TrackFinding> find_roles(const project::Snapshot& snapshot,
                                                   const Options& options);

// ---- The plan -----------------------------------------------------------------------

struct TrackPart {
  project::TrackId track;
  Role role = Role::fx;
  // One flag per bar of the arrangement: does the part play there?
  std::vector<bool> plays;
};

// A filter sweep on a track's own Contour, added at its chain's end: a
// low-pass opening (from_hz below to_hz) or a high-pass rising.
struct Sweep {
  project::TrackId track;
  bool high_pass = false;
  std::int64_t start_bar = 0;
  std::int64_t end_bar = 0;
  double from_hz = 0.0;
  double to_hz = 0.0;
  std::string reason;
};
inline constexpr double sweep_closed_hz = 250.0;
inline constexpr double sweep_open_hz = 18'000.0;
inline constexpr double high_pass_start_hz = 40.0;
inline constexpr double high_pass_end_hz = 900.0;
// Bars of high-pass rise before a drop.
inline constexpr std::int64_t high_pass_bars = 4;

// A send to a return track turned up for a moment: the tail of a phrase
// thrown into the reverb or delay.
struct Throw {
  project::TrackId track;
  project::SendId send;
  double start_bar = 0.0;
  double bars = 0.0;
  double gain = 0.0;
  std::string reason;
};
// Linear send gain during a throw, and its length: the phrase's last half bar.
inline constexpr double throw_gain = 1.0;
inline constexpr double throw_bars = 0.5;

// One bar of Chop on a track (its own, or one added with the Glitch fill
// preset whose mix is otherwise 0).
struct Fill {
  project::TrackId track;
  std::int64_t bar = 0;
  std::string reason;
};
inline constexpr std::string_view fill_preset = "Glitch fill";

enum class TransitionKind : std::uint8_t { riser, downlifter, impact };
inline constexpr std::size_t transition_kind_count = 3;
struct TransitionFacts {
  std::string_view track_name;
  std::string_view nova_preset;
  std::uint8_t key;
  std::uint8_t velocity;
};
inline constexpr std::array<TransitionFacts, transition_kind_count> transition_facts{{
    {"Riser", "Long Noise Riser", 60, 100},
    {"Downlifter", "Noise Drop", 60, 100},
    {"Impact", "Impact Boom", 36, 110},
}};
// The long noise riser sweeps over eight bars; downlifters and impacts ring
// out over two.
inline constexpr std::int64_t riser_bars = 8;
inline constexpr std::int64_t downlifter_bars = 2;
inline constexpr std::int64_t impact_bars = 2;
struct Transition {
  TransitionKind kind = TransitionKind::riser;
  std::int64_t start_bar = 0;
  std::int64_t bars = 0;
  std::string reason;
};

// One check per 16-bar line inside the arrangement: did something change
// in the 16 bars after it? Held by the template's own moves, added by the
// planner to keep the rule, or failed (nothing it could change).
enum class CheckStatus : std::uint8_t { held, added, failed };
struct Check {
  std::int64_t bar = 0;
  CheckStatus status = CheckStatus::held;
  std::string reason;
};

// A rule the plan keeps, with whether it holds.
struct Rule {
  std::string text;
  bool holds = true;
};

// A decision in plain words, at a bar, for the timeline's tooltips.
struct Decision {
  std::int64_t bar = 0;
  std::string text;
};

struct Plan {
  Options options;
  // The snapshot revision the plan was made from; a later edit stales it.
  std::uint64_t revision = 0;
  LoopRange loop{};
  std::int64_t length_bars = 0;
  std::vector<Section> sections;
  std::vector<TrackFinding> findings;
  std::vector<TrackPart> parts;
  std::vector<Sweep> sweeps;
  std::vector<Throw> throws;
  std::vector<Fill> fills;
  std::vector<Transition> transitions;
  std::vector<Check> checks;
  std::vector<Decision> decisions;
};

// The template's length in bars at the tempo of the project's first bar,
// rounded to the 8-bar grid.
[[nodiscard]] std::int64_t default_length_bars(Template style, const project::Snapshot& snapshot);
// The shortest length the template fits at this intro/outro length.
[[nodiscard]] std::int64_t min_length_bars(Template style, std::int64_t edge_bars);
// The template's sections over `length_bars`.
[[nodiscard]] foundation::Result<std::vector<Section>>
layout(Template style, std::int64_t length_bars, std::int64_t edge_bars, double breakdown);
// Moves the start of section `index` (1 .. size - 1) to `bar`, snapped to
// the 8-bar grid and kept between its neighbours with at least 8 bars each.
[[nodiscard]] std::vector<Section> move_edge(std::vector<Section> sections, std::size_t index,
                                             std::int64_t bar);
// The section at a bar, or the last.
[[nodiscard]] const Section& section_at(const std::vector<Section>& sections, std::int64_t bar);

[[nodiscard]] foundation::Result<Plan> plan(const project::Snapshot& snapshot, Options options);

// The rules as they hold for this plan: changes on the 8-bar grid,
// something new every 16 bars, beats only in the intro and outro, and
// locked tracks untouched (true by construction: they have no part).
[[nodiscard]] std::vector<Rule> rules(const Plan& plan);
// Bars where a part enters or leaves, over every part.
[[nodiscard]] std::vector<std::int64_t> part_changes(const TrackPart& part);

} // namespace oscillate::application::arrange
