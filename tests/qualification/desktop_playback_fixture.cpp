#include "oscillate/test/check.hpp"

#include <oscillate/application/instrument_playback.hpp>
#include <oscillate/application/project_session.hpp>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <utility>

namespace {
using namespace oscillate;
using namespace std::chrono_literals;

using oscillate::test::require;

using oscillate::test::take;

template <typename Predicate> void wait_for(std::string_view label, Predicate ready);

void audio_clip_playback(std::uint64_t serial, const char* wav, const char* project) {
  auto session = take(application::ProjectSession::create(project));
  const int input = ::open(wav, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  REQUIRE_GE(input, 0, "cannot open audio clip WAV");
  const auto imported = session->imports().import_wav_audio(input);
  (void)::close(input);
  take(imported);
  take(session->saving().save());

  application::InstrumentPlayback playback(*session);
  auto prepared = take(std::move(take(playback.prepare_start())).run());
  REQUIRE_OK(prepared.audio_clip, "prepared playback omitted audio clip");
  take(playback.start_prepared(std::move(prepared), serial));
  wait_for("audio clip did not prime", [&] {
    const auto ready = take(playback.poll_start());
    return ready;
  });
  wait_for("audio clip graph did not activate", [&] {
    playback.observe();
    return playback.output_state() == audio_io::OutputState::streaming &&
           playback.status().application == application::PlaybackApplication::active;
  });
  std::this_thread::sleep_for(1100ms); // Live monitor capture and source completion.
  const auto completed = playback.completed_sample();
  const auto underruns = playback.source_underruns();
  const auto output = playback.output_status();
  require(!playback.source_failure(), "audio source worker failed");
  REQUIRE_GT(completed, 48'000, "audio clip playback did not finish cleanly");
  REQUIRE_EQ(underruns, 0, "audio clip playback did not finish cleanly");
  REQUIRE_EQ(output.render_failures, 0, "audio clip playback did not finish cleanly");
  playback.stop();
  wait_for("audio source worker did not retire", [&] { return playback.poll_retirement(); });
  REQUIRE_OK(session->exports().prepare_wav_master_export({"/dev/null"}),
             "stopped audio playback retained export gate");
  std::cout << "desktop audio clip completed sample " << completed << ", source underruns "
            << underruns << ", stopped export gate released\n";
}

template <typename Predicate> void wait_for(std::string_view label, Predicate ready) {
  const auto deadline = std::chrono::steady_clock::now() + 4s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (ready()) {
      return;
    }
    std::this_thread::sleep_for(10ms); // Live external device readiness only.
  }
  throw std::runtime_error(std::string(label));
}

} // namespace

int main(int argc, char** argv) {
  try {
    REQUIRE_EQ(argc, 4, "usage: fixture TARGET_SERIAL SOURCE_WAV NEW_PROJECT_DIR");
    std::uint64_t serial = 0;
    const std::string_view serial_text(argv[1]);
    const auto parsed =
        std::from_chars(serial_text.data(), serial_text.data() + serial_text.size(), serial);
    REQUIRE_EQ(parsed.ec, std::errc{}, "invalid target serial");
    REQUIRE_EQ(parsed.ptr, serial_text.data() + serial_text.size(), "invalid target serial");
    REQUIRE_NE(serial, 0, "invalid target serial");
    audio_clip_playback(serial, argv[2], argv[3]);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
