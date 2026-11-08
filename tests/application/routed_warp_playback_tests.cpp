// Live playback of saved processed (warped/pitched) clips through the routed
// source: callback consumption, exact timing, seek/loop, handoff, cache
// backing, failures and quality step-down. Synthetic correctness only; this
// is not recorded listening approval.
#include "oscillate/application/clip_warp_edit.hpp"
#include "oscillate/application/project_session.hpp"
#include "oscillate/application/routed_audio_source.hpp"
#include "oscillate/audio_io/stereo_block.hpp"
#include "oscillate/export/offline_master.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/processing/verified_source_view.hpp"
#include "oscillate/processing/warp_render_cache.hpp"
#include "oscillate/processing/warp_stream_map.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"
#include "oscillate/test/wait.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

namespace {
namespace app = oscillate::application;
namespace audio = oscillate::audio_io;
namespace engine = oscillate::engine;
namespace exporting = oscillate::exporting;
namespace foundation = oscillate::foundation;
namespace media = oscillate::media;
namespace persistence = oscillate::persistence;
namespace processing = oscillate::processing;
namespace project = oscillate::project;
namespace timeline = oscillate::timeline;
namespace warp = oscillate::warp_dsp;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

using oscillate::test::require;

using oscillate::test::take;

class Directory : public oscillate::test::TempDir {
public:
  Directory() : TempDir("oscillate-routed-warp") {}
};

class Gate final : public project::TransportGate {
public:
  class Lease final : public project::StoppedTransportLease {};
  foundation::Result<std::unique_ptr<project::StoppedTransportLease>> acquire_stopped() override {
    return foundation::Result<std::unique_ptr<project::StoppedTransportLease>>::success(
        std::make_unique<Lease>());
  }
};

engine::RoutedMixerLimits limits(std::size_t max_frames) {
  return {{1, 64},
          max_frames,
          6ULL * max_frames * sizeof(engine::StereoFrame),
          5ULL * 65 * sizeof(engine::StereoFrame)};
}

void put16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value));
  bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
}
void put32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
  put16(bytes, static_cast<std::uint16_t>(value));
  put16(bytes, static_cast<std::uint16_t>(value >> 16U));
}

std::vector<float> read_export_left(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(input),
                                        std::istreambuf_iterator<char>()};
  REQUIRE_GE(bytes.size(), 12, "processed export is not RIFF");
  REQUIRE_EQ(std::memcmp(bytes.data(), "RIFF", 4), 0, "processed export is not RIFF");
  for (std::size_t at = 12; at + 8 <= bytes.size();) {
    const auto size = static_cast<std::uint32_t>(bytes[at + 4]) |
                      (static_cast<std::uint32_t>(bytes[at + 5]) << 8U) |
                      (static_cast<std::uint32_t>(bytes[at + 6]) << 16U) |
                      (static_cast<std::uint32_t>(bytes[at + 7]) << 24U);
    REQUIRE_LE(at + 8 + size, bytes.size(), "processed export chunk is truncated");
    if (std::memcmp(bytes.data() + at, "data", 4) == 0) {
      REQUIRE_EQ(size % (2 * sizeof(float)), 0, "processed export is not stereo float32");
      std::vector<float> left;
      left.reserve(size / (2 * sizeof(float)));
      for (std::size_t frame = at + 8; frame < at + 8 + size; frame += 2 * sizeof(float)) {
        const auto bits = static_cast<std::uint32_t>(bytes[frame]) |
                          (static_cast<std::uint32_t>(bytes[frame + 1]) << 8U) |
                          (static_cast<std::uint32_t>(bytes[frame + 2]) << 16U) |
                          (static_cast<std::uint32_t>(bytes[frame + 3]) << 24U);
        left.push_back(std::bit_cast<float>(bits));
      }
      return left;
    }
    at += 8 + size + (size & 1U);
  }
  throw std::runtime_error("processed export has no data chunk");
}

// Mono 16-bit PCM from a sample function in [-1, 1].
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
fs::path make_wav(const fs::path& directory, std::uint32_t frames, std::uint32_t rate,
                  const std::function<double(std::uint32_t)>& sample) {
  auto path = directory / "source.wav";
  std::vector<std::uint8_t> bytes{'R', 'I', 'F', 'F'};
  put32(bytes, 36 + frames * 2);
  bytes.insert(bytes.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  put32(bytes, 16);
  put16(bytes, 1);
  put16(bytes, 1);
  put32(bytes, rate);
  put32(bytes, rate * 2);
  put16(bytes, 2);
  put16(bytes, 16);
  bytes.insert(bytes.end(), {'d', 'a', 't', 'a'});
  put32(bytes, frames * 2);
  for (std::uint32_t frame = 0; frame < frames; ++frame)
    put16(bytes, static_cast<std::uint16_t>(
                     static_cast<std::int16_t>(std::lround(32'767.0 * sample(frame)))));
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  require(output.good(), "WAV fixture write failed");
  return path;
}

// A 440 Hz tone with a click every quarter second, so transient analysis has
// real onsets (Beats slices, Complex anchors) and pitch analysis a voiced tone.
double tone_with_clicks(std::uint32_t frame, std::uint32_t rate) {
  const double tone =
      0.3 * std::sin(2.0 * std::numbers::pi * 440.0 * static_cast<double>(frame) / rate);
  return tone + (frame % (rate / 4) == rate / 8 ? 0.6 : 0.0);
}

// A managed project with one audio clip on one track.
struct Song {
  Directory directory;
  fs::path root;
  std::unique_ptr<app::ProjectSession> session;
  project::ClipId clip;

  Song(std::uint32_t frames, std::uint32_t rate, const std::function<double(std::uint32_t)>& sample)
      : root(directory.path() / "song"), session(take(app::ProjectSession::create(root))),
        clip(make(frames, rate, sample)) {}

private:
  project::ClipId make(std::uint32_t frames, std::uint32_t rate,
                       const std::function<double(std::uint32_t)>& sample) {
    const auto wav = make_wav(directory.path(), frames, rate, sample);
    const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
    REQUIRE_GE(fd, 0, "WAV fixture open failed");
    const auto imported = session->imports().import_wav_audio(fd);
    (void)::close(fd);
    REQUIRE_OK(imported, "managed WAV import failed");
    REQUIRE_OK(session->saving().save(), "fixture save failed");
    return session->snapshot().tracks.front().clips.front().id;
  }
};

project::ClipProcessing with_mode(project::WarpMode mode, double pitch = 0.0) {
  project::ClipProcessing processing;
  processing.pitch_semitones = pitch;
  processing.warp.mode = mode;
  processing.warp.mode_overridden = true;
  return processing;
}

// One snapshot with the clip placed at [start, start + length) elapsed frames
// and the given processing.
project::Snapshot placed(const Song& song, std::int64_t start, std::int64_t length,
                         const project::ClipProcessing& processing) {
  auto state = song.session->snapshot();
  auto& clip = state.tracks.front().clips.front();
  clip.placement = project::ElapsedInterval{{start}, {start + length}};
  clip.processing = processing;
  ++clip.settings_generation;
  return state;
}

// Independent continuous offline render of the clip at Q0 (the export and
// cache path), left channel. A lower tier from a later output frame is the
// render a live quality step-down restarts at that frame.
std::vector<float> offline(const fs::path& root, const project::Snapshot& snapshot,
                           project::ClipId clip, warp::QualityTier tier = warp::QualityTier::q0,
                           std::uint64_t first = 0) {
  auto request = take(processing::warp_render_request(snapshot, clip));
  auto view =
      take(processing::ProjectRateSourceView::open(root, request.source, request.project_rate));
  auto map = take(processing::make_warp_stream_map(request.plan, request.source.sample_rate,
                                                   request.project_rate));
  auto analysis = take(processing::analyse_warp_source(request, *view));
  auto stream = take(processing::create_warp_stream(request, analysis, map, tier, 8'192));
  REQUIRE_EQ(stream->start(map, *view, first), warp::StreamStatus::ok,
             "offline stream did not start");
  std::vector<float> rendered;
  std::vector<float> left(8'192), right(8'192);
  while (stream->next_output_frame() < map.output_frames()) {
    const auto count = stream->render(left, right);
    REQUIRE_GT(count, 0, "offline stream stalled");
    rendered.insert(rendered.end(), left.begin(),
                    left.begin() + static_cast<std::ptrdiff_t>(count));
  }
  return rendered;
}

// A routed graph and source for one snapshot, opened as a fresh model.
struct Live {
  Gate gate;
  project::ProjectModel model;
  app::RoutedPlaybackControl control;
  std::unique_ptr<app::RoutedAudioSource> source;

  // Block size and ring budget are positional like RoutedAudioSource::prepare.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  Live(const fs::path& root, const project::Snapshot& snapshot, std::size_t block,
       std::size_t budget, const std::atomic<bool>* permit = nullptr)
      : model(take(project::ProjectModel::open(snapshot, gate))), control(model, limits(block)) {
    REQUIRE_OK(control.submit_current(), "processed routed graph did not prepare");
    source =
        take(app::RoutedAudioSource::prepare(root, *model.snapshot(), control, budget, permit));
  }
};

using oscillate::test::wait_until;

// A paced play's stream end while an armed loop keeps the worker queueing.
constexpr std::uint64_t looping_stream = std::numeric_limits<std::uint64_t>::max();

// Callbacks a stepping loop may make before it fails as stalled.
constexpr std::size_t max_step_attempts = 1'000'000;

// Drives the fake device callback and records what it heard per absolute
// project sample (left channel). Allocation is counted on the callback.
class Player {
public:
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  Player(app::RoutedAudioSource& source, std::size_t block, std::uint64_t timeline)
      : device_(source.render_source()), block_(block), stereo_(block), interleaved_(2 * block),
        heard_(timeline, std::numeric_limits<float>::quiet_NaN()) {}

  // One callback; returns true when it rendered (advanced) rather than held.
  bool step() {
    const auto before = transport_.completed_snapshot();
    counting_allocations = true;
    counted_allocations = 0;
    const auto result =
        audio::render_transport_block(device_, transport_, true, stereo_, interleaved_);
    counting_allocations = false;
    REQUIRE_EQ(result, audio::BlockResult::rendered,
               "processed routed callback failed or allocated");
    REQUIRE_EQ(counted_allocations, 0, "processed routed callback failed or allocated");
    const auto after = transport_.completed_snapshot();
    const auto start = after.generation == before.generation ? before.next_sample : target_;
    if (after.next_sample != start + block_)
      return false;
    for (std::size_t index = 0; index < block_; ++index)
      if (start + index < heard_.size())
        heard_[start + index] = interleaved_[2 * index];
    if (!resumed_at_)
      resumed_at_ = start;
    return true;
  }
  void play_until(std::uint64_t sample) {
    for (std::size_t attempt = 0; transport_.completed_snapshot().next_sample < sample; ++attempt) {
      REQUIRE_LT(attempt, max_step_attempts, "playback did not reach the requested sample");
      if (!step())
        std::this_thread::yield();
    }
  }
  // A planned loop wrap onto a pre-primed generation plays its first block
  // at once, with no hold.
  void loop_wrap(std::uint64_t sample) {
    target_ = sample;
    require(transport_.request_loop_wrap(sample), "loop wrap request was rejected");
    require(step(), "planned loop wrap held instead of playing");
  }
  // A loop wrap the source must not accept (stale or mismatched) becomes
  // a seek: the callback that takes it holds instead of playing.
  void refused_loop_wrap(std::uint64_t sample) {
    target_ = sample;
    require(transport_.request_loop_wrap(sample), "loop wrap request was rejected");
    require(!step(), "a loop wrap onto stale or mismatched frames played at once");
  }
  // Like a real-time device: before each callback, waits until the worker
  // has at least `lead` frames (or the rest of its stream) buffered, so the
  // fake device never outruns it and never forces a quality step-down.
  // `stream_end` is the project sample where the worker's queued stream
  // ends: the clip end, or looping_stream while an armed loop carries it on
  // from the loop start. Near the loop end the clip end is not a bound, and
  // waiting only for the rest of the clip lets the device drain the ring
  // below the step-down threshold whenever the worker is descheduled.
  void paced_play_until(std::uint64_t sample, app::RoutedAudioSource& source,
                        std::uint64_t stream_end, std::size_t lead = 8'192) {
    while (transport_.completed_snapshot().next_sample < sample) {
      const auto position = transport_.completed_snapshot().next_sample;
      const auto need =
          position < stream_end ? std::min<std::uint64_t>(lead, stream_end - position) : 0;
      wait_until(
          [&] {
            const auto status = source.processed_clip_status();
            return status.empty() || status.front().buffered_frames >= need;
          },
          "worker did not keep its lead");
      (void)step();
    }
  }
  void seek(std::uint64_t sample) {
    target_ = sample;
    resumed_at_.reset();
    require(transport_.request_seek(sample), "seek request was rejected");
    // The callback that takes the seek holds on the new generation until
    // the worker has primed it; it never emits stale-generation audio.
    require(!step(), "seek rendered before its generation was primed");
  }
  // A device keeps calling while a new generation primes; the held
  // callbacks let the source discard the old generation's frames. The
  // worker may finish priming between a check and the next callback, which
  // then plays: a hold does not pin the playhead.
  void hold_until(const std::function<bool()>& ready, std::string_view what) {
    const auto deadline = std::chrono::steady_clock::now() + 60s;
    while (!ready()) {
      REQUIRE_LT(std::chrono::steady_clock::now(), deadline, what);
      (void)step();
      std::this_thread::sleep_for(100us);
    }
  }
  // An in-place restart holds on its new generation until primed, without
  // moving the playhead.
  void restart_in_place() {
    target_ = transport_.completed_snapshot().next_sample;
    resumed_at_.reset();
    require(transport_.request_restart_in_place(), "in-place restart was rejected");
    require(!step(), "in-place restart rendered before its generation was primed");
  }
  // Like a device: renders one callback and appends exactly what it emitted
  // (left channel, device order), whatever parts the transport split it into.
  void step_stream(std::vector<float>& stream) {
    counting_allocations = true;
    counted_allocations = 0;
    const auto result =
        audio::render_transport_block(device_, transport_, true, stereo_, interleaved_);
    counting_allocations = false;
    REQUIRE_EQ(result, audio::BlockResult::rendered,
               "processed routed callback failed or allocated");
    REQUIRE_EQ(counted_allocations, 0, "processed routed callback failed or allocated");
    for (std::size_t index = 0; index < block_; ++index)
      stream.push_back(interleaved_[2 * index]);
  }
  // The project sample where the first block rendered since the last seek
  // or in-place restart started, stepping until one renders.
  [[nodiscard]] std::uint64_t resumed_at() {
    for (std::size_t attempt = 0; !resumed_at_; ++attempt) {
      REQUIRE_LT(attempt, max_step_attempts, "playback did not resume");
      if (!step())
        std::this_thread::yield();
    }
    return *resumed_at_;
  }
  [[nodiscard]] float at(std::uint64_t sample) const { return heard_[sample]; }
  [[nodiscard]] audio::TransportCursor& transport() { return transport_; }

private:
  audio::StereoRenderSource device_;
  std::size_t block_;
  std::vector<engine::StereoFrame> stereo_;
  std::vector<float> interleaved_;
  std::vector<float> heard_;
  audio::TransportCursor transport_;
  std::uint64_t target_ = 0;
  std::optional<std::uint64_t> resumed_at_;
};

// Waits until the worker has buffered `frames` ahead (or the whole rest of
// the clip), so a fast fake device cannot outrun it.
void wait_buffered(app::RoutedAudioSource& source, std::size_t frames) {
  wait_until(
      [&] {
        const auto status = source.processed_clip_status();
        return !status.empty() && status.front().buffered_frames >= frames;
      },
      "processed clip did not buffer ahead");
}

std::string mode_name(project::WarpMode mode) {
  return std::string(processing::warp_algorithm_id(mode));
}

// --- Every mode plays live, equal to the offline render ----------------------

void every_mode_plays_live_exactly_as_offline() {
  constexpr std::uint32_t rate = oscillate::test::sample_rate_48k;
  Song song(rate, rate, [](std::uint32_t frame) { return tone_with_clicks(frame, rate); });
  constexpr std::uint64_t start = 1'000;
  constexpr std::uint64_t length = 60'000;
  std::size_t index = 0;
  for (const auto mode :
       {project::WarpMode::repitch, project::WarpMode::beats, project::WarpMode::tones,
        project::WarpMode::texture, project::WarpMode::complex, project::WarpMode::complex_pro}) {
    const std::size_t block = index++ % 2 == 0 ? 64 : 128;
    const auto snapshot = placed(song, static_cast<std::int64_t>(start),
                                 static_cast<std::int64_t>(length), with_mode(mode, 2.0));
    const auto expected = offline(song.root, snapshot, song.clip);
    REQUIRE_EQ(expected.size(), length, mode_name(mode) + " offline duration is not the plan's");
    Live live(song.root, snapshot, block, 65'536);
    const auto status = live.source->status();
    const auto clips = live.source->processed_clip_status();
    require(status.processed_clips == 1 && status.cache_backed_clips == 0 &&
                status.reduced_quality_clips == 0 && clips.size() == 1 &&
                clips.front().clip == song.clip && clips.front().live.mode == mode &&
                clips.front().live.backing == processing::WarpLiveBacking::stream &&
                clips.front().live.tier == warp::QualityTier::q0,
            mode_name(mode) + " was not prepared as a live Q0 processed clip");
    wait_buffered(*live.source, length);
    Player player(*live.source, block, start + length + 1'024);
    player.play_until(start + length + 512);
    for (std::uint64_t sample = 0; sample < start; ++sample)
      REQUIRE_EQ(player.at(sample), 0.0F, mode_name(mode) + " sounded before its clip");
    for (std::uint64_t frame = 0; frame < length; ++frame)
      REQUIRE_EQ(player.at(start + frame), expected[frame],
                 mode_name(mode) + " live frame differs from offline at " + std::to_string(frame));
    for (std::uint64_t sample = start + length; sample < start + length + 512; ++sample)
      REQUIRE_EQ(player.at(sample), 0.0F, mode_name(mode) + " sounded past its clip end");
    require(live.source->status().missing_frames == 0 && !live.source->status().worker_failed &&
                live.source->status().quality_step_downs == 0,
            mode_name(mode) + " underran or stepped down with its whole clip buffered");
  }
}

// --- Exact marker placement ----------------------------------------------------

void markers_land_on_exact_project_frames() {
  constexpr std::uint32_t rate = oscillate::test::sample_rate_48k;
  constexpr std::uint32_t impulse = 12'000;
  Song song(rate, rate, [](std::uint32_t frame) { return frame == impulse ? 0.8 : 0.0; });
  for (const auto mode : {project::WarpMode::repitch, project::WarpMode::beats}) {
    // The impulse's source frame is pinned to output frame 20,000 of a clip
    // placed at 500, so it must sound exactly at project frame 20,500.
    auto processing = with_mode(mode);
    processing.reviewed_warp_markers.push_back({{impulse}, timeline::SamplePosition{20'000}});
    const auto snapshot = placed(song, 500, 60'000, processing);
    Live live(song.root, snapshot, 64, 65'536);
    wait_buffered(*live.source, 60'000);
    Player player(*live.source, 64, 61'000);
    player.play_until(60'600);
    std::uint64_t peak = 0;
    for (std::uint64_t sample = 0; sample < 60'500; ++sample)
      if (std::fabs(player.at(sample)) > std::fabs(player.at(peak)))
        peak = sample;
    REQUIRE_EQ(peak, 20'500,
               mode_name(mode) + " placed the marked impulse at " + std::to_string(peak) +
                   " instead of 20,500");
  }
}

// A reviewed source impulse is pinned one beat into a musical clip. The
// clip begins inside a 120→90 BPM ramp, so a constant-tempo placement would
// put the impulse at a different absolute sample. Compare the saved offline
// WAV with the worker-backed live route at both supported callback sizes.
void saved_marker_and_tempo_ramp_match_live_export() {
  constexpr std::uint32_t rate = oscillate::test::sample_rate_48k;
  constexpr auto beat = timeline::ticks_per_quarter;
  Song song(rate, rate, [](std::uint32_t frame) { return frame == 12'000 ? 0.8 : 0.0; });
  REQUIRE_OK(song.session->song().set_tempo_event({2 * beat}, 90.0, timeline::TempoShape::step),
             "processed parity tempo ramp was rejected");
  REQUIRE_OK(song.session->song().set_tempo_event({0}, 120.0, timeline::TempoShape::linear_bpm),
             "processed parity tempo ramp was rejected");
  for (const auto mode : {project::WarpMode::repitch, project::WarpMode::beats}) {
    auto processing = with_mode(mode);
    processing.reviewed_warp_markers.push_back({{12'000}, timeline::TickPosition{beat}});
    const auto generation =
        song.session->snapshot().tracks.front().clips.front().settings_generation;
    const auto applied = song.session->warp().apply_audio_clip_warp(
        song.clip, generation, project::MusicalInterval{{beat}, {3 * beat}}, processing);
    if (!applied)
      throw std::runtime_error("processed musical clip edit: " + applied.error().message);
    const auto saved_result = song.session->saving().save();
    if (!saved_result)
      throw std::runtime_error("processed musical clip save: " + saved_result.error().message);
    auto store = take(persistence::ProjectStore::open_reader(song.root));
    const auto saved = take(store.load_current()).snapshot;
    const auto start = take(saved.tempo.tick_to_sample({beat}, rate)).value;
    const auto marker = take(saved.tempo.tick_to_sample({2 * beat}, rate)).value;
    const auto end = take(saved.tempo.tick_to_sample({3 * beat}, rate)).value;
    // Independent fixture values: the first and second beats of a linear
    // 120→90 BPM ramp land at 25,638 and 55,235 rounded frames; the next
    // beat is 32,000 frames at 90 BPM.
    require(start == 25'638 && marker == 55'235 && end == 87'235,
            "musical clip did not follow the known tempo-ramp boundaries");
    const auto path = song.directory.path() / (mode_name(mode) + "-tempo-master.wav");
    Gate export_gate;
    auto authority = take(exporting::StoppedExportAuthority::acquire(saved, export_gate));
    REQUIRE_EQ(take(exporting::render_wav_master(saved, store, std::move(authority), {path})),
               exporting::PublishOutcome::durable, "saved processed tempo master did not publish");
    const auto exported = read_export_left(path);
    REQUIRE_EQ(exported.size(), static_cast<std::size_t>(end),
               "export duration disagrees with the musical clip end");
    const auto peak = static_cast<std::size_t>(
        std::ranges::max_element(exported, {}, [](float value) { return std::fabs(value); }) -
        exported.begin());
    REQUIRE_EQ(peak, static_cast<std::size_t>(marker),
               "export displaced the reviewed marker under the tempo ramp");
    for (const std::size_t block : {64U, 128U}) {
      Live live(song.root, saved, block, 262'144);
      REQUIRE_EQ(live.source->status().processed_clips, 1,
                 "live processed tempo clip did not use full quality");
      REQUIRE_EQ(live.source->status().reduced_quality_clips, 0,
                 "live processed tempo clip did not use full quality");
      wait_buffered(*live.source, static_cast<std::size_t>(end - start));
      Player player(*live.source, block, static_cast<std::uint64_t>(end + 512));
      player.play_until(static_cast<std::uint64_t>(end + 256));
      for (std::size_t sample = 0; sample < exported.size(); ++sample)
        REQUIRE_EQ(player.at(sample), exported[sample],
                   mode_name(mode) + " export/live tempo parity differs at " +
                       std::to_string(sample) + " with " + std::to_string(block) + " frames");
      REQUIRE_EQ(live.source->status().missing_frames, 0,
                 "processed tempo playback underrun or quality step-down");
      REQUIRE_EQ(live.source->status().quality_step_downs, 0,
                 "processed tempo playback underrun or quality step-down");
    }
  }
}

// --- Seek, loop wrap, cache backing and fallback --------------------------------

void require_matches(const Player& player, const std::vector<float>& expected, std::uint64_t start,
                     std::uint64_t first, std::uint64_t last, std::string_view what) {
  for (auto sample = first; sample < last; ++sample)
    REQUIRE_EQ(player.at(sample), expected[sample - start],
               std::string(what) + " differs at project sample " + std::to_string(sample));
}

void seeks_and_loop_wraps_resume_exactly() {
  constexpr std::uint32_t rate = oscillate::test::sample_rate_48k;
  Song song(rate, rate, [](std::uint32_t frame) { return tone_with_clicks(frame, rate); });
  constexpr std::uint64_t start = 2'000;
  constexpr std::uint64_t length = 60'000;
  // Re-Pitch, Beats and indexed Tones are exact after any seek; Complex is
  // exact here because it plays from a validated cache render.
  for (const auto mode : {project::WarpMode::repitch, project::WarpMode::beats,
                          project::WarpMode::tones, project::WarpMode::complex}) {
    const auto snapshot = placed(song, start, length, with_mode(mode, -1.0));
    const auto expected = offline(song.root, snapshot, song.clip);
    if (mode == project::WarpMode::complex) {
      auto store = take(persistence::ProjectStore::open_reader(song.root));
      auto cache = take(media::CacheStore::adopt_directory_fd(take(store.open_cache_directory())));
      (void)take(processing::render_warp_to_cache(
          cache, song.root, take(processing::warp_render_request(snapshot, song.clip))));
    }
    Live live(song.root, snapshot, 128, 16'384);
    const bool cached = mode == project::WarpMode::complex;
    REQUIRE_EQ(live.source->status().cache_backed_clips, (cached ? 1U : 0U),
               mode_name(mode) + " cache backing is wrong");
    wait_buffered(*live.source, 16'384);
    Player player(*live.source, 128, start + length + 1'024);
    player.paced_play_until(start + 4'096, *live.source, start + length);
    require_matches(player, expected, start, start, start + 4'096, mode_name(mode) + " start");

    // Seek into the middle of the clip.
    const auto target = start + 31'111;
    player.seek(target);
    player.hold_until([&] { return live.source->status().ready_generation == 2; },
                      "seek generation was not primed");
    wait_buffered(*live.source, 8'192);
    player.paced_play_until(target + 8'192, *live.source, start + length);
    require_matches(player, expected, start, target, target + 8'192, mode_name(mode) + " seek");

    // Arm a loop whose end lies beyond the worker's current lead and on this
    // generation's block grid. The worker stops the generation at the loop
    // end and pre-primes generation 3 from the loop start, so the wrap is
    // gapless: its first block already plays the loop start.
    const auto loop = start + 5'003;
    const auto loop_end = target + std::uint64_t{128} * 200;
    require(live.source->set_loop(loop, loop_end), "loop was not armed");
    player.paced_play_until(loop_end, *live.source, looping_stream);
    wait_until([&] { return live.source->loop_status().ready_generation == 3; },
               "next loop generation was not pre-primed");
    require(!live.source->loop_status().missed, "armed loop reported missed");
    player.loop_wrap(loop);
    wait_buffered(*live.source, 8'192);
    player.paced_play_until(loop + 8'192, *live.source, looping_stream);
    // A step-down restarts the stream at a lower tier, which differs slightly from
    // the Q0 reference; name that cause rather than the sample.
    REQUIRE_EQ(live.source->status().quality_step_downs, 0,
               mode_name(mode) + " stepped quality down: the paced device outran the worker");
    require_matches(player, expected, start, loop, loop + 8'192, mode_name(mode) + " loop wrap");
    require_matches(player, expected, start, target, loop_end, mode_name(mode) + " loop body");
    // Arming a loop whose end the worker has already streamed past misses,
    // so the caller knows to seek instead.
    require(live.source->set_loop(loop, loop + 256), "late loop was rejected outright");
    wait_until([&] { return live.source->loop_status().missed; }, "late loop did not miss");
    require(live.source->status().missing_frames == 0 &&
                live.source->status().quality_step_downs == 0,
            mode_name(mode) + " seek or loop underran or stepped quality down");
  }
}

// Loop readiness belongs to one loop configuration version. Loop A is
// primed; with the worker paused, a rearm to B must at once hide A's
// readiness, and a planned wrap even to A's start (where the queued frames
// would line up) must hold as a seek. The resumed worker primes that seek,
// discarding A's frames; B arms for the new generation and wraps gaplessly.
void rearm_after_ready_never_accepts_the_old_loop() {
  constexpr std::uint32_t rate = oscillate::test::sample_rate_48k;
  Song song(rate, rate, [](std::uint32_t frame) { return tone_with_clicks(frame, rate); });
  constexpr std::uint64_t start = 2'000;
  constexpr std::uint64_t length = 60'000;
  const auto snapshot = placed(song, start, length, with_mode(project::WarpMode::repitch));
  const auto expected = offline(song.root, snapshot, song.clip);
  // The worker starts paused: the ring holds more than the loop end, so a
  // worker let run before the loop is armed may stream past it and miss.
  std::atomic<bool> permit{false};
  Live live(song.root, snapshot, 128, 16'384, &permit);
  Player player(*live.source, 128, start + length + 1'024);
  constexpr std::uint64_t a_start = start + 1'000;
  constexpr std::uint64_t a_end = std::uint64_t{128} * 96; // on the block grid from 0
  require(live.source->set_loop(a_start, a_end), "loop A was not armed");
  permit.store(true, std::memory_order_release);
  player.paced_play_until(a_end, *live.source, looping_stream);
  wait_until([&] { return live.source->loop_status().ready_generation == 2; },
             "loop A was not primed");

  permit.store(false, std::memory_order_release);
  wait_until([&] { return live.source->status().worker_paused; }, "worker did not pause");
  constexpr std::uint64_t b_start = start + 7'777;
  constexpr std::uint64_t b_end = a_start + std::uint64_t{128} * 250; // on the grid after a seek
  require(live.source->set_loop(b_start, b_end), "loop B was not armed");
  REQUIRE_EQ(live.source->loop_status().ready_generation, 0,
             "loop A's readiness escaped through loop B's status");
  REQUIRE_TRUE(!live.source->loop_status().missed,
               "loop A's readiness escaped through loop B's status");
  player.refused_loop_wrap(a_start);
  permit.store(true, std::memory_order_release);
  player.hold_until([&] { return live.source->status().ready_generation == 2; },
                    "refused wrap did not prime as a seek");
  require(!live.source->loop_status().missed, "the seek did not re-arm loop B");
  player.paced_play_until(a_start + 8'192, *live.source, looping_stream);
  require_matches(player, expected, start, a_start, a_start + 8'192, "seek after refused wrap");

  player.paced_play_until(b_end, *live.source, looping_stream);
  wait_until([&] { return live.source->loop_status().ready_generation == 3; },
             "loop B was not primed after the seek");
  player.loop_wrap(b_start);
  player.paced_play_until(b_start + 8'192, *live.source, looping_stream);
  require_matches(player, expected, start, b_start, b_start + 8'192, "loop B wrap");
  REQUIRE_EQ(live.source->status().missing_frames, 0,
             "loop rearm underran or stepped quality down");
  REQUIRE_EQ(live.source->status().quality_step_downs, 0,
             "loop rearm underran or stepped quality down");
}

// With the worker running, a rearm after the old loop's next-generation
// frames are queued is reported missed (they cannot be withdrawn) and never
// ready; a seek discards them and re-arms the new loop for its generation.
void rearm_after_ready_reports_missed_until_seek() {
  constexpr std::uint32_t rate = oscillate::test::sample_rate_48k;
  Song song(rate, rate, [](std::uint32_t frame) { return tone_with_clicks(frame, rate); });
  constexpr std::uint64_t start = 2'000;
  constexpr std::uint64_t length = 60'000;
  const auto snapshot = placed(song, start, length, with_mode(project::WarpMode::tones));
  const auto expected = offline(song.root, snapshot, song.clip);
  // The worker starts paused: the ring holds more than the loop end, so a
  // worker let run before the loop is armed may stream past it and miss.
  std::atomic<bool> permit{false};
  Live live(song.root, snapshot, 128, 16'384, &permit);
  Player player(*live.source, 128, start + length + 1'024);
  constexpr std::uint64_t a_end = std::uint64_t{128} * 100;
  require(live.source->set_loop(start + 300, a_end), "loop A was not armed");
  permit.store(true, std::memory_order_release);
  player.paced_play_until(a_end - 2'048, *live.source, looping_stream);
  wait_until([&] { return live.source->loop_status().ready_generation == 2; },
             "loop A was not primed");
  constexpr std::uint64_t b_start = start + 5'000;
  require(live.source->set_loop(b_start, std::uint64_t{128} * 400), "loop B was not armed");
  wait_until([&] { return live.source->loop_status().missed; },
             "rearm over queued loop A frames was not reported missed");
  REQUIRE_EQ(live.source->loop_status().ready_generation, 0, "missed rearm reported ready");
  const auto position = player.transport().completed_snapshot().next_sample;
  player.seek(position);
  player.hold_until([&] { return live.source->status().ready_generation == 2; },
                    "seek after missed rearm was not primed");
  require(!live.source->loop_status().missed, "seek did not re-arm loop B");
  player.paced_play_until(a_end + 8'192, *live.source, looping_stream);
  require_matches(player, expected, start, position, a_end + 8'192,
                  "play through loop A's old end after rearm");
  REQUIRE_EQ(live.source->status().missing_frames, 0, "missed rearm and seek underran");
}

// Clearing a primed loop hides its readiness at once and reports missed
// (its queued frames cannot be withdrawn) until a seek before the old loop
// end discards them; playback then runs straight through the old loop end.
void clear_after_ready_requires_a_seek() {
  constexpr std::uint32_t rate = oscillate::test::sample_rate_48k;
  Song song(rate, rate, [](std::uint32_t frame) { return tone_with_clicks(frame, rate); });
  constexpr std::uint64_t start = 2'000;
  constexpr std::uint64_t length = 60'000;
  const auto snapshot = placed(song, start, length, with_mode(project::WarpMode::beats));
  const auto expected = offline(song.root, snapshot, song.clip);
  // The worker starts paused: the ring holds more than the loop end, so a
  // worker let run before the loop is armed may stream past it and miss.
  std::atomic<bool> permit{false};
  Live live(song.root, snapshot, 128, 16'384, &permit);
  Player player(*live.source, 128, start + length + 1'024);
  constexpr std::uint64_t loop_end = std::uint64_t{128} * 120;
  require(live.source->set_loop(start + 500, loop_end), "loop was not armed");
  permit.store(true, std::memory_order_release);
  player.paced_play_until(loop_end - 1'024, *live.source, looping_stream);
  wait_until([&] { return live.source->loop_status().ready_generation == 2; },
             "loop was not primed");
  permit.store(false, std::memory_order_release);
  wait_until([&] { return live.source->status().worker_paused; }, "worker did not pause");
  live.source->clear_loop();
  REQUIRE_EQ(live.source->loop_status().ready_generation, 0, "cleared loop still reported ready");
  permit.store(true, std::memory_order_release);
  wait_until([&] { return live.source->loop_status().missed; },
             "clear over queued loop frames was not reported missed");
  const auto position = player.transport().completed_snapshot().next_sample;
  player.seek(position);
  player.hold_until([&] { return live.source->status().ready_generation == 2; },
                    "seek after clear was not primed");
  REQUIRE_TRUE(!live.source->loop_status().missed, "seek after clear left a stale loop state");
  REQUIRE_EQ(live.source->loop_status().ready_generation, 0,
             "seek after clear left a stale loop state");
  player.paced_play_until(loop_end + 8'192, *live.source, start + length);
  require_matches(player, expected, start, position, loop_end + 8'192, "play through old loop");
  REQUIRE_EQ(live.source->status().missing_frames, 0, "clear and seek underran");
}

// A loop end off the device block grid wraps sample-exactly: the transport
// renders the block containing it as two parts, so the device stream is the
// clip up to the last frame before the end, then the loop start, with no
// silent remainder of the block in between.
void unaligned_loop_end_wraps_sample_exactly() {
  constexpr std::uint32_t rate = oscillate::test::sample_rate_48k;
  Song song(rate, rate, [](std::uint32_t frame) { return tone_with_clicks(frame, rate); });
  constexpr std::uint64_t start = 2'000;
  constexpr std::uint64_t length = 60'000;
  const auto snapshot = placed(song, start, length, with_mode(project::WarpMode::repitch, -1.0));
  const auto expected = offline(song.root, snapshot, song.clip);
  Live live(song.root, snapshot, 128, 16'384);
  Player player(*live.source, 128, start + length + 1'024);
  constexpr std::uint64_t loop_start = start + 777;
  constexpr std::uint64_t loop_end = start + 20'011; // 128 does not divide it
  require(live.source->set_loop(loop_start, loop_end), "loop was not armed");
  player.paced_play_until(loop_end - 4'096, *live.source, looping_stream);
  wait_until([&] { return live.source->loop_status().ready_generation == 2; },
             "next loop pass was not primed");
  require(player.transport().request_loop_wrap_at(loop_end, loop_start),
          "scheduled wrap was rejected");
  const auto from = player.transport().completed_snapshot().next_sample;
  std::vector<float> stream;
  while (stream.size() < loop_end - from + 4'096) {
    wait_until(
        [&] {
          const auto status = live.source->processed_clip_status();
          return !status.empty() && status.front().buffered_frames >= 2'048;
        },
        "worker did not keep its lead across the wrap");
    player.step_stream(stream);
  }
  for (std::size_t index = 0; index < stream.size(); ++index) {
    const auto project =
        from + index < loop_end ? from + index : loop_start + (from + index - loop_end);
    REQUIRE_EQ(stream[index], expected[project - start],
               "unaligned loop wrap differs at stream frame " + std::to_string(index));
  }
  const auto counts = player.transport().loop_wrap_counts();
  REQUIRE_EQ(counts.applied, 1, "the wrap was not exact");
  REQUIRE_EQ(counts.late, 0, "the wrap was not exact");
  REQUIRE_EQ(live.source->status().missing_frames, 0, "the exact wrap underran");
}

// Disabling a loop after its next pass was primed: the queued wrap is
// withdrawn (it never applies), and the frames already queued for the pass
// are discarded by an in-place restart, so playback runs straight through
// the old loop end with the right audio and no seek.
void withdrawn_loop_restarts_in_place() {
  constexpr std::uint32_t rate = oscillate::test::sample_rate_48k;
  Song song(rate, rate, [](std::uint32_t frame) { return tone_with_clicks(frame, rate); });
  constexpr std::uint64_t start = 2'000;
  constexpr std::uint64_t length = 60'000;
  const auto snapshot = placed(song, start, length, with_mode(project::WarpMode::beats));
  const auto expected = offline(song.root, snapshot, song.clip);
  // The worker starts paused: the ring holds more than the loop end, so a
  // worker let run before the loop is armed may stream past it and miss.
  std::atomic<bool> permit{false};
  Live live(song.root, snapshot, 128, 16'384, &permit);
  Player player(*live.source, 128, start + length + 1'024);
  constexpr std::uint64_t loop_end = start + 15'001;
  require(live.source->set_loop(start + 500, loop_end), "loop was not armed");
  permit.store(true, std::memory_order_release);
  player.paced_play_until(loop_end - 3'000, *live.source, looping_stream);
  wait_until([&] { return live.source->loop_status().ready_generation == 2; },
             "loop was not primed");
  require(player.transport().request_loop_wrap_at(loop_end, start + 500),
          "scheduled wrap was rejected");
  // Rapid toggling: withdraw, queue again, withdraw again.
  require(player.transport().cancel_loop_wrap(), "queued wrap was not withdrawn");
  require(player.transport().request_loop_wrap_at(loop_end, start + 500),
          "re-queued wrap was rejected");
  require(player.transport().cancel_loop_wrap(), "re-queued wrap was not withdrawn");
  live.source->clear_loop();
  wait_until([&] { return live.source->loop_status().missed; },
             "clear over the primed pass was not reported missed");
  const auto position = player.transport().completed_snapshot().next_sample;
  player.restart_in_place();
  player.hold_until([&] { return live.source->status().ready_generation == 2; },
                    "in-place restart was not primed");
  REQUIRE_EQ(player.resumed_at(), position, "in-place restart moved the playhead");
  require(!live.source->loop_status().missed, "restart left the stale pass reported");
  player.paced_play_until(loop_end + 8'192, *live.source, start + length);
  require_matches(player, expected, start, position, loop_end + 8'192,
                  "play through the withdrawn loop's end");
  REQUIRE_EQ(player.transport().loop_wrap_counts().applied, 0, "a withdrawn wrap applied");
  REQUIRE_EQ(live.source->status().missing_frames, 0, "withdrawal and restart underran");
}

// --- Split processed clips play and export as the whole ---------------------

// The snapshot with `clip` split at clip-relative sample `offset` through the
// shared re-window algebra (what ProjectSession::split_audio_clip_at_sample
// applies), the right half as `right`.
project::Snapshot split_snapshot(const project::Snapshot& snapshot, project::ClipId clip,
                                 std::int64_t offset, project::ClipId right) {
  const auto& original = snapshot.tracks.front().clips.front();
  const auto length = std::get<project::ElapsedInterval>(original.placement).end.value -
                      std::get<project::ElapsedInterval>(original.placement).start.value;
  auto left = take(app::rewindow_processed_clip(snapshot, clip, 0, offset, false));
  auto second = take(app::rewindow_processed_clip(snapshot, clip, offset, length, false));
  auto state = snapshot;
  auto& first = state.tracks.front().clips.front();
  first.placement = left.placement;
  first.source_range = left.source_range;
  first.processing = left.processing;
  auto copy = first;
  copy.id = right;
  copy.placement = second.placement;
  copy.source_range = second.source_range;
  copy.processing = second.processing;
  state.tracks.front().clips.push_back(copy);
  return state;
}

// Splitting at a reviewed marker keeps the map exact, so each half renders
// (offline, the export path) the same frames as the whole clip did there:
// Re-Pitch everywhere, Beats except where the cut starts a new slice; live
// playback of the two halves equals their offline renders frame for frame.
void split_processed_clip_plays_and_exports_as_the_whole() {
  constexpr std::uint32_t rate = oscillate::test::sample_rate_48k;
  Song song(rate, rate, [](std::uint32_t frame) { return tone_with_clicks(frame, rate); });
  constexpr std::int64_t start = 1'000;
  constexpr std::int64_t length = 60'000;
  const auto right = take(project::ClipId::from_bits({0, 9'001}));
  for (const auto mode : {project::WarpMode::repitch, project::WarpMode::beats}) {
    auto processing = with_mode(mode);
    // Source 24,000 plays at clip output 26,000: both segments stretch.
    processing.reviewed_warp_markers.push_back({{24'000}, timeline::SamplePosition{26'000}});
    const auto whole_snapshot = placed(song, start, length, processing);
    const auto whole = offline(song.root, whole_snapshot, song.clip);
    const auto split = split_snapshot(whole_snapshot, song.clip, 26'000, right);
    require(split.tracks.front().clips[0].source_range.end.value == 24'000 &&
                split.tracks.front().clips[1].processing.reviewed_warp_markers.empty(),
            mode_name(mode) + " split at the marker did not use it as the edge");
    const auto left = offline(song.root, split, song.clip);
    const auto second = offline(song.root, split, right);
    require(left.size() == 26'000 && second.size() == length - 26'000,
            mode_name(mode) + " split halves have the wrong durations");
    // The left half is identical. Re-Pitch is identical everywhere; a Beats
    // cut inside a slice starts a new slice at the cut, so the right half
    // differs only until the next preserve point (the click at source 30,000,
    // clip output 26,000 + 6,000 * 34,000 / 24,000 = 34,500) plus the 2 ms
    // (96-frame) fade of the slice it replaces.
    const std::int64_t converged = mode == project::WarpMode::repitch ? 26'000 : 34'500 + 96;
    std::int64_t last_difference = -1;
    for (std::int64_t frame = 0; frame < length; ++frame) {
      const auto heard = frame < 26'000 ? left[static_cast<std::size_t>(frame)]
                                        : second[static_cast<std::size_t>(frame - 26'000)];
      if (heard != whole[static_cast<std::size_t>(frame)])
        last_difference = frame;
    }
    REQUIRE_LT(last_difference, converged,
               mode_name(mode) + " split differs from the whole past its convergence point");
    Live live(song.root, split, 128, 262'144);
    wait_until(
        [&] {
          const auto status = live.source->processed_clip_status();
          return status.size() == 2 && status[0].buffered_frames >= 26'000 &&
                 status[1].buffered_frames >= static_cast<std::size_t>(length - 26'000);
        },
        "split halves did not buffer");
    Player player(*live.source, 128, static_cast<std::uint64_t>(start + length + 1'024));
    player.play_until(static_cast<std::uint64_t>(start + length + 256));
    for (std::int64_t frame = 0; frame < length; ++frame) {
      const auto expected = frame < 26'000 ? left[static_cast<std::size_t>(frame)]
                                           : second[static_cast<std::size_t>(frame - 26'000)];
      REQUIRE_EQ(player.at(static_cast<std::uint64_t>(start + frame)), expected,
                 mode_name(mode) + " live split playback differs from its export at " +
                     std::to_string(frame));
    }
  }
}

// A rearm before any next-generation frame is queued is not a miss: the
// earlier loop's end limit is lifted, so playback continues past it to the
// new loop end and the new loop wraps gaplessly.
void rearm_before_priming_lifts_the_old_limit() {
  constexpr std::uint32_t rate = oscillate::test::sample_rate_48k;
  Song song(rate, rate, [](std::uint32_t frame) { return tone_with_clicks(frame, rate); });
  constexpr std::uint64_t start = 2'000;
  constexpr std::uint64_t length = 60'000;
  const auto snapshot = placed(song, start, length, with_mode(project::WarpMode::repitch));
  const auto expected = offline(song.root, snapshot, song.clip);
  Live live(song.root, snapshot, 128, 16'384);
  Player player(*live.source, 128, start + length + 1'024);
  constexpr std::uint64_t a_end = std::uint64_t{128} * 200; // beyond the 16,384-frame lead
  require(live.source->set_loop(start + 100, a_end), "loop A was not armed");
  wait_buffered(*live.source, 16'384);
  REQUIRE_EQ(live.source->loop_status().ready_generation, 0,
             "unreached loop A reported ready or missed");
  REQUIRE_TRUE(!live.source->loop_status().missed, "unreached loop A reported ready or missed");
  constexpr std::uint64_t b_start = start + 4'321;
  constexpr std::uint64_t b_end = std::uint64_t{128} * 300;
  require(live.source->set_loop(b_start, b_end), "loop B was not armed");
  player.paced_play_until(b_end, *live.source, looping_stream);
  wait_until([&] { return live.source->loop_status().ready_generation == 2; },
             "loop B was not primed");
  require(!live.source->loop_status().missed, "loop B reported missed");
  player.loop_wrap(b_start);
  player.paced_play_until(b_start + 8'192, *live.source, looping_stream);
  require_matches(player, expected, start, start, b_end, "play past loop A's old end");
  require_matches(player, expected, start, b_start, b_start + 8'192, "loop B wrap");
  REQUIRE_EQ(live.source->status().missing_frames, 0, "rearm before priming underran");
}

// One control thread rearms, clears and requests wraps from loop_status()
// while the callback and worker run concurrently (meaningful under TSan).
void concurrent_loop_rearm_and_clear_stress() {
  constexpr std::uint32_t rate = oscillate::test::sample_rate_48k;
  Song song(rate, rate, [](std::uint32_t frame) { return tone_with_clicks(frame, rate); });
  const auto snapshot = placed(song, 0, 60'000, with_mode(project::WarpMode::repitch));
  Live live(song.root, snapshot, 64, 8'192);
  audio::TransportCursor transport;
  std::atomic<bool> done{false};
  std::atomic<std::size_t> callback_allocations{0};
  std::thread callback([&] {
    const auto device = live.source->render_source();
    std::array<engine::StereoFrame, 64> stereo{};
    std::array<float, 128> interleaved{};
    while (!done.load(std::memory_order_acquire)) {
      counting_allocations = true;
      counted_allocations = 0;
      (void)audio::render_transport_block(device, transport, true, stereo, interleaved);
      counting_allocations = false;
      callback_allocations.fetch_add(counted_allocations, std::memory_order_relaxed);
      std::this_thread::sleep_for(50us);
    }
  });
  std::uint64_t loop_start = 0;
  std::size_t wraps = 0;
  for (int round = 0; round < 3'000; ++round) {
    const auto position = transport.completed_snapshot();
    switch (round % 7) {
    case 0:
    case 3:
      // Just beyond the worker's 8,192-frame lead, so the loop can prime.
      loop_start = position.next_sample + 12'288;
      (void)live.source->set_loop(loop_start, loop_start + 4'096);
      break;
    case 5:
      live.source->clear_loop();
      break;
    default:
      break;
    }
    const auto status = live.source->loop_status();
    if (status.ready_generation == position.generation + 1 &&
        transport.request_loop_wrap(loop_start))
      ++wraps;
    std::this_thread::sleep_for(100us);
  }
  done.store(true, std::memory_order_release);
  callback.join();
  REQUIRE_TRUE(!live.source->status().worker_failed,
               "loop stress failed the worker or allocated on the callback");
  REQUIRE_EQ(callback_allocations.load(), 0,
             "loop stress failed the worker or allocated on the callback");
  std::fprintf(stderr, "loop stress: %zu wrap requests\n", wraps);
}

void corrupt_or_missing_cache_falls_back_to_live_render() {
  constexpr std::uint32_t rate = oscillate::test::sample_rate_48k;
  Song song(rate, rate, [](std::uint32_t frame) { return tone_with_clicks(frame, rate); });
  const auto snapshot = placed(song, 0, 52'000, with_mode(project::WarpMode::complex_pro, 3.0));
  const auto expected = offline(song.root, snapshot, song.clip);
  auto store = take(persistence::ProjectStore::open_reader(song.root));
  auto cache = take(media::CacheStore::adopt_directory_fd(take(store.open_cache_directory())));
  const auto request = take(processing::warp_render_request(snapshot, song.clip));
  const auto key = take(processing::render_warp_to_cache(cache, song.root, request));
  const auto play_all = [&](std::size_t cached) {
    Live live(song.root, snapshot, 64, 65'536);
    REQUIRE_EQ(live.source->status().cache_backed_clips, cached, "cache backing is wrong");
    wait_buffered(*live.source, 52'000);
    Player player(*live.source, 64, 53'000);
    player.play_until(52'000);
    require_matches(player, expected, 0, 0, 52'000, "processed playback");
  };
  play_all(1);
  // Damage the stored entry on disk: the store digest no longer matches, the
  // reader misses and the clip renders live, still equal to the Q0 offline
  // render.
  {
    const auto entry = song.root / "cache" / (key.hex() + ".cache");
    std::fstream file(entry, std::ios::in | std::ios::out | std::ios::binary);
    require(file.good(), "cache entry missing before corruption");
    file.seekg(4'096);
    char byte = 0;
    file.read(&byte, 1);
    file.seekp(4'096);
    byte = static_cast<char>(byte ^ 0x40);
    file.write(&byte, 1);
    require(file.good(), "cache corruption fixture failed");
  }
  play_all(0);
  REQUIRE_OK(cache.clear(), "cache clear failed");
  play_all(0);
}

// --- Different-rate source ---------------------------------------------------

double frequency(const std::vector<float>& signal) {
  double first = -1.0;
  double last = -1.0;
  int crossings = 0;
  for (std::size_t i = signal.size() / 5; i + 1 < signal.size() * 4 / 5; ++i) {
    if (signal[i] <= 0.0F && signal[i + 1] > 0.0F) {
      const double at = static_cast<double>(i) + signal[i] / (signal[i] - signal[i + 1]);
      if (first < 0.0)
        first = at;
      last = at;
      ++crossings;
    }
  }
  REQUIRE_GT(crossings, 10, "too few crossings to measure pitch");
  return (crossings - 1) / (last - first);
}

void different_rate_source_plays_at_its_own_pitch() {
  // A 48 kHz source in a project changed to 44.1 kHz: the change rescales the
  // elapsed placement, so the clip keeps its duration, and the processed
  // clip must keep its pitch too.
  constexpr std::uint32_t rate = oscillate::test::sample_rate_48k;
  Song song(rate, rate, [](std::uint32_t frame) {
    return 0.4 * std::sin(2.0 * std::numbers::pi * 440.0 * frame / 48'000.0);
  });
  auto processing = with_mode(project::WarpMode::complex);
  processing.reviewed_warp_markers.push_back({{24'000}, timeline::SamplePosition{24'000}});
  REQUIRE_OK(song.session->audio_clips().set_audio_clip_processing(song.clip, processing),
             "processing edit failed");
  Gate gate;
  auto model = take(project::ProjectModel::open(song.session->snapshot(), gate));
  const auto changed = take(model.apply({project::SetSampleRate{44'100}}));
  const auto& snapshot = *changed.snapshot;
  REQUIRE_EQ(snapshot.sample_rate, 44'100, "project rate change fixture failed");
  REQUIRE_EQ(snapshot.sources.front().sample_rate, 48'000, "project rate change fixture failed");
  Live live(song.root, snapshot, 128, 65'536);
  wait_buffered(*live.source, 44'100);
  Player player(*live.source, 128, 45'000);
  player.play_until(44'100 + 256);
  std::vector<float> heard;
  heard.reserve(44'100);
  for (std::uint64_t sample = 0; sample < 44'100; ++sample)
    heard.push_back(player.at(sample));
  const double cents = 1'200.0 * std::log2(frequency(heard) * 44'100.0 / 440.0);
  REQUIRE_LT(std::fabs(cents), 2.0,
             "different-rate processed clip is off pitch by " + std::to_string(cents) + " cents");
  REQUIRE_EQ(player.at(44'100 + 10), 0.0F,
             "different-rate processed clip did not keep its elapsed duration");
  REQUIRE_NE(player.at(44'099), 0.0F,
             "different-rate processed clip did not keep its elapsed duration");
}

// --- Failures and quality step-down --------------------------------------------

void source_failure_is_reported_as_silence() {
  constexpr std::uint32_t rate = oscillate::test::sample_rate_48k;
  Song song(rate, rate, [](std::uint32_t frame) { return tone_with_clicks(frame, rate); });
  const auto snapshot = placed(song, 0, 60'000, with_mode(project::WarpMode::complex));
  Live live(song.root, snapshot, 64, 4'096);
  // The managed asset is truncated after preparation verified it.
  const auto& record = snapshot.sources.front();
  fs::resize_file(song.root / "assets" / (record.sha256 + ".wav"), 44);
  Player player(*live.source, 64, 61'000);
  player.play_until(20'000);
  wait_until([&] { return live.source->status().worker_failed; },
             "truncated source did not fail the worker");
  REQUIRE_OK(live.source->worker_failure(), "source failure was not reported with its silence");
  REQUIRE_GT(live.source->status().missing_frames, 0,
             "source failure was not reported with its silence");
  player.play_until(30'000);
  REQUIRE_EQ(player.at(29'000), 0.0F, "failed source produced audio instead of silence");
}

void falling_behind_steps_down_quality_with_reported_underrun() {
  constexpr std::uint32_t rate = oscillate::test::sample_rate_48k;
  Song song(rate, rate, [](std::uint32_t frame) { return tone_with_clicks(frame, rate); });
  const auto snapshot = placed(song, 0, 60'000, with_mode(project::WarpMode::complex, 1.0));
  std::atomic<bool> permit{false};
  Live live(song.root, snapshot, 64, 4'096, &permit);
  Player player(*live.source, 64, 61'000);
  // The paused worker cannot keep up: the callback reports an underrun.
  player.play_until(512);
  REQUIRE_GT(live.source->status().missing_frames, 0, "starved processed clip did not underrun");
  const auto underrun = live.source->pop_source_underrun();
  REQUIRE_TRUE(underrun, "processed underrun was not reported");
  REQUIRE_EQ(underrun->clip, song.clip, "processed underrun was not reported");
  REQUIRE_EQ(live.source->status().quality_step_downs, 0,
             "quality stepped down while the worker was paused");
  permit.store(true, std::memory_order_release);
  wait_until([&] { return live.source->status().quality_step_downs >= 1; },
             "worker did not step quality down after an underrun");
  const auto clip = live.source->processed_clip_status().front();
  REQUIRE_EQ(live.source->status().reduced_quality_clips, 1, "step-down status does not show Q1");
  REQUIRE_EQ(clip.live.tier, warp::QualityTier::q1, "step-down status does not show Q1");
  REQUIRE_EQ(clip.live.step_downs, 1, "step-down status does not show Q1");
  // The lower tier keeps rendering: playback continues with audio.
  wait_buffered(*live.source, 4'096);
  player.play_until(4'000);
  float peak = 0.0F;
  for (std::uint64_t sample = 3'000; sample < 4'000; ++sample)
    peak = std::max(peak, std::fabs(player.at(sample)));
  REQUIRE_GT(peak, 0.1F, "stepped-down clip did not keep playing");
}

// The cause of a rare Re-Pitch loop parity failure, forced: once the ring
// has been three-quarters full, a device that drains the lead below a
// quarter of it (here with the worker paused, in the failure a worker
// descheduled under load) makes the worker step down without any underrun.
// Every queued Q0 frame still plays exactly; the next read restarts at Q1,
// which differs from Q0 only slightly. Paced plays must therefore keep the
// lead above that threshold for exact parity.
void draining_a_full_ring_steps_down_at_the_next_read() {
  constexpr std::uint32_t rate = oscillate::test::sample_rate_48k;
  Song song(rate, rate, [](std::uint32_t frame) { return tone_with_clicks(frame, rate); });
  constexpr std::uint64_t length = 60'000;
  constexpr std::size_t block = 128;
  constexpr std::size_t ring = 16'384;
  // RoutedAudioSource::manage_quality steps down below a quarter of the ring.
  constexpr std::size_t low_lead = ring / 4;
  constexpr std::uint64_t checked = 4'096;
  const auto snapshot = placed(song, 0, length, with_mode(project::WarpMode::repitch, -1.0));
  const auto expected = offline(song.root, snapshot, song.clip);
  std::atomic<bool> permit{true};
  Live live(song.root, snapshot, block, ring, &permit);
  Player player(*live.source, block, length + 1'024);
  // A full ring, refilled after one block, proves the worker has seen it
  // three-quarters full.
  wait_buffered(*live.source, ring);
  player.paced_play_until(block, *live.source, length);
  wait_buffered(*live.source, ring);
  permit.store(false, std::memory_order_release);
  wait_until([&] { return live.source->status().worker_paused; }, "worker did not pause");
  const auto buffered = [&] {
    return live.source->processed_clip_status().front().buffered_frames;
  };
  // Each rendered block takes `block` frames from the paused ring.
  for (std::size_t drained = 0; buffered() >= low_lead; ++drained) {
    REQUIRE_LT(drained, ring / block, "draining the queued lead did not converge");
    require(player.step(), "draining the queued lead held");
  }
  const auto position = player.transport().completed_snapshot().next_sample;
  const auto restart = position + buffered();
  REQUIRE_EQ(live.source->status().quality_step_downs, 0,
             "quality stepped down while the worker was paused");
  permit.store(true, std::memory_order_release);
  wait_until([&] { return live.source->status().quality_step_downs == 1; },
             "a lead drained below a quarter of the ring did not step down");
  player.paced_play_until(restart + checked, *live.source, length);
  require_matches(player, expected, 0, 0, restart, "queued Q0 frames before the step-down");
  const auto lower = offline(song.root, snapshot, song.clip, warp::QualityTier::q1, restart);
  require_matches(player, lower, restart, restart, restart + checked,
                  "Q1 restart after the step-down");
  REQUIRE_EQ(live.source->status().missing_frames, 0, "the drained lead underran");
}

// The same recovery serves unprocessed clips: a starved worker resumes ahead
// of the callback instead of queueing frames it has already passed, and no
// quality step applies.
void starved_unprocessed_clip_recovers_after_underrun() {
  constexpr std::uint32_t rate = oscillate::test::sample_rate_48k;
  Song song(rate, rate, [](std::uint32_t frame) { return tone_with_clicks(frame, rate); });
  std::atomic<bool> permit{false};
  Live live(song.root, song.session->snapshot(), 64, 4'096, &permit);
  REQUIRE_EQ(live.source->status().processed_clips, 0, "fixture clip is not unprocessed");
  Player player(*live.source, 64, rate);
  player.play_until(512);
  REQUIRE_GT(live.source->status().missing_frames, 0, "starved clip did not underrun");
  permit.store(true, std::memory_order_release);
  // Paced like a device, the clip comes back within the first second.
  while (player.transport().completed_snapshot().next_sample < 40'000) {
    (void)player.step();
    std::this_thread::sleep_for(500us);
  }
  float peak = 0.0F;
  for (std::uint64_t sample = 39'000; sample < 40'000; ++sample)
    peak = std::max(peak, std::fabs(player.at(sample)));
  REQUIRE_GT(peak, 0.1F, "starved unprocessed clip did not recover after its underrun");
  REQUIRE_EQ(live.source->status().quality_step_downs, 0,
             "starved unprocessed clip did not recover after its underrun");
  REQUIRE_TRUE(!live.source->status().worker_failed,
               "starved unprocessed clip did not recover after its underrun");
}

// --- Saved edits, handoff and reopen ---------------------------------------------

void saved_edit_prepares_handoff_and_survives_reopen() {
  constexpr std::uint32_t rate = oscillate::test::sample_rate_48k;
  Song song(rate, rate, [](std::uint32_t frame) { return tone_with_clicks(frame, rate); });
  // The active graph plays the clip as Complex with a +2 semitone pitch.
  require(song.session->audio_clips()
              .set_audio_clip_processing(song.clip, with_mode(project::WarpMode::complex, 2.0))
              .has_value(),
          "initial processing edit failed");
  Gate gate;
  auto model = take(project::ProjectModel::open(song.session->snapshot(), gate));
  app::RoutedPlaybackControl control(model, limits(64));
  REQUIRE_OK(control.submit_current(), "active graph did not prepare");
  auto active = take(app::RoutedAudioSource::prepare(song.root, *model.snapshot(), control, 8'192));
  Player player(*active, 64, 50'000);
  player.play_until(1'024);
  control.observe();
  const auto active_revision = control.status().active_graph_revision;
  REQUIRE_EQ(active_revision, model.snapshot()->revision, "processed graph did not activate");

  // A saved mode edit is captured as an audio clip edit. Until the handoff
  // completes, the audible source still reports the old mode and the control
  // reports the edit pending, not active.
  auto edited = take(model.apply(
      {project::SetClipProcessing{song.clip, with_mode(project::WarpMode::tones, -3.0)}}));
  auto capture = take(control.capture_source_edit(edited, "warp mode"));
  REQUIRE_EQ(control.status().model_revision, edited.snapshot->revision,
             "saved warp edit was reported active before its source was accepted");
  REQUIRE_EQ(control.status().active_graph_revision, active_revision,
             "saved warp edit was reported active before its source was accepted");
  REQUIRE_EQ(control.status().application, app::PlaybackApplication::pending,
             "saved warp edit was reported active before its source was accepted");
  REQUIRE_EQ(active->processed_clip_status().front().live.mode, project::WarpMode::complex,
             "saved warp edit was reported active before its source was accepted");
  const auto position = player.transport().completed_snapshot();
  auto candidate = take(app::RoutedAudioSource::prepare(
      song.root, capture, control, 8'192, nullptr, {}, position.next_sample, position.generation));
  REQUIRE_EQ(candidate->processed_clip_status().front().live.mode, project::WarpMode::tones,
             "candidate source does not render the saved mode");
  wait_until(
      [&] {
        return candidate->prime_handoff_window(position.generation, position.next_sample, 64) ==
               media::HandoffWindowStatus::ready;
      },
      "candidate processed source did not prime the exact handoff block");

  // Saved and reopened, the project prepares the same processed clip.
  require(song.session->audio_clips()
                  .set_audio_clip_processing(song.clip, with_mode(project::WarpMode::tones, -3.0))
                  .has_value() &&
              song.session->saving().save().has_value(),
          "saved warp edit did not persist");
  song.session.reset();
  auto reopened = take(app::ProjectSession::open(song.root));
  const auto& saved = reopened->snapshot().tracks.front().clips.front().processing;
  REQUIRE_EQ(saved.warp.mode, project::WarpMode::tones, "warp edit did not reopen");
  REQUIRE_EQ(saved.pitch_semitones, -3.0, "warp edit did not reopen");
  Live after(song.root, reopened->snapshot(), 64, 65'536);
  REQUIRE_EQ(after.source->processed_clip_status().front().live.mode, project::WarpMode::tones,
             "reopened project did not prepare the saved mode");
  const auto expected = offline(song.root, reopened->snapshot(), song.clip);
  wait_buffered(*after.source, expected.size());
  Player replay(*after.source, 64, expected.size() + 256);
  replay.play_until(expected.size());
  require_matches(replay, expected, 0, 0, expected.size(), "reopened processed playback");
}

void unprocessed_clips_keep_the_source_path() {
  constexpr std::uint32_t rate = oscillate::test::sample_rate_48k;
  Song song(rate, rate, [](std::uint32_t frame) { return tone_with_clicks(frame, rate); });
  // Re-Pitch with inactive transpose and an unchanged placement plays raw.
  const auto snapshot = placed(song, 0, rate, with_mode(project::WarpMode::repitch, 5.0));
  Live live(song.root, snapshot, 64, 65'536);
  REQUIRE_EQ(live.source->status().processed_clips, 0,
             "an unprocessed clip was routed through the warp renderer");
  REQUIRE_TRUE(live.source->processed_clip_status().empty(),
               "an unprocessed clip was routed through the warp renderer");
}

} // namespace

int main() {
  try {
    every_mode_plays_live_exactly_as_offline();
    markers_land_on_exact_project_frames();
    saved_marker_and_tempo_ramp_match_live_export();
    seeks_and_loop_wraps_resume_exactly();
    rearm_after_ready_never_accepts_the_old_loop();
    rearm_after_ready_reports_missed_until_seek();
    clear_after_ready_requires_a_seek();
    rearm_before_priming_lifts_the_old_limit();
    unaligned_loop_end_wraps_sample_exactly();
    withdrawn_loop_restarts_in_place();
    split_processed_clip_plays_and_exports_as_the_whole();
    concurrent_loop_rearm_and_clear_stress();
    corrupt_or_missing_cache_falls_back_to_live_render();
    different_rate_source_plays_at_its_own_pitch();
    source_failure_is_reported_as_silence();
    falling_behind_steps_down_quality_with_reported_underrun();
    draining_a_full_ring_steps_down_at_the_next_read();
    starved_unprocessed_clip_recovers_after_underrun();
    saved_edit_prepares_handoff_and_survives_reopen();
    unprocessed_clips_keep_the_source_path();
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
  return 0;
}
