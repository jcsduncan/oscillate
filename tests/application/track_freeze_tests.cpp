#include "oscillate/application/project_session.hpp"
#include "oscillate/export/offline_master.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/project/frozen_view.hpp"
#include "oscillate/routing/key_mute_lanes.hpp"
#include "oscillate/routing/project_route_plan.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/signal.hpp"
#include "oscillate/test/temp_dir.hpp"
#include "oscillate/test/wav.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

// ADR-0059: a track's render is what the track sounds before its channel;
// a frozen project exports what it exported unfrozen; bounce and flatten
// keep the mix; consolidate writes clips out as one.
namespace {
namespace app = oscillate::application;
namespace exporting = oscillate::exporting;
namespace foundation = oscillate::foundation;
namespace persistence = oscillate::persistence;
namespace project = oscillate::project;
namespace routing = oscillate::routing;
namespace timeline = oscillate::timeline;
namespace fs = std::filesystem;

constexpr std::int64_t bar = 4 * timeline::ticks_per_quarter;

using oscillate::test::require;

using oscillate::test::take;

void done(const foundation::Result<void>& result, std::string_view what) {
  if (!result)
    throw std::runtime_error(std::string(what) + ": " + result.error().message);
}

class Directory : public oscillate::test::TempDir {
public:
  Directory() : TempDir("oscillate-freeze") {}
};

class Gate final : public project::TransportGate {
public:
  class Lease final : public project::StoppedTransportLease {};
  foundation::Result<std::unique_ptr<project::StoppedTransportLease>> acquire_stopped() override {
    return foundation::Result<std::unique_ptr<project::StoppedTransportLease>>::success(
        std::make_unique<Lease>());
  }
};

using oscillate::test::read_float_wav;

std::vector<float> export_master(const fs::path& root, const project::Snapshot& saved,
                                 const fs::path& output) {
  Gate gate;
  auto store = take(persistence::ProjectStore::open_reader(root));
  auto authority = take(exporting::StoppedExportAuthority::acquire(saved, gate));
  REQUIRE_EQ(take(exporting::render_wav_master(saved, store, std::move(authority),
                                               {output, exporting::WavFormat::float32})),
             exporting::PublishOutcome::durable, "master export published");
  return read_float_wav(output);
}

// Equal where both have samples; whatever one has beyond the other is silent.
double largest_difference(const std::vector<float>& a, const std::vector<float>& b) {
  double largest = 0.0;
  const auto common = std::min(a.size(), b.size());
  for (std::size_t i = 0; i < common; ++i)
    largest = std::max(largest, std::fabs(static_cast<double>(a[i]) - b[i]));
  for (const auto* longer : {&a, &b})
    for (std::size_t i = common; i < longer->size(); ++i)
      largest = std::max(largest, std::fabs(static_cast<double>((*longer)[i])));
  return largest;
}

using oscillate::test::peak;

app::TrackRender render(app::ProjectSession& session, project::TrackId track,
                        std::optional<std::pair<std::uint64_t, std::uint64_t>> range = {}) {
  return take(take(session.freeze().begin_track_render(track, range)).run());
}

persistence::StagedWav stage(app::ProjectSession& session, const app::TrackRender& rendered) {
  auto staged = take(session.freeze().begin_track_render_stage(rendered));
  return take(std::move(staged.task).run());
}

void freeze(app::ProjectSession& session, project::TrackId track) {
  const auto rendered = render(session, track);
  done(session.freeze().finish_track_freeze(stage(session, rendered), rendered.origin_revision(),
                                            track, rendered.first_frame()),
       "freeze");
}

const project::Track& find(const project::Snapshot& snapshot, project::TrackId id) {
  const auto found = std::ranges::find(snapshot.tracks, id, &project::Track::id);
  REQUIRE_NE(found, snapshot.tracks.end(), "track present");
  return *found;
}

project::ClipId only_clip(const project::Snapshot& snapshot, project::TrackId track) {
  const auto found = std::ranges::find(snapshot.tracks, track, &project::Track::id);
  REQUIRE_NE(found, snapshot.tracks.end(), "track has a clip");
  REQUIRE_TRUE(!found->midi_clips.empty(), "track has a clip");
  return found->midi_clips.back().id;
}

// A synth track with notes from bar 2, a built-in effect, and a channel that
// is not unity: the render is before the channel.
project::TrackId add_synth(app::ProjectSession& session, std::uint8_t key) {
  const auto track = take(session.instruments().add_synth_track());
  done(session.instruments().select_native_instrument(track, "prism", 2), "synth preset");
  done(session.midi_clips().add_midi_clip(track, {bar}), "MIDI clip");
  const auto clip = only_clip(session.snapshot(), track);
  done(session.notes().add_note(clip, {0}, key), "note");
  done(session.notes().add_note(clip, {timeline::ticks_per_quarter}, key + 7), "note");
  (void)take(session.effects().add_effect_engine(project::SignalPathId{track}, 0, "shape"));
  return track;
}

void the_render_is_the_track_before_its_channel() {
  Directory directory;
  const auto root = directory.path() / "song";
  auto session = take(app::ProjectSession::create(root));
  const auto track = add_synth(*session, 48);
  done(session->saving().save(), "save");
  // With a unity channel and no master chain, the master is the track.
  const auto master = export_master(root, session->snapshot(), directory.path() / "master.wav");
  done(session->tracks().set_track_level_pan(track, 0.4, -0.6), "channel");
  const auto rendered = render(*session, track);
  const auto file = read_float_wav(fs::path{"/proc/self/fd"} / std::to_string(rendered.fd()));
  const auto& state = session->snapshot();
  const auto first = take(state.tempo.tick_to_sample({bar}, state.sample_rate));
  REQUIRE_EQ(rendered.first_frame(), static_cast<std::uint64_t>(first.value),
             "the render starts where the track first sounds");
  REQUIRE_GT(peak(file), 1e-3, "the render is audible");
  const auto offset = 2 * static_cast<std::size_t>(rendered.first_frame());
  REQUIRE_GE(master.size(), offset, "the master covers the render's start");
  REQUIRE_LT(peak(std::span<const float>(master).first(offset)), 1e-9,
             "nothing sounds before the render starts");
  const std::vector<float> tail(master.begin() + static_cast<std::ptrdiff_t>(offset), master.end());
  const auto largest = largest_difference(tail, file);
  std::cout << "render vs solo export: largest difference " << largest << '\n';
  REQUIRE_LT(largest, 1e-6, "the render is the track's own sound, before the channel");

  // A range renders exactly that range.
  const auto from = static_cast<std::uint64_t>(first.value) + 1'000;
  const auto ranged = render(*session, track, std::pair{from, from + 4'800});
  const auto part = read_float_wav(fs::path{"/proc/self/fd"} / std::to_string(ranged.fd()));
  REQUIRE_EQ(ranged.first_frame(), from, "a range renders exactly");
  REQUIRE_EQ(part.size(), 2 * 4'800, "a range renders exactly");
  REQUIRE_LT(largest_difference(part, {file.begin() + 2'000, file.begin() + 2'000 + 9'600}), 1e-6,
             "a range is the same sound");
}

void frozen_tracks_export_what_they_played() {
  Directory directory;
  const auto root = directory.path() / "song";
  auto session = take(app::ProjectSession::create(root));
  const auto lead = add_synth(*session, 60);
  const auto bass = add_synth(*session, 36);
  // The bass ducks to the lead through Pump: keys cross the freeze.
  const auto pump =
      take(session->effects().add_effect_engine(project::SignalPathId{bass}, 1, "pump"));
  (void)take(session->routing().add_sidechain(lead, bass, pump, 1.0));
  done(session->tracks().set_track_level_pan(lead, 0.7, -0.4), "lead channel");
  done(session->tracks().set_track_level_pan(bass, 1.3, 0.25), "bass channel");
  done(session->saving().save(), "save");
  const auto unfrozen = export_master(root, session->snapshot(), directory.path() / "unfrozen.wav");
  REQUIRE_GT(peak(unfrozen), 1e-3, "the mix is audible");

  freeze(*session, lead);
  REQUIRE_OK(find(session->snapshot(), lead).frozen, "the lead is frozen");
  const auto lead_frozen = export_master(root, session->snapshot(), directory.path() / "lead.wav");
  auto largest = largest_difference(unfrozen, lead_frozen);
  std::cout << "frozen key: largest difference " << largest << '\n';
  REQUIRE_LT(largest, 1e-6, "a frozen key exports what it played");

  freeze(*session, bass);
  const auto both = export_master(root, session->snapshot(), directory.path() / "both.wav");
  largest = largest_difference(unfrozen, both);
  std::cout << "frozen keyed track: largest difference " << largest << '\n';
  REQUIRE_LT(largest, 1e-6, "a frozen keyed track exports what it played");

  // The view runs no devices of a frozen track.
  const auto view = project::frozen_playback_view(session->snapshot());
  REQUIRE_TRUE(view, "a frozen project has a view");
  for (const auto id : {lead, bass}) {
    const auto& played = find(*view, id);
    REQUIRE_EQ(played.kind, project::TrackKind::audio, "a frozen track plays only its render");
    REQUIRE_TRUE(played.effects.empty(), "a frozen track plays only its render");
    REQUIRE_TRUE(played.midi_clips.empty(), "a frozen track plays only its render");
    REQUIRE_EQ(played.clips.size(), 1, "a frozen track plays only its render");
    REQUIRE_TRUE(!played.frozen, "a frozen track plays only its render");
  }
  require(find(*view, lead).sends.empty(), "keys into a frozen track go");

  // Frozen devices refuse edits; one undo unfreezes.
  require(
      !session->effects().add_effect_engine(project::SignalPathId{lead}, 0, "shape").has_value(),
      "a frozen track's devices take no edits");
  done(session->tracks().set_track_level_pan(lead, 0.5, 0.0), "the channel stays live");
  done(session->history().undo(), "undo the channel");
  done(session->history().undo(), "undo the bass freeze");
  REQUIRE_TRUE(!find(session->snapshot(), bass).frozen, "one undo unfreezes one track");
  REQUIRE_TRUE(find(session->snapshot(), lead).frozen, "one undo unfreezes one track");
  done(session->freeze().unfreeze_track(lead), "unfreeze");
  require(!project::has_frozen_track(session->snapshot()), "nothing frozen");
}

void a_stale_render_is_refused() {
  Directory directory;
  const auto root = directory.path() / "song";
  auto session = take(app::ProjectSession::create(root));
  const auto track = add_synth(*session, 52);
  done(session->saving().save(), "save");
  const auto tracks = session->snapshot().tracks.size();
  const auto rendered = render(*session, track);
  // Something audible changes while the render runs.
  done(session->notes().add_note(only_clip(session->snapshot(), track),
                                 {2 * timeline::ticks_per_quarter}, 55),
       "edit during the render");
  const auto refused = session->freeze().finish_track_freeze(
      stage(*session, rendered), rendered.origin_revision(), track, rendered.first_frame());
  REQUIRE_TRUE(!refused, "a stale freeze is refused");
  REQUIRE_EQ(refused.error().code, foundation::ErrorCode::conflict, "a stale freeze is refused");
  const auto bounce = session->freeze().finish_track_bounce(
      stage(*session, rendered), rendered.origin_revision(), track, rendered.first_frame(),
      app::BounceMode::new_track);
  REQUIRE_TRUE(!bounce, "a stale bounce is refused");
  REQUIRE_EQ(bounce.error().code, foundation::ErrorCode::conflict, "a stale bounce is refused");
  REQUIRE_TRUE(!project::has_frozen_track(session->snapshot()), "nothing changed");
  REQUIRE_EQ(session->snapshot().tracks.size(), tracks, "nothing changed");
}

void bounce_and_flatten_keep_the_mix() {
  Directory directory;
  const auto root = directory.path() / "song";
  auto session = take(app::ProjectSession::create(root));
  const auto track = add_synth(*session, 57);
  done(session->tracks().set_track_level_pan(track, 0.8, 0.3), "channel");
  // The track keys a compressor on another: the bounce keys it the same.
  constexpr std::uint8_t keyed_note = 45;
  const auto keyed = add_synth(*session, keyed_note);
  const auto press =
      take(session->effects().add_effect_engine(project::SignalPathId{keyed}, 0, "press"));
  (void)take(session->routing().add_sidechain(track, keyed, press, 1.0));
  done(session->saving().save(), "save");
  const auto before = export_master(root, session->snapshot(), directory.path() / "before.wav");
  const auto tracks = session->snapshot().tracks.size();

  // Bounce: a new audio track below, the source muted and kept.
  auto rendered = render(*session, track);
  const auto audio = take(session->freeze().finish_track_bounce(
      stage(*session, rendered), rendered.origin_revision(), track, rendered.first_frame(),
      app::BounceMode::new_track));
  {
    const auto& state = session->snapshot();
    const auto source = std::ranges::find(state.tracks, track, &project::Track::id);
    const auto copy = std::ranges::find(state.tracks, audio, &project::Track::id);
    REQUIRE_EQ(state.tracks.size(), tracks + 1,
               "the bounce is a new audio track below its muted source");
    REQUIRE_TRUE(source->muted, "the bounce is a new audio track below its muted source");
    REQUIRE_EQ(copy, source + 1, "the bounce is a new audio track below its muted source");
    REQUIRE_EQ(copy->kind, project::TrackKind::audio,
               "the bounce is a new audio track below its muted source");
    REQUIRE_EQ(copy->clips.size(), 1, "the bounce is a new audio track below its muted source");
    REQUIRE_EQ(copy->level, 0.8, "the bounce is a new audio track below its muted source");
    REQUIRE_EQ(copy->pan, 0.3, "the bounce is a new audio track below its muted source");
    REQUIRE_EQ(copy->sends.size(), 1, "the bounce keys what its muted source keyed");
  }
  const auto bounced = export_master(root, session->snapshot(), directory.path() / "bounce.wav");
  auto largest = largest_difference(before, bounced);
  std::cout << "bounce: largest difference " << largest << '\n';
  REQUIRE_LT(largest, 1e-6, "the bounce sounds as its source did");
  done(session->history().undo(), "undo the bounce");
  REQUIRE_EQ(session->snapshot().tracks.size(), tracks, "one undo removes the bounce");
  REQUIRE_TRUE(!session->snapshot().tracks.front().muted, "one undo removes the bounce");

  // Flatten: the audio takes the source's place.
  rendered = render(*session, track);
  const auto flat = take(session->freeze().finish_track_bounce(
      stage(*session, rendered), rendered.origin_revision(), track, rendered.first_frame(),
      app::BounceMode::flatten));
  {
    const auto& state = session->snapshot();
    REQUIRE_EQ(state.tracks.size(), tracks, "flatten replaces the track in place");
    REQUIRE_EQ(state.tracks.front().id, flat, "flatten replaces the track in place");
    REQUIRE_EQ(state.tracks.front().kind, project::TrackKind::audio,
               "flatten replaces the track in place");
    REQUIRE_TRUE(std::ranges::none_of(state.tracks,
                                      [&](const project::Track& item) { return item.id == track; }),
                 "flatten replaces the track in place");
  }
  const auto flattened = export_master(root, session->snapshot(), directory.path() / "flat.wav");
  largest = largest_difference(before, flattened);
  std::cout << "flatten: largest difference " << largest << '\n';
  REQUIRE_LT(largest, 1e-6, "the flattened track sounds as its source did");
  done(session->history().undo(), "undo the flatten");
  REQUIRE_EQ(session->snapshot().tracks.front().id, track, "one undo brings the devices back");
  REQUIRE_EQ(session->snapshot().tracks.front().effects.size(), 1,
             "one undo brings the devices back");
}

void consolidate_writes_clips_out() {
  Directory directory;
  auto session = take(app::ProjectSession::create(directory.path() / "song"));
  const auto track = take(session->instruments().add_synth_track());
  // A looped one-bar clip stretched to two bars, then a plain clip at bar 3.
  done(session->midi_clips().add_midi_clip(track, {0}), "first clip");
  const auto looped = only_clip(session->snapshot(), track);
  done(session->notes().add_note_with_duration(looped, {0}, {timeline::ticks_per_quarter}, 60),
       "note");
  done(session->midi_clips().resize_midi_clip(looped, {bar}), "one bar");
  done(session->midi_clips().set_midi_clip_loop(looped, true), "loop");
  done(session->midi_clips().resize_midi_clip(looped, {2 * bar}), "two bars");
  done(session->midi_clips().add_midi_clip(track, {2 * bar}), "second clip");
  const auto plain = only_clip(session->snapshot(), track);
  done(session->notes().add_note_with_duration(plain, {0}, {timeline::ticks_per_quarter}, 64),
       "note");
  const auto before = session->snapshot().revision;
  const auto clip = take(session->midi_clips().consolidate_midi_clips(track, {0}, {3 * bar}));
  const auto& state = session->snapshot();
  const auto& clips = state.tracks.front().midi_clips;
  REQUIRE_EQ(state.revision, before + 1, "one clip in one step");
  REQUIRE_EQ(clips.size(), 1, "one clip in one step");
  REQUIRE_EQ(clips.front().id, clip, "one clip in one step");
  const auto& merged = clips.front();
  const auto* placed = std::get_if<project::MusicalInterval>(&merged.placement);
  REQUIRE_TRUE(placed, "the clip spans both, unlooped");
  REQUIRE_EQ(placed->start.value, 0, "the clip spans both, unlooped");
  REQUIRE_TRUE(!merged.loop, "the clip spans both, unlooped");
  REQUIRE_EQ(merged.pattern_length.value, placed->end.value, "the clip spans both, unlooped");
  std::vector<std::pair<std::int64_t, int>> notes;
  notes.reserve(merged.notes.size());
  for (const auto& note : merged.notes)
    notes.emplace_back(note.start.value, note.key);
  const std::vector<std::pair<std::int64_t, int>> expected{{0, 60}, {bar, 60}, {2 * bar, 64}};
  REQUIRE_EQ(notes, expected, "the loop is written out and the second clip follows");
  done(session->history().undo(), "undo");
  REQUIRE_EQ(session->snapshot().tracks.front().midi_clips.size(), 2, "one undo restores both");
  require(!session->midi_clips().consolidate_midi_clips(track, {5 * bar}, {6 * bar}),
          "an empty selection is refused");
}

// An empty one-tick pattern looped over many bars repeats trillions of
// times: consolidating it writes an empty clip at once rather than walking
// every silent repeat.
void consolidating_a_silent_tiny_loop_is_bounded() {
  constexpr std::int64_t one_tick = 1;
  constexpr std::int64_t long_bars = 1'000;
  Directory directory;
  auto session = take(app::ProjectSession::create(directory.path() / "song"));
  const auto track = take(session->instruments().add_synth_track());
  done(session->midi_clips().add_midi_clip(track, {0}), "clip");
  const auto looped = only_clip(session->snapshot(), track);
  done(session->midi_clips().set_midi_clip_loop(looped, true), "loop");
  done(session->midi_clips().resize_midi_clip(looped, {long_bars * bar}), "many bars");
  done(session->midi_clips().replace_midi_clip_notes(looped, {one_tick}, {}), "one tick");
  const auto clip =
      take(session->midi_clips().consolidate_midi_clips(track, {0}, {long_bars * bar}));
  const auto& clips = session->snapshot().tracks.front().midi_clips;
  REQUIRE_EQ(clips.size(), 1, "the silent loop is written out as one clip");
  REQUIRE_EQ(clips.front().id, clip, "the silent loop is written out as one clip");
  REQUIRE_TRUE(clips.front().notes.empty(), "a silent loop writes no notes");
}

// A sounding loop whose written-out notes, or pedal points on one lane,
// would pass what a clip holds is refused, and nothing changes.
void consolidating_past_the_clip_limits_is_refused() {
  constexpr std::int64_t quarter = timeline::ticks_per_quarter;
  // A clip holds 4096 notes, and 4096 points on each pedal lane; one more
  // repeat than that passes both.
  constexpr std::size_t clip_limit = 4'096;
  static_assert(project::max_midi_notes_per_clip == clip_limit);
  static_assert(project::max_midi_pedal_points_per_lane == clip_limit);
  constexpr std::int64_t repeats = 4'097;
  constexpr int key = 60;
  constexpr double pedal_down = 1.0;
  const auto refused = [&](bool pedalled) {
    Directory directory;
    auto session = take(app::ProjectSession::create(directory.path() / "song"));
    const auto track = take(session->instruments().add_synth_track());
    done(session->midi_clips().add_midi_clip(track, {0}), "clip");
    const auto looped = only_clip(session->snapshot(), track);
    if (pedalled)
      done(session->pedals().set_midi_pedal_lane(looped, project::MidiPedal::sustain,
                                                 {{{0}, pedal_down}}),
           "pedal");
    else
      done(session->notes().add_note_with_duration(looped, {0}, {quarter / 2}, key), "note");
    done(session->midi_clips().resize_midi_clip(looped, {quarter}), "one quarter");
    done(session->midi_clips().set_midi_clip_loop(looped, true), "loop");
    done(session->midi_clips().resize_midi_clip(looped, {repeats * quarter}), "many repeats");
    const auto before = session->snapshot().revision;
    const auto result =
        session->midi_clips().consolidate_midi_clips(track, {0}, {repeats * quarter});
    REQUIRE_TRUE(!result, "a loop past the clip limits is refused");
    REQUIRE_EQ(result.error().code, foundation::ErrorCode::resource_exhausted,
               "a loop past the clip limits is refused");
    REQUIRE_EQ(session->snapshot().revision, before, "a refused consolidate changes nothing");
  };
  refused(false);
  refused(true);
}

} // namespace

// A bounce mutes its source. A mute lane would override that mute, and the
// bounce carries the lane, so the source's mute lanes move to the bounce:
// the source falls silent and keys nothing, and the bounce plays and keys
// under the lane, once (ADR-0099).
void a_bounced_source_under_a_mute_lane_falls_silent() {
  constexpr auto mute = project::TrackControlTarget::Key::channel_mute;
  constexpr double lane_unmuted = 0.0;
  constexpr double lane_muted = 1.0;
  // The notes sound in bar 1 (add_synth) and again in bar 3, where the lane
  // mutes them.
  constexpr std::int64_t open_bar = 1;
  constexpr std::int64_t muted_bar = 3;
  // Past the channel's 5 ms mute glide at the project rate.
  constexpr std::size_t past_the_glide = 960;
  constexpr double audible = 1e-3;
  constexpr double silent = 1e-6;
  constexpr std::uint8_t source_note = 57;
  constexpr std::uint8_t keyed_note = 45;
  Directory directory;
  const auto root = directory.path() / "song";
  auto session = take(app::ProjectSession::create(root));
  const auto track = add_synth(*session, source_note);
  done(session->midi_clips().add_midi_clip(track, {muted_bar * bar}), "muted clip");
  done(session->notes().add_note(only_clip(session->snapshot(), track), {0}, source_note),
       "muted note");
  const auto keyed = add_synth(*session, keyed_note);
  const auto press =
      take(session->effects().add_effect_engine(project::SignalPathId{keyed}, 0, "press"));
  (void)take(session->routing().add_sidechain(track, keyed, press, 1.0));
  (void)take(
      session->parameter_automation().set_channel_control_point(track, mute, 0, lane_unmuted));
  (void)take(session->parameter_automation().set_channel_control_point(track, mute, muted_bar,
                                                                       lane_muted));
  done(session->saving().save(), "save");
  const auto before = export_master(root, session->snapshot(), directory.path() / "before.wav");
  REQUIRE_GT(peak(before), audible, "the source sounds under its open lane");

  auto rendered = render(*session, track);
  // The render is before the channel: the bar-3 note sounds in it, and the
  // lane silences it in the mix.
  const auto& saved = session->snapshot();
  const auto muted_from =
      2 * static_cast<std::size_t>(
              take(saved.tempo.tick_to_sample({muted_bar * bar}, saved.sample_rate)).value) +
      2 * past_the_glide;
  // A quarter of a second of the bar-3 note, inside its render.
  constexpr std::size_t quarters_per_second = 4;
  const auto muted_to =
      muted_from + 2 * static_cast<std::size_t>(saved.sample_rate) / quarters_per_second;
  const auto file = read_float_wav(fs::path{"/proc/self/fd"} / std::to_string(rendered.fd()));
  const auto render_from = muted_from - 2 * static_cast<std::size_t>(rendered.first_frame());
  REQUIRE_GE(file.size(), render_from + (muted_to - muted_from), "the render covers bar 3");
  REQUIRE_GT(peak(std::span<const float>(file).subspan(render_from, muted_to - muted_from)),
             audible, "the source sounds in bar 3 before its channel");
  REQUIRE_GE(before.size(), muted_to, "the mix covers bar 3");
  REQUIRE_LT(peak(std::span<const float>(before).subspan(muted_from, muted_to - muted_from)),
             silent, "the lane mutes the source in bar 3");
  const auto audio = take(session->freeze().finish_track_bounce(
      stage(*session, rendered), rendered.origin_revision(), track, rendered.first_frame(),
      app::BounceMode::new_track));
  const auto bounced = export_master(root, session->snapshot(), directory.path() / "bounce.wav");
  const auto largest = largest_difference(before, bounced);
  std::cout << "bounce under a mute lane: largest difference " << largest << '\n';
  REQUIRE_LT(largest, silent, "the bounce plays once, as its source did");

  const auto& state = session->snapshot();
  const auto& source = find(state, track);
  const auto& copy = find(state, audio);
  const auto keys = [](const project::Track& item) {
    return std::ranges::count(item.sends, project::SendPurpose::sidechain,
                              &project::SendRoute::purpose);
  };
  REQUIRE_TRUE(source.muted, "the bounce mutes its source");
  REQUIRE_TRUE(!routing::has_mute_automation(source), "the source's mute lane moves to the bounce");
  REQUIRE_TRUE(routing::has_mute_automation(copy), "the source's mute lane moves to the bounce");
  REQUIRE_TRUE(routing::silent_key_sources(state).contains(track),
               "the muted source keys nothing, live and in export");
  REQUIRE_EQ(keys(copy), 1, "the bounce keys what its source keyed");
  const std::array<project::TrackId, 1> sources{audio};
  const auto lanes = take(routing::KeyMuteLanes::prepare(state, sources));
  const auto at_bar = [&](std::int64_t index) {
    return take(state.tempo.tick_to_sample({index * bar}, state.sample_rate));
  };
  REQUIRE_TRUE(!take(lanes.mutes(audio, at_bar(open_bar))),
               "the bounce keys where its lane is open");
  REQUIRE_TRUE(take(lanes.mutes(audio, at_bar(muted_bar))),
               "the bounce keys nothing where its lane mutes it");

  done(session->history().undo(), "undo the bounce");
  REQUIRE_TRUE(routing::has_mute_automation(find(session->snapshot(), track)),
               "one undo gives the source its lane back");
}

// A clip's channel envelope overrides the track's lane while the clip plays.
// A render is taken before the channel, and the bounce, the freeze and the
// flatten play it in slices that carry the clip's envelope at the clip's own
// place, so all three export what the source did (ADR-0102). The source keys
// a compressor before its fader, so the key hears no envelope either.
void a_clip_envelope_keeps_its_precedence_through_a_render() {
  constexpr auto level = project::TrackControlTarget::Key::channel_level;
  constexpr std::uint32_t rate = 48'000;
  constexpr std::uint16_t stereo = 2;
  constexpr std::size_t seconds = 3;
  constexpr double tone_hz = 220.0;
  constexpr float tone_peak = 0.5F;
  constexpr double two_pi = 2.0 * std::numbers::pi;
  constexpr double track_lane_level = 0.5;
  constexpr double clip_start_level = 1.5;
  constexpr double clip_end_level = 0.25;
  constexpr std::int64_t clip_end_bar = 1;
  constexpr std::uint8_t keyed_note = 45;
  constexpr double audible = 1e-3;
  constexpr double same_mix = 1e-6;
  Directory directory;
  const auto root = directory.path() / "song";
  auto session = take(app::ProjectSession::create(root));
  std::vector<float> tone(std::size_t{stereo} * rate * seconds);
  for (std::size_t frame = 0; frame < tone.size() / stereo; ++frame) {
    const auto value =
        tone_peak *
        static_cast<float>(std::sin(two_pi * tone_hz * static_cast<double>(frame) / rate));
    tone[stereo * frame] = value;
    tone[stereo * frame + 1] = value;
  }
  const auto wav = directory.path() / "tone.wav";
  oscillate::test::write_wav_float(wav, rate, stereo, tone);
  const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(fd, 0, "the tone opens");
  const auto track = take(session->imports().import_wav_audio(fd));
  (void)::close(fd);
  const auto clip = find(session->snapshot(), track).clips.front().id;
  (void)take(
      session->parameter_automation().set_channel_control_point(track, level, 0, track_lane_level));
  (void)take(session->automation().set_audio_clip_channel_level_point(clip, 0, clip_start_level));
  (void)take(
      session->automation().set_audio_clip_channel_level_point(clip, clip_end_bar, clip_end_level));
  const auto keyed = add_synth(*session, keyed_note);
  const auto press =
      take(session->effects().add_effect_engine(project::SignalPathId{keyed}, 0, "press"));
  (void)take(session->routing().add_sidechain(track, keyed, press, 1.0));
  done(session->saving().save(), "save");
  const auto before = export_master(root, session->snapshot(), directory.path() / "before.wav");
  REQUIRE_GT(peak(before), audible, "the mix is audible");
  const auto nulls = [&](std::string_view what) {
    const auto after =
        export_master(root, session->snapshot(), directory.path() / (std::string(what) + ".wav"));
    const auto largest = largest_difference(before, after);
    std::cout << what << " under a clip envelope: largest difference " << largest << '\n';
    REQUIRE_LT(largest, same_mix, std::string(what) + " keeps the clip envelope's precedence");
  };
  const auto carries_envelope = [](const project::Track& item) {
    return std::ranges::any_of(item.clips,
                               [](const project::Clip& part) { return !part.automation.empty(); });
  };

  auto rendered = render(*session, track);
  const auto audio = take(session->freeze().finish_track_bounce(
      stage(*session, rendered), rendered.origin_revision(), track, rendered.first_frame(),
      app::BounceMode::new_track));
  REQUIRE_TRUE(carries_envelope(find(session->snapshot(), audio)),
               "the bounce carries the clip's envelope");
  nulls("bounce");
  done(session->history().undo(), "undo the bounce");

  freeze(*session, track);
  const auto view = project::frozen_playback_view(session->snapshot());
  REQUIRE_TRUE(view && carries_envelope(find(*view, track)),
               "the frozen view carries the clip's envelope");
  nulls("freeze");
  done(session->freeze().unfreeze_track(track), "unfreeze");

  // A range that cuts the clip cannot place its envelope.
  constexpr std::pair<std::uint64_t, std::uint64_t> inside_the_clip{0, rate};
  REQUIRE_TRUE(!take(session->freeze().begin_track_render(track, inside_the_clip)).run(),
               "a range that cuts a clip's envelope is refused");

  rendered = render(*session, track);
  const auto flat = take(session->freeze().finish_track_bounce(
      stage(*session, rendered), rendered.origin_revision(), track, rendered.first_frame(),
      app::BounceMode::flatten));
  REQUIRE_TRUE(carries_envelope(find(session->snapshot(), flat)),
               "the flattened track carries the clip's envelope");
  nulls("flatten");
}

int main() {
  try {
    consolidating_a_silent_tiny_loop_is_bounded();
    consolidating_past_the_clip_limits_is_refused();
    the_render_is_the_track_before_its_channel();
    frozen_tracks_export_what_they_played();
    a_stale_render_is_refused();
    bounce_and_flatten_keep_the_mix();
    a_bounced_source_under_a_mute_lane_falls_silent();
    a_clip_envelope_keeps_its_precedence_through_a_render();
    consolidate_writes_clips_out();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "track freeze passed\n";
  return 0;
}
