#include "oscillate/test/check.hpp"
#include "oscillate/test/wait.hpp"

#include <oscillate/application/clip_prefetch_worker.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

namespace {
namespace app = oscillate::application;
namespace media = oscillate::media;
namespace project = oscillate::project;

using oscillate::test::require;

void put16(std::vector<unsigned char>& bytes, std::uint16_t value) {
  bytes.push_back(static_cast<unsigned char>(value));
  bytes.push_back(static_cast<unsigned char>(value >> 8U));
}
void put32(std::vector<unsigned char>& bytes, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8) {
    bytes.push_back(static_cast<unsigned char>(value >> shift));
  }
}

class WavFixture {
public:
  explicit WavFixture(std::uint32_t frames = 32) {
    auto chars = (std::filesystem::temp_directory_path() / "oscillate-worker-XXXXXX").string();
    const int fd = ::mkstemp(chars.data());
    REQUIRE_GE(fd, 0, "cannot create WAV fixture");
    ::close(fd);
    path_ = chars;
    std::vector<unsigned char> bytes;
    for (char character : std::string("RIFF")) {
      bytes.push_back(static_cast<unsigned char>(character));
    }
    put32(bytes, 36 + frames);
    for (char character : std::string("WAVEfmt ")) {
      bytes.push_back(static_cast<unsigned char>(character));
    }
    put32(bytes, 16);
    put16(bytes, 1);
    put16(bytes, 1);
    put32(bytes, 48000);
    put32(bytes, 48000);
    put16(bytes, 1);
    put16(bytes, 8);
    for (char character : std::string("data")) {
      bytes.push_back(static_cast<unsigned char>(character));
    }
    put32(bytes, frames);
    for (std::uint32_t index = 0; index < frames; ++index) {
      bytes.push_back(static_cast<unsigned char>(index * 8));
    }
    std::ofstream output(path_, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(output), "cannot write WAV fixture");
  }
  ~WavFixture() { std::filesystem::remove(path_); }
  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
  void truncate_data() const {
    REQUIRE_EQ(::truncate(path_.c_str(), 44), 0, "cannot truncate WAV fixture");
  }

private:
  std::filesystem::path path_;
};

struct Fixture {
  struct Geometry {
    std::uint32_t frames = 32;
    std::size_t capacity = 8;
  };
  WavFixture wav;
  std::shared_ptr<media::PrefetchRing> ring;
  std::shared_ptr<app::PlaybackReadiness> readiness = std::make_shared<app::PlaybackReadiness>();
  std::uint64_t tick = 100;
  static std::uint64_t now(void* context) noexcept { return *static_cast<std::uint64_t*>(context); }
  static project::ClipId clip() {
    auto id = project::ClipId::from_bits({0, 81});
    REQUIRE_OK(id, "clip ID failed");
    return id.value();
  }
  Fixture() : Fixture(Geometry{}) {}
  explicit Fixture(Geometry geometry)
      : wav(geometry.frames),
        ring(std::make_shared<media::PrefetchRing>(clip(), geometry.capacity)) {}
  [[nodiscard]] std::unique_ptr<app::ClipPrefetchWorker> start(std::uint64_t end = 32) {
    auto reader = media::WavReader::open(wav.path());
    REQUIRE_OK(reader, "WAV fixture did not open");
    auto started = app::ClipPrefetchWorker::start(std::move(reader).value(), ring, readiness,
                                                  {48000, 1, 0, end, {4, 100}}, {&tick, now});
    REQUIRE_OK(started, "worker did not start");
    return std::move(started).value();
  }
};

using oscillate::test::wait_until;

void prime_and_refill() {
  Fixture fixture;
  auto worker = fixture.start();
  REQUIRE_OK(worker->poll_priming(), "priming poll failed");
  wait_until([&] { return worker->status().published_frames == 8; }, "initial ring not filled");
  REQUIRE_EQ(worker->poll_priming().value(), app::PrimingStatus::ready,
             "worker did not prime before transport start");
  REQUIRE_TRUE(fixture.readiness->steady(), "worker did not prime before transport start");
  std::array<media::StereoFrame, 4> first{};
  const auto consumed = fixture.ring->consume(1, 0, first, fixture.readiness->steady());
  REQUIRE_EQ(consumed.copied, 4, "initial frames differ from independent 8-bit WAV values");
  REQUIRE_EQ(consumed.silent, 0, "initial frames differ from independent 8-bit WAV values");
  REQUIRE_EQ(first[0].left, -1.0F, "initial frames differ from independent 8-bit WAV values");
  REQUIRE_EQ(first[3].left, -0.8125F, "initial frames differ from independent 8-bit WAV values");
  wait_until([&] { return worker->status().published_frames >= 12; },
             "worker did not refill after consumption");
  std::array<media::StereoFrame, 8> next{};
  const auto refilled = fixture.ring->consume(1, 4, next, fixture.readiness->steady());
  REQUIRE_EQ(refilled.copied, 8, "continued refill lost source order or counted an underrun");
  REQUIRE_EQ(refilled.silent, 0, "continued refill lost source order or counted an underrun");
  REQUIRE_EQ(next[0].left, -0.75F, "continued refill lost source order or counted an underrun");
  REQUIRE_EQ(next[7].left, -0.3125F, "continued refill lost source order or counted an underrun");
  REQUIRE_EQ(fixture.ring->underrun_count(), 0,
             "continued refill lost source order or counted an underrun");
  worker->request_stop();
  wait_until([&] { return worker->finished(); }, "worker did not stop");
  REQUIRE_TRUE(worker->join_finished(), "stopped worker was not safely retired");
  REQUIRE_TRUE(!fixture.readiness->steady(), "stopped worker was not safely retired");
}

void complete_never_reads_past_range() {
  Fixture fixture;
  auto worker = fixture.start(5);
  wait_until([&] { return worker->finished(); }, "short job did not finish");
  REQUIRE_EQ(worker->status().state, app::ClipPrefetchState::complete,
             "worker read beyond requested source range");
  REQUIRE_EQ(worker->status().published_frames, 5, "worker read beyond requested source range");
  REQUIRE_TRUE(worker->join_finished(), "worker read beyond requested source range");
  require(!worker->failure(), "completed worker reported an error");
  std::array<media::StereoFrame, 5> output{};
  REQUIRE_EQ(fixture.ring->consume(1, 0, output, false).copied, 5,
             "short range frames not published");
}

void cancel_while_full() {
  Fixture fixture;
  auto worker = fixture.start();
  wait_until([&] { return worker->status().published_frames == 8; }, "ring not filled");
  require(!worker->join_finished(), "active producer was joined by UI-safe probe");
  worker->request_stop();
  wait_until([&] { return worker->finished(); }, "full-ring cancellation hung");
  REQUIRE_EQ(worker->status().state, app::ClipPrefetchState::stopped,
             "cancelled worker advanced or retained readiness");
  REQUIRE_EQ(worker->status().published_frames, 8,
             "cancelled worker advanced or retained readiness");
  REQUIRE_TRUE(worker->join_finished(), "cancelled worker advanced or retained readiness");
  REQUIRE_TRUE(!fixture.readiness->steady(), "cancelled worker advanced or retained readiness");
  REQUIRE_TRUE(!worker->poll_priming(), "cancelled prime was reported as ready or failed");
  REQUIRE_TRUE(!worker->failure(), "cancelled prime was reported as ready or failed");
}

void destruction_retires_active_producer_after_playback_stop() {
  Fixture fixture;
  auto worker = fixture.start();
  wait_until([&] { return worker->status().published_frames == 8; }, "ring not filled");
  worker.reset();
  require(!fixture.readiness->steady(), "retirement retained playback readiness");
}

void read_failure_is_visible_and_not_complete() {
  Fixture fixture;
  auto worker = fixture.start();
  wait_until([&] { return worker->status().published_frames == 8; }, "ring not filled");
  REQUIRE_EQ(worker->poll_priming().value(), app::PrimingStatus::ready,
             "read failure fixture did not prime");
  fixture.wav.truncate_data();
  std::array<media::StereoFrame, 8> initial{};
  REQUIRE_EQ(fixture.ring->consume(1, 0, initial, true).copied, 8,
             "initial buffer was not available");
  wait_until([&] { return worker->finished(); }, "read failure was not observed");
  const auto status = worker->status();
  const auto error = worker->failure();
  REQUIRE_EQ(status.state, app::ClipPrefetchState::failed,
             "read error was hidden or reported as completion");
  REQUIRE_EQ(status.published_frames, 8, "read error was hidden or reported as completion");
  REQUIRE_TRUE(error, "read error was hidden or reported as completion");
  REQUIRE_EQ(error->code, oscillate::foundation::ErrorCode::io_failure,
             "read error was hidden or reported as completion");
  REQUIRE_TRUE(!worker->poll_priming(), "read error was hidden or reported as completion");
  REQUIRE_TRUE(!fixture.readiness->steady(), "read error was hidden or reported as completion");
  REQUIRE_TRUE(worker->join_finished(), "read error was hidden or reported as completion");
}

void invalid_source_and_range_rejected_before_launch() {
  Fixture fixture;
  const auto attempt = [&](std::uint32_t rate, std::uint64_t first, std::uint64_t end,
                           app::PrimingPolicy policy) {
    auto reader = media::WavReader::open(fixture.wav.path());
    REQUIRE_OK(reader, "invalid-start fixture did not open");
    return app::ClipPrefetchWorker::start(std::move(reader).value(), fixture.ring,
                                          fixture.readiness, {rate, 1, first, end, policy},
                                          {&fixture.tick, Fixture::now});
  };
  require(!attempt(44100, 0, 32, {4, 100}), "rate mismatch started a worker");
  require(!attempt(48000, 0, 33, {4, 100}), "source overrun started a worker");
  require(!attempt(48000, 32, 32, {4, 100}), "empty range started a worker");
  require(!attempt(48000, 0, 32, {9, 100}), "oversized prime started a worker");
  require(!fixture.readiness->steady(), "rejected start changed playback readiness");
}

void prime_timeout_stops_producer_without_counting_an_underrun() {
  // More frames than a producer can fill before this control-thread poll;
  // the injected clock makes the zero-tick deadline immediate.
  constexpr std::size_t frames = 100'000;
  Fixture fixture({static_cast<std::uint32_t>(frames), frames});
  auto reader = media::WavReader::open(fixture.wav.path());
  REQUIRE_OK(reader, "timeout fixture did not open");
  auto started = app::ClipPrefetchWorker::start(
      std::move(reader).value(), fixture.ring, fixture.readiness,
      {48000, 1, 0, frames, {frames, 0}}, {&fixture.tick, Fixture::now});
  REQUIRE_OK(started, "timeout worker did not start");
  auto worker = std::move(started).value();
  const auto priming = worker->poll_priming();
  REQUIRE_TRUE(priming, "expired prime was reported as ready");
  REQUIRE_EQ(priming.value(), app::PrimingStatus::timed_out, "expired prime was reported as ready");
  wait_until([&] { return worker->finished(); }, "timed-out producer did not stop");
  REQUIRE_TRUE(worker->join_finished(), "prime timeout did not retire quietly");
  REQUIRE_EQ(worker->status().state, app::ClipPrefetchState::stopped,
             "prime timeout did not retire quietly");
  REQUIRE_TRUE(!fixture.readiness->steady(), "prime timeout did not retire quietly");
  REQUIRE_EQ(fixture.ring->underrun_count(), 0, "prime timeout did not retire quietly");
}

} // namespace

int main() {
  try {
    prime_and_refill();
    complete_never_reads_past_range();
    cancel_while_full();
    destruction_retires_active_producer_after_playback_stop();
    read_failure_is_visible_and_not_complete();
    invalid_source_and_range_rejected_before_launch();
    prime_timeout_stops_producer_without_counting_an_underrun();
    std::cout << "clip prefetch worker tests passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
