#include "oscillate/application/project_session.hpp"
#include "oscillate/application/routed_audio_source.hpp"
#include "oscillate/audio_io/stereo_block.hpp"
#include "oscillate/engine/midi_events.hpp"
#include "oscillate/export/offline_master.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/effects/pump.hpp"
#include "oscillate/native_dsp/synth/parameters.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"
#include "oscillate/test/tolerances.hpp"
#include "oscillate/test/wav.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Issue #248, ADR-0097: a key source's mute lane gates its keys. A lane
// mutes the kick for its middle bar of three; Pump, keyed by the kick's
// notes or by its audio, ducks only on the kicks of the bars the lane leaves
// audible, and export plays exactly what live playback plays.
namespace {
namespace app = oscillate::application;
namespace audio = oscillate::audio_io;
namespace engine = oscillate::engine;
namespace exporting = oscillate::exporting;
namespace foundation = oscillate::foundation;
namespace persistence = oscillate::persistence;
namespace project = oscillate::project;
namespace timeline = oscillate::timeline;
namespace synth = oscillate::native_dsp::synth;
namespace effects = oscillate::native_dsp::effects;
namespace fs = std::filesystem;
namespace tolerance = oscillate::test::tolerance;

using oscillate::test::take;

class Directory : public oscillate::test::TempDir {
public:
  Directory() : TempDir("oscillate-mute-lane-keys") {}
};

class Gate final : public project::TransportGate {
public:
  class Lease final : public project::StoppedTransportLease {};
  foundation::Result<std::unique_ptr<project::StoppedTransportLease>> acquire_stopped() override {
    return foundation::Result<std::unique_ptr<project::StoppedTransportLease>>::success(
        std::make_unique<Lease>());
  }
};

constexpr std::size_t block_frames = 64;
constexpr std::size_t stereo_channels = 2;
// The mixer's scratch and delay-history budgets, in blocks, as the other
// routed playback tests size them.
constexpr std::size_t scratch_blocks = 7;
constexpr std::size_t history_blocks = 5;

engine::RoutedMixerLimits limits() {
  return {{1, block_frames},
          block_frames,
          scratch_blocks * block_frames * sizeof(engine::StereoFrame),
          history_blocks * (block_frames + 1) * sizeof(engine::StereoFrame)};
}

// Three bars of kicks under a held bass and pad; the lane mutes the middle
// bar. Bars are the project's default four beats.
constexpr std::size_t beats_per_bar = 4;
constexpr std::size_t bars = 3;
constexpr std::size_t kick_beats = bars * beats_per_bar;
constexpr std::int64_t muted_bar = 1;
constexpr double lane_unmuted = 0.0;
constexpr double lane_muted = 1.0;
constexpr std::uint8_t kick_key = 36;
constexpr std::uint8_t kick_velocity = 127;
constexpr std::int64_t kick_note_ticks = timeline::ticks_per_quarter / 4;
constexpr std::size_t hard_gate_preset = 3;
constexpr double kick_audio_trigger = 1.0;
constexpr double midi_trigger = 2.0;
// A Hard Gate duck goes far below this; no duck stays above the other.
constexpr float ducked_db = -20.0F;
constexpr float unducked_db = -0.1F;
// Each beat's window runs from its kick to the next. The kicks on the lane's
// steps (the first of the muted bar and of the bar after it) sit on the
// mute's short glide, so the checks take the beats inside each bar.
constexpr std::array<std::size_t, 5> audible_beats{1, 2, 3, 9, 10};
constexpr std::array<std::size_t, 2> muted_beats{5, 6};

struct Song {
  project::Snapshot saved;
  project::TrackId kick;
  project::TrackId bass;
  project::InsertSlotId slot;
};

Song song(const fs::path& root, double trigger) {
  auto session = take(app::ProjectSession::create(root));
  const auto kick = take(session->instruments().add_synth_track());
  const auto bass = take(session->instruments().add_synth_track());
  const auto pad = take(session->instruments().add_synth_track());
  const timeline::TickPosition length{static_cast<std::int64_t>(kick_beats) *
                                      timeline::ticks_per_quarter};
  std::uint64_t serial = 0;
  for (const auto held : {bass, pad}) {
    const project::MidiNote note{
        take(project::NoteId::from_bits({0, ++serial})), {0}, length, kick_key, kick_velocity};
    (void)take(session->midi_clips().add_midi_clip_with_notes(held, {0}, length, {note}));
  }
  std::vector<project::MidiNote> notes;
  for (std::size_t beat = 0; beat < kick_beats; ++beat) {
    const auto start = static_cast<std::int64_t>(beat) * timeline::ticks_per_quarter;
    notes.push_back({take(project::NoteId::from_bits({0, ++serial})),
                     {start},
                     {start + kick_note_ticks},
                     kick_key,
                     kick_velocity});
  }
  (void)take(session->midi_clips().add_midi_clip_with_notes(kick, {0}, length, notes));
  const auto slot = take(session->effects().add_effect_engine(bass, 0, "pump", hard_gate_preset));
  (void)take(session->routing().add_sidechain(kick, bass, slot, 1.0));
  auto values = take(session->effects().native_effect_values(bass, slot));
  const auto trigger_index =
      synth::find_parameter(*effects::find_engine("pump")->descriptor, "pump.trigger");
  REQUIRE_TRUE(trigger_index.has_value(), "Pump has a trigger parameter");
  values[*trigger_index] = trigger;
  REQUIRE_OK(session->effects().set_native_effect_values(bass, slot, values), "Pump's trigger");
  constexpr auto mute = project::TrackControlTarget::Key::channel_mute;
  auto& lanes = session->parameter_automation();
  (void)take(lanes.set_channel_control_point(kick, mute, 0, lane_unmuted));
  (void)take(lanes.set_channel_control_point(kick, mute, muted_bar, lane_muted));
  (void)take(lanes.set_channel_control_point(kick, mute, muted_bar + 1, lane_unmuted));
  REQUIRE_OK(session->saving().save(), "project did not save");
  return {session->snapshot(), kick, bass, slot};
}

// The kick's note-on frames, in order.
std::vector<std::uint64_t> kick_frames(const Song& keyed) {
  const auto plan = take(engine::compile_midi_events(keyed.saved));
  std::vector<std::uint64_t> frames;
  for (const auto& instrument : plan.instruments()) {
    if (instrument.track() != keyed.kick)
      continue;
    for (const auto& event : instrument.events()) {
      if (event.kind == engine::MidiEventKind::note_on)
        frames.push_back(static_cast<std::uint64_t>(event.sample.value));
    }
  }
  std::ranges::sort(frames);
  REQUIRE_EQ(frames.size(), kick_beats, "every kick is on the timeline");
  return frames;
}

struct Live {
  Gate gate;
  project::ProjectModel model;
  app::RoutedPlaybackControl control;
  std::unique_ptr<app::RoutedAudioSource> source;
  audio::TransportCursor transport;
  std::array<engine::StereoFrame, block_frames> stereo{};
  std::array<float, stereo_channels * block_frames> samples{};
  std::uint64_t rendered_frames = 0;

  Live(const fs::path& root, const project::Snapshot& saved)
      : model(take(project::ProjectModel::open(saved, gate))), control(model, limits()) {
    REQUIRE_OK(control.submit_current(), "graph did not prepare");
    source = take(app::RoutedAudioSource::prepare(root, *model.snapshot(), control, block_frames));
    control.bind_live_source(*source);
  }
  Live(const Live&) = delete;
  Live& operator=(const Live&) = delete;
  ~Live() { control.unbind_live_source(); }

  void render() {
    REQUIRE_EQ(
        audio::render_transport_block(source->render_source(), transport, true, stereo, samples),
        audio::BlockResult::rendered, "block failed to render");
    rendered_frames += stereo.size();
  }
};

// Renders up to `frame` and returns Pump's deepest gain on the way, in dB.
float deepest_duck_until(Live& live, const Song& keyed, std::uint64_t frame) {
  float deepest = 0.0F;
  std::vector<float> telemetry;
  while (live.rendered_frames < frame) {
    live.render();
    REQUIRE_TRUE(live.control.active_effect_telemetry(keyed.bass, keyed.slot, telemetry),
                 "Pump reports its gain");
    deepest = std::min(deepest, telemetry[effects::pump::telemetry::gain_db]);
  }
  return deepest;
}

// The master export nulls against live playback of the same revision.
void export_nulls_against_live(const fs::path& root, const Song& keyed, const fs::path& output,
                               const std::string& name) {
  {
    Gate gate;
    auto store = take(persistence::ProjectStore::open_reader(root));
    auto authority = take(exporting::StoppedExportAuthority::acquire(keyed.saved, gate));
    REQUIRE_EQ(take(exporting::render_wav_master(keyed.saved, store, std::move(authority),
                                                 {output, exporting::WavFormat::float32})),
               exporting::PublishOutcome::durable, name + ": master export published");
  }
  const auto exported = oscillate::test::read_float_wav(output);
  Live live(root, keyed.saved);
  const auto offset =
      stereo_channels * static_cast<std::size_t>(live.control.master_output_latency_frames());
  std::vector<float> played;
  played.reserve(exported.size() + offset + live.samples.size());
  while (played.size() < exported.size() + offset) {
    live.render();
    played.insert(played.end(), live.samples.begin(), live.samples.end());
  }
  double largest = 0.0;
  for (std::size_t index = 0; index < exported.size(); ++index)
    largest = std::max(largest, std::fabs(static_cast<double>(exported[index]) -
                                          static_cast<double>(played[offset + index])));
  REQUIRE_LT(largest, tolerance::float_round_trip,
             name + ": the export nulls against live playback");
}

void pump_ducks_only_where_the_lane_leaves_the_kick(double trigger, std::string_view mode) {
  const std::string name(mode);
  Directory directory;
  const auto root = directory.path() / "song";
  const auto keyed = song(root, trigger);
  const auto beats = kick_frames(keyed);
  {
    Live live(root, keyed.saved);
    std::vector<float> deepest(beats.size() - 1);
    (void)deepest_duck_until(live, keyed, beats.front());
    for (std::size_t beat = 0; beat < deepest.size(); ++beat)
      deepest[beat] = deepest_duck_until(live, keyed, beats[beat + 1]);
    for (const auto beat : audible_beats)
      REQUIRE_LT(deepest[beat], ducked_db,
                 name + ": Pump ducks to a kick the lane leaves audible, beat " +
                     std::to_string(beat));
    for (const auto beat : muted_beats)
      REQUIRE_GT(deepest[beat], unducked_db,
                 name + ": a kick the lane mutes does not duck, beat " + std::to_string(beat));
  }
  export_nulls_against_live(root, keyed, directory.path() / "master.wav", name);
}

} // namespace

int main() {
  try {
    pump_ducks_only_where_the_lane_leaves_the_kick(midi_trigger, "MIDI");
    pump_ducks_only_where_the_lane_leaves_the_kick(kick_audio_trigger, "Kick Audio");
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "mute lane keys passed\n";
  return 0;
}
