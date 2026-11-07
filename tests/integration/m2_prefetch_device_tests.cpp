#include "oscillate/test/check.hpp"

#include <oscillate/application/clip_playback.hpp>
#include <oscillate/application/device_control.hpp>
#include <oscillate/application/playback_gate.hpp>
#include <oscillate/application/prefetch_control.hpp>
#include <oscillate/audio_io/graph_source.hpp>
#include <oscillate/audio_io/stereo_block.hpp>
#include <oscillate/engine/graph.hpp>
#include <oscillate/media/prefetch.hpp>
#include <oscillate/project/model.hpp>
#include <oscillate/timeline/timing.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <unistd.h>

namespace {

namespace app = oscillate::application;
namespace audio_io = oscillate::audio_io;
namespace engine = oscillate::engine;
namespace foundation = oscillate::foundation;
namespace media = oscillate::media;
namespace project = oscillate::project;
namespace timeline = oscillate::timeline;
namespace fs = std::filesystem;

using oscillate::test::require;

using oscillate::test::take;

template <typename T> T id(std::uint64_t low) { return take(T::from_bits({0, low})); }

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
    auto path = (fs::temp_directory_path() / "oscillate-prefetch-device-XXXXXX").string();
    const int fd = ::mkstemp(path.data());
    REQUIRE_GE(fd, 0, "cannot create WAV fixture");
    ::close(fd);
    path_ = path;
    std::vector<unsigned char> bytes;
    for (const char byte : std::string("RIFF")) {
      bytes.push_back(static_cast<unsigned char>(byte));
    }
    put32(bytes, 36 + 16);
    for (const char byte : std::string("WAVEfmt ")) {
      bytes.push_back(static_cast<unsigned char>(byte));
    }
    put32(bytes, 16);
    put16(bytes, 1);
    put16(bytes, 2);
    put32(bytes, 48000);
    put32(bytes, 96000);
    put16(bytes, 2);
    put16(bytes, 8);
    for (const char byte : std::string("data")) {
      bytes.push_back(static_cast<unsigned char>(byte));
    }
    put32(bytes, 16);
    for (unsigned index = 0; index < 8; ++index) {
      bytes.push_back(static_cast<unsigned char>(index * 16));
      bytes.push_back(static_cast<unsigned char>(255 - index * 16));
    }
    std::ofstream file(path_, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(file), "cannot write WAV fixture");
  }
  ~WavFixture() { fs::remove(path_); }
  [[nodiscard]] const fs::path& path() const noexcept { return path_; }

private:
  fs::path path_;
};

project::Snapshot clip_snapshot() {
  auto tempo = take(timeline::TempoMap::create({{{0}, 120.0, timeline::TempoShape::step}}, 1));
  auto meter = take(timeline::MeterMap::create({{{0}, 4, 4}}, 1));
  const auto clip = project::Clip{id<project::ClipId>(3),
                                  id<project::SourceId>(4),
                                  {{0}, {8}},
                                  project::ElapsedInterval{{0}, {8}},
                                  1};
  const auto source = project::SourceRecord{id<project::SourceId>(4),
                                            project::SourceEncoding::wav,
                                            std::string(64, 'a'),
                                            60,
                                            48000,
                                            2,
                                            8};
  return {id<project::ProjectId>(1),           1,       48000, std::move(tempo), std::move(meter),
          {{id<project::TrackId>(2), {clip}}}, {source}};
}

struct Fixture {
  WavFixture wav;
  media::WavReader reader;
  std::shared_ptr<media::PrefetchRing> ring =
      std::make_shared<media::PrefetchRing>(id<project::ClipId>(3), 8);
  std::shared_ptr<app::PlaybackReadiness> readiness = std::make_shared<app::PlaybackReadiness>();
  std::uint64_t tick = 100;
  app::PrefetchControl prefetch;
  app::DevicePlaybackControl device;
  app::PlaybackGate gate;
  engine::GraphPublisher graph;
  audio_io::TransportCursor transport;
  audio_io::StereoRenderSource render_source;
  std::array<engine::StereoFrame, 2> stereo_scratch{};
  std::array<float, 4> output{};

  static std::uint64_t now(void* context) noexcept { return *static_cast<std::uint64_t*>(context); }
  static media::WavReader open(const fs::path& path) { return take(media::WavReader::open(path)); }
  Fixture()
      : reader(open(wav.path())), prefetch(ring, readiness, {&tick, now}),
        gate(*readiness, &device), render_source(audio_io::graph_render_source(graph)) {
    auto snapshot = clip_snapshot();
    auto request = take(app::make_clip_request(snapshot, ring->clip(), ring, readiness, 7, 2));
    auto plan = take(engine::prepare_graph(1, 2, std::span(&request, 1), nullptr));
    REQUIRE_EQ(graph.publish(std::move(plan)), engine::PublishStatus::accepted,
               "clip graph publication failed");
  }

  [[nodiscard]] bool render_if_ready() noexcept {
    if (!gate.enabled()) {
      std::ranges::fill(output, 0.0F);
      return false;
    }
    return audio_io::render_transport_block(render_source, transport, true, stereo_scratch,
                                            output) == audio_io::BlockResult::rendered;
  }

  void expect_frames(std::size_t first) const {
    for (std::size_t index = 0; index < 2; ++index) {
      const auto frame = first + index;
      const auto left = (static_cast<float>(frame * 16) - 128.0F) / 128.0F;
      const auto right = (127.0F - static_cast<float>(frame * 16)) / 128.0F;
      REQUIRE_LT(std::fabs(output[index * 2] - left), 0.00001F,
                 "device render did not preserve stereo WAV frame or transport position");
      REQUIRE_LT(std::fabs(output[index * 2 + 1] - right), 0.00001F,
                 "device render did not preserve stereo WAV frame or transport position");
    }
  }
};

void cold_start_seek_loop_and_stall() {
  Fixture fixture;
  const auto epoch = fixture.device.select_target(42).value_or(0);
  REQUIRE_NE(epoch, 0, "device did not enter priming");
  REQUIRE_TRUE(fixture.device.observe({app::DeviceEventKind::ready, 42, epoch}),
               "device did not enter priming");
  auto start = take(fixture.prefetch.begin_start(1, 0, 8, {2, 10}));
  REQUIRE_TRUE(!fixture.render_if_ready(),
               "cold start advanced or counted an underrun before priming");
  REQUIRE_EQ(fixture.output, (std::array<float, 4>{}),
             "cold start advanced or counted an underrun before priming");
  REQUIRE_EQ(fixture.ring->underrun_count(), 0,
             "cold start advanced or counted an underrun before priming");
  fixture.tick = 105;
  REQUIRE_EQ(fixture.prefetch.poll(), app::PrimingStatus::priming,
             "fake-clock lead deadline expired before its bound");
  std::array<media::StereoFrame, 4> scratch{};
  REQUIRE_EQ(take(start->fill_step(fixture.reader, 48000, scratch)), 4,
             "worker lead did not enable the selected device");
  REQUIRE_EQ(fixture.prefetch.poll(), app::PrimingStatus::ready,
             "worker lead did not enable the selected device");
  REQUIRE_TRUE(fixture.device.primed(epoch), "worker lead did not enable the selected device");
  require(fixture.render_if_ready(), "primed first block did not render");
  fixture.expect_frames(0); // Also proves stopped cold start did not advance cursor.

  // Old frames 2–3 remain queued. An unanticipated seek must discard them and
  // keep the cursor pending until the new generation is ready.
  auto seek = take(fixture.prefetch.begin_seek(2, 4, 8, {2, 10}));
  REQUIRE_EQ(fixture.ring->occupancy(), 0, "seek did not invalidate the old generation");
  REQUIRE_TRUE(!fixture.readiness->steady(), "seek did not invalidate the old generation");
  REQUIRE_TRUE(fixture.transport.request_seek(4), "seek did not invalidate the old generation");
  REQUIRE_TRUE(!fixture.render_if_ready(),
               "seek advanced cursor or counted an expected priming gap");
  REQUIRE_EQ(fixture.output, (std::array<float, 4>{}),
             "seek advanced cursor or counted an expected priming gap");
  REQUIRE_EQ(fixture.ring->underrun_count(), 0,
             "seek advanced cursor or counted an expected priming gap");
  std::array<media::StereoFrame, 2> seek_scratch{};
  REQUIRE_EQ(take(seek->fill_step(fixture.reader, 48000, seek_scratch)), 2,
             "seek did not prime new generation");
  REQUIRE_EQ(fixture.prefetch.poll(), app::PrimingStatus::ready,
             "seek did not prime new generation");
  require(fixture.render_if_ready(), "primed seek block did not render");
  fixture.expect_frames(4); // Pending cursor seek applies at this exact block.

  require(take(seek->fill_step(fixture.reader, 48000, seek_scratch)) == 2 && seek->complete(),
          "worker did not queue the rest of the current loop");
  auto loop = take(fixture.prefetch.plan_loop_wrap(3, 0, 8, 2));
  REQUIRE_TRUE(!fixture.prefetch.loop_ready(),
               "current frames falsely satisfied next-generation loop readiness");
  REQUIRE_TRUE(!fixture.prefetch.commit_loop_wrap(),
               "current frames falsely satisfied next-generation loop readiness");
  REQUIRE_EQ(take(loop->fill_step(fixture.reader, 48000, seek_scratch)), 2,
             "worker did not prefetch the planned loop generation");
  REQUIRE_TRUE(fixture.prefetch.loop_ready(),
               "worker did not prefetch the planned loop generation");
  require(fixture.render_if_ready(), "last current-loop block did not render");
  fixture.expect_frames(6);
  REQUIRE_TRUE(fixture.prefetch.commit_loop_wrap(), "ready loop could not schedule transport wrap");
  REQUIRE_TRUE(fixture.transport.request_loop_wrap(0),
               "ready loop could not schedule transport wrap");
  require(fixture.render_if_ready(), "next-loop first block did not render");
  fixture.expect_frames(0);
  REQUIRE_EQ(fixture.ring->underrun_count(), 0, "planned loop introduced a source gap");

  // Hold the worker after the planned lead. Only a steady-state shortfall is
  // counted, and the device receives silence for exactly the missing block.
  require(fixture.render_if_ready() && fixture.output == std::array<float, 4>{} &&
              fixture.ring->underrun_count() == 1,
          "worker stall did not produce one counted silent device block");
}

} // namespace

int main() {
  try {
    cold_start_seek_loop_and_stall();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
