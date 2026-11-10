// Master rebalance (ADR-0123). The done-when: a role's gain and EQ change on
// the master renders what the same change made on the role's tracks renders
// (their faders raised by the gain, Shape with the same bands last in their
// chains), with grouped tracks and sends; an unchanged or switched-off
// rebalance leaves a render bit for bit; export plays what live playback
// plays; listening to an audio track saves the part it plays.

#include "oscillate/application/project_session.hpp"
#include "oscillate/application/role_listening.hpp"
#include "oscillate/application/routed_audio_source.hpp"
#include "oscillate/audio_io/stereo_block.hpp"
#include "oscillate/export/offline_master.hpp"
#include "oscillate/native_dsp/effects/shape.hpp"
#include "oscillate/native_dsp/rebalance.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/project/mix_roles.hpp"
#include "oscillate/render_graph/rebalance.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"
#include "oscillate/test/wav.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <numbers>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
namespace app = oscillate::application;
namespace audio = oscillate::audio_io;
namespace engine = oscillate::engine;
namespace exporting = oscillate::exporting;
namespace foundation = oscillate::foundation;
namespace persistence = oscillate::persistence;
namespace project = oscillate::project;
namespace timeline = oscillate::timeline;
namespace shape = oscillate::native_dsp::effects::shape;
namespace match_eq = oscillate::native_dsp::match_eq;
namespace fs = std::filesystem;
using oscillate::test::take;

class Directory : public oscillate::test::TempDir {
public:
  Directory() : TempDir("oscillate-rebalance") {}
};

class Gate final : public project::TransportGate {
public:
  class Lease final : public project::StoppedTransportLease {};
  foundation::Result<std::unique_ptr<project::StoppedTransportLease>> acquire_stopped() override {
    return foundation::Result<std::unique_ptr<project::StoppedTransportLease>>::success(
        std::make_unique<Lease>());
  }
};

constexpr std::int64_t quarter = timeline::ticks_per_quarter;
constexpr std::int64_t song_quarters = 8;
constexpr std::uint8_t velocity = 100;
constexpr double send_gain = 0.5;
// The change made on the master (and, for the reference, on the tracks).
constexpr double drum_gain_db = 3.0;
constexpr double drum_low_db = 4.0;
constexpr double drum_low_hz = 100.0;
constexpr double bass_gain_db = -2.0;
constexpr double bass_mid_db = 3.0;
constexpr double bass_mid_hz = 250.0;
constexpr double bass_mid_q = 1.2;
constexpr double vocal_gain_db = 2.0;
constexpr double vocal_high_db = 3.0;
constexpr double vocal_high_hz = 8'000.0;
constexpr double music_gain_db = -2.0;
constexpr double decibels_per_decade = 20.0;
// The two renders sum and scale in a different order; nothing else differs.
constexpr double null_tolerance = 1e-6;
// Export against live, as the effect parity contract holds it.
constexpr double live_tolerance = 1e-6;
constexpr double audible_peak = 1e-3;
constexpr std::size_t live_block_frames = 128;
// The bass recording an audio track plays: a low three-partial line.
constexpr double recording_seconds = 6.0;
constexpr std::uint32_t recording_rate = 48'000;
constexpr double recording_hz = 55.0;
constexpr double recording_level = 0.3;
constexpr std::size_t recording_partials = 3;

struct SongFiles {
  Directory directory;
  fs::path root = directory.path() / "song";
};

struct SongContent {
  project::Snapshot saved;
  project::TrackId kick;
  project::TrackId snare;
  project::TrackId bass;
  project::TrackId vox;
  project::TrackId pad;
};

project::MidiNote note(std::uint64_t serial, std::int64_t start, std::int64_t length,
                       std::uint8_t key) {
  return {take(project::NoteId::from_bits({0, serial})), {start}, {start + length}, key, velocity};
}

project::TrackId synth(app::ProjectSession& session, std::string name,
                       std::vector<project::MidiNote> notes) {
  const auto track = take(session.instruments().add_synth_track());
  REQUIRE_OK(session.tracks().set_track_name(track, std::move(name)));
  (void)take(session.midi_clips().add_midi_clip_with_notes(track, {0}, {song_quarters * quarter},
                                                           std::move(notes)));
  return track;
}

// Kick and snare summed on a group bus, a bass, a voice and the kick with
// sends to a delay on a return, and a pad.
SongContent write_song(const fs::path& root) {
  auto session = take(app::ProjectSession::create(root));
  constexpr std::uint8_t kick_key = 36;
  constexpr std::uint8_t snare_key = 50;
  constexpr std::uint8_t bass_key = 33;
  constexpr std::uint8_t vox_key = 67;
  constexpr std::uint8_t pad_key = 60;
  std::vector<project::MidiNote> kicks;
  std::vector<project::MidiNote> snares;
  for (std::int64_t beat = 0; beat < song_quarters; ++beat) {
    kicks.push_back(
        note(static_cast<std::uint64_t>(beat) + 1, beat * quarter, quarter / 2, kick_key));
    snares.push_back(note(static_cast<std::uint64_t>(beat) + 1, beat * quarter + quarter / 2,
                          quarter / 4, snare_key));
  }
  const auto kick = synth(*session, "Kick", kicks);
  const auto snare = synth(*session, "Snare", snares);
  const auto bass = synth(*session, "Bass", {note(1, 0, 4 * quarter, bass_key)});
  const auto vox = synth(*session, "Vox", {note(1, quarter, 2 * quarter, vox_key)});
  const auto pad = synth(*session, "Pad", {note(1, 0, song_quarters * quarter, pad_key)});
  const auto drums = take(session->tracks().add_bus_track(project::TrackKind::group_bus));
  REQUIRE_OK(session->routing().set_main_output(kick, drums));
  REQUIRE_OK(session->routing().set_main_output(snare, drums));
  const auto echo = take(session->tracks().add_bus_track(project::TrackKind::return_bus));
  (void)take(session->effects().add_effect_engine(echo, 0, "relay"));
  (void)take(session->routing().add_send(vox, echo, send_gain));
  // A grouped track's send reaches the return past its group.
  (void)take(session->routing().add_send(kick, echo, send_gain));
  REQUIRE_OK(session->saving().save());
  return {session->snapshot(), kick, snare, bass, vox, pad};
}

struct Song : SongFiles, SongContent {
  Song() : SongContent(write_song(root)) {}
};

project::RoleBalance& role(project::MasterRebalance& rebalance, project::MixRole which) {
  return rebalance.roles[static_cast<std::size_t>(which)];
}

project::MasterRebalance change() {
  project::MasterRebalance rebalance;
  rebalance.enabled = true;
  auto& drums = role(rebalance, project::MixRole::drums);
  drums.gain_db = drum_gain_db;
  drums.low_db = drum_low_db;
  drums.low_hz = drum_low_hz;
  auto& bass = role(rebalance, project::MixRole::bass);
  bass.gain_db = bass_gain_db;
  bass.mid_db = bass_mid_db;
  bass.mid_hz = bass_mid_hz;
  bass.mid_q = bass_mid_q;
  auto& vocals = role(rebalance, project::MixRole::vocals);
  vocals.gain_db = vocal_gain_db;
  vocals.high_db = vocal_high_db;
  vocals.high_hz = vocal_high_hz;
  role(rebalance, project::MixRole::music).gain_db = music_gain_db;
  return rebalance;
}

// A copy of the song to change, at `name` beside it.
struct Variant {
  fs::path root;
  project::Snapshot saved;
};

fs::path copy_of(const Song& song, const std::string& name) {
  const auto root = song.directory.path() / name;
  fs::copy(song.root, root, fs::copy_options::recursive);
  return root;
}

// The song with `rebalance` saved.
Variant with_rebalance(const Song& song, const project::MasterRebalance& rebalance,
                       const std::string& name) {
  const auto root = copy_of(song, name);
  auto session = take(app::ProjectSession::open(root));
  REQUIRE_OK(session->tracks().set_master_rebalance(rebalance));
  REQUIRE_OK(session->saving().save());
  return {root, session->snapshot()};
}

// The same change made on the tracks themselves: each role's tracks (the
// grouped kick and snare, not their bus) get their fader raised by the
// role's gain and Shape with the role's bands last in their chain.
Variant changed_on_tracks(const Song& song, const project::MasterRebalance& rebalance) {
  const auto root = copy_of(song, "tracks");
  auto session = take(app::ProjectSession::open(root));
  const auto rate = static_cast<double>(song.saved.sample_rate);
  const auto change_track = [&](project::TrackId track, project::MixRole which) {
    const auto& balance = rebalance.roles[static_cast<std::size_t>(which)];
    const auto tone = oscillate::render_graph::role_tone(balance);
    const auto settings = oscillate::native_dsp::rebalance_bands_for(tone, rate);
    std::vector<match_eq::Band> bands;
    bands.reserve(settings.size());
    for (const auto& band : settings)
      bands.push_back({band.kind, band.frequency_hz, band.shape, band.gain_db});
    project::NativeEffectPatch patch{"shape", 1, {}};
    const auto values = shape::band_values(bands);
    patch.parameters.reserve(values.size());
    for (const auto& [id, value] : values)
      patch.parameters.push_back({std::string(id), value});
    const auto saved = *std::ranges::find(session->snapshot().tracks, track, &project::Track::id);
    (void)take(session->effects().add_native_effect(track, saved.effects.size(), patch));
    REQUIRE_OK(session->tracks().set_track_level_pan(
        track, saved.level * std::pow(10.0, balance.gain_db / decibels_per_decade), saved.pan));
  };
  change_track(song.kick, project::MixRole::drums);
  change_track(song.snare, project::MixRole::drums);
  change_track(song.bass, project::MixRole::bass);
  change_track(song.vox, project::MixRole::vocals);
  change_track(song.pad, project::MixRole::music);
  REQUIRE_OK(session->saving().save());
  return {root, session->snapshot()};
}

std::vector<float> render(const Song& song, const Variant& variant, const std::string& name) {
  const auto& snapshot = variant.saved;
  const auto output = song.directory.path() / (name + ".wav");
  Gate gate;
  auto store = take(persistence::ProjectStore::open_reader(variant.root));
  auto authority = take(exporting::StoppedExportAuthority::acquire(snapshot, gate));
  REQUIRE_EQ(take(exporting::render_wav_master(snapshot, store, std::move(authority),
                                               {output, exporting::WavFormat::float32})),
             exporting::PublishOutcome::durable, "the master export published");
  return oscillate::test::read_float_wav(output);
}

// Over the whole of both renders: a render that runs longer (an EQ's
// settling counts towards its tail) is compared against silence there.
double largest_difference(const std::vector<float>& a, const std::vector<float>& b) {
  double largest = 0.0;
  for (std::size_t i = 0; i < std::max(a.size(), b.size()); ++i) {
    const double left = i < a.size() ? a[i] : 0.0;
    const double right = i < b.size() ? b[i] : 0.0;
    largest = std::max(largest, std::fabs(left - right));
  }
  return largest;
}

double peak(const std::vector<float>& samples) {
  double top = 0.0;
  for (const float sample : samples)
    top = std::max(top, std::fabs(static_cast<double>(sample)));
  return top;
}

void the_song_has_its_roles(const Song& song) {
  const auto roles = project::mix_roles(song.saved);
  const auto stage = [&](project::TrackId track) {
    return std::ranges::find(roles, track, &project::TrackRole::track)->stage;
  };
  REQUIRE_EQ(stage(song.kick), project::MixRole::drums, "a grouped track carries its stage");
  REQUIRE_EQ(stage(song.bass), project::MixRole::bass);
  REQUIRE_EQ(stage(song.vox), project::MixRole::vocals);
  REQUIRE_EQ(stage(song.pad), project::MixRole::music);
}

void master_change_nulls_against_the_tracks() {
  Song song;
  the_song_has_its_roles(song);
  const auto original = render(song, {song.root, song.saved}, "original");
  const auto on_master = render(song, with_rebalance(song, change(), "master"), "master");
  const auto on_tracks = render(song, changed_on_tracks(song, change()), "tracks");
  REQUIRE_GT(peak(original), audible_peak, "the song is audible");
  REQUIRE_GT(largest_difference(original, on_master), audible_peak, "the change is heard");
  const auto difference = largest_difference(on_master, on_tracks);
  std::cout << "master against tracks: largest difference " << difference << " of peak "
            << peak(on_master) << '\n';
  REQUIRE_LT(difference, null_tolerance, "the master's change nulls against the tracks'");
}

void off_or_neutral_is_exact() {
  Song song;
  const auto original = render(song, {song.root, song.saved}, "original");
  auto off = change();
  off.enabled = false;
  REQUIRE_EQ(largest_difference(original, render(song, with_rebalance(song, off, "off"), "off")),
             0.0, "a rebalance switched off changes nothing");
  project::MasterRebalance neutral;
  neutral.enabled = true;
  REQUIRE_EQ(largest_difference(original,
                                render(song, with_rebalance(song, neutral, "neutral"), "neutral")),
             0.0, "a neutral rebalance changes nothing");
}

engine::RoutedMixerLimits limits() {
  constexpr std::uint32_t compensation = 64;
  constexpr std::uint64_t buffers = 16;
  return {{1, compensation},
          live_block_frames,
          buffers * live_block_frames * sizeof(engine::StereoFrame),
          buffers * (compensation + 1ULL) * sizeof(engine::StereoFrame)};
}

void export_matches_live() {
  Song song;
  const auto variant = with_rebalance(song, change(), "live");
  const auto exported = render(song, variant, "export");
  Gate gate;
  auto model = take(project::ProjectModel::open(variant.saved, gate));
  app::RoutedPlaybackControl control(model, limits());
  REQUIRE_OK(control.submit_current(), "the live graph prepared");
  auto source = take(
      app::RoutedAudioSource::prepare(variant.root, *model.snapshot(), control, live_block_frames));
  audio::TransportCursor transport;
  std::vector<engine::StereoFrame> stereo(live_block_frames);
  std::vector<float> samples(2 * live_block_frames);
  const auto device = source->render_source();
  const std::size_t offset = 2 * static_cast<std::size_t>(control.master_output_latency_frames());
  std::vector<float> live;
  while (live.size() < exported.size() + offset) {
    REQUIRE_EQ(audio::render_transport_block(device, transport, true, stereo, samples),
               audio::BlockResult::rendered, "a live block rendered");
    live.insert(live.end(), samples.begin(), samples.end());
  }
  double largest = 0.0;
  for (std::size_t i = 0; i < exported.size(); ++i)
    largest = std::max(largest, std::fabs(static_cast<double>(exported[i]) - live[offset + i]));
  std::cout << "export against live: largest difference " << largest << '\n';
  REQUIRE_LT(largest, live_tolerance, "the export nulls against live playback");
}

// A 16-bit stereo WAV of a low bass line.
fs::path write_recording(const fs::path& directory) {
  constexpr std::uint16_t channels = 2;
  constexpr std::uint16_t bits = 16;
  constexpr std::uint16_t pcm = 1;
  constexpr std::uint32_t format_bytes = 16;
  constexpr std::uint32_t header_bytes = 36;
  constexpr double full_scale = 32'767.0;
  const auto frames = static_cast<std::uint32_t>(recording_seconds * recording_rate);
  const std::uint32_t data_bytes = frames * channels * (bits / 8);
  const auto path = directory / "bass.wav";
  std::ofstream out(path, std::ios::binary);
  const auto put = [&out](std::uint32_t value, int bytes) {
    for (int i = 0; i < bytes; ++i)
      out.put(static_cast<char>((value >> (8 * i)) & 0xFFU));
  };
  out.write("RIFF", 4);
  put(header_bytes + data_bytes, 4);
  out.write("WAVEfmt ", 8);
  put(format_bytes, 4);
  put(pcm, 2);
  put(channels, 2);
  put(recording_rate, 4);
  put(recording_rate * channels * (bits / 8), 4);
  put(channels * (bits / 8), 2);
  put(bits, 2);
  out.write("data", 4);
  put(data_bytes, 4);
  for (std::uint32_t i = 0; i < frames; ++i) {
    const double phase = 2.0 * std::numbers::pi * recording_hz * i / recording_rate;
    double sum = 0.0;
    for (std::size_t h = 1; h <= recording_partials; ++h)
      sum += std::sin(static_cast<double>(h) * phase) / static_cast<double>(h);
    const auto sample = static_cast<std::int16_t>(std::lround(recording_level * sum * full_scale));
    put(static_cast<std::uint16_t>(sample), 2);
    put(static_cast<std::uint16_t>(sample), 2);
  }
  return path;
}

void listening_saves_the_heard_part() {
  Directory directory;
  const auto root = directory.path() / "song";
  auto session = take(app::ProjectSession::create(root));
  const auto wav = write_recording(directory.path());
  const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_TRUE(fd >= 0, "the recording opens");
  auto imported = session->imports().import_wav_audio(fd);
  (void)::close(fd);
  const auto track = take(std::move(imported));
  REQUIRE_OK(session->saving().save());
  const auto before = project::mix_roles(session->snapshot());
  REQUIRE_EQ(before.front().role, project::MixRole::music, "unheard, a recording is music");
  auto store = take(persistence::ProjectStore::open_reader(root));
  const auto heard = take(app::listen_for_roles(store, session->snapshot()));
  REQUIRE_EQ(heard.size(), 1U, "one audio track was heard");
  REQUIRE_OK(session->tracks().apply_heard_roles(heard));
  const auto& saved = *std::ranges::find(session->snapshot().tracks, track, &project::Track::id);
  REQUIRE_EQ(saved.heard_role, project::MixRole::bass, "the bass line was heard as bass");
  REQUIRE_EQ(project::mix_roles(session->snapshot()).front().reason, project::RoleReason::heard);
  REQUIRE_TRUE(take(app::listen_for_roles(store, session->snapshot())).empty(),
               "listening again changes nothing");
}

} // namespace

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "rebalance contract passed",
      {{"master_change_nulls_against_the_tracks", master_change_nulls_against_the_tracks},
       {"off_or_neutral_is_exact", off_or_neutral_is_exact},
       {"export_matches_live", export_matches_live},
       {"listening_saves_the_heard_part", listening_saves_the_heard_part}});
}
