#include "oscillate/test/check.hpp"

#include <oscillate/application/project_session.hpp>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <iostream>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <variant>

namespace {
namespace app = oscillate::application;
namespace foundation = oscillate::foundation;
namespace project = oscillate::project;

using oscillate::test::take;

int run(int argc, char** argv) {
  if (argc == 3 && std::string_view(argv[1]) == "inspect") {
    auto session = take(app::ProjectSession::open(argv[2]));
    const auto& snapshot = session->snapshot();
    std::cout << "revision=" << snapshot.revision
              << " master_effects=" << snapshot.master_effects.size();
    for (const auto& track : snapshot.tracks)
      for (const auto& send : track.sends)
        std::cout << " send=" << send.id.to_string() << " gain=" << send.gain;
    std::cout << '\n';
    return 0;
  }
  const bool encoded = argc == 4 && std::string_view(argv[1]) == "create-encoded";
  if (argc != 4 || (!encoded && std::string_view(argv[1]) != "create")) {
    std::cerr << "Usage: routed_live_fixture create PROJECT WAV | create-encoded PROJECT AUDIO | "
                 "inspect PROJECT\n";
    return 2;
  }
  auto session = take(app::ProjectSession::create(std::filesystem::path(argv[2])));
  const int fd = ::open(argv[3], O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0)
    throw std::runtime_error("cannot open routed fixture source");
  const auto audio = [&]() -> project::TrackId {
    if (encoded) {
      auto task = session->imports().begin_encoded_audio_import(fd);
      (void)::close(fd);
      auto prepared = take(std::move(take(std::move(task))).run());
      return take(session->imports().finish_encoded_audio_import(std::move(prepared)));
    }
    auto imported = session->imports().import_wav_audio(fd);
    (void)::close(fd);
    return take(std::move(imported));
  }();
  const auto group = take(session->tracks().add_bus_track(project::TrackKind::group_bus));
  const auto returns = take(session->tracks().add_bus_track(project::TrackKind::return_bus));
  take(session->routing().set_main_output(audio, group));
  const auto send = take(session->routing().add_send(audio, returns, 0.5));
  take(session->saving().save());
  std::cout << "revision=" << session->snapshot().revision << " send=" << send.to_string()
            << " gain=0.5\n";
  return 0;
}
} // namespace

int main(int argc, char** argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception& error) {
    std::cerr << "Routed fixture: " << error.what() << '\n';
    return 1;
  }
}
