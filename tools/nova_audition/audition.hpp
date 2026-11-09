#pragma once

// Shared plumbing for the audition renders: error exits, a stopped
// transport for offline export, and a project rendered to one WAV.
#include "oscillate/application/project_session.hpp"
#include "oscillate/export/offline_master.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/persistence/store.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oscillate::tools::audition {

[[noreturn]] inline void fail(const std::string& message) {
  std::cerr << "nova audition: " << message << '\n';
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

inline std::size_t preset_index(const native_dsp::EngineDescriptor& engine,
                                std::string_view name) {
  for (std::size_t i = 0; i < engine.presets.size(); ++i)
    if (engine.presets[i].name == name)
      return i;
  fail("no " + std::string(engine.id) + " preset " + std::string(name));
}

inline const native_dsp::EngineDescriptor& synth(std::string_view engine) {
  const auto* found = native_dsp::synth::find_engine(engine);
  if (found == nullptr)
    fail("no synth " + std::string(engine));
  return *found->descriptor;
}

inline const native_dsp::EngineDescriptor& effect(std::string_view engine) {
  const auto* found = native_dsp::effects::find_engine(engine);
  if (found == nullptr)
    fail("no effect " + std::string(engine));
  return *found->descriptor;
}

inline std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    fail("cannot read " + path.string());
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// The project's master, its silent tail trimmed: 24-bit PCM unless another
// format is asked for. A zero `sample_rate` keeps the project's rate.
inline void render(const application::ProjectSession& session, const std::filesystem::path& root,
                   const std::filesystem::path& output, std::uint32_t sample_rate = 0,
                   exporting::WavFormat format = exporting::WavFormat::pcm24) {
  Gate gate;
  auto store = take(persistence::ProjectStore::open_reader(root));
  const auto& snapshot = session.snapshot();
  auto authority = take(exporting::StoppedExportAuthority::acquire(snapshot, gate));
  exporting::MasterRenderOptions options{output, format};
  options.sample_rate = sample_rate;
  options.trim_silent_tail = true;
  take(exporting::render_wav_master(snapshot, store, std::move(authority), options));
  std::cout << "  " << output.string() << '\n';
}

// Each instrument track alone, for checking the balance, as
// "<label> [stem <n>].wav" in 32-bit float.
inline void render_stems(application::ProjectSession& session, const std::filesystem::path& root,
                         const std::filesystem::path& output, const std::string& label,
                         std::uint32_t sample_rate) {
  std::vector<project::TrackId> tracks;
  for (const auto& track : session.snapshot().tracks)
    if (track.kind == project::TrackKind::instrument)
      tracks.push_back(track.id);
  for (std::size_t solo = 0; solo < tracks.size(); ++solo) {
    for (std::size_t i = 0; i < tracks.size(); ++i) {
      const bool mute = i != solo;
      if (session.snapshot().tracks[*session.tracks().track_index(tracks[i])].muted != mute)
        done(session.tracks().set_track_muted(tracks[i], mute));
    }
    render(session, root, output / (label + " [stem " + std::to_string(solo) + "].wav"),
           sample_rate, exporting::WavFormat::float32);
  }
}

} // namespace oscillate::tools::audition
