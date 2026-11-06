#include "oscillate/test/check.hpp"

#include <oscillate/application/prefetch_control.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

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
  for (unsigned shift = 0; shift != 32; shift += 8) {
    bytes.push_back(static_cast<unsigned char>(value >> shift));
  }
}

class WavFixture {
public:
  WavFixture() {
    const auto path = std::filesystem::temp_directory_path() / "oscillate-prime-XXXXXX";
    auto chars = path.string();
    const int fd = ::mkstemp(chars.data());
    REQUIRE_GE(fd, 0, "cannot create WAV fixture");
    ::close(fd);
    path_ = chars;

    std::vector<unsigned char> bytes;
    for (const char tag : std::string("RIFF")) {
      bytes.push_back(static_cast<unsigned char>(tag));
    }
    put32(bytes, 36 + 8);
    for (const char tag : std::string("WAVEfmt ")) {
      bytes.push_back(static_cast<unsigned char>(tag));
    }
    put32(bytes, 16);
    put16(bytes, 1);
    put16(bytes, 1);
    put32(bytes, 48000);
    put32(bytes, 48000);
    put16(bytes, 1);
    put16(bytes, 8);
    for (const char tag : std::string("data")) {
      bytes.push_back(static_cast<unsigned char>(tag));
    }
    put32(bytes, 8);
    for (unsigned char value : std::array<unsigned char, 8>{0, 32, 64, 96, 128, 160, 192, 224}) {
      bytes.push_back(value);
    }
    std::ofstream file(path_, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(file), "cannot write WAV fixture");
  }
  ~WavFixture() { std::filesystem::remove(path_); }
  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
  std::filesystem::path path_;
};

struct Fixture {
  WavFixture wav;
  media::WavReader reader;
  std::shared_ptr<media::PrefetchRing> ring;
  std::shared_ptr<app::PlaybackReadiness> readiness = std::make_shared<app::PlaybackReadiness>();
  std::uint64_t tick = 100;
  app::PrefetchControl control;

  static std::uint64_t now(void* context) noexcept { return *static_cast<std::uint64_t*>(context); }
  static media::WavReader open(const std::filesystem::path& path) {
    auto result = media::WavReader::open(path);
    REQUIRE_OK(result, "WAV fixture did not open");
    return std::move(result).value();
  }
  static project::ClipId clip() {
    auto result = project::ClipId::from_bits({0, 11});
    REQUIRE_OK(result, "clip ID failed");
    return result.value();
  }
  explicit Fixture(std::size_t ring_capacity = 8)
      : reader(open(wav.path())),
        ring(std::make_shared<media::PrefetchRing>(clip(), ring_capacity)),
        control(ring, readiness, {&tick, now}) {}
};

void priming_success_and_timeout() {
  Fixture fixture;
  auto started = fixture.control.begin_start(1, 0, 8, {2, 10});
  REQUIRE_TRUE(started, "cold start should be priming");
  REQUIRE_EQ(fixture.control.poll(), app::PrimingStatus::priming, "cold start should be priming");
  REQUIRE_TRUE(!fixture.readiness->steady(), "cold start should be priming");
  std::array<media::StereoFrame, 2> scratch{};
  REQUIRE_EQ(started.value()->fill_step(fixture.reader, 48000, scratch).value(), 2,
             "prime should start only after current-generation lead is queued");
  REQUIRE_EQ(fixture.control.poll(), app::PrimingStatus::ready,
             "prime should start only after current-generation lead is queued");
  REQUIRE_TRUE(fixture.readiness->steady(),
               "prime should start only after current-generation lead is queued");
  std::array<media::StereoFrame, 2> output{};
  const auto consumed = fixture.ring->consume(1, 0, output, true);
  REQUIRE_EQ(consumed.copied, 2, "primed playback should have exact WAV samples and no underrun");
  REQUIRE_EQ(consumed.silent, 0, "primed playback should have exact WAV samples and no underrun");
  REQUIRE_EQ(output[0].left, -1.0F,
             "primed playback should have exact WAV samples and no underrun");
  REQUIRE_EQ(output[1].left, -0.75F,
             "primed playback should have exact WAV samples and no underrun");
  REQUIRE_EQ(fixture.ring->underrun_count(), 0,
             "primed playback should have exact WAV samples and no underrun");

  Fixture timeout;
  auto pending = timeout.control.begin_start(1, 0, 8, {2, 10});
  REQUIRE_TRUE(pending, "pending start rejected");
  REQUIRE_EQ(timeout.control.poll(), app::PrimingStatus::priming, "pending start rejected");
  timeout.tick = 110;
  REQUIRE_EQ(timeout.control.poll(), app::PrimingStatus::timed_out,
             "timeout must leave playback stopped without an underrun");
  REQUIRE_TRUE(!timeout.readiness->steady(),
               "timeout must leave playback stopped without an underrun");
  REQUIRE_EQ(timeout.ring->underrun_count(), 0,
             "timeout must leave playback stopped without an underrun");
  REQUIRE_EQ(pending.value()->fill_step(timeout.reader, 48000, scratch).value(), 2,
             "late worker completion must not silently start transport");
  REQUIRE_EQ(timeout.control.poll(), app::PrimingStatus::timed_out,
             "late worker completion must not silently start transport");
  REQUIRE_TRUE(!timeout.readiness->steady(),
               "late worker completion must not silently start transport");
}

void seek_rejects_stale_and_restarts_priming() {
  Fixture fixture;
  auto first = fixture.control.begin_start(5, 0, 8, {2, 10});
  REQUIRE_OK(first, "first generation failed");
  std::array<media::StereoFrame, 4> scratch{};
  REQUIRE_EQ(first.value()->fill_step(fixture.reader, 48000, scratch).value(), 4,
             "first generation did not prime");
  REQUIRE_EQ(fixture.control.poll(), app::PrimingStatus::ready, "first generation did not prime");
  auto seek = fixture.control.begin_seek(6, 4, 8, {2, 10});
  REQUIRE_TRUE(seek, "seek did not invalidate old queued frames");
  REQUIRE_TRUE(!fixture.readiness->steady(), "seek did not invalidate old queued frames");
  REQUIRE_EQ(fixture.ring->occupancy(), 0, "seek did not invalidate old queued frames");
  require(!fixture.control.begin_seek(5, 2, 8, {2, 10}), "stale generation accepted after seek");
  // The old worker is quiesced, and only the new job is run.
  require(seek.value()->fill_step(fixture.reader, 48000, scratch).value() == 4 &&
              fixture.control.poll() == app::PrimingStatus::ready,
          "seek did not prime new generation");
  std::array<media::StereoFrame, 2> output{};
  const auto consumed = fixture.ring->consume(6, 4, output, true);
  REQUIRE_EQ(consumed.copied, 2, "seek copied stale samples or counted priming gap");
  REQUIRE_EQ(output[0].left, 0.0F, "seek copied stale samples or counted priming gap");
  REQUIRE_EQ(output[1].left, 0.25F, "seek copied stale samples or counted priming gap");
  REQUIRE_EQ(fixture.ring->underrun_count(), 0, "seek copied stale samples or counted priming gap");
}

void planned_loop_uses_generation_acknowledgement() {
  Fixture fixture;
  auto first = fixture.control.begin_start(1, 0, 4, {2, 10});
  REQUIRE_OK(first, "loop start failed");
  std::array<media::StereoFrame, 4> scratch{};
  REQUIRE_EQ(first.value()->fill_step(fixture.reader, 48000, scratch).value(), 4,
             "current loop did not fully queue");
  REQUIRE_TRUE(first.value()->complete(), "current loop did not fully queue");
  REQUIRE_EQ(fixture.control.poll(), app::PrimingStatus::ready, "current loop did not fully queue");
  REQUIRE_EQ(first.value()->fill_step(fixture.reader, 48000, scratch).value(), 0,
             "completed job did not remain finished");
  auto next = fixture.control.plan_loop_wrap(2, 0, 4, 2);
  REQUIRE_TRUE(next, "current-generation occupancy was mistaken for next-loop readiness");
  REQUIRE_TRUE(!fixture.control.loop_ready(),
               "current-generation occupancy was mistaken for next-loop readiness");
  REQUIRE_TRUE(!fixture.control.commit_loop_wrap(),
               "current-generation occupancy was mistaken for next-loop readiness");
  REQUIRE_EQ(next.value()->fill_step(fixture.reader, 48000, scratch).value(), 4,
             "next-loop generation failed to queue");
  REQUIRE_TRUE(fixture.control.loop_ready(), "next-loop generation failed to queue");
  std::array<media::StereoFrame, 4> output{};
  const auto current = fixture.ring->consume(1, 0, output, true);
  REQUIRE_EQ(current.copied, 4, "planned loop damaged current generation");
  REQUIRE_EQ(output[0].left, -1.0F, "planned loop damaged current generation");
  REQUIRE_EQ(output[3].left, -0.25F, "planned loop damaged current generation");
  REQUIRE_TRUE(fixture.control.commit_loop_wrap(), "ready loop could not commit seamlessly");
  REQUIRE_TRUE(fixture.readiness->steady(), "ready loop could not commit seamlessly");
  const auto wrapped = fixture.ring->consume(2, 0, output, true);
  REQUIRE_EQ(wrapped.copied, 4, "planned wrap had a gap or wrong samples");
  REQUIRE_EQ(output[0].left, -1.0F, "planned wrap had a gap or wrong samples");
  REQUIRE_EQ(output[3].left, -0.25F, "planned wrap had a gap or wrong samples");
  REQUIRE_EQ(fixture.ring->underrun_count(), 0, "planned wrap had a gap or wrong samples");
}

void worker_stall_counts_only_steady_shortfall() {
  Fixture fixture(4);
  auto first = fixture.control.begin_start(1, 0, 8, {2, 10});
  REQUIRE_OK(first, "stall start failed");
  std::array<media::StereoFrame, 2> scratch{};
  REQUIRE_EQ(first.value()->fill_step(fixture.reader, 48000, scratch).value(), 2,
             "stall fixture failed to prime");
  REQUIRE_EQ(fixture.control.poll(), app::PrimingStatus::ready, "stall fixture failed to prime");
  std::array<media::StereoFrame, 2> output{};
  REQUIRE_EQ(fixture.ring->consume(1, 0, output, true).copied, 2,
             "stall fixture did not consume lead");
  const auto missing = fixture.ring->consume(1, 2, output, fixture.readiness->steady());
  const auto event = missing.underruns[0].value_or(media::UnderrunEvent{Fixture::clip()});
  REQUIRE_EQ(missing.silent, 2, "steady worker stall did not produce exact silence and event");
  REQUIRE_TRUE(missing.underruns[0], "steady worker stall did not produce exact silence and event");
  REQUIRE_EQ(event.first_source_frame, 2,
             "steady worker stall did not produce exact silence and event");
  REQUIRE_EQ(event.missing_frames, 2,
             "steady worker stall did not produce exact silence and event");
  REQUIRE_EQ(fixture.ring->underrun_count(), 1,
             "steady worker stall did not produce exact silence and event");
  REQUIRE_EQ(output[0].left, 0.0F, "steady worker stall did not produce exact silence and event");
  REQUIRE_EQ(output[1].right, 0.0F, "steady worker stall did not produce exact silence and event");
}

void rejected_jobs_and_worker_errors_leave_readiness_safe() {
  Fixture fixture;
  REQUIRE_TRUE(!fixture.control.begin_start(0, 0, 8, {2, 10}), "invalid priming request accepted");
  REQUIRE_TRUE(!fixture.control.begin_start(1, 0, 8, {0, 10}), "invalid priming request accepted");
  REQUIRE_TRUE(!fixture.control.begin_start(1, 0, 8, {9, 10}), "invalid priming request accepted");
  fixture.tick = UINT64_MAX - 1;
  require(!fixture.control.begin_start(1, 0, 8, {2, 10}), "overflowing deadline accepted");
  fixture.tick = 100;
  auto first = fixture.control.begin_start(1, 0, 8, {2, 10});
  REQUIRE_OK(first, "valid job rejected after invalid requests");
  require(!fixture.control.plan_loop_wrap(2, 0, 4, 2),
          "loop scheduled before current source was queued");
  std::array<media::StereoFrame, 2> scratch{};
  REQUIRE_TRUE(!first.value()->fill_step(fixture.reader, 48000, std::span<media::StereoFrame>{}),
               "worker error advanced readiness");
  REQUIRE_TRUE(!first.value()->fill_step(fixture.reader, 44100, scratch),
               "worker error advanced readiness");
  REQUIRE_EQ(first.value()->published_frames(), 0, "worker error advanced readiness");
  REQUIRE_TRUE(!fixture.readiness->steady(), "worker error advanced readiness");
  REQUIRE_EQ(first.value()->fill_step(fixture.reader, 48000, scratch).value(), 2,
             "worker did not recover after validation error");

  Fixture short_source;
  auto overlong = short_source.control.begin_start(1, 0, 9, {2, 10});
  REQUIRE_OK(overlong, "overlong test job rejected before source check");
  std::array<media::StereoFrame, 8> all{};
  REQUIRE_EQ(overlong.value()->fill_step(short_source.reader, 48000, all).value(), 8,
             "early WAV EOF was not reported");
  REQUIRE_TRUE(!overlong.value()->fill_step(short_source.reader, 48000, all),
               "early WAV EOF was not reported");

  bool invalid_control = false;
  try {
    app::PrefetchControl bad({}, fixture.readiness, {&fixture.tick, Fixture::now});
  } catch (const std::invalid_argument&) {
    invalid_control = true;
  }
  require(invalid_control, "missing ring accepted by control");
}

} // namespace

int main() {
  try {
    priming_success_and_timeout();
    seek_rejects_stale_and_restarts_priming();
    planned_loop_uses_generation_acknowledgement();
    worker_stall_counts_only_steady_shortfall();
    rejected_jobs_and_worker_errors_leave_readiness_safe();
    std::cout << "application prefetch control tests passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
