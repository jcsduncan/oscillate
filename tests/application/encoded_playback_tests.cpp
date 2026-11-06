#include "oscillate/application/clip_playback.hpp"
#include "oscillate/application/clip_prefetch_worker.hpp"
#include "oscillate/application/instrument_playback.hpp"
#include "oscillate/engine/graph.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"
#include "oscillate/test/wait.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <utility>

#include <fcntl.h>

namespace {
namespace fs = std::filesystem;
using namespace oscillate;

using oscillate::test::require;

using oscillate::test::take;

template <typename T> T id(std::uint64_t low) { return take(T::from_bits({0, low})); }

class Directory : public oscillate::test::TempDir {
public:
  Directory() : TempDir("oscillate-encoded-play") {}
};

class InputFd {
public:
  explicit InputFd(const fs::path& path) : fd_(::open(path.c_str(), O_RDONLY | O_CLOEXEC)) {
    REQUIRE_GE(fd_, 0, "fixture open failed");
  }
  ~InputFd() { ::close(fd_); }
  InputFd(const InputFd&) = delete;
  InputFd& operator=(const InputFd&) = delete;
  [[nodiscard]] int get() const noexcept { return fd_; }

private:
  int fd_;
};

using oscillate::test::wait_until;

std::uint64_t now(void*) noexcept { return 1; }

struct ManagedClip {
  fs::path root;
  std::shared_ptr<const project::Snapshot> snapshot;
  project::SourceRecord source;
};

ManagedClip stage_clip(const fs::path& root, std::string_view file) {
  InputFd input(fs::path(OSCILLATE_CODEC_FIXTURE_DIR) / file);
  auto writer = take(persistence::ProjectStore::open_writer(root, true));
  auto staged = take(
      std::move(take(writer.begin_encoded_stage(id<project::SourceId>(4), input.get()))).run());
  auto receipt = take(writer.publish_encoded_stage(std::move(staged)));
  auto tempo = take(timeline::TempoMap::create({{{0}, 120.0, timeline::TempoShape::step}}, 1));
  auto meter = take(timeline::MeterMap::create({{{0}, 4, 4}}, 1));
  project::Clip clip{id<project::ClipId>(3),
                     receipt.record().id,
                     {{8}, {40}},
                     project::ElapsedInterval{{100}, {132}},
                     1};
  project::Snapshot snapshot{id<project::ProjectId>(1),
                             1,
                             48'000,
                             std::move(tempo),
                             std::move(meter),
                             {{id<project::TrackId>(2), {clip}}},
                             {receipt.record()}};
  std::array<persistence::SourceReceipt, 1> receipts{std::move(receipt)};
  REQUIRE_OK(writer.save(snapshot, persistence::SaveKind::explicit_save, receipts));
  auto reader = take(persistence::ProjectStore::open_reader(root));
  auto reopened = take(reader.load_current());
  require(reopened.source_diagnostics.empty(), "reopened encoded asset has diagnostics");
  return {root, std::make_shared<const project::Snapshot>(std::move(reopened.snapshot)),
          snapshot.sources[0]};
}

void exercise_source(std::string_view file) {
  Directory temporary;
  auto managed = stage_clip(temporary.path() / "project", file);
  auto prepared =
      take(application::InstrumentPlaybackPreparation{managed.root, managed.snapshot}.run());
  if (!prepared.audio_clip.has_value()) {
    throw std::runtime_error("managed encoded source was not prepared");
  }
  auto& clip = prepared.audio_clip.value();
  REQUIRE_EQ(clip.source.sample_rate(), 48'000, "prepared clip metadata or timeline was wrong");
  REQUIRE_GE(clip.source.frame_count(), 960, "prepared clip metadata or timeline was wrong");
  REQUIRE_EQ(clip.first_source_frame, 8, "prepared clip metadata or timeline was wrong");
  REQUIRE_EQ(clip.end_source_frame, 40, "prepared clip metadata or timeline was wrong");
  REQUIRE_EQ(clip.project_start_frame, 100, "prepared clip metadata or timeline was wrong");

  auto ring = std::make_shared<media::PrefetchRing>(clip.clip, 64);
  auto readiness = std::make_shared<application::PlaybackReadiness>();
  auto request =
      take(application::make_clip_request(*managed.snapshot, clip.clip, ring, readiness, 17, 32));
  auto plan = take(engine::prepare_graph(1, 32, std::span(&request, 1), nullptr));
  engine::GraphPublisher graph;
  REQUIRE_EQ(graph.publish(std::move(plan)), engine::PublishStatus::accepted,
             "encoded clip graph publish failed");
  auto worker = take(application::ClipPrefetchWorker::start(
      std::move(clip.source), ring, readiness, {48'000, 1, 8, 40, {32, 100}}, {nullptr, now}));
  wait_until([&] { return worker->finished(); }, "encoded clip did not prefetch");
  REQUIRE_EQ(worker->status().state, application::ClipPrefetchState::complete,
             "encoded clip did not prime");
  REQUIRE_TRUE(worker->join_finished(), "encoded clip did not prime");
  REQUIRE_EQ(worker->poll_priming().value(), application::PrimingStatus::ready,
             "encoded clip did not prime");
  std::array<engine::StereoFrame, 32> out{};
  require(graph.render_block(out, {96, 1}), "encoded graph render failed");
  for (std::size_t index = 0; index < 4; ++index) {
    REQUIRE_EQ(out[index].left, 0, "encoded clip started before project placement");
    REQUIRE_EQ(out[index].right, 0, "encoded clip started before project placement");
  }
  float energy = 0;
  for (std::size_t index = 4; index < out.size(); ++index) {
    energy += std::fabs(out[index].left);
    REQUIRE_LT(std::fabs(out[index].left - out[index].right), 0.00001F,
               "mono encoded clip was not duplicated to stereo");
  }
  REQUIRE_GT(energy, 0.1F, "encoded prefetch had silent or missing frames");
  REQUIRE_EQ(ring->underrun_count(), 0, "encoded prefetch had silent or missing frames");

  worker->request_stop();
  auto reader = take(persistence::ProjectStore::open_reader(managed.root));
  const int pinned = take(reader.open_managed_source(managed.source));
  auto reopened = media::AudioReader::open_fd(pinned, managed.source);
  ::close(pinned);
  auto next = take(std::move(reopened));
  auto seek_worker = take(application::ClipPrefetchWorker::start(
      std::move(next), ring, readiness, {48'000, 2, 12, 40, {28, 100}}, {nullptr, now}));
  wait_until([&] { return seek_worker->finished(); }, "encoded seek did not prefetch");
  REQUIRE_TRUE(seek_worker->join_finished(), "encoded seek failed to re-prime");
  REQUIRE_EQ(seek_worker->poll_priming().value(), application::PrimingStatus::ready,
             "encoded seek failed to re-prime");
  std::array<engine::StereoFrame, 4> seek{};
  require(graph.render_block(seek, {104, 2}), "encoded seek graph render failed");
  REQUIRE_GT(std::fabs(seek[0].left), 0.001F,
             "encoded seek lost source position or counted a priming underrun");
  REQUIRE_EQ(ring->underrun_count(), 0,
             "encoded seek lost source position or counted a priming underrun");
  ring->discard_pending();
  std::array<engine::StereoFrame, 4> missing{};
  require(graph.render_block(missing, {108, 2}), "encoded underrun graph render failed");
  REQUIRE_EQ(ring->underrun_count(), 1, "steady encoded ring gap did not report an underrun");
  seek_worker->request_stop();
}

void external_encoded_source_prepares_only_verified_saved_bytes(std::string_view file) {
  Directory temporary;
  auto managed = stage_clip(temporary.path() / "linked-project", file);
  const auto linked_path = temporary.path() / "linked-audio";
  fs::copy_file(fs::path(OSCILLATE_CODEC_FIXTURE_DIR) / file, linked_path);
  auto state = *managed.snapshot;
  ++state.revision;
  state.sources.front().external_link = project::ExternalSourceLink{linked_path.string()};
  {
    auto writer = take(persistence::ProjectStore::open_writer(managed.root, false));
    REQUIRE_OK(writer.save(state, persistence::SaveKind::explicit_save));
  }
  auto saved = std::make_shared<const project::Snapshot>(state);
  std::stop_source cancelled;
  cancelled.request_stop();
  require(
      !application::InstrumentPlaybackPreparation{managed.root, saved}.run(cancelled.get_token()),
      "cancelled external encoded preparation opened a reader");
  auto prepared = std::async(std::launch::async, [root = managed.root, saved] {
                    return application::InstrumentPlaybackPreparation{root, saved}.run();
                  }).get();
  if (!prepared)
    throw std::runtime_error("saved external encoded audio did not prepare on worker");
  auto& audio_clip = prepared.value().audio_clip;
  if (!audio_clip)
    throw std::runtime_error("saved external encoded playback has no audio clip");
  auto& clip = audio_clip.value();
  REQUIRE_EQ(clip.source.frame_count(), state.sources.front().frames,
             "saved external encoded reader has wrong length");
  std::array<media::StereoFrame, 4> audio{};
  REQUIRE_EQ(clip.source.read_at(8, audio).value(), audio.size(),
             "verified external encoded reader produced no audio");
  REQUIRE_GT(std::fabs(audio.front().left), 0.001F,
             "verified external encoded reader produced no audio");
  {
    std::fstream changed(linked_path, std::ios::in | std::ios::out | std::ios::binary);
    changed.seekp(100);
    changed.put('\x7f');
  }
  require(!application::InstrumentPlaybackPreparation{managed.root, saved}.run(),
          "changed external encoded source reached playback reader");
  fs::remove(linked_path);
  require(!application::InstrumentPlaybackPreparation{managed.root, saved}.run(),
          "missing external encoded source reached playback reader");
}

void rejection_and_cancellation() {
  Directory temporary;
  auto managed = stage_clip(temporary.path() / "project", "sine.flac");
  auto reader = take(persistence::ProjectStore::open_reader(managed.root));
  const int pinned = take(reader.open_managed_source(managed.source));
  auto wrong = managed.source;
  wrong.sample_rate = oscillate::test::sample_rate_44k;
  const auto rejected = media::AudioReader::open_fd(pinned, wrong);
  REQUIRE_TRUE(!rejected, "mismatched source rate was accepted");
  REQUIRE_EQ(rejected.error().code, foundation::ErrorCode::corrupt_data,
             "mismatched source rate was accepted");
  wrong = managed.source;
  wrong.encoding = project::SourceEncoding::aiff;
  const auto wrong_format = media::AudioReader::open_fd(pinned, wrong);
  REQUIRE_TRUE(!wrong_format, "mismatched encoded format was accepted");
  REQUIRE_EQ(wrong_format.error().code, foundation::ErrorCode::corrupt_data,
             "mismatched encoded format was accepted");
  require(!media::AudioReader::open_fd(-1, managed.source),
          "invalid encoded descriptor was accepted");
  auto opened = take(media::AudioReader::open_fd(pinned, managed.source));
  ::close(pinned);
  std::array<media::StereoFrame, 1024> oversized{};
  REQUIRE_EQ(opened.read_at(0, oversized).value(), managed.source.frames,
             "encoded reader did not stop at the source end");
  auto ring = std::make_shared<media::PrefetchRing>(id<project::ClipId>(3), 8);
  auto readiness = std::make_shared<application::PlaybackReadiness>();
  const auto unsupported = application::ClipPrefetchWorker::start(
      std::move(opened), ring, readiness, {44'100, 1, 0, 960, {4, 100}}, {nullptr, now});
  require(!unsupported, "rate conversion was silently accepted");
  const int another_fd = take(reader.open_managed_source(managed.source));
  auto another = take(media::AudioReader::open_fd(another_fd, managed.source));
  ::close(another_fd);
  auto worker = take(application::ClipPrefetchWorker::start(
      std::move(another), ring, readiness, {48'000, 1, 0, 960, {4, 100}}, {nullptr, now}));
  wait_until([&] { return worker->status().published_frames == 8; },
             "encoded cancellation ring did not fill");
  worker->request_stop();
  wait_until([&] { return worker->finished(); }, "encoded cancellation did not finish");
  REQUIRE_TRUE(worker->join_finished(), "encoded cancellation left worker or readiness active");
  REQUIRE_EQ(worker->status().state, application::ClipPrefetchState::stopped,
             "encoded cancellation left worker or readiness active");
  REQUIRE_TRUE(!readiness->steady(), "encoded cancellation left worker or readiness active");
}

void wav_compatibility_and_fill_failures() {
  InputFd wav(fs::path(OSCILLATE_CODEC_FIXTURE_DIR) / "stereo.wav");
  auto inspected = take(media::WavReader::open_fd(wav.get()));
  project::SourceRecord record{
      id<project::SourceId>(8), project::SourceEncoding::wav,
      std::string(64, 'a'),     fs::file_size(fs::path(OSCILLATE_CODEC_FIXTURE_DIR) / "stereo.wav"),
      inspected.sample_rate(),  inspected.channels(),
      inspected.frame_count()};
  auto wrong = record;
  ++wrong.frames;
  require(!media::AudioReader::open_fd(-1, record), "invalid WAV descriptor was accepted");
  require(!media::AudioReader::open_fd(wav.get(), wrong), "WAV metadata mismatch was accepted");
  auto reader = take(media::AudioReader::open_fd(wav.get(), record));
  REQUIRE_EQ(reader.channels(), 2, "WAV compatibility reader metadata changed");
  REQUIRE_EQ(reader.sample_rate(), 48'000, "WAV compatibility reader metadata changed");
  REQUIRE_EQ(reader.frame_count(), record.frames, "WAV compatibility reader metadata changed");
  std::array<media::StereoFrame, 4> audio{};
  REQUIRE_EQ(reader.read_at(200, audio).value(), audio.size(),
             "WAV compatibility reader lost channels");
  REQUIRE_LT(std::fabs(audio[0].left - 0.25F), 0.00001F, "WAV compatibility reader lost channels");
  REQUIRE_LT(std::fabs(audio[0].right - 0.5F), 0.00001F, "WAV compatibility reader lost channels");
  media::PrefetchRing ring(id<project::ClipId>(9), 8);
  require(!media::fill_from_audio(reader, 44'100, ring, {1, 200}, audio),
          "unsupported WAV rate conversion was accepted");
  require(!media::fill_from_audio(reader, 48'000, ring,
                                  {1, std::numeric_limits<std::uint64_t>::max() - 1}, audio),
          "overflowing source cursor was accepted");
  REQUIRE_EQ(media::fill_from_audio(reader, 48'000, ring, {1, 200}, audio).value(), 4,
             "WAV compatibility fill failed");
}
} // namespace

int main() {
  try {
    exercise_source("sine.aiff");
    exercise_source("sine.flac");
    exercise_source("sine.mp3");
    external_encoded_source_prepares_only_verified_saved_bytes("sine.flac");
    external_encoded_source_prepares_only_verified_saved_bytes("sine.mp3");
    rejection_and_cancellation();
    wav_compatibility_and_fill_failures();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
