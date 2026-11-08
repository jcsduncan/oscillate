#include "oscillate/application/project_session.hpp"
#include "oscillate/application/routed_audio_source.hpp"
#include "oscillate/application/routed_playback_control.hpp"
#include "oscillate/audio_io/stereo_block.hpp"
#include "oscillate/engine/midi_events.hpp"
#include "oscillate/export/offline_master.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/native_dsp/synth/lock.hpp"
#include "oscillate/native_dsp/synth/parameters.hpp"
#include "oscillate/native_dsp/synth/player.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"
#include "oscillate/test/wav.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

// Kick & Bass Lock (ADR-0072) through the session, the player and export:
// one click locks a Nova bass and every unmuted Anvil kick as one undo
// step; a kept kick player follows a bass edit once re-offered; and a
// locked pair exports exactly as it plays live.
namespace {
namespace app = oscillate::application;
namespace audio = oscillate::audio_io;
namespace engine = oscillate::engine;
namespace exporting = oscillate::exporting;
namespace synth = oscillate::native_dsp::synth;
namespace foundation = oscillate::foundation;
namespace persistence = oscillate::persistence;
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
  Directory() : TempDir("oscillate-kick-lock") {}
};

class Gate final : public project::TransportGate {
public:
  class Lease final : public project::StoppedTransportLease {};
  foundation::Result<std::unique_ptr<project::StoppedTransportLease>> acquire_stopped() override {
    return foundation::Result<std::unique_ptr<project::StoppedTransportLease>>::success(
        std::make_unique<Lease>());
  }
};

constexpr auto quarter = timeline::ticks_per_quarter;
constexpr std::uint8_t kick_key = 36;
constexpr std::uint8_t root_key = 34;  // A#1: Anvil's tail takes its root.
constexpr std::uint8_t fifth_key = 40; // E2: Anvil's tail takes its fifth.

double value_of(app::ProjectSession& session, project::TrackId track, std::string_view id) {
  const auto values = take(session.instruments().native_instrument_values(track));
  const auto& found = *std::ranges::find(session.snapshot().tracks, track, &project::Track::id);
  const auto& patch = std::get<project::NativeInstrumentPatch>(found.instrument_source);
  const auto* engine = synth::find_engine(patch.engine);
  REQUIRE_NE(engine, nullptr, "the track's engine is registered");
  const auto index = synth::find_parameter(*engine->descriptor, id);
  REQUIRE_TRUE(index, "the engine has " + std::string(id));
  return values[*index];
}

// Sets an Anvil track's Lock choice (Off, Bass note or Keep tone).
void set_kick_lock(app::ProjectSession& session, project::TrackId kick, double choice) {
  auto values = take(session.instruments().native_instrument_values(kick));
  const auto* anvil = synth::find_engine(synth::lock_kick_engine);
  REQUIRE_NE(anvil, nullptr, "Anvil is registered");
  values[take(synth::find_parameter(*anvil->descriptor, synth::lock_kick_parameter))] = choice;
  accepted(session.instruments().set_native_instrument_values(kick, values));
}

project::ClipId first_clip(app::ProjectSession& session, project::TrackId track) {
  const auto row = session.tracks().track_index(track);
  REQUIRE_TRUE(row, "track row");
  return session.snapshot().tracks[*row].midi_clips.front().id;
}

struct Pair {
  project::TrackId kick;
  project::TrackId bass;
};

// An Anvil kick on every beat of a bar over a Nova bassline: a half note
// on the root key, then one on the fifth key.
constexpr double mix_level = 0.35;

Pair kick_and_bass(app::ProjectSession& session) {
  const auto kick =
      take(session.instruments().add_native_instrument_track(synth::lock_kick_engine, 0));
  const auto bass = take(
      session.instruments().add_native_instrument_track(synth::lock_bass_engine, std::nullopt));
  // Kept under full scale together: live output clamps to ±1, the float
  // export does not.
  accepted(session.tracks().set_track_level_pan(kick, mix_level, 0.0));
  accepted(session.tracks().set_track_level_pan(bass, mix_level, 0.0));
  accepted(session.midi_clips().add_midi_clip(kick, {0}));
  accepted(session.midi_clips().add_midi_clip(bass, {0}));
  const auto kicks = first_clip(session, kick);
  for (std::int64_t beat = 0; beat < 4; ++beat)
    accepted(
        session.notes().add_note_with_duration(kicks, {beat * quarter}, {quarter / 4}, kick_key));
  const auto line = first_clip(session, bass);
  accepted(session.notes().add_note_with_duration(line, {0}, {2 * quarter}, root_key));
  accepted(session.notes().add_note_with_duration(line, {2 * quarter}, {2 * quarter}, fifth_key));
  return {kick, bass};
}

// ---- One click ----------------------------------------------------------------------

void one_click_locks_the_pair_as_one_undo_step() {
  Directory directory;
  auto session = take(app::ProjectSession::create(directory.path() / "song"));
  const auto [kick, bass] = kick_and_bass(*session);
  const auto muted_kick =
      take(session->instruments().add_native_instrument_track(synth::lock_kick_engine, 0));
  accepted(session->tracks().set_track_muted(muted_kick, true));
  const auto other_bass = take(
      session->instruments().add_native_instrument_track(synth::lock_bass_engine, std::nullopt));

  auto state = app::kick_bass_lock_state(session->snapshot(), bass);
  REQUIRE_TRUE(state.offered, "a Nova track beside an Anvil track offers Lock");
  REQUIRE_TRUE(!state.locked, "a Nova track beside an Anvil track offers Lock");
  require(!app::kick_bass_lock_state(session->snapshot(), kick).offered,
          "an Anvil track does not offer the bass's Lock");

  const auto revision = session->snapshot().revision;
  require(!session->instruments().lock_kick_and_bass(kick, true).has_value(),
          "only a Nova track is a bass");
  REQUIRE_EQ(session->snapshot().revision, revision, "a refused lock changes nothing");

  const auto steps = session->history().undo_steps().size();
  accepted(session->instruments().lock_kick_and_bass(bass, true));
  REQUIRE_EQ(session->history().undo_steps().size(), steps + 1, "locking is one undo step");
  REQUIRE_EQ(value_of(*session, bass, synth::lock_bass_parameter), synth::lock_choice_on,
             "the bass and the kick lock together, the kick keeping its tone");
  REQUIRE_EQ(value_of(*session, kick, synth::lock_kick_parameter), synth::lock_kick_keep_tone,
             "the bass and the kick lock together, the kick keeping its tone");
  REQUIRE_EQ(value_of(*session, muted_kick, synth::lock_kick_parameter), synth::lock_choice_off,
             "a muted kick is left alone");
  REQUIRE_EQ(value_of(*session, other_bass, synth::lock_bass_parameter), synth::lock_choice_off,
             "another bass is left alone");
  require(app::kick_bass_lock_state(session->snapshot(), bass).locked, "the menu reads locked");

  const auto locked = session->snapshot().revision;
  accepted(session->instruments().lock_kick_and_bass(bass, true));
  REQUIRE_EQ(session->snapshot().revision, locked, "locking a locked pair changes nothing");

  accepted(session->history().undo());
  REQUIRE_EQ(value_of(*session, bass, synth::lock_bass_parameter), synth::lock_choice_off,
             "one undo unlocks both");
  REQUIRE_EQ(value_of(*session, kick, synth::lock_kick_parameter), synth::lock_choice_off,
             "one undo unlocks both");
  accepted(session->history().redo());
  REQUIRE_EQ(value_of(*session, bass, synth::lock_bass_parameter), synth::lock_choice_on,
             "one redo locks both again");
  REQUIRE_EQ(value_of(*session, kick, synth::lock_kick_parameter), synth::lock_kick_keep_tone,
             "one redo locks both again");

  accepted(session->instruments().lock_kick_and_bass(bass, false));
  REQUIRE_EQ(value_of(*session, bass, synth::lock_bass_parameter), synth::lock_choice_off,
             "unlocking releases both");
  REQUIRE_EQ(value_of(*session, kick, synth::lock_kick_parameter), synth::lock_choice_off,
             "unlocking releases both");
  require(!app::kick_bass_lock_state(session->snapshot(), bass).locked, "the menu reads unlocked");

  // A kick the user set to Bass note keeps it: the one click only turns
  // locks on, and either locked choice reads locked.
  set_kick_lock(*session, kick, synth::lock_kick_bass_note);
  accepted(session->instruments().lock_kick_and_bass(bass, true));
  REQUIRE_EQ(value_of(*session, kick, synth::lock_kick_parameter), synth::lock_kick_bass_note,
             "a kick on Bass note stays on Bass note");
  require(app::kick_bass_lock_state(session->snapshot(), bass).locked,
          "Bass note reads locked too");
}

void lock_is_offered_only_beside_an_anvil_track() {
  Directory directory;
  auto session = take(app::ProjectSession::create(directory.path() / "song"));
  const auto bass = take(
      session->instruments().add_native_instrument_track(synth::lock_bass_engine, std::nullopt));
  require(!app::kick_bass_lock_state(session->snapshot(), bass).offered,
          "without an Anvil track Lock is not offered");
  (void)take(session->instruments().add_native_instrument_track(synth::lock_kick_engine, 0));
  require(app::kick_bass_lock_state(session->snapshot(), bass).offered,
          "an Anvil track makes it offered");
}

// ---- The player follows a bass edit -------------------------------------------------

std::vector<float> render_kick(synth::NativeSynthPlayer& player, std::uint64_t frames) {
  constexpr std::size_t block = 128;
  std::vector<engine::StereoFrame> buffer;
  std::vector<float> out;
  for (std::uint64_t frame = 0; frame < frames; frame += block) {
    buffer.assign(std::min<std::uint64_t>(block, frames - frame), {});
    require(player.render(buffer, frame, 1), "the kick renders");
    for (const auto& sample : buffer)
      out.push_back(sample.left);
  }
  return out;
}

synth::NativeSynthPlayer kick_player(const project::Snapshot& snapshot, project::TrackId kick) {
  const auto plan = take(engine::compile_midi_events(snapshot));
  const auto events = std::ranges::find(plan.instruments(), kick, &engine::InstrumentEvents::track);
  REQUIRE_NE(events, plan.instruments().end(), "the kick has events");
  const auto& track = *std::ranges::find(snapshot.tracks, kick, &project::Track::id);
  return take(synth::NativeSynthPlayer::prepare(snapshot, track, *events, {}, 128,
                                                std::numeric_limits<std::uint64_t>::max()));
}

// A kick player kept across a bass edit tunes to the new notes once it is
// re-offered the snapshot (what live playback does after each publish), and
// renders exactly as one prepared from that snapshot.
void a_kept_kick_follows_a_bass_edit() {
  Directory directory;
  auto session = take(app::ProjectSession::create(directory.path() / "song"));
  const auto [kick, bass] = kick_and_bass(*session);
  accepted(session->instruments().lock_kick_and_bass(bass, true));
  // Bass note: the tail follows the bassline (the one click keeps the tone).
  set_kick_lock(*session, kick, synth::lock_kick_bass_note);
  const auto before = session->snapshot();
  // The bassline's first note moves from the root key to the fifth key.
  const auto line = first_clip(*session, bass);
  const auto& notes = session->snapshot()
                          .tracks[take(session->tracks().track_index(bass))]
                          .midi_clips.front()
                          .notes;
  const auto first = std::ranges::find(notes, root_key, &project::MidiNote::key)->id;
  accepted(session->notes().remove_note(line, first));
  accepted(session->notes().add_note_with_duration(line, {0}, {2 * quarter}, fifth_key));
  const auto after = session->snapshot();

  constexpr std::uint64_t frames = 96'000;
  auto kept = kick_player(before, kick);
  auto stale = kick_player(before, kick);
  auto fresh = kick_player(after, kick);
  accepted(kept.refresh_lock(after));
  const auto followed = render_kick(kept, frames);
  REQUIRE_EQ(followed, render_kick(fresh, frames), "the kept kick plays as a fresh one");
  REQUIRE_NE(followed, render_kick(stale, frames), "without the re-offer it plays the old bass");
}

// ---- Export equals live -------------------------------------------------------------

// Interleaved float32 samples from the exported WAV's data chunk.
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

std::vector<float> play_live(const fs::path& root, const project::Snapshot& saved,
                             std::size_t block_frames, std::size_t samples_wanted) {
  const auto tracks = saved.tracks.size();
  const engine::RoutedMixerLimits limits{{1, 64},
                                         block_frames,
                                         (tracks + 4) * block_frames * sizeof(engine::StereoFrame),
                                         (tracks + 4) * 65 * sizeof(engine::StereoFrame)};
  Gate gate;
  auto model = take(project::ProjectModel::open(saved, gate));
  app::RoutedPlaybackControl control(model, limits);
  REQUIRE_OK(control.submit_current(), "live graph prepared");
  auto source =
      take(app::RoutedAudioSource::prepare(root, *model.snapshot(), control, block_frames));
  audio::TransportCursor transport;
  std::vector<engine::StereoFrame> stereo(block_frames);
  std::vector<float> samples(2 * block_frames);
  const auto device = source->render_source();
  std::vector<float> live;
  live.reserve(samples_wanted + samples.size());
  while (live.size() < samples_wanted) {
    REQUIRE_EQ(audio::render_transport_block(device, transport, true, stereo, samples),
               audio::BlockResult::rendered, "live block rendered");
    live.insert(live.end(), samples.begin(), samples.end());
  }
  return live;
}

void a_locked_pair_exports_as_it_plays() {
  Directory directory;
  const auto root = directory.path() / "song";
  auto session = take(app::ProjectSession::create(root));
  const auto [kick, bass] = kick_and_bass(*session);
  (void)kick;
  accepted(session->saving().save());
  const auto unlocked = session->snapshot();
  const auto open = export_master(root, unlocked, directory.path() / "unlocked.wav");
  accepted(session->instruments().lock_kick_and_bass(bass, true));
  accepted(session->saving().save());
  const auto locked = session->snapshot();
  const auto exported = export_master(root, locked, directory.path() / "locked.wav");
  REQUIRE_NE(exported, open, "Lock changes the export");

  double peak = 0.0;
  for (const float sample : exported)
    peak = std::max(peak, std::fabs(static_cast<double>(sample)));
  REQUIRE_GT(peak, 0.01, "the export is audible and under full scale");
  REQUIRE_LT(peak, 1.0, "the export is audible and under full scale");
  for (const std::size_t block_frames : {std::size_t{64}, std::size_t{128}}) {
    const auto live = play_live(root, locked, block_frames, exported.size());
    double largest = 0.0;
    for (std::size_t i = 0; i < exported.size(); ++i)
      largest = std::max(largest, std::fabs(static_cast<double>(exported[i]) - live[i]));
    std::cout << block_frames << " frames: locked export vs live, largest difference " << largest
              << '\n';
    REQUIRE_LT(largest, 1e-6, "the locked export nulls against live playback");
  }
}

} // namespace

int main() {
  try {
    one_click_locks_the_pair_as_one_undo_step();
    lock_is_offered_only_beside_an_anvil_track();
    a_kept_kick_follows_a_bass_edit();
    a_locked_pair_exports_as_it_plays();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "kick and bass lock passed\n";
  return 0;
}
