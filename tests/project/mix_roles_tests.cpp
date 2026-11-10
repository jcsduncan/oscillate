// Master rebalance roles (ADR-0123): inference, groups and stages, the
// frozen view, and the commands that save roles and settings.

#include "oscillate/project/frozen_view.hpp"
#include "oscillate/project/mix_roles.hpp"
#include "oscillate/project/model.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace oscillate::project;
using oscillate::foundation::Result;
using oscillate::test::take;
using oscillate::timeline::MeterEvent;
using oscillate::timeline::MeterMap;
using oscillate::timeline::TempoEvent;
using oscillate::timeline::TempoMap;
using oscillate::timeline::TempoShape;

constexpr double bpm = 120.0;
constexpr std::uint8_t beats = 4;
constexpr std::int64_t note_ticks = 960;
constexpr std::uint8_t velocity = 100;
// A bass line around C2 and chords around C4.
constexpr std::array<std::uint8_t, 6> bass_line{36, 36, 43, 41, 38, 48};
constexpr std::array<std::uint8_t, 6> chord_notes{48, 55, 60, 64, 67, 72};
// A role setting just past its limit.
constexpr double past_limit_db = rebalance_gain_limit_db + 1.0;
constexpr double some_db = 3.0;

template <typename Id> Id id(std::uint64_t value) { return take(Id::from_bits({0, value})); }

class Gate final : public TransportGate {
public:
  struct Lease final : StoppedTransportLease {};
  Result<std::unique_ptr<StoppedTransportLease>> acquire_stopped() override {
    return Result<std::unique_ptr<StoppedTransportLease>>::success(std::make_unique<Lease>());
  }
};

Snapshot snapshot(std::vector<Track> tracks) {
  return {id<ProjectId>(1),
          1,
          oscillate::test::sample_rate_48k,
          take(TempoMap::create({TempoEvent{{0}, bpm, TempoShape::step}}, 1)),
          take(MeterMap::create({MeterEvent{{0}, beats, beats}}, 1)),
          std::move(tracks),
          {}};
}

Track instrument(std::uint64_t number, std::string_view engine, std::string name = {}) {
  Track track{id<TrackId>(number),
              {},
              TrackKind::instrument,
              NativeInstrumentPatch{std::string(engine), 1, {}}};
  track.name = std::move(name);
  return track;
}

Track audio(std::uint64_t number, std::string name = {}) {
  Track track{id<TrackId>(number)};
  track.name = std::move(name);
  return track;
}

Track bus(std::uint64_t number, TrackKind kind, std::string name = {}) {
  Track track{id<TrackId>(number), {}, kind};
  track.name = std::move(name);
  return track;
}

Track into(Track track, std::uint64_t group) {
  track.main_output = id<TrackId>(group);
  return track;
}

template <std::size_t N>
MidiClip notes(std::uint64_t clip, const std::array<std::uint8_t, N>& keys) {
  std::vector<MidiNote> list;
  for (std::size_t i = 0; i < N; ++i) {
    const auto start = static_cast<std::int64_t>(i) * note_ticks;
    list.push_back(
        {id<NoteId>(clip * 100 + i + 1), {start}, {start + note_ticks}, keys[i], velocity});
  }
  return {id<ClipId>(clip),
          ElapsedInterval{{0}, {1}},
          {static_cast<std::int64_t>(N) * note_ticks},
          false,
          std::move(list)};
}

const TrackRole& role_of(const std::vector<TrackRole>& roles, std::uint64_t number) {
  for (const auto& role : roles)
    if (role.track == id<TrackId>(number))
      return role;
  throw oscillate::test::TestFailure("no role for the track");
}

void devices_name_their_part() {
  const auto roles =
      mix_roles(snapshot({instrument(1, "stick"), instrument(2, "knock"), instrument(3, "anvil"),
                          instrument(4, "pulse"), instrument(5, "fret"), instrument(6, "torque"),
                          instrument(7, "nova"), instrument(8, "stick", "Vox")}));
  for (const std::uint64_t drums : {1U, 2U, 3U, 4U, 8U}) {
    REQUIRE_EQ(role_of(roles, drums).role, MixRole::drums, "a drum instrument plays drums");
    REQUIRE_EQ(role_of(roles, drums).reason, RoleReason::device);
  }
  REQUIRE_EQ(role_of(roles, 5).role, MixRole::bass, "Fret plays bass");
  REQUIRE_EQ(role_of(roles, 6).role, MixRole::bass, "Torque plays bass");
  REQUIRE_EQ(role_of(roles, 7).role, MixRole::music, "an untold synth is music");
  REQUIRE_EQ(role_of(roles, 7).reason, RoleReason::fallback);
}

void names_say_the_part() {
  REQUIRE_EQ(role_from_name("Lead Vox"), MixRole::vocals);
  REQUIRE_EQ(role_from_name("vox2"), MixRole::vocals, "digits split words");
  REQUIRE_EQ(role_from_name("Kick 2"), MixRole::drums);
  REQUIRE_EQ(role_from_name("Hi-Hat"), MixRole::drums, "punctuation splits words");
  REQUIRE_EQ(role_from_name("Sub Bass"), MixRole::bass);
  REQUIRE_EQ(role_from_name("Bass Drum"), MixRole::drums, "drums come before bass");
  REQUIRE_TRUE(!role_from_name("Subtle Pad"), "a word only matches whole");
  REQUIRE_TRUE(!role_from_name(""), "an empty name says nothing");

  auto clips = audio(2);
  for (const auto& [number, name] : std::vector<std::pair<std::uint64_t, std::string>>{
           {1, "Snare"}, {2, "Snare roll"}, {3, "Pad"}}) {
    Clip clip{id<ClipId>(number), id<SourceId>(1), SourceRange{{0}, {1}},
              ElapsedInterval{{0}, {1}}};
    clip.name = name;
    clips.clips.push_back(clip);
  }
  const auto roles = mix_roles(snapshot({audio(1, "Backing Vocals"), clips}));
  REQUIRE_EQ(role_of(roles, 1).role, MixRole::vocals);
  REQUIRE_EQ(role_of(roles, 1).reason, RoleReason::name);
  REQUIRE_EQ(role_of(roles, 2).role, MixRole::drums, "clip names vote");
}

void heard_and_notes_follow_names() {
  auto heard = audio(1);
  heard.heard_role = MixRole::bass;
  auto named = audio(2, "Kick");
  named.heard_role = MixRole::vocals;
  auto ignored = instrument(3, "nova");
  ignored.heard_role = MixRole::vocals;
  auto low = instrument(4, "nova");
  low.midi_clips.push_back(notes(1, bass_line));
  auto chords = instrument(5, "nova");
  chords.midi_clips.push_back(notes(2, chord_notes));
  const auto roles = mix_roles(snapshot({heard, named, ignored, low, chords}));
  REQUIRE_EQ(role_of(roles, 1).role, MixRole::bass);
  REQUIRE_EQ(role_of(roles, 1).reason, RoleReason::heard);
  REQUIRE_EQ(role_of(roles, 2).role, MixRole::drums, "a name says more than listening");
  REQUIRE_EQ(role_of(roles, 3).role, MixRole::music, "only audio tracks are heard");
  REQUIRE_EQ(role_of(roles, 4).role, MixRole::bass, "notes in the bass register");
  REQUIRE_EQ(role_of(roles, 4).reason, RoleReason::notes);
  REQUIRE_EQ(role_of(roles, 5).role, MixRole::music, "chords are not a bass line");
}

void stages_sit_on_the_tracks() {
  // Group 10 sums two drum tracks; group 20 sums drums and bass; group 40 a
  // drum track and one left out; return 30.
  auto chosen = audio(5);
  chosen.mix_role = MixRole::vocals;
  auto left_out = audio(7);
  left_out.mix_role = MixRole::none;
  const auto roles = mix_roles(
      snapshot({bus(10, TrackKind::group_bus), bus(20, TrackKind::group_bus),
                bus(30, TrackKind::return_bus, "Vocal Plate"), bus(40, TrackKind::group_bus),
                into(instrument(1, "stick"), 10), into(audio(2, "Snare"), 10),
                into(instrument(3, "knock"), 20), into(instrument(4, "fret"), 20), into(chosen, 20),
                into(instrument(6, "anvil"), 40), into(left_out, 40)}));
  REQUIRE_EQ(role_of(roles, 10).role, MixRole::drums, "a drum bus is drums");
  REQUIRE_EQ(role_of(roles, 10).reason, RoleReason::summed);
  REQUIRE_EQ(role_of(roles, 10).stage, MixRole::none, "a bus carries no stage");
  REQUIRE_EQ(role_of(roles, 1).stage, MixRole::drums, "each track carries its role's stage");
  REQUIRE_EQ(role_of(roles, 2).stage, MixRole::drums);
  REQUIRE_EQ(role_of(roles, 20).role, MixRole::none, "a mixed bus has no role");
  REQUIRE_EQ(role_of(roles, 3).stage, MixRole::drums);
  REQUIRE_EQ(role_of(roles, 4).stage, MixRole::bass);
  REQUIRE_EQ(role_of(roles, 5).stage, MixRole::vocals);
  REQUIRE_EQ(role_of(roles, 40).role, MixRole::none, "a track left out makes its bus mixed");
  REQUIRE_EQ(role_of(roles, 7).stage, MixRole::none, "a track left out has no stage");
  REQUIRE_EQ(role_of(roles, 30).role, MixRole::none, "a return has none, whatever its name");
  REQUIRE_EQ(role_of(roles, 30).stage, MixRole::none);
}

void a_chosen_group_role_passes_down() {
  auto group = bus(10, TrackKind::group_bus, "Stems");
  group.mix_role = MixRole::music;
  auto own = into(instrument(2, "fret"), 10);
  own.mix_role = MixRole::bass;
  auto nested = bus(11, TrackKind::group_bus);
  const auto roles =
      mix_roles(snapshot({group, into(nested, 10), into(instrument(1, "stick"), 11), own}));
  REQUIRE_EQ(role_of(roles, 1).role, MixRole::music, "a chosen bus role holds below it");
  REQUIRE_EQ(role_of(roles, 1).reason, RoleReason::group);
  REQUIRE_EQ(role_of(roles, 1).stage, MixRole::music, "the track carries the bus's role");
  REQUIRE_EQ(role_of(roles, 11).stage, MixRole::none);
  REQUIRE_EQ(role_of(roles, 10).stage, MixRole::none);
  REQUIRE_EQ(role_of(roles, 2).role, MixRole::bass, "a track's own choice wins");
  REQUIRE_EQ(role_of(roles, 2).stage, MixRole::bass);
}

void frozen_tracks_keep_their_role() {
  constexpr std::uint64_t encoded_bytes = 400'044;
  constexpr std::uint32_t channels = 2;
  constexpr std::uint64_t frames = 1'000;
  const auto rate = oscillate::test::sample_rate_48k;
  const std::string sha(64, 'a');
  // Its device says drums; once frozen it plays audio and its name says bass.
  auto drums = instrument(1, "stick", "Bass line");
  drums.frozen = FrozenTrack{{id<SourceId>(1), sha, encoded_bytes, rate, channels, frames}, 0, 1};
  auto saved = snapshot({drums});
  saved.sources.push_back(
      {id<SourceId>(1), SourceEncoding::wav, sha, encoded_bytes, rate, channels, frames});
  REQUIRE_EQ(mix_roles(saved)[0].role, MixRole::drums);
  const auto view = frozen_playback_view(saved);
  REQUIRE_TRUE(view.has_value(), "the frozen view exists");
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access): checked above.
  REQUIRE_EQ(view->tracks[0].kind, TrackKind::audio, "the frozen track plays audio");
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access): checked above.
  REQUIRE_EQ(mix_roles(*view)[0].role, MixRole::drums, "the view keeps the device's role");
}

void commands_save_roles_and_settings() {
  Gate gate;
  auto model =
      take(ProjectModel::create(id<ProjectId>(1), oscillate::test::sample_rate_48k,
                                take(TempoMap::create({TempoEvent{{0}, bpm, TempoShape::step}}, 1)),
                                take(MeterMap::create({MeterEvent{{0}, beats, beats}}, 1)), gate));
  const auto track = id<TrackId>(1);
  REQUIRE_OK(model.apply({AddTrack{track}}));
  REQUIRE_OK(model.apply({SetTrackMixRole{track, MixRole::vocals}}));
  REQUIRE_EQ(model.snapshot()->tracks[0].mix_role, MixRole::vocals);
  REQUIRE_TRUE(!model.apply({SetTrackMixRole{track, MixRole::vocals}}), "an unchanged role");
  REQUIRE_TRUE(!model.apply({SetTrackMixRole{id<TrackId>(2), MixRole::bass}}), "a missing track");
  // An out-of-range role is exactly what this checks.
  constexpr auto unknown_role = static_cast<std::uint8_t>(MixRole::none) + 1;
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
  REQUIRE_TRUE(!model.apply({SetTrackHeardRole{track, static_cast<MixRole>(unknown_role)}}),
               "an unknown role");
  REQUIRE_OK(model.apply({SetTrackHeardRole{track, MixRole::drums}}));
  REQUIRE_EQ(model.snapshot()->tracks[0].heard_role, MixRole::drums);

  MasterRebalance rebalance;
  rebalance.enabled = true;
  rebalance.roles[static_cast<std::size_t>(MixRole::drums)].gain_db = some_db;
  const auto before = model.snapshot()->audible_revision;
  auto applied = take(model.apply({SetMasterRebalance{rebalance}}));
  REQUIRE_EQ(model.snapshot()->rebalance, rebalance);
  REQUIRE_TRUE(applied.changes.routing, "a rebalance is heard");
  REQUIRE_TRUE(model.snapshot()->audible_revision > before, "a rebalance is audible");
  REQUIRE_TRUE(requires_routed_rendering(*model.snapshot()), "the stages run on routed nodes");
  REQUIRE_TRUE(!model.apply({SetMasterRebalance{rebalance}}), "an unchanged rebalance");
  for (const auto bad : {past_limit_db, -past_limit_db}) {
    auto wrong = rebalance;
    wrong.roles[0].mid_db = bad;
    REQUIRE_TRUE(!model.apply({SetMasterRebalance{wrong}}), "a gain past its limit");
  }
  auto wrong = rebalance;
  wrong.roles[0].low_hz = rebalance_low_max_hz + 1.0;
  REQUIRE_TRUE(!model.apply({SetMasterRebalance{wrong}}), "a frequency out of range");
  wrong = rebalance;
  wrong.roles[0].mid_q = rebalance_mid_min_q / 2.0;
  REQUIRE_TRUE(!model.apply({SetMasterRebalance{wrong}}), "a Q out of range");
  REQUIRE_OK(model.undo());
  REQUIRE_EQ(model.snapshot()->rebalance, MasterRebalance{}, "undo restores the rebalance");
}

} // namespace

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "mix role contract passed",
      {{"devices_name_their_part", devices_name_their_part},
       {"names_say_the_part", names_say_the_part},
       {"heard_and_notes_follow_names", heard_and_notes_follow_names},
       {"stages_sit_on_the_tracks", stages_sit_on_the_tracks},
       {"a_chosen_group_role_passes_down", a_chosen_group_role_passes_down},
       {"frozen_tracks_keep_their_role", frozen_tracks_keep_their_role},
       {"commands_save_roles_and_settings", commands_save_roles_and_settings}});
}
