// Kick & Bass Lock A/B (ADR-0072): copies a project, renders it as it is,
// switches Lock on for every Anvil (kick) and Nova (bass) track and nothing
// else, renders again, and saves the copy with Lock on. With --solo it also
// renders kick and bass alone, both ways, for listening to the low end.
//   oscillate_lock_ab <project> <copy> <output directory> [--solo]
#include "oscillate/application/project_session.hpp"
#include "oscillate/export/offline_master.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/native_dsp/synth/runtime.hpp"
#include "oscillate/persistence/store.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {
namespace fs = std::filesystem;
using namespace oscillate;

[[noreturn]] void fail(const std::string& message) {
  std::cerr << "lock a/b: " << message << '\n';
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

struct Locked {
  project::TrackId track;
  std::string_view engine;
};

// Every Anvil and Nova track, with the parameter that switches its Lock.
std::vector<Locked> lock_tracks(const project::Snapshot& snapshot) {
  std::vector<Locked> found;
  for (const auto& track : snapshot.tracks) {
    const auto* patch = std::get_if<project::NativeInstrumentPatch>(&track.instrument_source);
    if (patch != nullptr && (patch->engine == "anvil" || patch->engine == "nova"))
      found.push_back({track.id, patch->engine == "anvil" ? "anvil.lock" : "nova.lock.on"});
  }
  return found;
}

void set_lock(application::ProjectSession& session, const std::vector<Locked>& tracks, bool on) {
  for (const auto& [track, parameter] : tracks) {
    const auto& snapshot = session.snapshot();
    const auto* patch = std::get_if<project::NativeInstrumentPatch>(
        &std::ranges::find(snapshot.tracks, track, &project::Track::id)->instrument_source);
    const auto& table = *native_dsp::synth::find_engine(patch->engine)->descriptor;
    auto values = take(session.instruments().native_instrument_values(track));
    for (std::size_t i = 0; i < table.parameters.size(); ++i) {
      if (table.parameters[i].id == parameter)
        values[i] = on ? 1.0 : 0.0;
    }
    done(session.instruments().set_native_instrument_values(track, values));
  }
}

void render(const application::ProjectSession& session, const fs::path& root,
            const fs::path& output) {
  Gate gate;
  auto store = take(persistence::ProjectStore::open_reader(root));
  const auto& snapshot = session.snapshot();
  auto authority = take(exporting::StoppedExportAuthority::acquire(snapshot, gate));
  take(exporting::render_wav_master(snapshot, store, std::move(authority),
                                    {output, exporting::WavFormat::float32}));
  std::cout << "  " << output.string() << '\n';
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 4)
    fail("usage: oscillate_lock_ab <project> <copy> <output directory> [--solo]");
  const fs::path source = argv[1];
  const fs::path copy = argv[2];
  const fs::path out = argv[3];
  const bool solo = argc > 4 && std::string_view(argv[4]) == "--solo";
  const bool stems = argc > 4 && std::string_view(argv[4]) == "--stems";
  const bool tracks_alone = argc > 4 && std::string_view(argv[4]) == "--tracks";
  if (fs::exists(copy))
    fail(copy.string() + " already exists");
  fs::copy(source, copy, fs::copy_options::recursive);
  fs::remove(copy / "state" / "WRITER.lock");
  fs::create_directories(out);
  const std::string name = copy.filename().string();

  auto session = take(application::ProjectSession::open(copy));
  const auto tracks = lock_tracks(session->snapshot());
  if (tracks.empty())
    fail("the project has no Anvil or Nova tracks");
  std::cout << name << ": " << tracks.size() << " tracks to lock\n";

  set_lock(*session, tracks, false);
  render(*session, copy, out / (name + " - Lock off.wav"));
  set_lock(*session, tracks, true);
  render(*session, copy, out / (name + " - Lock on.wav"));
  if (solo) {
    // Solo exactly the locked tracks, then put every solo back as it was.
    std::vector<std::pair<project::TrackId, bool>> before;
    for (const auto& track : session->snapshot().tracks)
      before.emplace_back(track.id, track.soloed);
    const auto set_solo = [&](project::TrackId track, bool on) {
      const auto& now = session->snapshot().tracks;
      if (std::ranges::find(now, track, &project::Track::id)->soloed != on)
        done(session->tracks().set_track_soloed(track, on));
    };
    for (const auto& [track, was] : before)
      set_solo(track,
               std::ranges::any_of(tracks, [&](const Locked& l) { return l.track == track; }));
    render(*session, copy, out / (name + " - kick and bass, Lock on.wav"));
    set_lock(*session, tracks, false);
    render(*session, copy, out / (name + " - kick and bass, Lock off.wav"));
    set_lock(*session, tracks, true);
    for (const auto& [track, was] : before)
      set_solo(track, was);
  }
  if (tracks_alone) {
    // Every track that was playing, alone, Lock off: what each adds to the mix.
    set_lock(*session, tracks, false);
    std::vector<project::TrackId> playing;
    for (const auto& track : session->snapshot().tracks)
      if (track.soloed && track.kind != project::TrackKind::return_bus)
        playing.push_back(track.id);
    for (std::size_t i = 0; i < playing.size(); ++i) {
      for (const auto& track : session->snapshot().tracks)
        if (track.kind != project::TrackKind::return_bus &&
            track.soloed != (track.id == playing[i]))
          done(session->tracks().set_track_soloed(track.id, track.id == playing[i]));
      render(*session, copy, out / ("track " + std::to_string(i) + ".wav"));
    }
    return 0;
  }
  if (stems) {
    // Each engine's tracks alone, both ways, for measuring how they combine.
    const auto solo_only = [&](std::string_view engine) {
      for (const auto& track : session->snapshot().tracks) {
        const auto* patch = std::get_if<project::NativeInstrumentPatch>(&track.instrument_source);
        const bool want = patch != nullptr && patch->engine == engine;
        if (track.soloed != want)
          done(session->tracks().set_track_soloed(track.id, want));
      }
    };
    if (argc > 5 && std::string_view(argv[5]) == "--dry") {
      // The bass's own effects bypassed, to hear Nova's Lock alone.
      for (const auto& track : session->snapshot().tracks) {
        const auto* patch = std::get_if<project::NativeInstrumentPatch>(&track.instrument_source);
        if (patch == nullptr || patch->engine != "nova")
          continue;
        for (const auto& insert : track.effects)
          if (insert.enabled)
            done(session->effects().set_effect_insert_enabled(track.id, insert.id, false));
      }
    }
    for (const bool on : {false, true}) {
      set_lock(*session, tracks, on);
      const std::string state = on ? "on" : "off";
      solo_only("anvil");
      render(*session, copy, out / ("kick, Lock " + state + ".wav"));
      solo_only("nova");
      render(*session, copy, out / ("bass, Lock " + state + ".wav"));
    }
    return 0;
  }
  done(session->saving().save());
  std::cout << "Saved " << copy.string() << " with Lock on\n";
  return 0;
}
