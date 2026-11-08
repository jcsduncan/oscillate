#include "oscillate/application/arrange_from_loop.hpp"
#include "oscillate/application/project_session.hpp"
#include "oscillate/project/model.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"
#include "oscillate/timeline/timing.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

// Arrange from a loop (ADR-0080): roles read from a synthetic loop, the same
// plan for the same seed, every rule held across templates, locked tracks
// untouched, Keep as one undo step that undo takes back exactly, and
// transitions only when asked for.
namespace {
namespace app = oscillate::application;
namespace arrange = oscillate::application::arrange;
namespace foundation = oscillate::foundation;
namespace project = oscillate::project;
namespace timeline = oscillate::timeline;
namespace fs = std::filesystem;

using oscillate::test::require;

using oscillate::test::take;

void accepted(const foundation::Result<void>& result) {
  if (!result)
    throw std::runtime_error(result.error().message);
}

class Directory : public oscillate::test::TempDir {
public:
  Directory() : TempDir("oscillate-arrange") {}
};

constexpr auto quarter = timeline::ticks_per_quarter;
constexpr std::int64_t bar = 4 * quarter;
constexpr std::int64_t loop_bars = 16;
constexpr std::uint8_t velocity = 100;
constexpr double send_gain = 0.3;
// The loop's keys: a kick, a closed hat, a bassline's root and fifth (A1, E2),
// a two-note stab (A3, C4), a three-note pad (A3, C4, E4) and a high lead (E5).
constexpr std::uint8_t kick_key = 36;
constexpr std::uint8_t hat_key = 42;
constexpr std::uint8_t bass_root = 33;
constexpr std::uint8_t bass_fifth = 40;
constexpr std::uint8_t chord_low = 57;
constexpr std::uint8_t chord_mid = 60;
constexpr std::uint8_t chord_high = 64;
constexpr std::uint8_t lead_key = 76;

project::MidiNote note(std::uint64_t serial, std::int64_t start, std::int64_t length,
                       std::uint8_t key) {
  return {
      project::NoteId::from_bits({0, serial}).value(), {start}, {start + length}, key, velocity};
}

// A 16-bar loop at bar 1: an Anvil kick on every beat, Nova hats on the
// off-beats, a low Nova bassline, two-note stabs, three-note held pads, a
// single high lead, and an empty Nova track. The stab sends to a reverb.
struct Loop {
  std::unique_ptr<app::ProjectSession> session;
  project::TrackId kick, hats, bass, stab, pad, lead, empty, reverb;
};

project::TrackId midi_track(app::ProjectSession& session, std::string_view engine, std::string name,
                            const std::vector<project::MidiNote>& notes) {
  const auto track = take(session.instruments().add_native_instrument_track(engine, std::nullopt));
  if (!name.empty())
    accepted(session.tracks().set_track_name(track, std::move(name)));
  if (!notes.empty())
    static_cast<void>(
        take(session.midi_clips().add_midi_clip_with_notes(track, {0}, {loop_bars * bar}, notes)));
  return track;
}

Loop make_loop(const fs::path& root) {
  auto owned = take(app::ProjectSession::create(root / "song"));
  auto& session = *owned;
  std::vector<project::MidiNote> kicks, hats, bass, stabs, pads, lead;
  std::uint64_t serial = 1;
  for (std::int64_t beat = 0; beat < loop_bars * 4; ++beat) {
    kicks.push_back(note(serial++, beat * quarter, quarter / 4, kick_key));
    hats.push_back(note(serial++, beat * quarter + quarter / 2, quarter / 4, hat_key));
  }
  for (std::int64_t b = 0; b < loop_bars; ++b) {
    bass.push_back(note(serial++, b * bar, quarter / 2, bass_root));
    bass.push_back(note(serial++, b * bar + 2 * quarter, quarter / 2, bass_fifth));
    stabs.push_back(note(serial++, b * bar + 3 * quarter / 2, quarter / 4, chord_low));
    stabs.push_back(note(serial++, b * bar + 3 * quarter / 2, quarter / 4, chord_mid));
  }
  for (std::int64_t b = 0; b < loop_bars; b += 2)
    for (const std::uint8_t key : {chord_low, chord_mid, chord_high})
      pads.push_back(note(serial++, b * bar, 2 * bar, key));
  for (std::int64_t b = 0; b < loop_bars; b += 4)
    lead.push_back(note(serial++, b * bar + 3 * quarter, quarter, lead_key));
  const auto kick = midi_track(session, "anvil", "", kicks);
  const auto hat_track = midi_track(session, "nova", "Hats", hats);
  const auto bass_track = midi_track(session, "nova", "", bass);
  const auto stab = midi_track(session, "nova", "", stabs);
  const auto pad = midi_track(session, "nova", "", pads);
  const auto lead_track = midi_track(session, "nova", "", lead);
  const auto empty = midi_track(session, "nova", "Spare", {});
  const auto reverb = take(session.tracks().add_bus_track(project::TrackKind::return_bus));
  static_cast<void>(take(session.routing().add_send(stab, reverb, send_gain)));
  return {std::move(owned), kick, hat_track, bass_track, stab, pad, lead_track, empty, reverb};
}

arrange::Options options_for(arrange::Template style) {
  arrange::Options options;
  options.loop_start_bar = 1;
  options.loop_bars = loop_bars;
  options.style = style;
  return options;
}

const arrange::TrackFinding& finding(const arrange::Plan& plan, project::TrackId track) {
  const auto found = std::ranges::find(plan.findings, track, &arrange::TrackFinding::track);
  REQUIRE_NE(found, plan.findings.end(), "every audio and instrument track is found");
  return *found;
}

// ---- Roles ----------------------------------------------------------------------------

void roles_from_names_engines_and_notes() {
  Directory directory;
  auto loop = make_loop(directory.path());
  const auto plan =
      take(arrange::plan(loop.session->snapshot(), options_for(arrange::Template::dj_tool)));
  REQUIRE_EQ(finding(plan, loop.kick).role, arrange::Role::kick, "Anvil is the kick");
  REQUIRE_EQ(finding(plan, loop.hats).role, arrange::Role::hats, "a track named Hats is the hats");
  REQUIRE_EQ(finding(plan, loop.bass).role, arrange::Role::bass, "low single notes are the bass");
  REQUIRE_EQ(finding(plan, loop.stab).role, arrange::Role::stab, "short two-note chords are stabs");
  REQUIRE_EQ(finding(plan, loop.pad).role, arrange::Role::pad, "long three-note chords are a pad");
  REQUIRE_EQ(finding(plan, loop.lead).role, arrange::Role::lead, "a single high line is the lead");
  REQUIRE_TRUE(!finding(plan, loop.empty).has_content,
               "a track with nothing in the loop is left out");
  REQUIRE_TRUE(!finding(plan, loop.empty).arranged(),
               "a track with nothing in the loop is left out");
  require(
      std::ranges::none_of(plan.findings, [&](const auto& f) { return f.track == loop.reverb; }),
      "a return bus has no role");
  for (const auto& found : plan.findings)
    require(!found.evidence.empty(), "every role says why");

  auto corrected = options_for(arrange::Template::dj_tool);
  corrected.roles.emplace_back(loop.lead, arrange::Role::vocal);
  const auto again = take(arrange::plan(loop.session->snapshot(), corrected));
  REQUIRE_EQ(finding(again, loop.lead).role, arrange::Role::vocal,
             "the user's role wins and is marked as theirs");
  REQUIRE_TRUE(!finding(again, loop.lead).detected, "the user's role wins and is marked as theirs");
}

// ---- Determinism ----------------------------------------------------------------------

std::string fingerprint(const arrange::Plan& plan) {
  std::string text;
  for (const auto& section : plan.sections)
    text += std::to_string(static_cast<int>(section.kind)) + ":" +
            std::to_string(section.start_bar) + "+" + std::to_string(section.bars) + ";";
  for (const auto& part : plan.parts) {
    text += '|';
    for (const bool plays : part.plays)
      text += plays ? '1' : '0';
  }
  for (const auto& fill : plan.fills)
    text += "f" + std::to_string(fill.bar);
  for (const auto& check : plan.checks)
    text += "c" + std::to_string(check.bar) + std::to_string(static_cast<int>(check.status));
  for (const auto& sweep : plan.sweeps)
    text += "s" + std::to_string(sweep.start_bar) + "-" + std::to_string(sweep.end_bar);
  for (const auto& transition : plan.transitions)
    text += "t" + std::to_string(transition.start_bar);
  return text;
}

void same_seed_same_plan() {
  Directory directory;
  auto loop = make_loop(directory.path());
  for (std::size_t style = 0; style < arrange::template_count; ++style) {
    auto options = options_for(static_cast<arrange::Template>(style));
    options.variation = 1.0;
    options.seed = 41;
    const auto first = take(arrange::plan(loop.session->snapshot(), options));
    const auto second = take(arrange::plan(loop.session->snapshot(), options));
    REQUIRE_EQ(fingerprint(first), fingerprint(second), "the same seed gives the same plan");
    REQUIRE_EQ(first.decisions.size(), second.decisions.size(), "and the same reasons");
  }
}

// ---- Rules ----------------------------------------------------------------------------

void every_rule_holds() {
  Directory directory;
  auto loop = make_loop(directory.path());
  const auto& snapshot = loop.session->snapshot();
  for (std::size_t style = 0; style < arrange::template_count; ++style)
    for (const auto edge : {std::int64_t{16}, std::int64_t{32}})
      for (const double variation : {0.0, 0.5, 1.0})
        for (const std::uint64_t seed : {1U, 2U, 3U}) {
          auto options = options_for(static_cast<arrange::Template>(style));
          options.edge_bars = edge;
          options.variation = variation;
          options.seed = seed;
          options.length_bars = arrange::min_length_bars(options.style, edge) + 48;
          const auto plan = take(arrange::plan(snapshot, options));
          for (const auto& rule : arrange::rules(plan))
            require(rule.holds, "rule broken: " + rule.text);
          // The rules, checked here directly too.
          std::int64_t covered = 0;
          for (const auto& section : plan.sections) {
            REQUIRE_EQ(section.start_bar, covered, "sections follow each other on the 8-bar grid");
            REQUIRE_EQ(section.start_bar % arrange::phrase_bars, 0,
                       "sections follow each other on the 8-bar grid");
            covered += section.bars;
          }
          REQUIRE_EQ(covered, plan.length_bars, "the sections cover the length");
          for (const auto& part : plan.parts) {
            for (const auto change : arrange::part_changes(part)) {
              const bool kick_out =
                  part.role == arrange::Role::kick &&
                  std::ranges::any_of(plan.sections, [&](const auto& section) {
                    return section.start_bar == change + arrange::templates[style].kick_out_bars;
                  });
              require(change % arrange::phrase_bars == 0 || kick_out,
                      "a part enters or leaves on an 8-bar line, or kicks out before one");
            }
            for (const auto& section : plan.sections)
              if (section.kind == arrange::SectionKind::intro ||
                  section.kind == arrange::SectionKind::outro)
                for (auto b = section.start_bar; b < section.start_bar + section.bars; ++b)
                  require(arrange::is_beat(part.role) || !part.plays[static_cast<std::size_t>(b)],
                          "the intro and outro are beats only");
          }
          std::vector<std::int64_t> lines;
          for (auto b = arrange::change_bars; b < plan.length_bars; b += arrange::change_bars)
            lines.push_back(b);
          REQUIRE_EQ(plan.checks.size(), lines.size(), "one check per 16-bar line");
          for (const auto& check : plan.checks)
            REQUIRE_NE(check.status, arrange::CheckStatus::failed,
                       "something new every 16 bars: " + check.reason);
        }
  // A dragged edge snaps to the grid and keeps its neighbours.
  auto plan = take(arrange::plan(snapshot, options_for(arrange::Template::dj_tool)));
  const auto moved = arrange::move_edge(plan.sections, 2, plan.sections[2].start_bar + 11);
  REQUIRE_EQ(moved[2].start_bar % arrange::phrase_bars, 0, "a dragged edge snaps to 8 bars");
  REQUIRE_EQ(moved[1].bars + moved[2].bars, plan.sections[1].bars + plan.sections[2].bars,
             "the neighbours share the bars");
  auto dragged = options_for(arrange::Template::dj_tool);
  dragged.length_bars = plan.length_bars;
  dragged.sections = moved;
  const auto replanned = take(arrange::plan(snapshot, dragged));
  REQUIRE_EQ(replanned.sections, moved, "a dragged layout is kept");
  for (const auto& rule : arrange::rules(replanned))
    require(rule.holds, "rules hold after a drag: " + rule.text);
}

// ---- Keep, undo and locks -------------------------------------------------------------

void keep_is_one_step_and_locks_hold() {
  Directory directory;
  auto loop = make_loop(directory.path());
  auto& session = *loop.session;
  auto options = options_for(arrange::Template::hypnotic);
  options.locked.push_back(loop.pad);
  options.transitions = false;
  const auto before = session.snapshot();
  const auto pad_before = *std::ranges::find(before.tracks, loop.pad, &project::Track::id);
  const auto plan = take(arrange::plan(before, options));
  REQUIRE_TRUE(finding(plan, loop.pad).locked, "a locked track is not arranged");
  REQUIRE_TRUE(!finding(plan, loop.pad).arranged(), "a locked track is not arranged");
  require(plan.transitions.empty(), "no transitions unless asked for");

  const auto steps = session.history().undo_steps().size();
  accepted(session.song().apply_arrangement(plan));
  REQUIRE_EQ(session.history().undo_steps().size(), steps + 1, "Keep is one undo step");
  const auto after = session.snapshot();
  REQUIRE_EQ(*std::ranges::find(after.tracks, loop.pad, &project::Track::id), pad_before,
             "a locked track is untouched, byte for byte");
  REQUIRE_EQ(after.tracks.size(), before.tracks.size(), "no tracks added without transitions");
  const auto& kick = *std::ranges::find(after.tracks, loop.kick, &project::Track::id);
  REQUIRE_GT(kick.midi_clips.size(), 1, "the kick plays in several runs");
  for (const auto& clip : kick.midi_clips)
    require(clip.loop && clip.pattern_length.value == loop_bars * bar, "each run repeats the loop");
  const auto& stab = *std::ranges::find(after.tracks, loop.stab, &project::Track::id);
  require(std::ranges::any_of(stab.automation,
                              [](const auto& lane) {
                                return std::holds_alternative<project::SendGainTarget>(lane.target);
                              }),
          "the stab's reverb send is thrown");
  require(std::ranges::any_of(stab.effects,
                              [](const auto& insert) {
                                const auto* patch =
                                    std::get_if<project::NativeEffectPatch>(&insert.processor);
                                return patch != nullptr && patch->engine == "contour";
                              }),
          "the stab gets a Contour for its sweeps");
  REQUIRE_EQ(after.markers.size(), plan.sections.size(), "a marker per section");
  require(!session.song().apply_arrangement(plan), "a stale plan is refused");

  accepted(session.history().undo());
  REQUIRE_EQ(session.snapshot().tracks, before.tracks, "one undo restores every track exactly");
  REQUIRE_EQ(session.snapshot().markers, before.markers, "and the markers");
  accepted(session.history().redo());
  REQUIRE_EQ(session.snapshot().tracks, after.tracks, "redo brings the arrangement back");
}

void transitions_only_when_asked() {
  Directory directory;
  auto loop = make_loop(directory.path());
  auto& session = *loop.session;
  auto options = options_for(arrange::Template::peak_time);
  options.transitions = true;
  const auto count = session.snapshot().tracks.size();
  const auto plan = take(arrange::plan(session.snapshot(), options));
  require(!plan.transitions.empty(), "transitions are planned when asked for");
  accepted(session.song().apply_arrangement(plan));
  const auto& tracks = session.snapshot().tracks;
  REQUIRE_EQ(tracks.size(), count + arrange::transition_kind_count,
             "one new track per kind of transition");
  for (const auto& facts : arrange::transition_facts) {
    const auto found =
        std::ranges::find(tracks, std::string(facts.track_name), &project::Track::name);
    REQUIRE_NE(found, tracks.end(), "a " + std::string(facts.track_name) + " track");
    const auto* patch = std::get_if<project::NativeInstrumentPatch>(&found->instrument_source);
    REQUIRE_NE(patch, nullptr, "transitions play on Nova");
    REQUIRE_EQ(patch->engine, "nova", "transitions play on Nova");
    require(!found->midi_clips.empty(), "each transition track plays");
  }
}

} // namespace

int main(int argc, char** argv) {
  const std::vector<std::pair<std::string_view, void (*)()>> cases{
      {"roles", roles_from_names_engines_and_notes},
      {"determinism", same_seed_same_plan},
      {"rules", every_rule_holds},
      {"keep", keep_is_one_step_and_locks_hold},
      {"transitions", transitions_only_when_asked},
  };
  const std::vector<std::string_view> wanted(argv + 1, argv + argc);
  try {
    for (const auto& [name, run] : cases)
      if (wanted.empty() || std::ranges::find(wanted, name) != wanted.end())
        run();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "arrange from a loop contract passed\n";
  return 0;
}
