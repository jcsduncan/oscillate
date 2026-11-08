#pragma once

// Shared plumbing for the audition renders: error exits, a stopped
// transport for offline export, and a project rendered to one WAV.
#include "oscillate/application/project_session.hpp"
#include "oscillate/export/offline_master.hpp"
#include "oscillate/persistence/store.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

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

// The project's master as 24-bit PCM, its silent tail trimmed. A zero
// `sample_rate` keeps the project's rate.
inline void render(const application::ProjectSession& session, const std::filesystem::path& root,
                   const std::filesystem::path& output, std::uint32_t sample_rate = 0) {
  Gate gate;
  auto store = take(persistence::ProjectStore::open_reader(root));
  const auto& snapshot = session.snapshot();
  auto authority = take(exporting::StoppedExportAuthority::acquire(snapshot, gate));
  exporting::MasterRenderOptions options{output, exporting::WavFormat::pcm24};
  options.sample_rate = sample_rate;
  options.trim_silent_tail = true;
  take(exporting::render_wav_master(snapshot, store, std::move(authority), options));
  std::cout << "  " << output.string() << '\n';
}

} // namespace oscillate::tools::audition
