// Generates the "Minimal Sub Pump" demo: eight bars of minimal at 124 BPM in
// A minor. An Anvil kick (Minimal Tick, tuned onto A1), a Torque sub and a
// Torque Moog bass an octave above it, both basses on one linked Pump keyed
// from the kick. Built through the project session and rendered through the
// offline master, the same paths the DAW uses.
#include "oscillate/application/project_session.hpp"
#include "oscillate/export/offline_master.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/native_dsp/synth/runtime.hpp"
#include "oscillate/persistence/store.hpp"

#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {
namespace fs = std::filesystem;
using namespace oscillate;

[[noreturn]] void fail(const std::string& message) {
  std::cerr << "minimal demo: " << message << '\n';
  std::exit(1);
}
template <class R> auto take(R result) {
  if (!result)
    fail(result.error().message);
  return std::move(result).value();
}
template <class R> void done(R result) {
  if (!result)
    fail(result.error().message);
}

class Gate final : public project::TransportGate {
public:
  class Lease final : public project::StoppedTransportLease {};
  foundation::Result<std::unique_ptr<project::StoppedTransportLease>> acquire_stopped() override {
    return foundation::Result<std::unique_ptr<project::StoppedTransportLease>>::success(
        std::make_unique<Lease>());
  }
};

constexpr std::int64_t quarter = timeline::ticks_per_quarter;
constexpr std::int64_t sixteenth = quarter / 4;
constexpr std::int64_t bar = 4 * quarter;
constexpr int bars = 8;
// Factory presets: Anvil "Minimal Tick", Torque "Dub Sub" and "Moog Bass".
constexpr std::size_t minimal_tick = 13;
constexpr std::size_t dub_sub = 14;
constexpr std::size_t moog_bass = 4;
// Faders: the sub sits about 5 dB under the kick below 80 Hz; the Moog bass
// carries the line on small speakers.
constexpr double sub_level = 0.17;
constexpr double bass_level = 0.5;

struct Hit {
  int step;
  int length;
  int key;
};
// A1 = 33, C2 = 36, D2 = 38, E2 = 40, G1 = 31: a rolling line between kicks.
const std::vector<Hit> roll{{2, 2, 33}, {6, 2, 33}, {10, 2, 33}, {14, 1, 33}, {15, 1, 33}};
const std::vector<Hit> lift{{2, 2, 33}, {6, 2, 33}, {9, 1, 36}, {10, 2, 36}, {14, 2, 31}};
const std::vector<Hit> drop{{2, 2, 31}, {6, 2, 31}, {10, 2, 33}, {14, 2, 33}};
const std::vector<Hit> climb{{2, 2, 33},  {6, 2, 33},  {10, 1, 36},
                             {12, 1, 38}, {13, 1, 40}, {14, 2, 38}};
const std::array<const std::vector<Hit>*, bars> line{&roll, &lift, &roll, &drop,
                                                     &roll, &lift, &roll, &climb};

project::ClipId new_clip(application::ProjectSession& session, project::TrackId track) {
  done(session.midi_clips().add_midi_clip(track, {0}));
  for (const auto& candidate : session.snapshot().tracks) {
    if (candidate.id == track) {
      const auto clip = candidate.midi_clips.back().id;
      done(session.midi_clips().resize_midi_clip(clip, {bars * bar}));
      return clip;
    }
  }
  fail("the new clip's track is missing");
}

void play_line(application::ProjectSession& session, project::ClipId clip, int transpose,
               std::int64_t release) {
  for (int index = 0; index < bars; ++index) {
    for (const auto& hit : *line[static_cast<std::size_t>(index)]) {
      done(session.notes().add_note_with_duration(clip, {index * bar + hit.step * sixteenth},
                                                  {hit.length * sixteenth - release},
                                                  static_cast<std::uint8_t>(hit.key + transpose)));
    }
  }
}

} // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: oscillate_minimal_demo PROJECT_FOLDER OUTPUT.wav\n";
    return 2;
  }
  const fs::path root = argv[1];
  const fs::path output = argv[2];
  if (fs::exists(root) || fs::exists(output))
    fail("refusing to replace an existing project or WAV");

  auto session = take(application::ProjectSession::create(root, {.default_returns = true}));
  done(session->song().set_tempo_event({0}, 124.0, timeline::TempoShape::step));
  done(session->song().set_project_key(project::MusicalKey{9, project::KeyMode::minor}));

  const auto kick = take(session->instruments().add_native_instrument_track("anvil", minimal_tick));
  done(session->tracks().set_track_name(kick, "Kick"));
  {
    // Tune to key: the preset's 54 Hz tail up onto A1 (55 Hz), the sub's root.
    auto values = take(session->instruments().native_instrument_values(kick));
    const auto& table = *native_dsp::synth::find_engine("anvil")->descriptor;
    for (std::size_t i = 0; i < table.parameters.size(); ++i) {
      if (table.parameters[i].id == "anvil.tune")
        values[i] += 12.0 * std::log2(55.0 / 54.0);
    }
    done(session->instruments().set_native_instrument_values(kick, values));
  }
  const auto kicks = new_clip(*session, kick);
  for (int beat = 0; beat < bars * 4; ++beat)
    done(session->notes().add_note_with_duration(kicks, {beat * quarter}, {quarter / 2}, 36));

  const auto sub = take(session->instruments().add_native_instrument_track("torque", dub_sub));
  done(session->tracks().set_track_name(sub, "Sub"));
  play_line(*session, new_clip(*session, sub), 0, sixteenth / 8);
  done(session->tracks().set_track_level_pan(sub, sub_level, 0.0));

  const auto bass = take(session->instruments().add_native_instrument_track("torque", moog_bass));
  done(session->tracks().set_track_name(bass, "Bass"));
  play_line(*session, new_clip(*session, bass), 12, sixteenth / 4);
  done(session->tracks().set_track_level_pan(bass, bass_level, 0.0));

  // Both basses duck under the kick on one linked Pump.
  const std::array targets{sub, bass};
  take(session->effects().duck_tracks_to(kick, targets));
  done(session->saving().save());

  Gate gate;
  auto store = take(persistence::ProjectStore::open_reader(root));
  const auto& snapshot = session->snapshot();
  auto authority = take(exporting::StoppedExportAuthority::acquire(snapshot, gate));
  take(exporting::render_wav_master(snapshot, store, std::move(authority),
                                    {output, exporting::WavFormat::float32}));
  std::cout << "Minimal Sub Pump: " << root.string() << " and " << output.string() << '\n';
  return 0;
}
