#include "../../../apps/daw_quick/src/routed_playback.hpp"

#include "oscillate/application/project_session.hpp"
#include "oscillate/audio_io/stereo_block.hpp"
#include "oscillate/native_dsp/compressor.hpp"
#include "oscillate/native_dsp/effects/ceiling.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/eq.hpp"
#include "oscillate/native_dsp/limiter.hpp"
#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/native_inserts/effect_binding.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/wait.hpp"
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
#include "oscillate/plugins/vst3_scan.hpp"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <initializer_list>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
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
namespace foundation = oscillate::foundation;
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
namespace plugins = oscillate::plugins;
#endif
namespace project = oscillate::project;
namespace quick = oscillate::quick;
namespace fs = std::filesystem;

const project::Vst3PluginInstance& effect_on(const project::Track& track) {
  return std::get<project::Vst3PluginInstance>(track.effects.front().processor);
}

// The rest edit or read saved plugins only in VST3 builds.
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
project::Vst3PluginInstance& effect_on(project::Track& track) {
  return std::get<project::Vst3PluginInstance>(track.effects.front().processor);
}

project::Vst3PluginInstance& instrument_on(project::Track& track) {
  return std::get<project::Vst3PluginInstance>(track.instrument_source);
}

const project::Vst3PluginInstance& instrument_on(const project::Track& track) {
  return std::get<project::Vst3PluginInstance>(track.instrument_source);
}
#endif

using oscillate::test::require;
// Between stepped blocks, time for the source worker to prepare an edit.
constexpr std::chrono::milliseconds worker_step_pause{1};

template <typename T> T take(foundation::Result<T> result) {
  if (!result)
    throw std::runtime_error(result.error().message);
  return std::move(result).value();
}

class Directory {
public:
  Directory() {
    std::string pattern = (fs::temp_directory_path() / "oscillate-quick-route-XXXXXX").string();
    auto* made = ::mkdtemp(pattern.data());
    require(made != nullptr, "temporary directory failed");
    path_ = made;
  }
  ~Directory() {
    std::error_code ignored;
    fs::remove_all(path_, ignored);
  }
  [[nodiscard]] const fs::path& path() const { return path_; }

private:
  fs::path path_;
};

// Built-in effect fixtures (ADR-0047): engine defaults with `changes` applied.
project::NativeEffectPatch
engine_patch(std::string_view engine_id,
             std::initializer_list<std::pair<std::string_view, double>> changes = {}) {
  const auto* engine = oscillate::native_dsp::effects::find_engine(engine_id);
  require(engine != nullptr, "built-in effect fixture engine is missing");
  auto values = oscillate::native_dsp::default_values(*engine->descriptor);
  for (const auto& [id, value] : changes) {
    const auto index = oscillate::native_dsp::find_parameter(*engine->descriptor, id);
    require(index.has_value(), "built-in effect fixture names an unknown parameter");
    values[*index] = value;
  }
  return take(oscillate::native_inserts::make_native_effect_patch(*engine, values));
}

// Shape's band kinds, in its table order.
namespace shape_kind {
constexpr double low_pass = 0;
constexpr double high_pass = 1;
constexpr double low_shelf = 2;
constexpr double high_shelf = 3;
constexpr double peaking = 4;
} // namespace shape_kind

// One EQ band: Shape with only its first band on.
struct Band {
  double kind;
  double frequency_hz;
  double shape;
  double gain_db;
};
project::NativeEffectPatch shape_band(const Band& band) {
  return engine_patch("shape", {{"shape.on_1", 1.0},
                                {"shape.kind_1", band.kind},
                                {"shape.freq_1", band.frequency_hz},
                                {"shape.q_1", band.shape},
                                {"shape.gain_1", band.gain_db}});
}

// Press with the classic compressor's controls.
struct Compression {
  double threshold_db = -18.0;
  double ratio = 4.0;
  double knee_db = 6.0;
  double attack_ms = 10.0;
  double release_ms = 100.0;
  double makeup_db = 0.0;
};
project::NativeEffectPatch press(const Compression& settings) {
  return engine_patch("press", {{"press.threshold", settings.threshold_db},
                                {"press.ratio", settings.ratio},
                                {"press.knee", settings.knee_db},
                                {"press.attack", settings.attack_ms},
                                {"press.release", settings.release_ms},
                                {"press.makeup", settings.makeup_db}});
}

// Ceiling: ceiling dB, lookahead ms, release ms; no room to clip, so what
// it does not limit passes as an exact delay of its latency (ADR-0117).
project::NativeEffectPatch ceiling(double ceiling_db, double lookahead_ms, double release_ms) {
  return engine_patch("ceiling", {{"ceiling.ceiling", ceiling_db},
                                  {"ceiling.lookahead", lookahead_ms},
                                  {"ceiling.release", release_ms},
                                  {"ceiling.transients", 0.0},
                                  {"ceiling.clip", 0.0}});
}

std::uint32_t ceiling_latency(double lookahead_ms) {
  return oscillate::native_dsp::effects::ceiling::latency_frames(lookahead_ms, 48'000.0);
}

// Playback blocks in the limiter Stop tests, and the frames each holds.
constexpr std::size_t stop_block = 64;
constexpr std::size_t stop_block_samples = 2 * stop_block;

void put16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value));
  bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
}
void put32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
  put16(bytes, static_cast<std::uint16_t>(value));
  put16(bytes, static_cast<std::uint16_t>(value >> 16U));
}
struct WavFrameCount {
  std::uint32_t value = 4096;
};
fs::path make_wav(const fs::path& root, std::string_view name = "source.wav",
                  std::uint16_t sample = 8192, WavFrameCount frame_count = {}) {
  auto path = root / name;
  const auto frames = frame_count.value;
  std::vector<std::uint8_t> bytes{'R', 'I', 'F', 'F'};
  put32(bytes, 36 + frames * 2);
  bytes.insert(bytes.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  put32(bytes, 16);
  put16(bytes, 1);
  put16(bytes, 1);
  put32(bytes, 48'000);
  put32(bytes, 96'000);
  put16(bytes, 2);
  put16(bytes, 16);
  bytes.insert(bytes.end(), {'d', 'a', 't', 'a'});
  put32(bytes, frames * 2);
  for (std::uint32_t index = 0; index < frames; ++index)
    put16(bytes, sample);
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  require(output.good(), "WAV fixture write failed");
  return path;
}

fs::path make_ramp_wav(const fs::path& root) {
  constexpr std::uint32_t frames = 4096;
  auto path = root / "ramp.wav";
  std::vector<std::uint8_t> bytes{'R', 'I', 'F', 'F'};
  put32(bytes, 36 + frames * 2);
  bytes.insert(bytes.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  put32(bytes, 16);
  put16(bytes, 1);
  put16(bytes, 1);
  put32(bytes, 48'000);
  put32(bytes, 96'000);
  put16(bytes, 2);
  put16(bytes, 16);
  bytes.insert(bytes.end(), {'d', 'a', 't', 'a'});
  put32(bytes, frames * 2);
  for (std::uint32_t index = 0; index < frames; ++index)
    put16(bytes, static_cast<std::uint16_t>(4096 + index));
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  require(output.good(), "ramp WAV fixture write failed");
  return path;
}

struct OutputEvidence {
  float first_sample = 0.0F;
  std::array<float, 128> first_channel{};
  std::uint64_t completed_sample = 0;
  std::uint32_t requested_buffer_frames = 0;
  bool stopped = false;
  bool stop_off_caller = false;
  bool count_allocations = false;
  std::size_t callback_allocation_count = 0;
  std::thread::id caller;
};

class FakeOutput final : public app::PlaybackOutput {
public:
  FakeOutput(audio::StereoRenderSource source, audio::TransportCursor& cursor,
             OutputEvidence& evidence, bool fail)
      : source_(source), cursor_(cursor), evidence_(evidence), fail_(fail) {}
  // PlaybackOutput fixes the adjacent rate and requested-frame argument order.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  [[nodiscard]] foundation::Result<void> start(std::uint32_t rate, std::uint32_t requested_frames,
                                               std::optional<std::uint64_t>) override {
    evidence_.requested_buffer_frames = requested_frames;
    if (fail_)
      return foundation::Result<void>::failure(
          {foundation::ErrorCode::unavailable, "fake device refused start"});
    std::array<engine::StereoFrame, 128> frames{};
    std::array<float, 256> samples{};
    counted_allocations = 0;
    counting_allocations = evidence_.count_allocations;
    const auto rendered = audio::render_transport_block(
        source_, cursor_, true, std::span(frames).first(requested_frames),
        std::span(samples).first(static_cast<std::size_t>(requested_frames) * 2));
    counting_allocations = false;
    evidence_.callback_allocation_count = counted_allocations;
    require(rendered == audio::BlockResult::rendered, "fake routed output did not render");
    evidence_.first_sample = samples[0];
    for (std::size_t frame = 0; frame < requested_frames; ++frame)
      evidence_.first_channel[frame] = samples[frame * 2];
    evidence_.completed_sample = cursor_.completed_snapshot().next_sample;
    status_.state = audio::OutputState::streaming;
    status_.requested_client_rate = rate;
    return foundation::Result<void>::success();
  }
  void stop() noexcept override {
    evidence_.stopped = true;
    evidence_.stop_off_caller = std::this_thread::get_id() != evidence_.caller;
    status_.state = audio::OutputState::stopped;
  }
  [[nodiscard]] audio::OutputStatus status() const noexcept override { return status_; }

private:
  audio::StereoRenderSource source_;
  audio::TransportCursor& cursor_;
  OutputEvidence& evidence_;
  bool fail_;
  audio::OutputStatus status_;
};

struct SteppedEvidence {
  std::mutex mutex;
  std::condition_variable wake;
  std::size_t permits = 0;
  std::size_t blocks = 0;
  bool stop_requested = false;
  bool stopped = false;
  bool stop_off_caller = false;
  bool count_allocations = false;
  std::uint32_t actual_frames = 0;
  std::size_t callback_allocation_count = 0;
  std::size_t frozen_silent_blocks = 0;
  std::thread::id caller;
  std::vector<float> samples;

  void permit(std::size_t count) {
    {
      std::lock_guard lock(mutex);
      permits += count;
    }
    wake.notify_all();
  }

  void await_blocks(std::size_t count) {
    std::unique_lock lock(mutex);
    require(wake.wait_for(lock, std::chrono::seconds(2), [&] { return blocks >= count; }),
            "fake output did not render permitted blocks");
  }
};

class SteppedOutput final : public app::PlaybackOutput {
public:
  SteppedOutput(audio::StereoRenderSource source, audio::TransportCursor& cursor,
                std::shared_ptr<SteppedEvidence> evidence)
      : source_(source), cursor_(cursor), evidence_(std::move(evidence)) {}
  ~SteppedOutput() override { stop(); }

  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  [[nodiscard]] foundation::Result<void> start(std::uint32_t rate, std::uint32_t requested_frames,
                                               std::optional<std::uint64_t>) override {
    const auto block_frames =
        evidence_->actual_frames == 0 ? requested_frames : evidence_->actual_frames;
    if (requested_frames == 0 || requested_frames > 128 || block_frames > 256)
      return foundation::Result<void>::failure(
          {foundation::ErrorCode::invalid_argument, "stepped output needs 1..256 actual frames"});
    {
      std::lock_guard lock(evidence_->mutex);
      evidence_->stop_requested = false;
    }
    rate_ = rate;
    state_.store(audio::OutputState::streaming, std::memory_order_release);
    worker_ = std::thread([this, block_frames] {
      for (;;) {
        {
          std::unique_lock lock(evidence_->mutex);
          evidence_->wake.wait(
              lock, [&] { return evidence_->stop_requested || evidence_->permits != 0; });
          if (evidence_->stop_requested)
            break;
          --evidence_->permits;
        }
        std::array<engine::StereoFrame, 256> frames{};
        std::array<float, 512> samples{};
        const auto block = std::span(samples).first(std::size_t{block_frames} * 2);
        counted_allocations = 0;
        counting_allocations = evidence_->count_allocations;
        const auto before = cursor_.completed_snapshot();
        const auto result = audio::render_transport_block(
            source_, cursor_, true, std::span(frames).first(block_frames), block);
        const auto after = cursor_.completed_snapshot();
        counting_allocations = false;
        {
          std::lock_guard lock(evidence_->mutex);
          evidence_->callback_allocation_count += counted_allocations;
          if (result == audio::BlockResult::rendered)
            evidence_->samples.insert(evidence_->samples.end(), block.begin(), block.end());
          if (after.next_sample == before.next_sample &&
              std::ranges::all_of(block, [](float value) { return value == 0.0F; }))
            ++evidence_->frozen_silent_blocks;
          ++evidence_->blocks;
        }
        evidence_->wake.notify_all();
      }
    });
    return foundation::Result<void>::success();
  }

  void stop() noexcept override {
    {
      std::lock_guard lock(evidence_->mutex);
      evidence_->stop_requested = true;
    }
    evidence_->wake.notify_all();
    if (worker_.joinable())
      worker_.join();
    {
      std::lock_guard lock(evidence_->mutex);
      evidence_->stopped = true;
      evidence_->stop_off_caller = std::this_thread::get_id() != evidence_->caller;
    }
    state_.store(audio::OutputState::stopped, std::memory_order_release);
    evidence_->wake.notify_all();
  }

  [[nodiscard]] audio::OutputStatus status() const noexcept override {
    audio::OutputStatus result;
    result.state = state_.load(std::memory_order_acquire);
    result.requested_client_rate = rate_;
    return result;
  }

private:
  audio::StereoRenderSource source_;
  audio::TransportCursor& cursor_;
  std::shared_ptr<SteppedEvidence> evidence_;
  std::thread worker_;
  std::atomic<audio::OutputState> state_{audio::OutputState::stopped};
  std::uint32_t rate_ = 0;
};

// Fills as a run does, except that it can refuse the next staged candidate's
// exact fill, as an incomplete candidate does: that block plays the active
// graph and the edit stays pending.
class RefusingCandidateFill final : public quick::RoutedSourceFill {
public:
  void refuse_next_candidate() noexcept { refuse_next_.store(true, std::memory_order_release); }

  [[nodiscard]] bool fill(app::RoutedAudioSource& source, bool exact, engine::BlockContext block,
                          std::size_t frames,
                          std::span<const std::span<const engine::StereoFrame>>& selected,
                          engine::AudioWorkerPool* workers) noexcept override {
    if (exact && refuse_next_.exchange(false, std::memory_order_acq_rel))
      return false;
    return quick::RoutedSourceFill::fill(source, exact, block, frames, selected, workers);
  }

private:
  std::atomic<bool> refuse_next_{false};
};

void routed_controller_reserves_stopped_export_and_retires_in_order() {
  Directory directory;
  const auto root = directory.path() / "song";
  const auto wav = make_wav(directory.path());
  auto session = take(app::ProjectSession::create(root));
  const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  require(fd >= 0, "routed test source open failed");
  auto track = session->imports().import_wav_audio(fd);
  (void)::close(fd);
  require(track.has_value(), "routed test import failed");
  const auto group = take(session->tracks().add_bus_track(project::TrackKind::group_bus));
  const auto returns = take(session->tracks().add_bus_track(project::TrackKind::return_bus));
  require(session->routing().set_main_output(track.value(), group).has_value() &&
              session->routing().add_send(track.value(), returns, 0.5).has_value() &&
              session->saving().save().has_value(),
          "routed test route save failed");
  OutputEvidence evidence{.caller = std::this_thread::get_id()};
  app::PlaybackOutputFactory factory = [&evidence](audio::StereoRenderSource source,
                                                   audio::TransportCursor& cursor) {
    return std::make_unique<FakeOutput>(source, cursor, evidence, false);
  };
  auto snapshot = session->snapshot();
  auto token = take(session->reserve_playback_start());
  auto starter = std::async(std::launch::async, [snapshot = std::move(snapshot), &root, &factory,
                                                 token = std::move(token)]() mutable {
    return quick::RoutedPlayback::prepare_and_start(root, std::move(snapshot), std::move(token),
                                                    factory, std::nullopt, 64);
  });
  auto run = take(starter.get());
  require(std::fabs(evidence.first_sample - 0.375F) < 0.00001F && evidence.completed_sample == 64 &&
              evidence.requested_buffer_frames == 64 &&
              run->status().active_graph_revision == session->snapshot().revision &&
              run->completed_sample() == 64 && run->sample_rate() == 48'000 &&
              run->output_status().state == audio::OutputState::streaming &&
              !run->source_failure().has_value(),
          "saved routed graph was not audible at first fake block");
  (void)run->source_status();
  auto blocked = session->exports().prepare_wav_master_export(
      {root / "blocked.wav", oscillate::exporting::WavFormat::float32});
  require(!blocked && blocked.error().code == foundation::ErrorCode::conflict,
          "stopped export was not excluded during routed playback");
  auto stopper = std::async(std::launch::async, [&run] { run->stop_and_retire(); });
  stopper.get();
  require(evidence.stopped && evidence.stop_off_caller && run->sample_rate() == 0 &&
              run->status().active_graph_revision == 0 && !run->source_failure() &&
              run->output_status().state == audio::OutputState::stopped &&
              session->exports()
                  .prepare_wav_master_export(
                      {root / "after.wav", oscillate::exporting::WavFormat::float32})
                  .has_value(),
          "device/source retirement did not release stopped-export gate");
  (void)run->source_status();
  run.reset();
  session.reset();
  auto reopened = take(app::ProjectSession::open(root));
  require(reopened->snapshot().tracks.size() == 3 &&
              reopened->snapshot().tracks.front().sends.size() == 1,
          "saved routed project did not reopen after playback");
}

void routed_pause_and_seek_keep_runtime_and_exact_completed_sample() {
  Directory directory;
  const auto root = directory.path() / "pause-song";
  const auto wav = make_wav(directory.path());
  auto session = take(app::ProjectSession::create(root));
  const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  require(fd >= 0, "pause source open failed");
  auto imported = session->imports().import_wav_audio(fd);
  (void)::close(fd);
  require(imported && session->saving().save(), "pause source import/save failed");
  OutputEvidence evidence{.caller = std::this_thread::get_id()};
  app::PlaybackOutputFactory factory = [&evidence](audio::StereoRenderSource source,
                                                   audio::TransportCursor& cursor) {
    return std::make_unique<FakeOutput>(source, cursor, evidence, false);
  };
  auto token = take(session->reserve_playback_start());
  auto run = take(quick::RoutedPlayback::prepare_and_start(
      root, session->snapshot(), std::move(token), factory, std::nullopt, 64));
  require(run->completed_sample() == 64 && !run->paused(),
          "first routed block did not complete before pause");
  require(run->pause() && run->paused() && run->completed_sample() == 64 &&
              run->output_status().state == audio::OutputState::stopped,
          "pause did not join output at the exact completed boundary");
  const auto blocked = session->exports().prepare_wav_master_export(
      {root / "paused-export.wav", oscillate::exporting::WavFormat::float32});
  require(!blocked && blocked.error().code == foundation::ErrorCode::conflict,
          "paused graph released the playback reservation prematurely");
  require(run->resume() && !run->paused() && run->completed_sample() >= 64,
          "resume failed or rewound the completed sample");
  require(run->pause().has_value(), "second pause failed");
  const auto before_seek = run->completed_position();
  require(run->seek_paused(1024) && run->completed_sample() == 1024 &&
              run->completed_position().generation == before_seek.generation + 1,
          "paused seek failed to install the requested sample");
  require(!run->seek_paused(std::numeric_limits<std::uint64_t>::max()) &&
              run->completed_sample() == 1024 &&
              run->completed_position().generation == before_seek.generation + 1,
          "invalid paused seek changed the playhead");
  require(run->resume() && run->completed_sample() >= 1024,
          "resumed seek did not start at the requested sample");
  (void)run->stop_and_retire();
  require(session->exports()
              .prepare_wav_master_export(
                  {root / "after-pause.wav", oscillate::exporting::WavFormat::float32})
              .has_value(),
          "Stop after pause/seek did not release playback reservation");
  auto from_later = take(quick::RoutedPlayback::prepare_and_start(
      root, session->snapshot(), take(session->reserve_playback_start()), factory, std::nullopt, 64,
      2048));
  require(from_later->completed_sample() >= 2048 &&
              std::fabs(evidence.first_sample - 0.25F) < 0.00001F &&
              from_later->source_status().missing_frames == 0,
          "starting later in the arrangement used stale beginning-of-source frames");
  (void)from_later->stop_and_retire();
}

void pause_preserves_master_eq_history() {
  Directory directory;
  const auto root = directory.path() / "pause-eq-history";
  const auto wav = make_wav(directory.path());
  auto session = take(app::ProjectSession::create(root));
  const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  require(fd >= 0, "pause EQ fixture could not open WAV");
  (void)take(session->imports().import_wav_audio(fd));
  (void)::close(fd);
  const Band band{shape_kind::high_pass, 100.0, 0.707, 0.0};
  require(session->effects()
              .add_native_effect(project::MasterSignalPath{}, 0, shape_band(band))
              .has_value(),
          "pause EQ fixture did not save the master EQ");

  auto continuous_evidence = std::make_shared<SteppedEvidence>();
  app::PlaybackOutputFactory continuous_factory =
      [continuous_evidence](audio::StereoRenderSource source, audio::TransportCursor& cursor) {
        return std::make_unique<SteppedOutput>(source, cursor, continuous_evidence);
      };
  auto continuous = take(quick::RoutedPlayback::prepare_and_start(
      root, session->snapshot(), take(session->reserve_playback_start()), continuous_factory,
      std::nullopt, 64));
  continuous_evidence->permit(1);
  continuous_evidence->await_blocks(1);
  continuous_evidence->permit(1);
  continuous_evidence->await_blocks(2);
  float expected_after_first_block = 0.0F;
  {
    std::lock_guard lock(continuous_evidence->mutex);
    require(continuous_evidence->samples.size() == 256,
            "continuous EQ fixture did not render two audio blocks");
    expected_after_first_block = continuous_evidence->samples[128];
  }
  (void)continuous->stop_and_retire();

  OutputEvidence paused_evidence{.caller = std::this_thread::get_id()};
  app::PlaybackOutputFactory paused_factory = [&paused_evidence](audio::StereoRenderSource source,
                                                                 audio::TransportCursor& cursor) {
    return std::make_unique<FakeOutput>(source, cursor, paused_evidence, false);
  };
  auto paused = take(quick::RoutedPlayback::prepare_and_start(
      root, session->snapshot(), take(session->reserve_playback_start()), paused_factory,
      std::nullopt, 64));
  const float first_block = paused_evidence.first_sample;
  require(paused->pause() && paused->resume(), "EQ fixture could not pause and resume");
  require(std::fabs(first_block - expected_after_first_block) > 0.001F &&
              std::fabs(paused_evidence.first_sample - expected_after_first_block) < 0.00001F,
          "pause discarded running EQ history instead of continuing the same sample stream");
  (void)paused->stop_and_retire();
}

void live_routed_level_edit_becomes_audible_without_restarting_output() {
  Directory directory;
  const auto root = directory.path() / "live-level-song";
  const auto wav = make_wav(directory.path(), "source.wav", 8192, {32'768});
  auto session = take(app::ProjectSession::create(root));
  const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  require(fd >= 0, "live-level source open failed");
  const auto track = take(session->imports().import_wav_audio(fd));
  (void)::close(fd);
  auto evidence = std::make_shared<SteppedEvidence>();
  app::PlaybackOutputFactory factory = [evidence](audio::StereoRenderSource source,
                                                  audio::TransportCursor& cursor) {
    return std::make_unique<SteppedOutput>(source, cursor, evidence);
  };
  auto run = take(quick::RoutedPlayback::prepare_and_start(root, session->snapshot(),
                                                           take(session->reserve_playback_start()),
                                                           factory, std::nullopt, 64));
  evidence->permit(1);
  evidence->await_blocks(1);
  const auto original_revision = run->status().active_graph_revision;
  require(original_revision == session->snapshot().revision && run->completed_sample() == 64,
          "live-level fixture did not start at its saved revision");

  require(session->tracks().set_track_level_pan(track, 0.5, 0.0).has_value(),
          "live level edit was not saved");
  const auto edited_revision = session->snapshot().revision;
  require(run->submit_revision(session->snapshot()).has_value() && run->edit_pending() &&
              run->status().active_graph_revision == original_revision &&
              run->graph_handoff_in_progress() && !run->pause(),
          "live level edit did not remain pending until a callback boundary");
  evidence->permit(1);
  evidence->await_blocks(2);
  require(run->status().active_graph_revision == edited_revision && !run->edit_pending() &&
              run->output_status().state == audio::OutputState::streaming,
          "live level edit did not activate without a held audio block");
  evidence->permit(1);
  evidence->await_blocks(3);
  require(run->status().active_graph_revision == edited_revision && !run->edit_pending() &&
              run->output_status().state == audio::OutputState::streaming,
          "live level graph was not activated on the running output");
  evidence->permit(2);
  evidence->await_blocks(5);
  {
    std::lock_guard lock(evidence->mutex);
    require(evidence->samples.size() == std::size_t{5} * 128 &&
                std::fabs(evidence->samples.back() - 0.125F) < 0.00001F &&
                std::ranges::all_of(evidence->samples, [](float value) { return value > 0.0F; }),
            "live level edit did not reach its audible target after the gain ramp");
  }
  require(run->completed_sample() >= std::uint64_t{4} * 64,
          "live level edit stopped the output or rewound the transport");

  std::size_t completed_blocks = 5;
  constexpr std::uint64_t channel_gesture = 7'777;
  for (const double level : {0.6, 0.4, 0.25}) {
    require(session->tracks().set_track_level_pan(track, level, 0.0, channel_gesture).has_value() &&
                run->submit_revision(session->snapshot()).has_value(),
            "running channel gesture rejected a saved intermediate value");
  }
  const auto gesture_revision = session->snapshot().revision;
  bool gesture_active = false;
  for (std::size_t attempt = 0; attempt < 8; ++attempt) {
    evidence->permit(1);
    evidence->await_blocks(++completed_blocks);
    const auto status = run->status();
    gesture_active = status.active_graph_revision == gesture_revision && !run->edit_pending();
    if (gesture_active)
      break;
  }
  require(gesture_active, "running channel gesture did not activate its latest value");
  evidence->permit(1);
  evidence->await_blocks(++completed_blocks);
  (void)run->status();
  {
    std::lock_guard lock(evidence->mutex);
    require(std::fabs(evidence->samples.back() - 0.0625F) < 0.00001F &&
                std::ranges::all_of(evidence->samples, [](float value) { return value > 0.0F; }),
            "running channel gesture lost audio or failed to reach its final level");
  }
  require(session->tracks().set_track_level_pan(track, 0.5, 0.0).has_value() &&
              run->submit_revision(session->snapshot()).has_value(),
          "channel gesture fixture could not restore its original level");
  const auto restored_revision = session->snapshot().revision;
  for (std::size_t attempt = 0;
       attempt < 8 && run->status().active_graph_revision != restored_revision; ++attempt) {
    evidence->permit(1);
    evidence->await_blocks(++completed_blocks);
  }
  require(run->status().active_graph_revision == restored_revision,
          "channel level did not restore while playback continued");
  evidence->permit(2);
  completed_blocks += 2;
  evidence->await_blocks(completed_blocks);

  const auto clip = session->snapshot().tracks.front().clips.front().id;
  const auto activate = [&](std::uint64_t revision) {
    for (std::size_t index = 0; index < 128; ++index) {
      // The worker prepares between stepped blocks and exposes no "ready"
      // signal before the block that takes it: give it a moment per block.
      std::this_thread::sleep_for(worker_step_pause);
      (void)run->status();
      evidence->permit(1);
      evidence->await_blocks(++completed_blocks);
      const auto current = run->status();
      if (current.application == app::PlaybackApplication::failed)
        throw std::runtime_error("live clip handoff failed: " +
                                 (current.failure ? current.failure->message : "unknown"));
      if (current.active_graph_revision == revision && !run->edit_pending())
        return;
    }
    throw std::runtime_error("live clip edit did not activate without restarting output");
  };
  require(session->audio_clips()
              .move_audio_clip(clip, {4 * oscillate::timeline::ticks_per_quarter})
              .has_value(),
          "live clip move was not accepted into the project");
  const auto moved_revision = session->snapshot().revision;
  require(run->submit_revision(session->snapshot()).has_value(),
          "live clip move was not submitted for source preparation");
  activate(moved_revision);
  require(run->output_status().state == audio::OutputState::streaming,
          "live clip move did not activate without restarting output");
  {
    std::lock_guard lock(evidence->mutex);
    require(std::fabs(evidence->samples.back()) < 0.00001F,
            "live clip move kept rendering the old audio source");
  }
  require(session->audio_clips().move_audio_clip(clip, {0}).has_value() &&
              run->submit_revision(session->snapshot()).has_value(),
          "second live source handoff was not accepted");
  activate(session->snapshot().revision);
  {
    std::lock_guard lock(evidence->mutex);
    require(std::fabs(evidence->samples.back() - 0.125F) < 0.00001F,
            "second live source handoff did not restore the clip audio");
  }
  require(session->audio_clips().duplicate_audio_clip_after(clip).has_value() &&
              session->snapshot().tracks.front().clips.size() == 2 &&
              run->submit_revision(session->snapshot()).has_value(),
          "live audio clip duplication was not accepted");
  const auto duplicate = session->snapshot().tracks.front().clips.back().id;
  activate(session->snapshot().revision);
  {
    std::lock_guard lock(evidence->mutex);
    require(std::fabs(evidence->samples.back() - 0.125F) < 0.00001F,
            "duplicating a future clip interrupted the current source");
  }
  require(session->audio_clips()
                  .set_audio_clip_fades(
                      clip, {1.0, 0.0, project::FadeCurve::linear, project::FadeCurve::linear})
                  .has_value() &&
              run->submit_revision(session->snapshot()).has_value(),
          "live audio clip fade was not accepted");
  activate(session->snapshot().revision);
  {
    std::lock_guard lock(evidence->mutex);
    require(evidence->samples.back() > 0.0F && evidence->samples.back() < 0.125F,
            "live clip fade did not attenuate the playing source");
  }
  require(session->audio_clips().set_audio_clip_fades(clip, {}).has_value() &&
              run->submit_revision(session->snapshot()).has_value(),
          "clearing a live audio clip fade was not accepted");
  activate(session->snapshot().revision);
  {
    std::lock_guard lock(evidence->mutex);
    require(std::fabs(evidence->samples.back() - 0.125F) < 0.00001F,
            "clearing a live clip fade did not restore its source level");
  }
  require(session->audio_clips()
                  .trim_audio_clip(clip, app::AudioClipEdge::left, {20'000})
                  .has_value() &&
              run->submit_revision(session->snapshot()).has_value(),
          "live clip trim was not accepted");
  activate(session->snapshot().revision);
  {
    std::lock_guard lock(evidence->mutex);
    require(std::fabs(evidence->samples.back()) < 0.00001F,
            "live trim kept rendering the removed clip prefix");
  }
  const auto split_result = session->audio_clips().split_audio_clip_at_sample(clip, {21'000});
  if (!split_result)
    throw std::runtime_error("live audio clip split failed: " + split_result.error().message);
  require(session->snapshot().tracks.front().clips.size() == 3 &&
              run->submit_revision(session->snapshot()).has_value(),
          "live audio clip split was not accepted");
  const auto& split_clips = session->snapshot().tracks.front().clips;
  const auto split = std::ranges::find_if(split_clips, [&](const project::Clip& item) {
    return item.id != clip && item.id != duplicate;
  });
  require(split != split_clips.end(), "split clip identity was not saved");
  const auto split_id = split->id;
  activate(session->snapshot().revision);
  require(session->audio_clips().remove_audio_clip(clip).has_value() &&
              session->snapshot().tracks.front().clips.size() == 2 &&
              run->submit_revision(session->snapshot()).has_value(),
          "live audio clip deletion was not accepted");
  activate(session->snapshot().revision);
  require(run->output_status().state == audio::OutputState::streaming &&
              run->source_status().missing_frames == 0,
          "deleting an audio clip interrupted playback or source delivery");
  require(session->audio_clips().remove_audio_clip(split_id).has_value() &&
              run->submit_revision(session->snapshot()).has_value(),
          "deleting the split clip was not accepted");
  activate(session->snapshot().revision);
  require(session->audio_clips().remove_audio_clip(duplicate).has_value() &&
              session->snapshot().tracks.front().clips.empty() &&
              run->submit_revision(session->snapshot()).has_value(),
          "deleting the last audio clip was not accepted");
  activate(session->snapshot().revision);
  {
    std::lock_guard lock(evidence->mutex);
    require(std::fabs(evidence->samples.back()) < 0.00001F,
            "the empty audio track still produced its deleted clip");
  }
  (void)run->stop_and_retire();
}

void live_send_gain_edit_reaches_running_quick_output(std::uint32_t frames) {
  Directory directory;
  const auto root = directory.path() / "live-send-gain-song";
  const auto wav = make_wav(directory.path(), "source.wav", 8192, {32'768});
  auto session = take(app::ProjectSession::create(root));
  const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  require(fd >= 0, "live send source open failed");
  const auto track = take(session->imports().import_wav_audio(fd));
  (void)::close(fd);
  const auto returns = take(session->tracks().add_bus_track(project::TrackKind::return_bus));
  const auto send = take(session->routing().add_send(track, returns, 0.25));
  auto evidence = std::make_shared<SteppedEvidence>();
  evidence->count_allocations = true;
  app::PlaybackOutputFactory factory = [evidence](audio::StereoRenderSource source,
                                                  audio::TransportCursor& cursor) {
    return std::make_unique<SteppedOutput>(source, cursor, evidence);
  };
  auto run = take(quick::RoutedPlayback::prepare_and_start(root, session->snapshot(),
                                                           take(session->reserve_playback_start()),
                                                           factory, std::nullopt, frames));
  evidence->permit(1);
  evidence->await_blocks(1);
  const auto original_revision = run->status().active_graph_revision;
  require(original_revision == session->snapshot().revision &&
              session->routing().set_send_gain(send, 0.5).has_value(),
          "live send gain fixture did not save its edit");
  const auto next_revision = session->snapshot().revision;
  require(run->submit_revision(session->snapshot()).has_value() &&
              run->status().active_graph_revision == original_revision && run->edit_pending(),
          "Quick send gain claimed an edit audible before a callback boundary");
  evidence->permit(1);
  evidence->await_blocks(2);
  require(run->status().active_graph_revision == next_revision && !run->edit_pending() &&
              run->output_status().state == audio::OutputState::streaming,
          "Quick send gain did not activate while output kept running");
  const auto settle_blocks = std::uint32_t{128} / frames;
  if (settle_blocks > 1) {
    evidence->permit(settle_blocks - 1);
    evidence->await_blocks(1 + settle_blocks);
  }
  {
    std::lock_guard lock(evidence->mutex);
    require(evidence->samples.size() == (1 + settle_blocks) * std::size_t{frames} * 2,
            "Quick live send gain inserted a silent or incomplete callback block");
    const auto first = std::size_t{frames} * 2;
    const auto last = evidence->samples.size() - 2;
    require(evidence->callback_allocation_count == 0 && evidence->frozen_silent_blocks == 0 &&
                std::fabs(evidence->samples[0] - 0.3125F) < 0.00001F &&
                evidence->samples[first] > 0.3125F && evidence->samples[first] < 0.375F &&
                std::fabs(evidence->samples[last] - 0.375F) < 0.00001F,
            "Quick live send gain lost its ramp, audio or callback safety");
  }
  (void)run->stop_and_retire();
}

// Source-only publication keeps every device block audible and advances the
// project cursor exactly once, even while a worker prepares the edited clip.
void live_clip_move_overlay_keeps_source_timing(std::uint32_t frames,
                                                bool refuse_first_candidate = false) {
  Directory directory;
  const auto root = directory.path() / "live-clip-hold";
  const auto wav = make_ramp_wav(directory.path());
  auto session = take(app::ProjectSession::create(root));
  const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  require(fd >= 0, "live clip ramp open failed");
  (void)take(session->imports().import_wav_audio(fd));
  (void)::close(fd);
  const auto clip = session->snapshot().tracks.front().clips.front().id;
  const auto original_placement = session->snapshot().tracks.front().clips.front().placement;
  auto evidence = std::make_shared<SteppedEvidence>();
  evidence->count_allocations = true;
  app::PlaybackOutputFactory factory = [evidence](audio::StereoRenderSource source,
                                                  audio::TransportCursor& cursor) {
    return std::make_unique<SteppedOutput>(source, cursor, evidence);
  };
  auto candidate_fill = std::make_shared<RefusingCandidateFill>();
  auto run = take(quick::RoutedPlayback::prepare_and_start(
      root, session->snapshot(), take(session->reserve_playback_start()), factory, std::nullopt,
      frames, 0, {}, {}, candidate_fill));
  evidence->permit(1);
  evidence->await_blocks(1);
  const auto original_revision = run->status().active_graph_revision;
  require(run->completed_sample() == frames, "ramp playback did not reach first boundary");
  {
    std::lock_guard lock(evidence->mutex);
    require(evidence->samples.size() == static_cast<std::size_t>(frames) * 2 &&
                evidence->samples[0] == 4096.0F / 32768.0F &&
                evidence->samples[static_cast<std::size_t>(frames - 1) * 2] ==
                    static_cast<float>(4096 + frames - 1) / 32768.0F,
            "ramp source did not reach the correct initial samples");
  }

  // One 960th of a beat at this tempo is exactly 25 project samples. Both
  // the old and moved clip cover the handoff, making a silent block observable.
  constexpr auto move_tick = oscillate::timeline::ticks_per_quarter / 960;
  require(session->audio_clips().move_audio_clip(clip, {move_tick}).has_value(),
          "live clip move did not save");
  const auto moved_placement = session->snapshot().tracks.front().clips.front().placement;
  require(session->snapshot().revision == original_revision + 1 &&
              std::get<project::ElapsedInterval>(moved_placement).start.value == 25,
          "clip move did not create exactly one revision at the exact sample");
  const auto edited_revision = session->snapshot().revision;
  require(run->submit_revision(session->snapshot()).has_value(),
          "live clip move was refused by the source preparer");
  auto status = run->status();
  require(status.model_revision == edited_revision &&
              status.active_graph_revision == original_revision &&
              status.application == app::PlaybackApplication::pending,
          "pending source edit was represented as audible");
  oscillate::test::wait_until(
      [&] {
        status = run->status();
        return run->graph_handoff_in_progress();
      },
      "the routed run reached the awaited state");
  require(run->graph_handoff_in_progress() && run->completed_sample() == frames,
          "prepared source did not offer an exact source-only revision");
  if (refuse_first_candidate)
    candidate_fill->refuse_next_candidate();
  evidence->permit(1);
  evidence->await_blocks(2);
  status = run->status();
  {
    std::lock_guard lock(evidence->mutex);
    require(evidence->frozen_silent_blocks == 0 &&
                evidence->samples.size() == static_cast<std::size_t>(frames) * 4 &&
                evidence->samples[static_cast<std::size_t>(frames) * 2] ==
                    static_cast<float>(4096 + frames - (refuse_first_candidate ? 0 : 25)) /
                        32768.0F &&
                evidence->callback_allocation_count == 0,
            std::string("source-only switch inserted silence or lost timing: ") +
                std::to_string(evidence->frozen_silent_blocks) + "/" +
                std::to_string(evidence->samples.size()) + "/" +
                std::to_string(evidence->callback_allocation_count));
  }
  require(status.model_revision == edited_revision &&
              status.active_graph_revision ==
                  (refuse_first_candidate ? original_revision : edited_revision) &&
              status.application == (refuse_first_candidate ? app::PlaybackApplication::pending
                                                            : app::PlaybackApplication::active) &&
              run->completed_sample() == static_cast<std::uint64_t>(frames) * 2 &&
              run->output_status().state == audio::OutputState::streaming,
          "candidate miss did not keep the old audible revision and moving cursor");
  evidence->permit(1);
  evidence->await_blocks(3);
  status = run->status();
  require(run->completed_sample() == static_cast<std::uint64_t>(frames) * 3,
          "source-only handoff did not keep a monotonic project cursor");
  {
    std::lock_guard lock(evidence->mutex);
    require(
        evidence->samples.size() == static_cast<std::size_t>(frames) * 6 &&
            evidence->samples[static_cast<std::size_t>(frames) * 4] ==
                static_cast<float>(4096 + 2 * frames - (refuse_first_candidate ? 0 : 25)) /
                    32768.0F &&
            evidence->frozen_silent_blocks == 0 && evidence->callback_allocation_count == 0,
        std::string("new clip geometry read the wrong source frame or allocated in callback: ") +
            std::to_string(evidence->samples.size()) + "/" +
            std::to_string(evidence->samples[static_cast<std::size_t>(frames) * 4]) + "/" +
            std::to_string(evidence->frozen_silent_blocks) + "/" +
            std::to_string(evidence->callback_allocation_count));
  }
  require(status.active_graph_revision ==
                  (refuse_first_candidate ? original_revision : edited_revision) &&
              status.application == (refuse_first_candidate ? app::PlaybackApplication::pending
                                                            : app::PlaybackApplication::active),
          "candidate refusal falsely activated the saved edit or froze old audio");
  (void)run->stop_and_retire();
  require(session->history().undo().has_value() &&
              session->snapshot().tracks.front().clips.front().placement == original_placement &&
              session->history().redo().has_value() &&
              session->snapshot().tracks.front().clips.front().placement == moved_placement,
          "live clip move did not remain one undoable edit");
  require(session->saving().save().has_value(), "live clip move failed to save");
  session.reset();
  auto reopened = take(app::ProjectSession::open(root));
  require(reopened->snapshot().tracks.front().clips.front().placement == moved_placement,
          "live clip placement changed on reopen");
  if (refuse_first_candidate) {
    auto restart_evidence = std::make_shared<SteppedEvidence>();
    app::PlaybackOutputFactory restart_factory =
        [restart_evidence](audio::StereoRenderSource source, audio::TransportCursor& cursor) {
          return std::make_unique<SteppedOutput>(source, cursor, restart_evidence);
        };
    auto restarted = take(quick::RoutedPlayback::prepare_and_start(
        root, reopened->snapshot(), take(reopened->reserve_playback_start()), restart_factory,
        std::nullopt, frames));
    restart_evidence->permit(1);
    restart_evidence->await_blocks(1);
    const auto restarted_status = restarted->status();
    {
      std::lock_guard lock(restart_evidence->mutex);
      require(restart_evidence->samples.size() == static_cast<std::size_t>(frames) * 2 &&
                  restart_evidence->samples.front() == 0.0F &&
                  restart_evidence->samples[std::size_t{25} * 2] == 4096.0F / 32768.0F &&
                  restart_evidence->frozen_silent_blocks == 0,
              "Stop and Play did not render the saved clip move from its new position");
    }
    require(restarted_status.model_revision == reopened->snapshot().revision &&
                restarted_status.active_graph_revision == reopened->snapshot().revision &&
                restarted_status.application == app::PlaybackApplication::active,
            "Stop and Play did not reconcile the saved and audible revisions");
    (void)restarted->stop_and_retire();
  }
}

void live_clip_overlay_keeps_master_eq_history(std::uint32_t frames) {
  Directory directory;
  const auto root = directory.path() / "live-clip-master-eq";
  const auto wav = make_wav(directory.path());
  auto session = take(app::ProjectSession::create(root));
  const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  require(fd >= 0, "master EQ fixture source open failed");
  (void)take(session->imports().import_wav_audio(fd));
  (void)::close(fd);
  const Band band{shape_kind::high_pass, 1'000.0, 0.707, 0.0};
  (void)take(
      session->effects().add_native_effect(project::MasterSignalPath{}, 0, shape_band(band)));
  const std::array eq_settings{oscillate::native_dsp::EqBandSettings{
      oscillate::native_dsp::EqBandKind::high_pass, band.frequency_hz, band.shape, band.gain_db}};
  oscillate::native_dsp::EqRuntime expected_eq(
      take(oscillate::native_dsp::prepare_eq(eq_settings, 48'000)));
  auto evidence = std::make_shared<SteppedEvidence>();
  app::PlaybackOutputFactory factory = [evidence](audio::StereoRenderSource source,
                                                  audio::TransportCursor& cursor) {
    return std::make_unique<SteppedOutput>(source, cursor, evidence);
  };
  auto run = take(quick::RoutedPlayback::prepare_and_start(root, session->snapshot(),
                                                           take(session->reserve_playback_start()),
                                                           factory, std::nullopt, frames));
  evidence->permit(1);
  evidence->await_blocks(1);
  const auto clip = session->snapshot().tracks.front().clips.front().id;
  constexpr auto move_tick = oscillate::timeline::ticks_per_quarter / 960;
  require(session->audio_clips().move_audio_clip(clip, {move_tick}) &&
              run->submit_revision(session->snapshot()),
          "master EQ live clip move was refused");
  oscillate::test::wait_until(
      [&] {
        (void)run->status();
        return run->graph_handoff_in_progress();
      },
      "the routed run reached the awaited state");
  require(run->graph_handoff_in_progress(), "master EQ source overlay did not prepare");
  evidence->permit(1);
  evidence->await_blocks(2);
  require(run->status().active_graph_revision == session->snapshot().revision,
          "master EQ source overlay did not activate");
  std::array<engine::StereoFrame, 256> expected{};
  expected.fill({0.25F, 0.25F});
  expected_eq.process(std::span(expected).first(frames), {0, 1});
  expected_eq.process(std::span(expected).subspan(frames, frames), {frames, 1});
  {
    std::lock_guard lock(evidence->mutex);
    require(evidence->samples.size() == static_cast<std::size_t>(frames) * 4 &&
                evidence->frozen_silent_blocks == 0,
            "master EQ source overlay inserted a held block");
    for (std::size_t index = 0; index < static_cast<std::size_t>(frames) * 2; ++index) {
      require(std::fabs(evidence->samples[index * 2] - expected[index].left) < 0.00001F,
              "master EQ history reset across source-only revision");
    }
  }
  (void)run->stop_and_retire();
}

void live_clip_trim_overlay_keeps_playing(std::uint32_t frames) {
  Directory directory;
  const auto root = directory.path() / "live-clip-trim";
  const auto wav = make_ramp_wav(directory.path());
  auto session = take(app::ProjectSession::create(root));
  const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  require(fd >= 0, "live trim source open failed");
  (void)take(session->imports().import_wav_audio(fd));
  (void)::close(fd);
  auto evidence = std::make_shared<SteppedEvidence>();
  app::PlaybackOutputFactory factory = [evidence](audio::StereoRenderSource source,
                                                  audio::TransportCursor& cursor) {
    return std::make_unique<SteppedOutput>(source, cursor, evidence);
  };
  auto run = take(quick::RoutedPlayback::prepare_and_start(root, session->snapshot(),
                                                           take(session->reserve_playback_start()),
                                                           factory, std::nullopt, frames));
  evidence->permit(1);
  evidence->await_blocks(1);
  const auto clip = session->snapshot().tracks.front().clips.front().id;
  const std::uint64_t trim_end = frames == 64 ? 100 : 200;
  require(session->audio_clips().trim_audio_clip(clip, app::AudioClipEdge::right,
                                                 {static_cast<std::int64_t>(trim_end)}) &&
              run->submit_revision(session->snapshot()),
          "live right trim was refused");
  oscillate::test::wait_until(
      [&] {
        (void)run->status();
        return run->graph_handoff_in_progress();
      },
      "the routed run reached the awaited state");
  require(run->graph_handoff_in_progress(), "live right trim did not offer a source overlay");
  evidence->permit(1);
  evidence->await_blocks(2);
  require(run->status().active_graph_revision == session->snapshot().revision &&
              run->completed_sample() == static_cast<std::uint64_t>(frames) * 2,
          "live right trim did not activate without freezing the cursor");
  {
    std::lock_guard lock(evidence->mutex);
    require(evidence->frozen_silent_blocks == 0 &&
                evidence->samples[static_cast<std::size_t>(frames) * 2] ==
                    static_cast<float>(4096 + frames) / 32768.0F &&
                evidence->samples[(trim_end - 1) * 2] ==
                    static_cast<float>(4096 + trim_end - 1) / 32768.0F &&
                evidence->samples[trim_end * 2] == 0.0F,
            "live right trim did not end at its exact sample while playback continued");
  }
  (void)run->stop_and_retire();
  require(session->saving().save().has_value(), "live right trim did not save");
  const auto saved_range = session->snapshot().tracks.front().clips.front().source_range;
  session.reset();
  auto reopened = take(app::ProjectSession::open(root));
  require(reopened->snapshot().tracks.front().clips.front().source_range == saved_range,
          "live right trim changed on reopen");
}

void live_clip_split_overlay_keeps_playing(std::uint32_t frames, bool refuse_candidate = false) {
  Directory directory;
  const auto root = directory.path() / "live-clip-split";
  const auto wav = make_ramp_wav(directory.path());
  auto session = take(app::ProjectSession::create(root));
  const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  require(fd >= 0, "live split source open failed");
  (void)take(session->imports().import_wav_audio(fd));
  (void)::close(fd);
  const auto original = session->snapshot().tracks.front().clips.front();
  auto evidence = std::make_shared<SteppedEvidence>();
  evidence->count_allocations = true;
  app::PlaybackOutputFactory factory = [evidence](audio::StereoRenderSource source,
                                                  audio::TransportCursor& cursor) {
    return std::make_unique<SteppedOutput>(source, cursor, evidence);
  };
  auto candidate_fill = std::make_shared<RefusingCandidateFill>();
  auto run = take(quick::RoutedPlayback::prepare_and_start(
      root, session->snapshot(), take(session->reserve_playback_start()), factory, std::nullopt,
      frames, 0, {}, {}, candidate_fill));
  evidence->permit(1);
  evidence->await_blocks(1);
  const auto active = run->status().active_graph_revision;
  const auto boundary = static_cast<std::int64_t>(frames) + 17;
  const auto right =
      take(session->audio_clips().split_audio_clip_at_sample(original.id, {boundary}));
  const auto split = session->snapshot().tracks.front().clips;
  require(split.size() == 2 && split.front().id == original.id && split.back().id == right &&
              session->snapshot().revision == active + 1 &&
              run->submit_revision(session->snapshot()).has_value(),
          "live split was not one accepted source edit");
  auto status = run->status();
  require(status.model_revision == session->snapshot().revision &&
              status.active_graph_revision == active &&
              status.application == app::PlaybackApplication::pending,
          "pending live split was reported audible");
  oscillate::test::wait_until(
      [&] {
        (void)run->status();
        return run->graph_handoff_in_progress();
      },
      "the routed run reached the awaited state");
  require(run->graph_handoff_in_progress() && run->completed_sample() == frames,
          "live split did not offer a source-only revision");
  if (refuse_candidate)
    candidate_fill->refuse_next_candidate();
  evidence->permit(1);
  evidence->await_blocks(2);
  status = run->status();
  require(run->output_status().state == audio::OutputState::streaming &&
              run->completed_sample() == static_cast<std::uint64_t>(frames) * 2 &&
              status.model_revision == session->snapshot().revision &&
              status.active_graph_revision ==
                  (refuse_candidate ? active : session->snapshot().revision) &&
              status.application == (refuse_candidate ? app::PlaybackApplication::pending
                                                      : app::PlaybackApplication::active),
          "live split lost the saved/audible revision distinction");
  evidence->permit(1);
  evidence->await_blocks(3);
  {
    std::lock_guard lock(evidence->mutex);
    require(evidence->samples.size() == std::size_t{frames} * 6 &&
                evidence->frozen_silent_blocks == 0 && evidence->callback_allocation_count == 0,
            "live split froze, allocated or lost a callback block");
    for (std::size_t frame = 0; frame < std::size_t{frames} * 3; ++frame)
      require(evidence->samples[frame * 2] == static_cast<float>(4096 + frame) / 32768.0F,
              "live split changed a source sample at the partition boundary");
  }
  (void)run->stop_and_retire();
  require(session->history().undo().has_value() &&
              session->snapshot().tracks.front().clips.size() == 1 &&
              session->snapshot().tracks.front().clips.front() == original &&
              session->history().redo().has_value() &&
              session->snapshot().tracks.front().clips == split,
          "live split did not remain one atomic undo step");
  require(session->saving().save().has_value(), "live split save failed");
  session.reset();
  auto reopened = take(app::ProjectSession::open(root));
  require(reopened->snapshot().tracks.front().clips == split, "live split changed on reopen");
  if (refuse_candidate) {
    auto restart_evidence = std::make_shared<SteppedEvidence>();
    app::PlaybackOutputFactory restart_factory =
        [restart_evidence](audio::StereoRenderSource source, audio::TransportCursor& cursor) {
          return std::make_unique<SteppedOutput>(source, cursor, restart_evidence);
        };
    auto restarted = take(quick::RoutedPlayback::prepare_and_start(
        root, reopened->snapshot(), take(reopened->reserve_playback_start()), restart_factory,
        std::nullopt, frames));
    restart_evidence->permit(1);
    restart_evidence->await_blocks(1);
    require(restarted->status().application == app::PlaybackApplication::active &&
                restarted->status().active_graph_revision == reopened->snapshot().revision,
            "Stop and Play did not activate the saved split");
    (void)restarted->stop_and_retire();
  }
}

void multi_track_clip_overlay_preserves_native_processing(std::uint32_t frames,
                                                          bool refuse_candidate = false) {
  Directory directory;
  const auto root = directory.path() / "multi-track-source-overlay";
  const auto first_wav = make_ramp_wav(directory.path());
  const auto second_wav = make_wav(directory.path(), "second.wav", 4096);
  auto session = take(app::ProjectSession::create(root));
  const auto import = [&](const fs::path& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    require(fd >= 0, "multi-track source open failed");
    auto track = session->imports().import_wav_audio(fd);
    (void)::close(fd);
    return take(std::move(track));
  };
  const auto first_track = import(first_wav);
  const auto second_track = import(second_wav);
  const auto& tracks = session->snapshot().tracks;
  const auto first_clip = tracks.front().clips.front().id;
  const auto second_clip = tracks.back().clips.front().id;
  require(session->audio_clips().duplicate_audio_clip(first_clip,
                                                      {oscillate::timeline::ticks_per_quarter}) &&
              session->audio_clips().duplicate_audio_clip(second_clip,
                                                          {oscillate::timeline::ticks_per_quarter}),
          "multi-track fixture could not place its second clips");
  require(session->snapshot().tracks.size() == 2 &&
              session->snapshot().tracks.front().clips.size() == 2 &&
              session->snapshot().tracks.back().clips.size() == 2,
          "multi-track fixture did not preserve both clip banks");
  const Band track_band{shape_kind::high_pass, 1'000.0, 0.707, 0.0};
  const Band master_band{shape_kind::high_pass, 500.0, 0.707, 0.0};
  (void)take(session->effects().add_native_effect(first_track, 0, shape_band(track_band)));
  (void)take(session->effects().add_native_effect(project::MasterSignalPath{}, 0,
                                                  shape_band(master_band)));
  const auto prepared = [](const Band& band) {
    const std::array settings{oscillate::native_dsp::EqBandSettings{
        oscillate::native_dsp::EqBandKind::high_pass, band.frequency_hz, band.shape, band.gain_db}};
    return take(oscillate::native_dsp::prepare_eq(settings, 48'000));
  };
  oscillate::native_dsp::EqRuntime expected_track_eq(prepared(track_band));
  oscillate::native_dsp::EqRuntime expected_master_eq(prepared(master_band));
  auto evidence = std::make_shared<SteppedEvidence>();
  evidence->count_allocations = true;
  app::PlaybackOutputFactory factory = [evidence](audio::StereoRenderSource source,
                                                  audio::TransportCursor& cursor) {
    return std::make_unique<SteppedOutput>(source, cursor, evidence);
  };
  auto candidate_fill = std::make_shared<RefusingCandidateFill>();
  auto run = take(quick::RoutedPlayback::prepare_and_start(
      root, session->snapshot(), take(session->reserve_playback_start()), factory, std::nullopt,
      frames, 0, {}, {}, candidate_fill));
  evidence->permit(1);
  evidence->await_blocks(1);
  const auto original_revision = run->status().active_graph_revision;
  constexpr auto move_tick = oscillate::timeline::ticks_per_quarter / 960;
  require(session->audio_clips().move_audio_clip(first_clip, {move_tick}) &&
              run->submit_revision(session->snapshot()),
          "multi-track clip move was refused");
  const auto edited_revision = session->snapshot().revision;
  oscillate::test::wait_until(
      [&] {
        (void)run->status();
        return run->graph_handoff_in_progress();
      },
      "the routed run reached the awaited state");
  require(run->graph_handoff_in_progress(), "multi-track source overlay did not prepare");
  if (refuse_candidate)
    candidate_fill->refuse_next_candidate();
  evidence->permit(1);
  evidence->await_blocks(2);
  const auto status = run->status();
  require(status.model_revision == edited_revision &&
              status.active_graph_revision ==
                  (refuse_candidate ? original_revision : edited_revision) &&
              status.application == (refuse_candidate ? app::PlaybackApplication::pending
                                                      : app::PlaybackApplication::active) &&
              run->completed_sample() == static_cast<std::uint64_t>(frames) * 2,
          "multi-track source overlay reported the wrong audible revision");
  std::array<engine::StereoFrame, 256> expected{};
  for (std::size_t block = 0; block < 2; ++block) {
    const auto start = block * frames;
    auto section = std::span(expected).subspan(start, frames);
    for (std::size_t index = 0; index < frames; ++index) {
      const auto source_frame = start + index - (block == 0 || refuse_candidate ? 0 : 25);
      const auto sample = static_cast<float>(4096 + source_frame) / 32768.0F;
      section[index] = {sample, sample};
    }
    expected_track_eq.process(section, {start, 1});
    for (auto& frame : section) {
      frame.left += 0.125F;
      frame.right += 0.125F;
    }
    expected_master_eq.process(section, {start, 1});
  }
  {
    std::lock_guard lock(evidence->mutex);
    require(evidence->samples.size() == static_cast<std::size_t>(frames) * 4 &&
                evidence->frozen_silent_blocks == 0 && evidence->callback_allocation_count == 0,
            "multi-track source overlay froze or allocated in the callback");
    for (std::size_t index = 0; index < static_cast<std::size_t>(frames) * 2; ++index) {
      require(std::fabs(evidence->samples[index * 2] - expected[index].left) < 0.00001F,
              "multi-track overlay changed source order or reset native processor history");
    }
  }
  (void)run->stop_and_retire();
  require(session->saving().save().has_value(), "multi-track clip move did not save");
  session.reset();
  auto reopened = take(app::ProjectSession::open(root));
  require(reopened->snapshot().tracks.size() == 2 &&
              reopened->snapshot().tracks.front().id == first_track &&
              reopened->snapshot().tracks.back().id == second_track &&
              reopened->snapshot().tracks.front().clips.size() == 2 &&
              reopened->snapshot().tracks.back().clips.size() == 2 &&
              reopened->snapshot().tracks.front().clips.front().id == first_clip &&
              reopened->snapshot().tracks.back().clips.front().id == second_clip,
          "multi-track clip or route order changed on reopen");
}

void failed_live_clip_preparation_keeps_old_audio_audible() {
  Directory directory;
  const auto root = directory.path() / "failed-live-source";
  const auto wav = make_wav(directory.path());
  auto session = take(app::ProjectSession::create(root));
  const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  require(fd >= 0, "failed-live-source fixture could not open WAV");
  (void)take(session->imports().import_wav_audio(fd));
  (void)::close(fd);
  auto evidence = std::make_shared<SteppedEvidence>();
  app::PlaybackOutputFactory factory = [evidence](audio::StereoRenderSource source,
                                                  audio::TransportCursor& cursor) {
    return std::make_unique<SteppedOutput>(source, cursor, evidence);
  };
  auto run = take(quick::RoutedPlayback::prepare_and_start(root, session->snapshot(),
                                                           take(session->reserve_playback_start()),
                                                           factory, std::nullopt, 64));
  evidence->permit(1);
  evidence->await_blocks(1);
  const auto prior_revision = run->status().active_graph_revision;
  const auto clip = session->snapshot().tracks.front().clips.front().id;
  require(session->audio_clips()
              .move_audio_clip(clip, {4 * oscillate::timeline::ticks_per_quarter})
              .has_value(),
          "failed-live-source clip move did not save");
  const auto edited_revision = session->snapshot().revision;
  std::filesystem::rename(root, directory.path() / "temporarily-unavailable");
  require(run->submit_revision(session->snapshot()).has_value(),
          "failed-live-source revision was not accepted for preparation");
  auto status = run->status();
  oscillate::test::wait_until(
      [&] {
        status = run->status();
        return status.application == app::PlaybackApplication::failed;
      },
      "the routed run reached the awaited state");
  require(status.application == app::PlaybackApplication::failed && status.failure &&
              status.model_revision == edited_revision &&
              status.active_graph_revision == prior_revision && !run->edit_pending() &&
              run->output_status().state == audio::OutputState::streaming,
          "failed preparation did not retain the saved edit and active graph separately");
  evidence->permit(1);
  evidence->await_blocks(2);
  {
    std::lock_guard lock(evidence->mutex);
    require(evidence->samples.size() == std::size_t{2} * 128 &&
                std::fabs(evidence->samples.back() - 0.25F) < 0.00001F,
            "failed preparation interrupted the already opened audio source");
  }
  (void)run->stop_and_retire();
}

void live_clip_preparation_survives_pause_and_seek() {
  Directory directory;
  const auto root = directory.path() / "seek-live-source";
  const auto wav = make_wav(directory.path());
  auto session = take(app::ProjectSession::create(root));
  const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  require(fd >= 0, "seek-live-source fixture could not open WAV");
  (void)take(session->imports().import_wav_audio(fd));
  (void)::close(fd);
  auto evidence = std::make_shared<SteppedEvidence>();
  app::PlaybackOutputFactory factory = [evidence](audio::StereoRenderSource source,
                                                  audio::TransportCursor& cursor) {
    return std::make_unique<SteppedOutput>(source, cursor, evidence);
  };
  auto run = take(quick::RoutedPlayback::prepare_and_start(root, session->snapshot(),
                                                           take(session->reserve_playback_start()),
                                                           factory, std::nullopt, 64));
  evidence->permit(1);
  evidence->await_blocks(1);
  const auto clip = session->snapshot().tracks.front().clips.front().id;
  require(
      session->audio_clips().move_audio_clip(clip, {4 * oscillate::timeline::ticks_per_quarter}) &&
          run->submit_revision(session->snapshot()),
      "seek-live-source edit was not accepted");
  require(run->pause() && run->seek_paused(256) && run->resume(),
          "seek-live-source transport could not pause and resume");
  bool activated = false;
  for (std::size_t index = 0; index < 128 && !activated; ++index) {
    // As in the clip handoff above: the worker prepares between stepped blocks.
    std::this_thread::sleep_for(worker_step_pause);
    (void)run->status();
    evidence->permit(1);
    evidence->await_blocks(2 + index);
    const auto status = run->status();
    if (status.application == app::PlaybackApplication::failed)
      throw std::runtime_error("source candidate failed after seek: " +
                               (status.failure ? status.failure->message : "unknown"));
    activated =
        status.active_graph_revision == session->snapshot().revision && !run->edit_pending();
  }
  require(activated && run->completed_sample() >= 256 &&
              run->output_status().state == audio::OutputState::streaming,
          "source candidate did not activate at the new transport generation");
  {
    std::lock_guard lock(evidence->mutex);
    require(std::fabs(evidence->samples.back()) < 0.00001F,
            "seeked source handoff rendered stale clip audio");
  }
  (void)run->stop_and_retire();
}

void pause_waits_for_live_source_switch() {
  Directory directory;
  const auto root = directory.path() / "pause-during-source-handoff";
  const auto wav = make_wav(directory.path());
  auto session = take(app::ProjectSession::create(root));
  const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  require(fd >= 0, "source handoff pause fixture could not open WAV");
  (void)take(session->imports().import_wav_audio(fd));
  (void)::close(fd);
  const auto clip = session->snapshot().tracks.front().clips.front().id;
  // A fade change replaces the source (it is no source-only overlay): the
  // primed replacement switches in live at the block it was primed for.
  require(session->audio_clips().duplicate_audio_clip_after(clip).has_value(),
          "source switch fixture could not add its second clip");
  auto evidence = std::make_shared<SteppedEvidence>();
  app::PlaybackOutputFactory factory = [evidence](audio::StereoRenderSource source,
                                                  audio::TransportCursor& cursor) {
    return std::make_unique<SteppedOutput>(source, cursor, evidence);
  };
  auto run = take(quick::RoutedPlayback::prepare_and_start(root, session->snapshot(),
                                                           take(session->reserve_playback_start()),
                                                           factory, std::nullopt, 64));
  evidence->permit(1);
  evidence->await_blocks(1);
  require(session->audio_clips().set_audio_clip_fades(
              clip, {0.25, 0.0, project::FadeCurve::linear, project::FadeCurve::linear}) &&
              run->submit_revision(session->snapshot()),
          "source switch pause fixture did not accept the clip fade");
  oscillate::test::wait_until(
      [&] {
        (void)run->status();
        return run->graph_handoff_in_progress();
      },
      "the routed run reached the awaited state");
  require(run->graph_handoff_in_progress() && !run->pause() && !run->paused() &&
              run->output_status().state == audio::OutputState::streaming,
          "pause stopped output while the source switch was published");
  evidence->permit(1);
  evidence->await_blocks(2);
  const auto active = run->status();
  {
    std::lock_guard lock(evidence->mutex);
    require(evidence->frozen_silent_blocks == 0, "the source switch held a block");
  }
  require(active.active_graph_revision == session->snapshot().revision &&
              !run->graph_handoff_in_progress() && run->pause() && run->seek_paused(256) &&
              run->resume(),
          "source switch did not settle at its primed block before pause and seek");
  evidence->permit(1);
  evidence->await_blocks(3);
  require(run->completed_sample() >= 256 &&
              run->output_status().state == audio::OutputState::streaming,
          "source playback did not restart after the settled source switch");
  (void)run->stop_and_retire();
}

// An audio clip edit in a song with a built-in synth replaces the source,
// and the synth's running player carries over into the replacement with its
// voice (ADR-0094). The audio is silent, so the output is the synth alone: a
// fresh player would drop the held note, and a held block would be silent.
void audio_edit_beside_a_synth_keeps_its_voice() {
  constexpr std::uint32_t frames = 64;
  constexpr std::size_t settle_blocks = 8;
  constexpr std::size_t steady_blocks = 8;
  constexpr std::size_t after_blocks = 8;
  constexpr std::size_t max_switch_blocks = 256;
  constexpr double click_ratio = 2.0;
  constexpr std::int64_t held_note_ticks = 4 * oscillate::timeline::ticks_per_quarter;
  constexpr std::uint8_t held_key = 60;
  Directory directory;
  const auto root = directory.path() / "audio-edit-beside-synth";
  const auto wav = make_wav(directory.path(), "silent.wav", 0);
  auto session = take(app::ProjectSession::create(root));
  const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  require(fd >= 0, "silent audio fixture could not open WAV");
  (void)take(session->imports().import_wav_audio(fd));
  (void)::close(fd);
  const auto clip = session->snapshot().tracks.front().clips.front().id;
  const auto synth = take(session->instruments().add_synth_track());
  require(session->midi_clips().add_midi_clip(synth, {0}).has_value(),
          "synth MIDI clip setup failed");
  const auto midi = std::ranges::find(session->snapshot().tracks, synth, &project::Track::id)
                        ->midi_clips.front()
                        .id;
  // A bar-long note, held through the whole run.
  require(
      session->notes().add_note_with_duration(midi, {0}, {held_note_ticks}, held_key).has_value() &&
          session->saving().save().has_value(),
      "synth held note setup failed");
  auto evidence = std::make_shared<SteppedEvidence>();
  evidence->count_allocations = true;
  app::PlaybackOutputFactory factory = [evidence](audio::StereoRenderSource source,
                                                  audio::TransportCursor& cursor) {
    return std::make_unique<SteppedOutput>(source, cursor, evidence);
  };
  auto run = take(quick::RoutedPlayback::prepare_and_start(root, session->snapshot(),
                                                           take(session->reserve_playback_start()),
                                                           factory, std::nullopt, frames));
  std::size_t blocks = settle_blocks + steady_blocks;
  evidence->permit(blocks);
  evidence->await_blocks(blocks);
  require(session->audio_clips()
                  .set_audio_clip_fades(
                      clip, {0.25, 0.0, project::FadeCurve::linear, project::FadeCurve::linear})
                  .has_value() &&
              run->submit_revision(session->snapshot()).has_value(),
          "the audio clip fade beside a synth was refused while playing");
  const auto edited = session->snapshot().revision;
  bool activated = false;
  for (std::size_t attempt = 0; attempt < max_switch_blocks && !activated; ++attempt) {
    std::this_thread::sleep_for(worker_step_pause);
    (void)run->status();
    evidence->permit(1);
    evidence->await_blocks(++blocks);
    const auto status = run->status();
    if (status.application == app::PlaybackApplication::failed)
      throw std::runtime_error("audio edit beside a synth failed: " +
                               (status.failure ? status.failure->message : "unknown"));
    activated = status.active_graph_revision == edited && !run->edit_pending();
  }
  require(activated, "the audio clip fade beside a synth did not switch live");
  blocks += after_blocks;
  evidence->permit(after_blocks);
  evidence->await_blocks(blocks);
  (void)run->stop_and_retire();
  std::lock_guard lock(evidence->mutex);
  require(evidence->frozen_silent_blocks == 0 && evidence->callback_allocation_count == 0,
          "the source switch beside a synth held a block or allocated");
  // Left channel; the steady window is the held note before the edit.
  std::vector<float> left;
  for (std::size_t index = 0; index < evidence->samples.size(); index += 2)
    left.push_back(evidence->samples[index]);
  require(left.size() == blocks * frames, "a block was lost around the source switch");
  double steady = 0.0;
  for (std::size_t index = settle_blocks * frames + 1;
       index < (settle_blocks + steady_blocks) * frames; ++index)
    steady = std::max(steady, std::fabs(static_cast<double>(left[index]) - left[index - 1]));
  for (std::size_t block = settle_blocks; block < blocks; ++block) {
    const auto window = std::span(left).subspan(block * frames, frames);
    require(std::ranges::any_of(window, [](float value) { return value != 0.0F; }),
            "the synth's held note was dropped at the source switch");
  }
  for (std::size_t index = (settle_blocks + steady_blocks) * frames; index < left.size(); ++index)
    require(std::fabs(static_cast<double>(left[index]) - left[index - 1]) <= click_ratio * steady,
            "the synth's output jumped at the source switch");
}

// A track removed while playing switches with its prepared source
// (ADR-0098): the transport never holds, and stays free to pause and seek
// while the switch prepares and after it.
void structural_edit_never_holds_the_transport() {
  Directory directory;
  const auto root = directory.path() / "structural-edit-pause";
  const auto wav = make_wav(directory.path());
  auto session = take(app::ProjectSession::create(root));
  const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  require(fd >= 0, "structural edit fixture could not open WAV");
  const auto track = take(session->imports().import_wav_audio(fd));
  (void)::close(fd);
  auto evidence = std::make_shared<SteppedEvidence>();
  app::PlaybackOutputFactory factory = [evidence](audio::StereoRenderSource source,
                                                  audio::TransportCursor& cursor) {
    return std::make_unique<SteppedOutput>(source, cursor, evidence);
  };
  auto run = take(quick::RoutedPlayback::prepare_and_start(root, session->snapshot(),
                                                           take(session->reserve_playback_start()),
                                                           factory, std::nullopt, 64));
  evidence->permit(1);
  evidence->await_blocks(1);
  require(session->tracks().remove_track(track).has_value() &&
              run->submit_revision(session->snapshot()).has_value(),
          "a track removal while playing was refused");
  const auto removed = session->snapshot().revision;
  require(run->pause() && run->seek_paused(256) && run->resume(),
          "a structural edit held the transport");
  std::size_t blocks = 1;
  bool switched = false;
  for (std::size_t attempt = 0; attempt < 400 && !switched; ++attempt) {
    evidence->permit(8);
    blocks += 8;
    evidence->await_blocks(blocks);
    const auto state = run->status();
    require(state.application != app::PlaybackApplication::failed,
            "the track removal failed to switch");
    switched = state.active_graph_revision == removed && !run->edit_pending();
  }
  require(switched && run->completed_sample() >= 256 &&
              run->output_status().state == audio::OutputState::streaming,
          "the track removal did not switch live with the transport running");
  (void)run->stop_and_retire();
}

void limiter_stop_renders_exact_lookahead_before_transport_release() {
  Directory directory;
  const auto root = directory.path() / "limiter-song";
  const auto wav = make_wav(directory.path());
  auto session = take(app::ProjectSession::create(root));
  const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  require(fd >= 0, "limiter source open failed");
  auto imported = session->imports().import_wav_audio(fd);
  (void)::close(fd);
  require(imported.has_value(), "limiter source import failed");
  require(session->effects()
                  .add_native_effect(project::MasterSignalPath{}, 0, ceiling(-6.0, 5.0, 100.0))
                  .has_value() &&
              session->saving().save().has_value(),
          "limiter fixture save failed");
  auto evidence = std::make_shared<SteppedEvidence>();
  evidence->caller = std::this_thread::get_id();
  app::PlaybackOutputFactory factory = [evidence](audio::StereoRenderSource source,
                                                  audio::TransportCursor& cursor) {
    return std::make_unique<SteppedOutput>(source, cursor, evidence);
  };
  auto run = take(quick::RoutedPlayback::prepare_and_start(root, session->snapshot(),
                                                           take(session->reserve_playback_start()),
                                                           factory, std::nullopt, 64));
  evidence->permit(1);
  evidence->await_blocks(1);
  require(run->completed_sample() == 64 &&
              !session->exports().prepare_wav_master_export({root / "blocked.wav"}),
          "limiter playback did not hold the transport before Stop");

  // The drain renders the lookahead in whole blocks.
  const auto latency = ceiling_latency(5.0);
  const std::size_t drain_blocks = (latency + stop_block - 1) / stop_block;
  run->begin_graceful_stop();
  auto stopper = std::async(std::launch::async, [&run] { return run->stop_and_retire(); });
  evidence->permit(drain_blocks - 1);
  evidence->await_blocks(drain_blocks);
  require(stopper.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout &&
              !session->exports().prepare_wav_master_export({root / "still-blocked.wav"}),
          "limiter Stop released transport before its prepared lookahead completed");
  evidence->permit(1);
  evidence->await_blocks(drain_blocks + 1);
  require(stopper.get() == quick::RoutedPlayback::StopDrainOutcome::completed,
          "limiter Stop did not report a complete drain");
  require(evidence->stopped && evidence->stop_off_caller &&
              session->exports().prepare_wav_master_export({root / "after.wav"}).has_value(),
          "limiter output was not joined before transport release");
  {
    std::lock_guard lock(evidence->mutex);
    require(evidence->samples.size() == (drain_blocks + 1) * stop_block_samples,
            "limiter drain did not render the played block and its lookahead");
    for (std::size_t frame = 0; frame < (drain_blocks + 1) * stop_block; ++frame) {
      const float expected = frame >= latency && frame < latency + stop_block ? 0.25F : 0.0F;
      require(std::fabs(evidence->samples[frame * 2] - expected) < 0.00001F &&
                  std::fabs(evidence->samples[frame * 2 + 1] - expected) < 0.00001F,
              "limiter Stop tail was truncated or continued reading project source");
    }
  }

  auto stalled = std::make_shared<SteppedEvidence>();
  stalled->caller = std::this_thread::get_id();
  app::PlaybackOutputFactory stalled_factory = [stalled](audio::StereoRenderSource source,
                                                         audio::TransportCursor& cursor) {
    return std::make_unique<SteppedOutput>(source, cursor, stalled);
  };
  auto stalled_run = take(quick::RoutedPlayback::prepare_and_start(
      root, session->snapshot(), take(session->reserve_playback_start()), stalled_factory,
      std::nullopt, 64));
  stalled_run->begin_graceful_stop();
  auto stalled_stop =
      std::async(std::launch::async, [&stalled_run] { return stalled_run->stop_and_retire(); });
  require(!session->exports().prepare_wav_master_export({root / "stalled-blocked.wav"}),
          "stalled limiter output released transport before bounded fallback");
  require(
      stalled_stop.get() == quick::RoutedPlayback::StopDrainOutcome::truncated &&
          stalled->stopped && stalled->stop_off_caller &&
          session->exports().prepare_wav_master_export({root / "stalled-after.wav"}).has_value(),
      "stalled limiter output did not fall back and release transport");
}

void routed_track_limiter_stop_drains_before_releasing_transport() {
  Directory directory;
  const auto root = directory.path() / "route-limiter-song";
  const auto wav = make_wav(directory.path());
  auto session = take(app::ProjectSession::create(root));
  const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  require(fd >= 0, "route limiter source open failed");
  require(session->imports().import_wav_audio(fd).has_value(),
          "route limiter source import failed");
  (void)::close(fd);
  auto snapshot = session->snapshot();
  const auto slot = take(project::InsertSlotId::from_bits({0, 9781}));
  snapshot.tracks.front().effects.push_back({slot, true, ceiling(0.0, 1.0, 100.0)});
  auto evidence = std::make_shared<SteppedEvidence>();
  evidence->caller = std::this_thread::get_id();
  app::PlaybackOutputFactory factory = [evidence](audio::StereoRenderSource source,
                                                  audio::TransportCursor& cursor) {
    return std::make_unique<SteppedOutput>(source, cursor, evidence);
  };
  auto run = take(quick::RoutedPlayback::prepare_and_start(
      root, snapshot, take(session->reserve_playback_start()), factory, std::nullopt, 64));
  evidence->permit(1);
  evidence->await_blocks(1);
  const auto latency = ceiling_latency(1.0);
  const std::size_t drain_blocks = (latency + stop_block - 1) / stop_block;
  run->begin_graceful_stop();
  auto stopper = std::async(std::launch::async, [&run] { return run->stop_and_retire(); });
  evidence->permit(drain_blocks);
  evidence->await_blocks(drain_blocks + 1);
  require(stopper.get() == quick::RoutedPlayback::StopDrainOutcome::completed &&
              session->exports()
                  .prepare_wav_master_export({root / "after-track-limiter.wav"})
                  .has_value(),
          "route limiter Stop did not drain and release the transport");
  std::lock_guard lock(evidence->mutex);
  require(evidence->samples.size() == (drain_blocks + 1) * stop_block_samples,
          "route limiter did not render exactly its lookahead's drain blocks");
  for (std::size_t frame = 0; frame < (drain_blocks + 1) * stop_block; ++frame) {
    const float expected = frame >= latency && frame < latency + stop_block ? 0.25F : 0.0F;
    require(std::fabs(evidence->samples[frame * 2] - expected) < 0.00001F,
            "route limiter Stop truncated the saved lookahead or read new source frames");
  }
}

void unsupported_shape_and_device_failure_release_reservation() {
  Directory directory;
  const auto root = directory.path() / "song";
  const auto wav = make_wav(directory.path());
  auto session = take(app::ProjectSession::create(root));
  const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  require(fd >= 0, "unsupported fixture source open failed");
  auto track = session->imports().import_wav_audio(fd);
  (void)::close(fd);
  require(track.has_value(), "unsupported fixture import failed");
  auto group = take(session->tracks().add_bus_track(project::TrackKind::group_bus));
  require(session->routing().set_main_output(track.value(), group).has_value(),
          "unsupported fixture route failed");
  OutputEvidence evidence{.caller = std::this_thread::get_id()};
  app::PlaybackOutputFactory fail_factory = [&evidence](audio::StereoRenderSource source,
                                                        audio::TransportCursor& cursor) {
    return std::make_unique<FakeOutput>(source, cursor, evidence, true);
  };
  auto invalid_buffer = quick::RoutedPlayback::prepare_and_start(
      root, session->snapshot(), take(session->reserve_playback_start()), fail_factory,
      std::nullopt, 96);
  require(!invalid_buffer &&
              invalid_buffer.error().code == foundation::ErrorCode::invalid_argument &&
              session->exports().prepare_wav_master_export({root / "after-invalid-buffer.wav"}),
          "invalid routed buffer request retained playback reservation");
  auto failed_device = quick::RoutedPlayback::prepare_and_start(
      root, session->snapshot(), take(session->reserve_playback_start()), fail_factory,
      std::nullopt, 128);
  require(!failed_device && failed_device.error().code == foundation::ErrorCode::unavailable &&
              evidence.requested_buffer_frames == 128 &&
              session->exports()
                  .prepare_wav_master_export(
                      {root / "after-device.wav", oscillate::exporting::WavFormat::float32})
                  .has_value(),
          "failed device retained routed playback reservation");
  auto null_factory = [](audio::StereoRenderSource,
                         audio::TransportCursor&) -> std::unique_ptr<app::PlaybackOutput> {
    return {};
  };
  auto missing_device = quick::RoutedPlayback::prepare_and_start(
      root, session->snapshot(), take(session->reserve_playback_start()), null_factory,
      std::nullopt);
  require(!missing_device && missing_device.error().code == foundation::ErrorCode::internal &&
              session->exports()
                  .prepare_wav_master_export(
                      {root / "after-null-device.wav", oscillate::exporting::WavFormat::float32})
                  .has_value(),
          "null output retained routed playback reservation");
  require(session->tracks().add_instrument_track().has_value(),
          "unsupported routed instrument track fixture failed");
  const auto instrument = session->snapshot().tracks.back().id;
  require(session->midi_clips().add_midi_clip(instrument, {0}).has_value(),
          "unsupported routed instrument fixture failed");
  auto unsupported = quick::RoutedPlayback::prepare_and_start(
      root, session->snapshot(), take(session->reserve_playback_start()), {}, std::nullopt);
  require(!unsupported && unsupported.error().code == foundation::ErrorCode::unsupported &&
              unsupported.error().message.find("verified bound MIDI instrument") !=
                  std::string::npos &&
              session->exports()
                  .prepare_wav_master_export(
                      {root / "after-unsupported.wav", oscillate::exporting::WavFormat::float32})
                  .has_value(),
          "unsupported routed shape silently played or retained reservation");
}

void live_native_eq_revision_keeps_the_output_stream() {
  Directory directory;
  const auto root = directory.path() / "live-native-eq-song";
  const auto wav = make_wav(directory.path());
  auto session = take(app::ProjectSession::create(root));
  const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  require(fd >= 0, "live EQ source open failed");
  const auto track = take(session->imports().import_wav_audio(fd));
  (void)::close(fd);
  const Band band{shape_kind::high_pass, 100.0, 0.707, 0.0};
  const auto slot = take(session->effects().add_native_effect(track, 0, shape_band(band)));
  auto evidence = std::make_shared<SteppedEvidence>();
  app::PlaybackOutputFactory factory = [evidence](audio::StereoRenderSource source,
                                                  audio::TransportCursor& cursor) {
    return std::make_unique<SteppedOutput>(source, cursor, evidence);
  };
  auto run = take(quick::RoutedPlayback::prepare_and_start(root, session->snapshot(),
                                                           take(session->reserve_playback_start()),
                                                           factory, std::nullopt, 64));
  evidence->permit(1);
  evidence->await_blocks(1);
  const auto original = run->status().active_graph_revision;
  require(original == session->snapshot().revision &&
              run->output_status().state == audio::OutputState::streaming,
          "live EQ did not begin on the running output");

  auto edited_band = band;
  edited_band.frequency_hz = 200.0;
  require(session->effects().set_native_effect(track, slot, shape_band(edited_band)).has_value(),
          "live EQ project edit failed");
  const auto edited = session->snapshot().revision;
  require(run->submit_revision(session->snapshot()).has_value() && run->edit_pending(),
          "live EQ project revision was not accepted");
  require(run->status().application == app::PlaybackApplication::pending,
          "live EQ was reported active before the callback adopted it");
  evidence->permit(1);
  evidence->await_blocks(2);
  {
    std::lock_guard lock(evidence->mutex);
    require(evidence->samples.size() == 256 &&
                std::ranges::any_of(std::span(evidence->samples).subspan(128, 128),
                                    [](float sample) { return sample != 0.0F; }),
            "live EQ parameter edit emitted a silent output block");
  }
  require(run->status().active_graph_revision == edited && !run->edit_pending() &&
              run->output_status().state == audio::OutputState::streaming,
          "live EQ revision did not activate without stopping or holding output");
  (void)run->stop_and_retire();
}

void live_native_compressor_revision_changes_running_output() {
  Directory directory;
  const auto root = directory.path() / "live-native-compressor-song";
  const auto wav = make_wav(directory.path());
  auto session = take(app::ProjectSession::create(root));
  const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  require(fd >= 0, "live compressor source open failed");
  const auto track = take(session->imports().import_wav_audio(fd));
  (void)::close(fd);
  Compression settings;
  settings.ratio = 1.0;
  settings.makeup_db = 0.0;
  const auto slot = take(session->effects().add_native_effect(track, 0, press(settings)));
  auto evidence = std::make_shared<SteppedEvidence>();
  app::PlaybackOutputFactory factory = [evidence](audio::StereoRenderSource source,
                                                  audio::TransportCursor& cursor) {
    return std::make_unique<SteppedOutput>(source, cursor, evidence);
  };
  auto run = take(quick::RoutedPlayback::prepare_and_start(root, session->snapshot(),
                                                           take(session->reserve_playback_start()),
                                                           factory, std::nullopt, 64));
  evidence->permit(1);
  evidence->await_blocks(1);
  float before = 0.0F;
  {
    std::lock_guard lock(evidence->mutex);
    require(evidence->samples.size() == 128, "initial compressor block was not audible");
    before = evidence->samples.front();
  }
  settings.makeup_db = 6.0;
  require(session->effects().set_native_effect(track, slot, press(settings)).has_value(),
          "live compressor project edit failed");
  const auto revised = session->snapshot().revision;
  require(run->submit_revision(session->snapshot()).has_value() && run->edit_pending(),
          "live compressor revision was not accepted");
  require(run->status().application == app::PlaybackApplication::pending,
          "live compressor was reported active before its callback adopted settings");
  evidence->permit(1);
  evidence->await_blocks(2);
  float after = 0.0F;
  {
    std::lock_guard lock(evidence->mutex);
    require(evidence->samples.size() == 256, "live compressor did not continue output; captured " +
                                                 std::to_string(evidence->samples.size()) +
                                                 " samples");
    require(std::ranges::any_of(std::span(evidence->samples).subspan(128, 128),
                                [](float sample) { return sample != 0.0F; }),
            "live compressor parameter edit emitted a silent output block");
    after = evidence->samples[128];
  }
  // Press glides its makeup towards the new value: the first block after the
  // edit moves from the old level towards the 6 dB target without a jump.
  const auto target = static_cast<float>(0.25 * std::pow(10.0, 6.0 / 20.0));
  float last = 0.0F;
  {
    std::lock_guard lock(evidence->mutex);
    last = evidence->samples[254];
  }
  require(std::fabs(before - 0.25F) < 0.00001F && after >= before && after < target &&
              last > after && last <= target + 0.00001F &&
              run->status().active_graph_revision == revised &&
              run->output_status().state == audio::OutputState::streaming,
          "live compressor control was not audible on the running output: before " +
              std::to_string(before) + ", first " + std::to_string(after) + ", last " +
              std::to_string(last));
  (void)run->stop_and_retire();
}

void live_native_reverb_revision_changes_running_output() {
  Directory directory;
  const auto root = directory.path() / "live-native-reverb-song";
  const auto wav = make_wav(directory.path());
  auto session = take(app::ProjectSession::create(root));
  const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  require(fd >= 0, "live reverb source open failed");
  const auto track = take(session->imports().import_wav_audio(fd));
  (void)::close(fd);
  const auto slot = take(session->effects().add_native_effect(
      track, 0, engine_patch("expanse", {{"expanse.mix", 0.0}, {"expanse.decay", 1.0}})));
  auto evidence = std::make_shared<SteppedEvidence>();
  app::PlaybackOutputFactory factory = [evidence](audio::StereoRenderSource source,
                                                  audio::TransportCursor& cursor) {
    return std::make_unique<SteppedOutput>(source, cursor, evidence);
  };
  auto run = take(quick::RoutedPlayback::prepare_and_start(root, session->snapshot(),
                                                           take(session->reserve_playback_start()),
                                                           factory, std::nullopt, 64));
  evidence->permit(1);
  evidence->await_blocks(1);
  // A mix edit is a value offer: the running stage adopts it without a hold.
  require(
      session->effects()
          .set_native_effect(
              track, slot, engine_patch("expanse", {{"expanse.mix", 0.5}, {"expanse.decay", 1.0}}))
          .has_value(),
      "live reverb project edit failed");
  const auto revised = session->snapshot().revision;
  require(run->submit_revision(session->snapshot()).has_value() && run->edit_pending() &&
              run->status().application == app::PlaybackApplication::pending,
          "live reverb revision was not held pending for callback acknowledgement");
  evidence->permit(1);
  evidence->await_blocks(2);
  {
    std::lock_guard lock(evidence->mutex);
    require(evidence->samples.size() == 256 &&
                std::fabs(evidence->samples.front() - 0.25F) < 0.00001F &&
                std::ranges::any_of(std::span(evidence->samples).subspan(128, 128),
                                    [](float sample) { return sample != 0.25F; }),
            "live reverb mix edit did not reach the running output without a hold");
  }
  require(run->status().active_graph_revision == revised && !run->edit_pending() &&
              run->output_status().state == audio::OutputState::streaming,
          "live reverb revision did not activate while output streamed");
  require(
      session->effects()
          .set_native_effect(
              track, slot, engine_patch("expanse", {{"expanse.mix", 0.5}, {"expanse.decay", 2.0}}))
          .has_value(),
      "live track reverb decay project edit failed");
  const auto decay_revision = session->snapshot().revision;
  require(run->submit_revision(session->snapshot()).has_value() && run->edit_pending(),
          "live track reverb decay was not accepted");
  evidence->permit(1);
  evidence->await_blocks(3);
  require(run->status().active_graph_revision == decay_revision && !run->edit_pending() &&
              run->output_status().state == audio::OutputState::streaming,
          "track reverb decay did not activate while output streamed");
  (void)run->stop_and_retire();
}

void saved_native_eq_is_audible_from_quick() {
  Directory directory;
  const auto root = directory.path() / "native-eq-song";
  const auto wav = make_wav(directory.path());
  auto session = take(app::ProjectSession::create(root));
  const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  require(fd >= 0, "native EQ source open failed");
  auto track = session->imports().import_wav_audio(fd);
  (void)::close(fd);
  require(track.has_value(), "native EQ source import failed");
  auto snapshot = session->snapshot();
  require(!quick::has_saved_track_effect(snapshot), "empty chain detected as an active insert");
  const Band band{shape_kind::high_pass, 1000.0, 0.707, 0.0};
  const auto slot = take(project::InsertSlotId::from_bits({0, 9001}));
  snapshot.tracks.front().effects.push_back({slot, true, shape_band(band)});
  require(quick::has_saved_track_effect(snapshot), "native EQ did not select routed playback");
  const std::array settings{oscillate::native_dsp::EqBandSettings{
      oscillate::native_dsp::EqBandKind::high_pass, band.frequency_hz, band.shape, band.gain_db}};
  const auto prepared = take(oscillate::native_dsp::prepare_eq(settings, snapshot.sample_rate));
  const auto first_sample = static_cast<float>(0.25 * prepared.coefficients(0).b0);
  const auto verify = [&](const project::Snapshot& saved) {
    for (const auto frames : {64U, 128U}) {
      OutputEvidence evidence{.caller = std::this_thread::get_id()};
      app::PlaybackOutputFactory factory = [&evidence](audio::StereoRenderSource source,
                                                       audio::TransportCursor& cursor) {
        return std::make_unique<FakeOutput>(source, cursor, evidence, false);
      };
      auto run = quick::RoutedPlayback::prepare_and_start(
          root, saved, take(session->reserve_playback_start()), factory, std::nullopt, frames);
      if (!run)
        throw std::runtime_error(run.error().message);
      require(std::fabs(evidence.first_sample - first_sample) < 0.00001F &&
                  run.value()->status().active_graph_revision == saved.revision &&
                  !run.value()->processor_fault(),
              "saved native EQ was not audible at requested buffer size");
      run.value()->stop_and_retire();
    }
  };
  verify(snapshot);
  const auto group = take(session->tracks().add_bus_track(project::TrackKind::group_bus));
  require(session->routing().set_main_output(track.value(), group).has_value(),
          "native EQ group route failed");
  auto bus_snapshot = session->snapshot();
  bus_snapshot.tracks.back().effects.push_back({slot, true, shape_band(band)});
  require(quick::has_saved_track_effect(bus_snapshot), "bus EQ did not select routed playback");
  verify(bus_snapshot);

  Compression compression;
  compression.threshold_db = -30.0;
  compression.ratio = 20.0;
  compression.knee_db = 0.0;
  compression.attack_ms = 0.1;
  compression.release_ms = 1.0;
  const auto compressor_slot = take(project::InsertSlotId::from_bits({0, 9002}));
  for (auto* chained : {&snapshot, &bus_snapshot}) {
    auto& owner = chained == &snapshot ? chained->tracks.front() : chained->tracks.back();
    owner.effects.push_back({compressor_slot, true, press(compression)});
    for (const auto frames : {64U, 128U}) {
      oscillate::native_dsp::EqRuntime eq(prepared);
      auto prepared_compressor = take(oscillate::native_dsp::prepare_compressor(
          {compression.threshold_db, compression.ratio, compression.knee_db, compression.attack_ms,
           compression.release_ms, compression.makeup_db},
          chained->sample_rate));
      oscillate::native_dsp::CompressorRuntime compressor(prepared_compressor);
      std::array<engine::StereoFrame, 128> expected{};
      expected.fill({0.25F, 0.25F});
      eq.process(std::span(expected).first(frames), {0, 1});
      compressor.process(std::span(expected).first(frames), {0, 1});
      OutputEvidence evidence{.caller = std::this_thread::get_id()};
      app::PlaybackOutputFactory factory = [&evidence](audio::StereoRenderSource source,
                                                       audio::TransportCursor& cursor) {
        return std::make_unique<FakeOutput>(source, cursor, evidence, false);
      };
      auto run = quick::RoutedPlayback::prepare_and_start(
          root, *chained, take(session->reserve_playback_start()), factory, std::nullopt, frames);
      if (!run)
        throw std::runtime_error(run.error().message);
      for (std::size_t frame = 0; frame < frames; ++frame)
        require(std::fabs(evidence.first_channel[frame] - expected[frame].left) < 0.00001F,
                "Quick native chain omitted or reordered a saved stage");
      require(run.value()->status().active_graph_revision == chained->revision &&
                  !run.value()->processor_fault(),
              "Quick native chain did not activate the saved graph");
      run.value()->stop_and_retire();
    }
    const struct {
      double ceiling_db = -18.0;
      double lookahead_ms = 1.0;
      double release_ms = 100.0;
    } limiter_settings;
    owner.effects[1].processor = ceiling(limiter_settings.ceiling_db, limiter_settings.lookahead_ms,
                                         limiter_settings.release_ms);
    for (const auto frames : {64U, 128U}) {
      oscillate::native_dsp::EqRuntime eq(prepared);
      // Ceiling itself, with the patch's values, is the oracle.
      const auto& ceiling_engine = oscillate::native_dsp::effects::ceiling::engine();
      auto ceiling_values = oscillate::native_dsp::default_values(*ceiling_engine.descriptor);
      for (const auto& value :
           std::get<project::NativeEffectPatch>(owner.effects[1].processor).parameters)
        ceiling_values[*oscillate::native_dsp::find_parameter(*ceiling_engine.descriptor,
                                                              value.id)] = value.value;
      auto limiter = take(oscillate::native_dsp::effects::NativeEffectRuntime::create(
          ceiling_engine, ceiling_values, static_cast<double>(chained->sample_rate)));
      std::array<engine::StereoFrame, 128> expected{};
      expected.fill({0.25F, 0.25F});
      eq.process(std::span(expected).first(frames), {0, 1});
      const oscillate::native_dsp::Transport stopped{0.0, 0.0, 0.0, 4.0};
      (void)limiter->process(std::span(expected).first(frames), {}, 0, 1, stopped, {});
      OutputEvidence evidence{.caller = std::this_thread::get_id()};
      app::PlaybackOutputFactory factory = [&evidence](audio::StereoRenderSource source,
                                                       audio::TransportCursor& cursor) {
        return std::make_unique<FakeOutput>(source, cursor, evidence, false);
      };
      auto run = quick::RoutedPlayback::prepare_and_start(
          root, *chained, take(session->reserve_playback_start()), factory, std::nullopt, frames);
      if (!run)
        throw std::runtime_error(run.error().message);
      for (std::size_t frame = 0; frame < frames; ++frame)
        require(std::fabs(evidence.first_channel[frame] - expected[frame].left) < 0.00001F,
                "Quick native limiter was omitted or shifted at the requested block size");
      require(run.value()->status().active_graph_revision == chained->revision &&
                  !run.value()->processor_fault(),
              "Quick limiter chain did not activate the saved graph");
      run.value()->stop_and_retire();
    }
  }
}

void saved_vst3_is_not_silently_omitted() {
  Directory directory;
  const auto root = directory.path() / "vst3-song";
  const auto wav = make_wav(directory.path());
  auto session = take(app::ProjectSession::create(root));
  const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  require(fd >= 0, "VST3 fixture source open failed");
  auto track = session->imports().import_wav_audio(fd);
  (void)::close(fd);
  require(track.has_value(), "VST3 fixture import failed");
  require(!quick::has_saved_vst3_insert(session->snapshot()),
          "empty project incorrectly requires VST3 routed playback");
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  oscillate::plugins::Vst3Scanner scanner(OSCILLATE_VST3_HELPER_PATH);
  const auto scan = scanner.scan(VST3_GAIN_BUNDLE);
  require(scan.ok() && scan.metadata.classes.size() == 1, "VST3 gain fixture scan failed");
  const auto& klass = scan.metadata.classes.front();
  auto inserted = session->effects().insert_vst3_plugin(
      track.value(), 0,
      {.class_id = klass.id,
       .vendor = klass.vendor,
       .name = klass.name,
       .version = klass.version,
       .canonical_bundle_path = scan.binary.canonical_bundle.string(),
       .binary_sha256 = scan.binary.sha256});
#else
  auto inserted =
      session->effects().insert_vst3_plugin(track.value(), 0,
                                            {.class_id = std::string(32, 'a'),
                                             .vendor = "Fixture Vendor",
                                             .name = "Missing Effect",
                                             .version = "1",
                                             .canonical_bundle_path = "/tmp/Missing.vst3",
                                             .binary_sha256 = std::string(64, 'a')});
#endif
  require(inserted.has_value(), "VST3 fixture insert failed");
  require(quick::has_saved_vst3_insert(session->snapshot()),
          "saved VST3 insert did not select routed playback");
  const auto plugin_id = inserted.value();
  session.reset();
  session = take(app::ProjectSession::open(root));
  require(effect_on(session->snapshot().tracks.front()).id == plugin_id,
          "saved VST3 insert identity changed on reopen");
  for (const auto frames : {64U, 128U}) {
    OutputEvidence evidence{.caller = std::this_thread::get_id()};
    app::PlaybackOutputFactory factory = [&evidence](audio::StereoRenderSource source,
                                                     audio::TransportCursor& cursor) {
      return std::make_unique<FakeOutput>(source, cursor, evidence, false);
    };
    auto run = quick::RoutedPlayback::prepare_and_start(root, session->snapshot(),
                                                        take(session->reserve_playback_start()),
                                                        factory, std::nullopt, frames);
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
    if (!run)
      throw std::runtime_error(run.error().message);
    require(evidence.first_channel[0] == 0.0F &&
                std::fabs(evidence.first_channel[7] - 0.5F) < 0.00001F &&
                evidence.completed_sample == frames && evidence.requested_buffer_frames == frames &&
                run.value()->prepared_block_limit() == 8192 && !run.value()->processor_fault(),
            "saved VST3 gain was not audible at requested block size: first=" +
                std::to_string(evidence.first_channel[0]) +
                " frame7=" + std::to_string(evidence.first_channel[7]));
    // Quick reaches the exact callback-accepted slot, whose closed latency
    // gate still refuses every manual value for this unqualified plugin.
    const auto& saved_track = session->snapshot().tracks.front();
    (void)run.value()->status();
    const auto binding = run.value()->active_vst3_effect_manual_events(
        saved_track.id, saved_track.effects.front().id, plugin_id);
    require(binding && binding->slot &&
                binding->identity.graph_revision == session->snapshot().revision &&
                binding->identity.instance_id != 0 &&
                binding->slot->admission(binding->identity, session->snapshot().revision + 1,
                                         {0, 0, 0.5}) != plugins::Vst3ManualEventStatus::accepted &&
                !run.value()->active_vst3_effect_manual_events(
                    saved_track.id, saved_track.effects.front().id,
                    project::PluginInstanceId::from_bits({9, 9}).value()),
            "Quick did not resolve the exact active effect slot or admitted an unqualified edit");
    require(run.value()->pause().has_value() &&
                !run.value()->active_vst3_effect_manual_events(
                    saved_track.id, saved_track.effects.front().id, plugin_id),
            "paused routed playback exposed a live manual event slot");
    run.value()->stop_and_retire();
#else
    require(!run && run.error().code == foundation::ErrorCode::unsupported &&
                run.error().message.find("VST3 playback disabled") != std::string::npos,
            "scanner-off Quick build silently ignored a saved VST3 insert");
#endif
  }
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  const auto reject_snapshot = [&](project::Snapshot snapshot, std::string_view reason) {
    auto rejected = quick::RoutedPlayback::prepare_and_start(
        root, std::move(snapshot), take(session->reserve_playback_start()), {}, std::nullopt, 64);
    require(!rejected && rejected.error().message.find(reason) != std::string::npos &&
                session->exports()
                    .prepare_wav_master_export({root / "after-vst3-reject.wav"})
                    .has_value(),
            "unavailable VST3 insert did not fail closed and release transport");
  };
  auto changed = session->snapshot();
  effect_on(changed.tracks.front()).binary_sha256.assign(64, '0');
  reject_snapshot(std::move(changed), "binary changed");
  auto malformed = session->snapshot();
  effect_on(malformed.tracks.front()).class_id = "broken";
  reject_snapshot(std::move(malformed), "invalid VST3");
  auto missing = session->snapshot();
  effect_on(missing.tracks.front()).bundle_path = "/tmp/Missing.vst3";
  reject_snapshot(std::move(missing), "could not be scanned");
  auto wrong_class = session->snapshot();
  effect_on(wrong_class.tracks.front()).class_id.assign(32, 'f');
  reject_snapshot(std::move(wrong_class), "class identity");
  auto faulty = session->snapshot();
  constexpr float fail_processing = 999.0F;
  auto& state = effect_on(faulty.tracks.front()).component_state;
  state.resize(sizeof(fail_processing));
  std::memcpy(state.data(), &fail_processing, sizeof(fail_processing));
  OutputEvidence fault_evidence{.caller = std::this_thread::get_id()};
  app::PlaybackOutputFactory fault_factory = [&fault_evidence](audio::StereoRenderSource source,
                                                               audio::TransportCursor& cursor) {
    return std::make_unique<FakeOutput>(source, cursor, fault_evidence, false);
  };
  auto fault_run = quick::RoutedPlayback::prepare_and_start(root, std::move(faulty),
                                                            take(session->reserve_playback_start()),
                                                            fault_factory, std::nullopt, 64);
  require(fault_run.has_value() && fault_evidence.first_sample == 0.0F &&
              fault_run.value()->processor_fault().has_value() &&
              fault_run.value()->status().active_graph_revision == session->snapshot().revision,
          "VST3 process fault did not mute the track with a separate visible diagnostic");
  fault_run.value()->stop_and_retire();
#endif
  const auto slot = session->snapshot().tracks.front().effects.front().id;
  require(session->effects().set_effect_insert_enabled(track.value(), slot, false).has_value() &&
              session->saving().save().has_value(),
          "saved VST3 bypass edit failed");
  session.reset();
  session = take(app::ProjectSession::open(root));
  require(!session->snapshot().tracks.front().effects.front().enabled &&
              session->snapshot().tracks.front().effects.front().id == slot &&
              effect_on(session->snapshot().tracks.front()).id == plugin_id,
          "bypassed VST3 identity or state changed on reopen");
  for (const auto frames : {64U, 128U}) {
    OutputEvidence evidence{.caller = std::this_thread::get_id()};
    app::PlaybackOutputFactory factory = [&evidence](audio::StereoRenderSource source,
                                                     audio::TransportCursor& cursor) {
      return std::make_unique<FakeOutput>(source, cursor, evidence, false);
    };
    auto run = quick::RoutedPlayback::prepare_and_start(root, session->snapshot(),
                                                        take(session->reserve_playback_start()),
                                                        factory, std::nullopt, frames);
    if (!run)
      throw std::runtime_error("bypassed VST3 Quick playback: " + run.error().message);
    require(std::fabs(evidence.first_sample - 0.25F) < 0.00001F &&
                run.value()->status().active_graph_revision == session->snapshot().revision &&
                !run.value()->processor_fault(),
            "bypassed VST3 Quick playback was not dry after save/reopen");
    run.value()->stop_and_retire();
  }
}

#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
void dry_graph_accepts_backend_block_larger_than_request() {
  Directory directory;
  const auto root = directory.path() / "larger-device-quantum";
  const auto wav = make_wav(directory.path(), "source.wav", 8192, {32'768});
  auto session = take(app::ProjectSession::create(root));
  const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  require(fd >= 0, "larger-quantum source open failed");
  (void)take(session->imports().import_wav_audio(fd));
  (void)::close(fd);
  auto stepped = std::make_shared<SteppedEvidence>();
  stepped->actual_frames = 256;
  app::PlaybackOutputFactory factory = [stepped](audio::StereoRenderSource source,
                                                 audio::TransportCursor& cursor) {
    return std::make_unique<SteppedOutput>(source, cursor, stepped);
  };
  auto run = take(quick::RoutedPlayback::prepare_and_start(root, session->snapshot(),
                                                           take(session->reserve_playback_start()),
                                                           factory, std::nullopt, 64));
  stepped->permit(1);
  stepped->await_blocks(1);
  require(run->prepared_block_limit() == 8192 &&
              run->status().application == app::PlaybackApplication::active &&
              run->completed_sample() == 256,
          "dry graph truncated a larger negotiated backend block");
  {
    std::lock_guard lock(stepped->mutex);
    require(stepped->samples.size() == 512 && std::fabs(stepped->samples[0] - 0.25F) < 0.00001F,
            "larger backend block was silent or invalid");
  }
  (void)run->stop_and_retire();
}

// A plugin new to the run is scanned on a worker while the old graph plays;
// the edit then switches live at a block boundary, with no held block.
// Removing the effect again switches live back to the dry source.
void live_zero_latency_vst3_insert_uses_worker_and_keeps_source(std::uint32_t requested_frames,
                                                                std::uint32_t actual_frames = 0,
                                                                bool remove = false,
                                                                bool loop_on_remove = false) {
  const auto frames = actual_frames == 0 ? requested_frames : actual_frames;
  Directory directory;
  const auto root = directory.path() / "live-vst3-insert";
  const auto wav = make_wav(directory.path(), "source.wav", 8192, {32'768});
  auto session = take(app::ProjectSession::create(root));
  const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  require(fd >= 0, "live VST3 source open failed");
  const auto track = take(session->imports().import_wav_audio(fd));
  (void)::close(fd);
  plugins::Vst3Scanner scanner(OSCILLATE_VST3_HELPER_PATH);
  const auto scan = scanner.scan(VST3_LANE_EFFECT_BUNDLE);
  require(scan.ok() && !scan.metadata.classes.empty(), "zero-latency fixture scan failed");
  const auto& klass = scan.metadata.classes.front();
  const app::Vst3InsertIdentity identity{.class_id = klass.id,
                                         .vendor = klass.vendor,
                                         .name = klass.name,
                                         .version = klass.version,
                                         .canonical_bundle_path =
                                             scan.binary.canonical_bundle.string(),
                                         .binary_sha256 = scan.binary.sha256};
  quick::Vst3LiveQualification fixture_evidence;
  // LaneEffect is a synthetic zero-latency fixture. This explicit evidence
  // never qualifies an installed third-party binary.
  fixture_evidence.fixed_latency_binaries.push_back(scan.binary.sha256);
  fixture_evidence.parameters.push_back(
      {scan.binary.sha256, klass.id, 1, 48'000, requested_frames});
  // VST3 stages run in chunks of at most 128 frames.
  if (requested_frames != app::vst3_host_block_frames)
    fixture_evidence.parameters.push_back(
        {scan.binary.sha256, klass.id, 1, 48'000, app::vst3_host_block_frames});
  auto stepped = std::make_shared<SteppedEvidence>();
  stepped->count_allocations = true;
  stepped->actual_frames = actual_frames;
  app::PlaybackOutputFactory factory = [stepped](audio::StereoRenderSource source,
                                                 audio::TransportCursor& cursor) {
    return std::make_unique<SteppedOutput>(source, cursor, stepped);
  };
  auto run = take(quick::RoutedPlayback::prepare_and_start(
      root, session->snapshot(), take(session->reserve_playback_start()), factory, std::nullopt,
      requested_frames, 0, std::move(fixture_evidence)));
  stepped->permit(1);
  stepped->await_blocks(1);
  const auto before = run->status();
  require(before.active_graph_revision == session->snapshot().revision &&
              run->prepared_block_limit() == 8192,
          "dry graph or its block limit was wrong before the edit");
  const auto inserted = take(session->effects().insert_vst3_plugin(track, 0, identity));
  const auto saved_revision = session->snapshot().revision;
  require(run->submit_revision(session->snapshot()).has_value() && run->edit_pending(),
          "Quick refused the effect insertion while playing");
  // The scan runs on a worker; the old graph keeps playing meanwhile.
  stepped->permit(1);
  stepped->await_blocks(2);
  {
    std::lock_guard lock(stepped->mutex);
    require(stepped->samples.size() == static_cast<std::size_t>(frames) * 2 * 2 &&
                std::fabs(stepped->samples[static_cast<std::size_t>(frames) * 2] - 0.25F) <
                    0.00001F,
            "old source stopped or changed while the VST3 worker scanned");
  }
  oscillate::test::wait_until(
      [&] {
        (void)run->status();
        return run->graph_handoff_in_progress();
      },
      "the routed run offered the prepared graph");
  const auto during = run->status();
  require(run->graph_handoff_in_progress() &&
              during.application == app::PlaybackApplication::pending &&
              during.model_revision == saved_revision &&
              during.active_graph_revision == before.active_graph_revision,
          std::string("scanned VST3 effect was not staged for a block-boundary switch: ") +
              (during.failure ? during.failure->message : "no failure receipt"));
  stepped->permit(1);
  stepped->await_blocks(3);
  stepped->permit(1);
  stepped->await_blocks(4);
  const auto active = run->status();
  require(active.application == app::PlaybackApplication::active &&
              active.active_graph_revision == saved_revision && !run->processor_fault() &&
              run->output_status().state == audio::OutputState::streaming,
          "live effect did not activate");
  const auto& saved_effect = effect_on(session->snapshot().tracks.front());
  const auto manual = run->active_vst3_effect_manual_events(
      track, session->snapshot().tracks.front().effects.front().id, saved_effect.id);
  require(manual && manual->slot && manual->identity.graph_revision == saved_revision &&
              manual->slot->admission(manual->identity, saved_revision + 1, {1, 0, 0.5}) ==
                  plugins::Vst3ManualEventStatus::accepted,
          "newly inserted effect was absent from active manual-parameter bookkeeping");
  {
    std::lock_guard lock(stepped->mutex);
    require(stepped->callback_allocation_count == 0 && stepped->frozen_silent_blocks == 0 &&
                stepped->samples.size() == static_cast<std::size_t>(frames) * 2 * 4 &&
                std::all_of(stepped->samples.begin() + static_cast<std::ptrdiff_t>(frames) * 2 * 2,
                            stepped->samples.end(),
                            [](float sample) { return std::fabs(sample - 0.125F) < 0.00001F; }),
            "seamless insertion lost source/effect output, held or allocated");
  }
  if (remove) {
    if (loop_on_remove)
      require(run->set_loop(0, std::uint64_t{frames} * 8), "VST3 removal loop did not start");
    const auto slot = session->snapshot().tracks.front().effects.front().id;
    require(session->effects().remove_effect_insert(track, slot).has_value(),
            "effect removal did not save");
    const auto removal_revision = session->snapshot().revision;
    require(run->submit_revision(session->snapshot()).has_value(),
            "effect removal was refused while playing");
    stepped->permit(1);
    stepped->await_blocks(5);
    const auto removed = run->status();
    require(removed.application == app::PlaybackApplication::active &&
                removed.active_graph_revision == removal_revision &&
                !run->active_vst3_effect_manual_events(track, slot, inserted) &&
                run->completed_sample() == std::uint64_t{frames} * 5,
            "removed VST3 insert remained active after its switch");
    {
      std::lock_guard lock(stepped->mutex);
      require(
          stepped->frozen_silent_blocks == 0 && stepped->callback_allocation_count == 0 &&
              stepped->samples.size() == static_cast<std::size_t>(frames) * 2 * 5 &&
              std::all_of(stepped->samples.begin() + static_cast<std::ptrdiff_t>(frames) * 2 * 4,
                          stepped->samples.end(),
                          [](float sample) { return std::fabs(sample - 0.25F) < 0.00001F; }),
          "live removal interrupted the dry source or allocated");
    }
  }
  (void)run->stop_and_retire();
  require(session->saving().save().has_value(), "live VST3 insertion save failed");
  session.reset();
  session = take(app::ProjectSession::open(root));
  require(remove ? session->snapshot().tracks.front().effects.empty()
                 : (session->snapshot().tracks.front().effects.size() == 1 &&
                    effect_on(session->snapshot().tracks.front()).id == inserted),
          "saved VST3 effect removal or identity changed on reopen");
}

// A latent effect switches live as well. The fresh plugin emits its own
// latency first; the graph never holds or stops the source.
void live_latent_vst3_insert_switches_without_holding() {
  Directory directory;
  const auto root = directory.path() / "latent-vst3-insert";
  const auto wav = make_wav(directory.path(), "source.wav", 8192, {32'768});
  auto session = take(app::ProjectSession::create(root));
  const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  require(fd >= 0, "latent VST3 source open failed");
  const auto track = take(session->imports().import_wav_audio(fd));
  (void)::close(fd);
  plugins::Vst3Scanner scanner(OSCILLATE_VST3_HELPER_PATH);
  const auto scan = scanner.scan(VST3_GAIN_BUNDLE);
  require(scan.ok() && !scan.metadata.classes.empty(), "latent fixture scan failed");
  const auto& klass = scan.metadata.classes.front();
  const app::Vst3InsertIdentity identity{.class_id = klass.id,
                                         .vendor = klass.vendor,
                                         .name = klass.name,
                                         .version = klass.version,
                                         .canonical_bundle_path =
                                             scan.binary.canonical_bundle.string(),
                                         .binary_sha256 = scan.binary.sha256};
  auto stepped = std::make_shared<SteppedEvidence>();
  stepped->count_allocations = true;
  app::PlaybackOutputFactory factory = [stepped](audio::StereoRenderSource source,
                                                 audio::TransportCursor& cursor) {
    return std::make_unique<SteppedOutput>(source, cursor, stepped);
  };
  auto run = take(quick::RoutedPlayback::prepare_and_start(root, session->snapshot(),
                                                           take(session->reserve_playback_start()),
                                                           factory, std::nullopt, 64));
  stepped->permit(1);
  stepped->await_blocks(1);
  const auto old_revision = run->status().active_graph_revision;
  const auto inserted = take(session->effects().insert_vst3_plugin(track, 0, identity));
  require(run->submit_revision(session->snapshot()).has_value(),
          "latent effect edit did not enter worker preparation");
  oscillate::test::wait_until(
      [&] {
        (void)run->status();
        return run->graph_handoff_in_progress();
      },
      "the routed run offered the prepared graph");
  const auto staged = run->status();
  require(run->graph_handoff_in_progress() &&
              staged.application == app::PlaybackApplication::pending &&
              staged.active_graph_revision == old_revision &&
              staged.model_revision == session->snapshot().revision,
          std::string("latent effect was not staged for a live switch: ") +
              (staged.failure ? staged.failure->message : "no failure receipt"));
  stepped->permit(1);
  stepped->await_blocks(2);
  stepped->permit(1);
  stepped->await_blocks(3);
  const auto active = run->status();
  require(active.application == app::PlaybackApplication::active &&
              active.active_graph_revision == session->snapshot().revision &&
              !run->processor_fault(),
          "latent effect did not become active");
  {
    std::lock_guard lock(stepped->mutex);
    require(stepped->samples.size() == std::size_t{64} * 2 * 3 &&
                stepped->frozen_silent_blocks == 0 && stepped->callback_allocation_count == 0 &&
                std::fabs(stepped->samples[0] - 0.25F) < 0.00001F,
            "latent effect switch held, stopped the source or allocated");
    // The switching block plays the plugin's output after its seven frames
    // of latency; the next block is fully processed.
    for (std::size_t sample = 64 * 2 + 7 * 2; sample < stepped->samples.size(); ++sample)
      require(std::fabs(stepped->samples[sample] - 0.5F) < 0.00001F,
              "latent effect output was wrong after its switch");
  }
  (void)run->stop_and_retire();
  require(session->saving().save().has_value(), "live latent insert save failed");
  session.reset();
  session = take(app::ProjectSession::open(root));
  require(session->snapshot().tracks.front().effects.size() == 1 &&
              effect_on(session->snapshot().tracks.front()).id == inserted,
          "live latent insert identity changed on reopen");
}
#endif

void missing_bypassed_vst3_between_native_effects_is_dry() {
  for (const bool bus_path : {false, true}) {
    Directory directory;
    const auto root = directory.path() / "bypassed-missing-song";
    const auto wav = make_wav(directory.path());
    auto session = take(app::ProjectSession::create(root));
    const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
    require(fd >= 0, "bypassed missing fixture source open failed");
    const auto audio_track = take(session->imports().import_wav_audio(fd));
    (void)::close(fd);
    auto owner = audio_track;
    if (bus_path) {
      owner = take(session->tracks().add_bus_track(project::TrackKind::group_bus));
      require(session->routing().set_main_output(audio_track, owner).has_value(),
              "bypassed missing fixture bus route failed");
    }
    const Band band{shape_kind::high_pass, 1'000.0, 0.707, 0.0};
    const auto eq = take(session->effects().add_native_effect(owner, 0, shape_band(band)));
    Compression compression;
    compression.threshold_db = -30.0;
    compression.ratio = 20.0;
    compression.attack_ms = 0.1;
    compression.release_ms = 1.0;
    const auto compressor =
        take(session->effects().add_native_effect(owner, 1, press(compression)));
    auto baseline = session->snapshot();
    auto bypassed = baseline;
    const auto path = std::ranges::find(bypassed.tracks, owner, &project::Track::id);
    require(path != bypassed.tracks.end(), "bypassed missing owner is absent");
    const auto bypass = take(project::InsertSlotId::from_bits({0, 9'001}));
    const auto plugin = take(project::PluginInstanceId::from_bits({0, 9'002}));
    project::Vst3PluginInstance missing{plugin,
                                        std::string(32, 'a'),
                                        "Missing vendor",
                                        "Missing effect",
                                        "1",
                                        "/missing/Broken.vst3",
                                        std::string(64, 'a'),
                                        {},
                                        {},
                                        true};
    path->effects.insert(path->effects.begin() + 1, {bypass, false, missing});
    require(path->effects.size() == 3 && path->effects[0].id == eq &&
                path->effects[1].id == bypass && path->effects[2].id == compressor &&
                std::get<project::Vst3PluginInstance>(path->effects[1].processor) == missing,
            "bypassed slot changed saved order or identity");
    for (const auto frames : {64U, 128U}) {
      const auto play = [&](project::Snapshot snapshot) {
        OutputEvidence evidence{.caller = std::this_thread::get_id()};
        app::PlaybackOutputFactory factory = [&evidence](audio::StereoRenderSource source,
                                                         audio::TransportCursor& cursor) {
          return std::make_unique<FakeOutput>(source, cursor, evidence, false);
        };
        const auto revision = snapshot.revision;
        auto run = quick::RoutedPlayback::prepare_and_start(root, std::move(snapshot),
                                                            take(session->reserve_playback_start()),
                                                            factory, std::nullopt, frames);
        if (!run)
          throw std::runtime_error("bypassed VST3 Quick playback: " + run.error().message);
        require(run.value()->status().active_graph_revision == revision &&
                    !run.value()->processor_fault(),
                "bypassed missing slot reported an inactive or failed graph");
        run.value()->stop_and_retire();
        return evidence.first_channel;
      };
      const auto dry = play(baseline);
      const auto with_bypass = play(bypassed);
      require(
          std::ranges::equal(std::span(dry).first(frames), std::span(with_bypass).first(frames)) &&
              std::ranges::any_of(std::span(dry).first(frames),
                                  [](float sample) { return sample != 0.0F; }),
          "bypassed missing slot altered the native chain output");
    }
    path->effects[1].enabled = true;
    bool output_created = false;
    app::PlaybackOutputFactory factory = [&output_created](audio::StereoRenderSource,
                                                           audio::TransportCursor&) {
      output_created = true;
      return std::unique_ptr<app::PlaybackOutput>{};
    };
    const auto rejected = quick::RoutedPlayback::prepare_and_start(
        root, bypassed, take(session->reserve_playback_start()), factory, std::nullopt, 64);
    require(!rejected && !output_created,
            "enabled missing effect was silently omitted or opened an output");
  }
}

// A qualified synthetic plugin edited while routed playback runs. Each
// gesture saves one revision, stays pending until its exact callback receipt,
// then becomes audible without replacing the runtime. ConnectedGain scales the
// 0.25 source by 4 x value; TimedInstrument outputs Level x velocity.
void qualified_manual_vst3_edits_become_live() {
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  plugins::Vst3Scanner scanner(OSCILLATE_VST3_HELPER_PATH);
  for (const bool instrument : {false, true}) {
    const auto scan = scanner.scan(instrument ? VST3_INSTRUMENT_BUNDLE : VST3_CONNECTED_BUNDLE);
    require(scan.ok(), "manual live fixture scan failed");
    const auto klass = std::ranges::find_if(scan.metadata.classes, [](const auto& candidate) {
      return candidate.category == "Audio Module Class" || candidate.category == "Instrument";
    });
    require(klass != scan.metadata.classes.end(), "manual live fixture class missing");
    const app::Vst3InsertIdentity identity{.class_id = klass->id,
                                           .vendor = klass->vendor,
                                           .name = klass->name,
                                           .version = klass->version,
                                           .canonical_bundle_path =
                                               scan.binary.canonical_bundle.string(),
                                           .binary_sha256 = scan.binary.sha256};
    const std::uint32_t parameter = instrument ? 1 : 101;
    const float scale = instrument ? 100.0F / 127.0F : 1.0F;
    for (const std::uint32_t frames : {64U, 128U}) {
      Directory directory;
      const auto root = directory.path() / "manual-live-song";
      auto session = take(app::ProjectSession::create(root));
      std::optional<project::InsertSlotId> insert;
      project::TrackId track = project::TrackId::from_bits({0, 1}).value();
      if (instrument) {
        require(session->tracks().add_instrument_track().has_value(),
                "manual instrument track failed");
        track = session->snapshot().tracks.front().id;
        require(session->midi_clips().add_midi_clip(track, {0}).has_value(),
                "manual MIDI clip failed");
        const auto clip = session->snapshot().tracks.front().midi_clips.front().id;
        require(session->notes().add_note(clip, {0}, 60).has_value(), "manual note failed");
      } else {
        const auto wav = make_wav(directory.path());
        const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
        require(fd >= 0, "manual live WAV open failed");
        track = take(session->imports().import_wav_audio(fd));
        (void)::close(fd);
      }
      (void)take(session->effects().insert_vst3_plugin(track, 0, identity));
      const auto saved = [&] {
        const auto& owner = session->snapshot().tracks.front();
        return instrument ? std::get<project::Vst3PluginInstance>(owner.instrument_source)
                          : std::get<project::Vst3PluginInstance>(owner.effects.front().processor);
      };
      if (!instrument)
        insert = session->snapshot().tracks.front().effects.front().id;
      const auto baseline =
          instrument
              ? session->instruments().set_vst3_instrument_state(
                    session->snapshot().revision, track, saved(), {}, {{parameter, 0.5}})
              : session->effects().set_vst3_editor_state(session->snapshot().revision, track,
                                                         *insert, saved(), {}, {{parameter, 0.25}});
      require(baseline.has_value(), "manual live baseline reference failed");
      auto evidence = std::make_shared<SteppedEvidence>();
      evidence->caller = std::this_thread::get_id();
      app::PlaybackOutputFactory factory = [evidence](audio::StereoRenderSource source,
                                                      audio::TransportCursor& cursor) {
        return std::make_unique<SteppedOutput>(source, cursor, evidence);
      };
      quick::Vst3LiveQualification exact;
      exact.parameters.push_back({scan.binary.sha256, klass->id, parameter, 48'000, frames});
      auto run = take(quick::RoutedPlayback::prepare_and_start(
          root, session->snapshot(), take(session->reserve_playback_start()), factory, std::nullopt,
          frames, 0, std::move(exact)));
      evidence->permit(1);
      evidence->await_blocks(1);
      const auto opened = session->snapshot().revision;
      require(run->status().active_graph_revision == opened, "manual live graph did not activate");
      const auto gesture = [&](double value) {
        auto original = saved();
        auto references = original.parameters;
        references.front().normalized_value = value;
        return quick::Vst3ManualGesture{session->snapshot().revision,
                                        track,
                                        insert,
                                        original,
                                        original.component_state,
                                        std::move(references),
                                        {parameter, 0, value}};
      };
      const auto block_value = [&](std::size_t block) {
        std::lock_guard lock(evidence->mutex);
        const auto first = block * frames * 2;
        const auto last = first + (std::size_t{frames} - 1) * 2;
        require(evidence->samples.size() > last, "manual live block was not captured");
        return std::pair{evidence->samples[first], evidence->samples[last]};
      };
      for (const auto& [index, value] : {std::pair{1U, 0.6}, std::pair{2U, 0.75}}) {
        const auto pending = take(run->commit_vst3_manual_edit(*session, gesture(value)));
        const auto held = run->status();
        require(pending.phase == app::Vst3ManualEditPhase::pending &&
                    pending.saved_revision == opened + index &&
                    held.model_revision == opened + index &&
                    held.active_graph_revision == opened + index - 1 && run->edit_pending() &&
                    !run->commit_vst3_manual_edit(*session, gesture(0.9)) &&
                    session->snapshot().revision == opened + index,
                "pending manual value was implied live or admitted a second gesture");
        evidence->permit(1);
        evidence->await_blocks(index + 1);
        (void)run->status();
        const auto live = run->vst3_manual_edit_status();
        require(live && live->plugin == saved().id &&
                    live->status.phase == app::Vst3ManualEditPhase::applied &&
                    live->status.live_revision == opened + index &&
                    live->status.applied_sample == static_cast<std::int64_t>(index) * frames &&
                    run->status().active_graph_revision == opened + index && !run->edit_pending() &&
                    !run->processor_fault(),
                "manual receipt did not make the exact revision live");
        const auto [front, back] = block_value(index);
        const auto expected = static_cast<float>(instrument ? value : value * 4.0 * 0.25) * scale;
        require(std::fabs(front - expected) < 0.00001F && std::fabs(back - expected) < 0.00001F,
                "manual live value was not audible from the receipt block: " +
                    std::to_string(front) + " expected " + std::to_string(expected));
      }
      require(run->stop_and_retire() !=
                  quick::RoutedPlayback::StopDrainOutcome::instrument_release_failed,
              "manual live run did not stop");
      run.reset();
      const auto intent = saved();
      require(session->saving().save().has_value(), "manual live intent did not save");
      session.reset();
      auto reopened = take(app::ProjectSession::open(root));
      const auto& owner = reopened->snapshot().tracks.front();
      const auto& restored =
          instrument ? std::get<project::Vst3PluginInstance>(owner.instrument_source)
                     : std::get<project::Vst3PluginInstance>(owner.effects.front().processor);
      require(restored == intent && restored.parameters.front().normalized_value == 0.75,
              "manual live intent did not reopen");
    }
  }
#endif
}

void two_saved_effects_on_distinct_audio_tracks() {
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  Directory directory;
  const auto root = directory.path() / "two-effects-song";
  auto session = take(app::ProjectSession::create(root));
  const auto import = [&](const fs::path& wav) {
    const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
    require(fd >= 0, "two-effect WAV source open failed");
    auto track = session->imports().import_wav_audio(fd);
    (void)::close(fd);
    require(track.has_value(), "two-effect WAV import failed");
    return track.value();
  };
  const auto first_track = import(make_wav(directory.path(), "first.wav", 4096));
  const auto second_track = import(make_wav(directory.path(), "second.wav", 2048));
  require(first_track != second_track, "two-effect fixture did not create distinct tracks");
  plugins::Vst3Scanner scanner(OSCILLATE_VST3_HELPER_PATH);
  const auto scan = scanner.scan(VST3_GAIN_BUNDLE);
  require(scan.ok() && scan.metadata.classes.size() == 1, "two-effect fixture scan failed");
  const auto& klass = scan.metadata.classes.front();
  const app::Vst3InsertIdentity request{.class_id = klass.id,
                                        .vendor = klass.vendor,
                                        .name = klass.name,
                                        .version = klass.version,
                                        .canonical_bundle_path =
                                            scan.binary.canonical_bundle.string(),
                                        .binary_sha256 = scan.binary.sha256};
  const auto first_id = take(session->effects().insert_vst3_plugin(first_track, 0, request));
  const auto second_id = take(session->effects().insert_vst3_plugin(second_track, 0, request));
  require(first_id != second_id, "two saved effects reused one plugin ID");
  session.reset();
  session = take(app::ProjectSession::open(root));
  const auto snapshot = session->snapshot();
  require(snapshot.tracks.size() == 2 && snapshot.tracks[0].id == first_track &&
              snapshot.tracks[0].effects.size() == 1 &&
              effect_on(snapshot.tracks[0]).id == first_id &&
              snapshot.tracks[1].id == second_track && snapshot.tracks[1].effects.size() == 1 &&
              effect_on(snapshot.tracks[1]).id == second_id,
          "reopen changed exact saved track and plugin binding");

  const auto play = [&](std::uint32_t frames) {
    OutputEvidence evidence{.caller = std::this_thread::get_id()};
    app::PlaybackOutputFactory factory = [&evidence](audio::StereoRenderSource source,
                                                     audio::TransportCursor& cursor) {
      return std::make_unique<FakeOutput>(source, cursor, evidence, false);
    };
    auto run = quick::RoutedPlayback::prepare_and_start(root, session->snapshot(),
                                                        take(session->reserve_playback_start()),
                                                        factory, std::nullopt, frames);
    if (!run)
      throw std::runtime_error(run.error().message);
    require(evidence.first_channel[0] == 0.0F &&
                std::fabs(evidence.first_channel[7] - 0.375F) < 0.00001F &&
                evidence.completed_sample == frames && evidence.requested_buffer_frames == frames &&
                run.value()->status().active_graph_revision == snapshot.revision &&
                !run.value()->processor_fault(),
            "two saved effects did not render distinct source tracks into active revision");
    run.value()->stop_and_retire();
  };
  play(64);
  play(128);

  // The same two saved processor identities can run serially on one audio
  // path. Both serial gain stages declare and apply seven frames of latency;
  // PDC holds the other track's dry source for the same 14 frames.
  auto serial = snapshot;
  serial.tracks[0].effects.push_back(serial.tracks[1].effects.front());
  serial.tracks[1].effects.clear();
  for (const auto frames : {64U, 128U}) {
    OutputEvidence evidence{.caller = std::this_thread::get_id()};
    app::PlaybackOutputFactory factory = [&evidence](audio::StereoRenderSource source,
                                                     audio::TransportCursor& cursor) {
      return std::make_unique<FakeOutput>(source, cursor, evidence, false);
    };
    auto run = quick::RoutedPlayback::prepare_and_start(
        root, serial, take(session->reserve_playback_start()), factory, std::nullopt, frames);
    if (!run)
      throw std::runtime_error(run.error().message);
    require(evidence.first_channel[0] == 0.0F &&
                std::fabs(evidence.first_channel[14] - (0.5F + 0.0625F)) < 0.00001F &&
                run.value()->status().active_graph_revision == serial.revision &&
                !run.value()->processor_fault(),
            "two saved serial effects were omitted or reordered during Quick preparation: " +
                std::to_string(evidence.first_channel[14]));
    run.value()->stop_and_retire();
  }

  const auto third_track = take(project::TrackId::from_bits({0, 778}));
  auto with_empty_audio = snapshot;
  with_empty_audio.tracks.emplace_back(third_track);
  OutputEvidence empty_evidence{.caller = std::this_thread::get_id()};
  app::PlaybackOutputFactory empty_factory = [&empty_evidence](audio::StereoRenderSource source,
                                                               audio::TransportCursor& cursor) {
    return std::make_unique<FakeOutput>(source, cursor, empty_evidence, false);
  };
  auto empty_run = quick::RoutedPlayback::prepare_and_start(root, std::move(with_empty_audio),
                                                            take(session->reserve_playback_start()),
                                                            empty_factory, std::nullopt, 64);
  require(empty_run && std::fabs(empty_evidence.first_channel[7] - 0.375F) < 0.00001F,
          "unbound empty audio track changed the two exact effect source routes");
  empty_run.value()->stop_and_retire();

  auto failed_second = snapshot;
  effect_on(failed_second.tracks[1]).component_state = {1, 2, 3};
  bool output_created = false;
  app::PlaybackOutputFactory should_not_start = [&output_created](audio::StereoRenderSource,
                                                                  audio::TransportCursor&) {
    output_created = true;
    return std::unique_ptr<app::PlaybackOutput>{};
  };
  auto rejected = quick::RoutedPlayback::prepare_and_start(root, std::move(failed_second),
                                                           take(session->reserve_playback_start()),
                                                           should_not_start, std::nullopt, 64);
  require(!rejected && !output_created && effect_on(session->snapshot().tracks[0]).id == first_id &&
              effect_on(session->snapshot().tracks[1]).id == second_id &&
              session->snapshot().revision == snapshot.revision,
          "failed second preparation started output or altered saved model identity");
  // A third effect on a third, empty audio track plays too (ADR-0050): its
  // path adds nothing audible and the other two stay aligned.
  auto third_insert = snapshot.tracks[0].effects.front();
  third_insert.id = take(project::InsertSlotId::from_bits({0, 777}));
  std::get<project::Vst3PluginInstance>(third_insert.processor).id =
      take(project::PluginInstanceId::from_bits({0, 777}));
  auto three_effects = snapshot;
  three_effects.tracks.emplace_back(third_track, std::vector<project::Clip>{},
                                    project::TrackKind::audio, project::InstrumentSource{},
                                    std::vector<project::MidiClip>{},
                                    std::vector<project::EffectInsert>{third_insert});
  OutputEvidence third_evidence{.caller = std::this_thread::get_id()};
  app::PlaybackOutputFactory third_factory = [&third_evidence](audio::StereoRenderSource source,
                                                               audio::TransportCursor& cursor) {
    return std::make_unique<FakeOutput>(source, cursor, third_evidence, false);
  };
  auto third_run = quick::RoutedPlayback::prepare_and_start(root, std::move(three_effects),
                                                            take(session->reserve_playback_start()),
                                                            third_factory, std::nullopt, 64);
  if (!third_run)
    throw std::runtime_error("third VST3 effect track: " + third_run.error().message);
  require(third_evidence.first_channel[6] == 0.0F &&
              std::fabs(third_evidence.first_channel[7] - 0.375F) < 0.00001F &&
              !third_run.value()->processor_fault(),
          "a third VST3 effect track changed the two exact effect routes");
  third_run.value()->stop_and_retire();
  play(64); // A rejected preparation cannot poison the next exact saved graph.
#endif
}

void three_saved_effects_on_one_audio_track() {
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  Directory directory;
  const auto root = directory.path() / "three-effects-song";
  auto session = take(app::ProjectSession::create(root));
  const auto import = [&](std::string_view name, std::uint16_t sample) {
    const auto wav = make_wav(directory.path(), name, sample);
    const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
    require(fd >= 0, "three-effect WAV source open failed");
    auto track = session->imports().import_wav_audio(fd);
    (void)::close(fd);
    require(track.has_value(), "three-effect WAV import failed");
    return track.value();
  };
  const auto wet_track = import("wet.wav", 4096);
  const auto dry_track = import("dry.wav", 2048);
  plugins::Vst3Scanner scanner(OSCILLATE_VST3_HELPER_PATH);
  const auto scan = scanner.scan(VST3_GAIN_BUNDLE);
  require(scan.ok() && scan.metadata.classes.size() == 1, "three-effect fixture scan failed");
  const auto& klass = scan.metadata.classes.front();
  const app::Vst3InsertIdentity request{.class_id = klass.id,
                                        .vendor = klass.vendor,
                                        .name = klass.name,
                                        .version = klass.version,
                                        .canonical_bundle_path =
                                            scan.binary.canonical_bundle.string(),
                                        .binary_sha256 = scan.binary.sha256};
  std::vector<project::PluginInstanceId> plugin_ids;
  plugin_ids.reserve(3);
  for (std::size_t stage = 0; stage < 3; ++stage)
    plugin_ids.push_back(take(session->effects().insert_vst3_plugin(wet_track, stage, request)));
  const auto middle = session->snapshot().tracks.front().effects[1];
  float nonlinear = -94.0F;
  std::vector<std::uint8_t> state(sizeof(nonlinear));
  std::memcpy(state.data(), &nonlinear, state.size());
  require(session->effects()
              .set_vst3_editor_state(session->snapshot().revision, wet_track, middle.id,
                                     std::get<project::Vst3PluginInstance>(middle.processor),
                                     std::move(state), {})
              .has_value(),
          "middle effect fixture state was not saved");
  session.reset();
  session = take(app::ProjectSession::open(root));
  const auto saved = session->snapshot();
  require(saved.tracks.size() == 2 && saved.tracks[0].id == wet_track &&
              saved.tracks[1].id == dry_track && saved.tracks[0].effects.size() == 3 &&
              saved.tracks[1].effects.empty(),
          "three-effect route changed on reopen");
  for (std::size_t stage = 0; stage < plugin_ids.size(); ++stage) {
    const auto& slot = saved.tracks[0].effects[stage];
    const auto* plugin = std::get_if<project::Vst3PluginInstance>(&slot.processor);
    require(slot.enabled && plugin && plugin->id == plugin_ids[stage] &&
                (stage == 0 || slot.id != saved.tracks[0].effects[stage - 1].id),
            "three-effect exact saved slot or plugin identity changed on reopen");
  }
  require(std::get<project::Vst3PluginInstance>(saved.tracks[0].effects[1].processor)
                  .component_state.size() == sizeof(nonlinear),
          "middle effect opaque state changed on reopen");
  for (const auto frames : {64U, 128U}) {
    OutputEvidence evidence{.caller = std::this_thread::get_id()};
    app::PlaybackOutputFactory factory = [&evidence](audio::StereoRenderSource source,
                                                     audio::TransportCursor& cursor) {
      return std::make_unique<FakeOutput>(source, cursor, evidence, false);
    };
    auto run = quick::RoutedPlayback::prepare_and_start(
        root, saved, take(session->reserve_playback_start()), factory, std::nullopt, frames);
    if (!run)
      throw std::runtime_error(run.error().message);
    // The first two stages transform 0.125 -> 0.25 -> 0.0625; the third
    // doubles it. The dry 0.0625 path is aligned after all 21 latency frames.
    require(evidence.first_channel[20] == 0.0F &&
                std::fabs(evidence.first_channel[21] - 0.1875F) < 0.00001F &&
                run.value()->status().active_graph_revision == saved.revision &&
                !run.value()->processor_fault(),
            "three serial effects lost order, PDC or active-revision status");
    run.value()->stop_and_retire();
  }

  bool output_created = false;
  app::PlaybackOutputFactory should_not_start = [&output_created](audio::StereoRenderSource,
                                                                  audio::TransportCursor&) {
    output_created = true;
    return std::unique_ptr<app::PlaybackOutput>{};
  };
  // The third effect moved to the dry track plays there (ADR-0050). The wet
  // path is 0.125 -> 0.25 -> 0.0625 after 14 frames; the dry 0.0625 is
  // doubled after 7 and compensated to the same 14.
  auto distributed = saved;
  distributed.tracks[1].effects.push_back(distributed.tracks[0].effects.back());
  distributed.tracks[0].effects.pop_back();
  OutputEvidence distributed_evidence{.caller = std::this_thread::get_id()};
  app::PlaybackOutputFactory distributed_factory =
      [&distributed_evidence](audio::StereoRenderSource source, audio::TransportCursor& cursor) {
        return std::make_unique<FakeOutput>(source, cursor, distributed_evidence, false);
      };
  auto distributed_run = quick::RoutedPlayback::prepare_and_start(
      root, std::move(distributed), take(session->reserve_playback_start()), distributed_factory,
      std::nullopt, 64);
  if (!distributed_run)
    throw std::runtime_error("distributed VST3 effects: " + distributed_run.error().message);
  require(distributed_evidence.first_channel[13] == 0.0F &&
              std::fabs(distributed_evidence.first_channel[14] - 0.1875F) < 0.00001F &&
              !distributed_run.value()->processor_fault(),
          "VST3 effects distributed over two tracks lost order or PDC");
  distributed_run.value()->stop_and_retire();
  auto bad_state = saved;
  std::get<project::Vst3PluginInstance>(bad_state.tracks[0].effects.back().processor)
      .component_state = {1, 2, 3};
  auto refused = quick::RoutedPlayback::prepare_and_start(root, std::move(bad_state),
                                                          take(session->reserve_playback_start()),
                                                          should_not_start, std::nullopt, 64);
  require(!refused && !output_created && session->snapshot().revision == saved.revision,
          "failed third processor preparation started output or altered the saved project");
#endif
}

void saved_vst3_instrument_plays_midi_and_fails_closed() {
  Directory directory;
  const auto root = directory.path() / "instrument-song";
  auto session = take(app::ProjectSession::create(root));
  require(session->tracks().add_instrument_track().has_value(), "instrument track creation failed");
  const auto track = session->snapshot().tracks.front().id;
  require(session->midi_clips().add_midi_clip(track, {0}).has_value(),
          "instrument MIDI clip failed");
  const auto clip = session->snapshot().tracks.front().midi_clips.front().id;
  require(session->notes().add_note(clip, {0}, 60).has_value(), "instrument note failed");
  auto unbound = quick::RoutedPlayback::prepare_and_start(
      root, session->snapshot(), take(session->reserve_playback_start()), {}, std::nullopt, 64);
  auto after_unbound = session->reserve_playback_start();
  require(!unbound &&
              unbound.error().message.find("verified bound MIDI instrument") != std::string::npos &&
              after_unbound.has_value(),
          "unbound MIDI was accepted or playback reservation was not released");
  after_unbound.value().finish_after_join();
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  plugins::Vst3Scanner scanner(OSCILLATE_VST3_HELPER_PATH);
  auto scan = scanner.scan(VST3_INSTRUMENT_BUNDLE);
  require(scan.ok() && scan.metadata.classes.size() == 1, "instrument fixture scan failed");
  const auto& klass = scan.metadata.classes.front();
  auto inserted = session->effects().insert_vst3_plugin(
      track, 0,
      {.class_id = klass.id,
       .vendor = klass.vendor,
       .name = klass.name,
       .version = klass.version,
       .canonical_bundle_path = scan.binary.canonical_bundle.string(),
       .binary_sha256 = scan.binary.sha256});
  require(inserted.has_value(), "instrument fixture insert failed");
  session.reset();
  session = take(app::ProjectSession::open(root));
  require(session->snapshot().tracks.front().midi_clips.front().notes.size() == 1 &&
              instrument_on(session->snapshot().tracks.front()).id == inserted.value(),
          "instrument MIDI and plugin identity did not reopen");
  for (const auto frames : {64U, 128U}) {
    OutputEvidence evidence{.caller = std::this_thread::get_id()};
    app::PlaybackOutputFactory factory = [&evidence](audio::StereoRenderSource source,
                                                     audio::TransportCursor& cursor) {
      return std::make_unique<FakeOutput>(source, cursor, evidence, false);
    };
    auto run = quick::RoutedPlayback::prepare_and_start(root, session->snapshot(),
                                                        take(session->reserve_playback_start()),
                                                        factory, std::nullopt, frames);
    if (!run)
      throw std::runtime_error(run.error().message);
    require(std::fabs(evidence.first_sample - 0.5F * 100.0F / 127.0F) < 0.00001F &&
                evidence.completed_sample == frames && evidence.requested_buffer_frames == frames &&
                run.value()->status().active_graph_revision == session->snapshot().revision &&
                !run.value()->processor_fault(),
            "saved instrument was not audible on first MIDI sample");
    require(run.value()->status().active_graph_revision == session->snapshot().revision &&
                !run.value()->active_vst3_instrument_manual_events(track, inserted.value()),
            "unqualified VST3 instrument exposed a live manual event slot");
    require(run.value()->stop_and_retire() ==
                quick::RoutedPlayback::StopDrainOutcome::not_requested,
            "instrument note release or stopped transport failed");
    auto after_stop = session->reserve_playback_start();
    require(after_stop.has_value(), "instrument stop did not release playback reservation");
    after_stop.value().finish_after_join();

    quick::Vst3LiveQualification exact;
    exact.parameters.push_back({scan.binary.sha256, klass.id, 1, 48'000, frames});
    auto qualified = quick::RoutedPlayback::prepare_and_start(
        root, session->snapshot(), take(session->reserve_playback_start()), factory, std::nullopt,
        frames, 0, std::move(exact));
    require(qualified.has_value(), "parameter-scoped instrument run did not prepare");
    require(qualified.value()->status().active_graph_revision == session->snapshot().revision,
            "parameter-scoped instrument graph did not activate");
    const auto binding =
        qualified.value()->active_vst3_instrument_manual_events(track, inserted.value());
    require(
        binding &&
            binding->slot->admission(binding->identity, session->snapshot().revision + 1,
                                     {1, 0, 0.75}) == plugins::Vst3ManualEventStatus::accepted &&
            binding->slot->admission(binding->identity, session->snapshot().revision + 1,
                                     {2, 0, 0.75}) == plugins::Vst3ManualEventStatus::unavailable,
        "exact parameter evidence exposed the wrong live controls");
    require(qualified.value()->stop_and_retire() ==
                quick::RoutedPlayback::StopDrainOutcome::not_requested,
            "qualified instrument did not retire");
  }
  const auto wav = make_wav(directory.path(), "instrument-effect.wav");
  const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  require(fd >= 0, "mixed instrument/effect WAV open failed");
  auto audio_track = session->imports().import_wav_audio(fd);
  (void)::close(fd);
  require(audio_track.has_value(), "mixed instrument/effect WAV import failed");
  auto gain_scan = scanner.scan(VST3_GAIN_BUNDLE);
  require(gain_scan.ok() && gain_scan.metadata.classes.size() == 1,
          "mixed instrument/effect gain scan failed");
  const auto& gain = gain_scan.metadata.classes.front();
  const auto effect_id = take(session->effects().insert_vst3_plugin(
      audio_track.value(), 0,
      {.class_id = gain.id,
       .vendor = gain.vendor,
       .name = gain.name,
       .version = gain.version,
       .canonical_bundle_path = gain_scan.binary.canonical_bundle.string(),
       .binary_sha256 = gain_scan.binary.sha256}));
  session.reset();
  session = take(app::ProjectSession::open(root));
  const auto mixed = session->snapshot();
  require(mixed.tracks.size() == 2 && instrument_on(mixed.tracks.front()).id == inserted.value() &&
              effect_on(mixed.tracks.back()).id == effect_id,
          "mixed instrument/effect identities did not reopen");
  for (const bool effect_first : {false, true}) {
    auto ordered = mixed;
    if (effect_first)
      std::ranges::reverse(ordered.tracks);
    for (const auto frames : {64U, 128U}) {
      OutputEvidence evidence{.caller = std::this_thread::get_id()};
      app::PlaybackOutputFactory factory = [&evidence](audio::StereoRenderSource source,
                                                       audio::TransportCursor& cursor) {
        return std::make_unique<FakeOutput>(source, cursor, evidence, false);
      };
      auto run = quick::RoutedPlayback::prepare_and_start(
          root, ordered, take(session->reserve_playback_start()), factory, std::nullopt, frames);
      if (!run)
        throw std::runtime_error(run.error().message);
      require(std::fabs(evidence.first_channel[7] - (0.5F * 100.0F / 127.0F + 0.5F)) < 0.00001F &&
                  run.value()->status().active_graph_revision == mixed.revision &&
                  !run.value()->processor_fault(),
              "mixed instrument/effect path did not align or render both verified plugins");
      require(run.value()->stop_and_retire() ==
                  quick::RoutedPlayback::StopDrainOutcome::not_requested,
              "mixed instrument/effect stop did not release held MIDI notes");
    }
  }
  auto bad_effect = mixed;
  effect_on(bad_effect.tracks.back()).component_state = {1, 2, 3};
  bool output_created = false;
  app::PlaybackOutputFactory should_not_start = [&output_created](audio::StereoRenderSource,
                                                                  audio::TransportCursor&) {
    output_created = true;
    return std::unique_ptr<app::PlaybackOutput>{};
  };
  auto failed_effect = quick::RoutedPlayback::prepare_and_start(
      root, std::move(bad_effect), take(session->reserve_playback_start()), should_not_start,
      std::nullopt, 64);
  auto after_failed_effect = session->reserve_playback_start();
  require(
      !failed_effect && !output_created && after_failed_effect &&
          effect_on(session->snapshot().tracks.back()).id == effect_id,
      "failed mixed effect preparation started output, retained transport or changed saved state");
  after_failed_effect.value().finish_after_join();
  const auto reject = [&](project::Snapshot snapshot, std::string_view reason) {
    auto refused = quick::RoutedPlayback::prepare_and_start(
        root, std::move(snapshot), take(session->reserve_playback_start()), {}, std::nullopt, 64);
    auto after_reject = session->reserve_playback_start();
    require(!refused && refused.error().message.find(reason) != std::string::npos &&
                after_reject.has_value(),
            "unavailable instrument did not fail closed and release transport");
    after_reject.value().finish_after_join();
  };
  auto changed = session->snapshot();
  instrument_on(changed.tracks.front()).binary_sha256.assign(64, '0');
  reject(std::move(changed), "binary changed");
  auto missing = session->snapshot();
  instrument_on(missing.tracks.front()).bundle_path = "/tmp/Missing.vst3";
  reject(std::move(missing), "could not be scanned");
  auto wrong_class = session->snapshot();
  instrument_on(wrong_class.tracks.front()).class_id.assign(32, 'f');
  reject(std::move(wrong_class), "class differs");
  auto wrong_kind = session->snapshot();
  auto& saved = instrument_on(wrong_kind.tracks.front());
  const auto& effect = gain_scan.metadata.classes.front();
  saved.class_id = effect.id;
  saved.vendor = effect.vendor;
  saved.name = effect.name;
  saved.version = effect.version;
  saved.bundle_path = gain_scan.binary.canonical_bundle.string();
  saved.binary_sha256 = gain_scan.binary.sha256;
  reject(std::move(wrong_kind), "could not load");
#endif
}

void two_bare_vst3_instruments_keep_distinct_midi_and_stop_together() {
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  Directory directory;
  const auto root = directory.path() / "two-bare-instruments";
  auto session = take(app::ProjectSession::create(root));
  plugins::Vst3Scanner scanner(OSCILLATE_VST3_HELPER_PATH);
  const auto scan = scanner.scan(VST3_INSTRUMENT_BUNDLE);
  require(scan.ok() && scan.metadata.classes.size() == 1,
          "two-source instrument fixture scan failed");
  const auto& klass = scan.metadata.classes.front();
  const app::Vst3InsertIdentity selection{.class_id = klass.id,
                                          .vendor = klass.vendor,
                                          .name = klass.name,
                                          .version = klass.version,
                                          .canonical_bundle_path =
                                              scan.binary.canonical_bundle.string(),
                                          .binary_sha256 = scan.binary.sha256};
  require(session->tracks().add_instrument_track().has_value() &&
              session->tracks().add_instrument_track().has_value(),
          "two bare instrument tracks could not be created");
  const auto first_track = session->snapshot().tracks[0].id;
  const auto second_track = session->snapshot().tracks[1].id;
  require(first_track != second_track, "two instruments shared a track identity");
  const auto first_plugin = take(session->effects().insert_vst3_plugin(first_track, 0, selection));
  const auto second_plugin =
      take(session->effects().insert_vst3_plugin(second_track, 0, selection));
  require(first_plugin != second_plugin, "two instruments shared a plugin identity");
  require(session->midi_clips().add_midi_clip(first_track, {0}).has_value() &&
              session->midi_clips().add_midi_clip(second_track, {0}).has_value(),
          "two instrument MIDI clips could not be created");
  const auto first_clip = session->snapshot().tracks[0].midi_clips.front().id;
  const auto second_clip = session->snapshot().tracks[1].midi_clips.front().id;
  require(session->notes().add_note(first_clip, {0}, 60).has_value() &&
              session->notes().add_note(second_clip, {1'000'000}, 60).has_value(),
          "two instruments could not retain distinct MIDI note schedules");
  const float second_level = 0.25F;
  std::vector<std::uint8_t> state(sizeof(second_level));
  std::memcpy(state.data(), &second_level, state.size());
  require(session->instruments().set_vst3_instrument_state(
              session->snapshot().revision, second_track,
              instrument_on(session->snapshot().tracks[1]), std::move(state), {{1, 0.25}}) &&
              session->saving().save(),
          "second instrument level or project save failed");
  session.reset();
  session = take(app::ProjectSession::open(root));
  const auto saved = session->snapshot();
  require(saved.tracks.size() == 2 && saved.tracks[0].id == first_track &&
              saved.tracks[1].id == second_track &&
              instrument_on(saved.tracks[0]).id == first_plugin &&
              instrument_on(saved.tracks[1]).id == second_plugin &&
              saved.tracks[0].midi_clips.front().notes.front().key == 60 &&
              saved.tracks[1].midi_clips.front().notes.front().start.value == 1'000'000 &&
              instrument_on(saved.tracks[1]).component_state.size() == sizeof(second_level),
          "two-source save/reopen lost a plugin, state or MIDI schedule");
  for (const auto frames : {64U, 128U}) {
    OutputEvidence evidence{.caller = std::this_thread::get_id()};
    app::PlaybackOutputFactory factory = [&evidence](audio::StereoRenderSource source,
                                                     audio::TransportCursor& cursor) {
      return std::make_unique<FakeOutput>(source, cursor, evidence, false);
    };
    auto run = quick::RoutedPlayback::prepare_and_start(
        root, saved, take(session->reserve_playback_start()), factory, std::nullopt, frames);
    if (!run)
      throw std::runtime_error(run.error().message);
    const float first = 0.5F * 100.0F / 127.0F;
    const float second = 0.25F * 100.0F / 127.0F;
    require(std::fabs(evidence.first_channel[0] - first) < 0.00001F &&
                std::fabs(evidence.first_channel[24] - first) < 0.00001F &&
                std::fabs(evidence.first_channel[25] - first - second) < 0.00001F &&
                run.value()->status().active_graph_revision == saved.revision &&
                !run.value()->processor_fault(),
            "two bare instruments did not render distinct MIDI/state on one routed graph");
    require(run.value()->stop_and_retire() ==
                quick::RoutedPlayback::StopDrainOutcome::not_requested,
            "two-source stop did not release both prepared instruments");
    auto released = session->reserve_playback_start();
    require(released.has_value(), "two-source stop retained the playback reservation");
    released.value().finish_after_join();
  }
  auto corrupt = saved;
  instrument_on(corrupt.tracks[1]).component_state = {1, 2, 3};
  bool output_created = false;
  app::PlaybackOutputFactory should_not_start = [&output_created](audio::StereoRenderSource,
                                                                  audio::TransportCursor&) {
    output_created = true;
    return std::unique_ptr<app::PlaybackOutput>{};
  };
  auto refused = quick::RoutedPlayback::prepare_and_start(root, std::move(corrupt),
                                                          take(session->reserve_playback_start()),
                                                          should_not_start, std::nullopt, 64);
  auto released = session->reserve_playback_start();
  require(!refused && !output_created && released.has_value() &&
              instrument_on(session->snapshot().tracks[1]).id == second_plugin,
          "failed second instrument started output or damaged saved source identity");
  released.value().finish_after_join();
#endif
}

void two_vst3_instrument_effect_paths_render_independently() {
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  Directory directory;
  const auto root = directory.path() / "paired-instrument-effects";
  auto session = take(app::ProjectSession::create(root));
  plugins::Vst3Scanner scanner(OSCILLATE_VST3_HELPER_PATH);
  const auto source_scan = scanner.scan(VST3_INSTRUMENT_BUNDLE);
  const auto effect_scan = scanner.scan(VST3_GAIN_BUNDLE);
  require(source_scan.ok() && effect_scan.ok(), "paired source/effect fixture scans failed");
  const auto& source_class = source_scan.metadata.classes.front();
  const auto& effect_class = effect_scan.metadata.classes.front();
  const app::Vst3InsertIdentity source_identity{.class_id = source_class.id,
                                                .vendor = source_class.vendor,
                                                .name = source_class.name,
                                                .version = source_class.version,
                                                .canonical_bundle_path =
                                                    source_scan.binary.canonical_bundle.string(),
                                                .binary_sha256 = source_scan.binary.sha256};
  const app::Vst3InsertIdentity effect_identity{.class_id = effect_class.id,
                                                .vendor = effect_class.vendor,
                                                .name = effect_class.name,
                                                .version = effect_class.version,
                                                .canonical_bundle_path =
                                                    effect_scan.binary.canonical_bundle.string(),
                                                .binary_sha256 = effect_scan.binary.sha256};
  const auto first_track =
      take(session->instruments().create_vst3_instrument_track(source_identity));
  const auto second_track =
      take(session->instruments().create_vst3_instrument_track(source_identity));
  const auto first_source = instrument_on(session->snapshot().tracks[0]).id;
  const auto second_source = instrument_on(session->snapshot().tracks[1]).id;
  const auto first_effect =
      take(session->effects().insert_vst3_plugin(first_track, 0, effect_identity));
  const auto second_effect =
      take(session->effects().insert_vst3_plugin(second_track, 0, effect_identity));
  require(first_source != second_source && first_effect != second_effect,
          "paired VST3 paths reused a saved plugin identity");
  require(session->midi_clips().add_midi_clip(first_track, {0}) &&
              session->midi_clips().add_midi_clip(second_track, {0}),
          "paired VST3 paths could not create MIDI clips");
  const auto first_clip = session->snapshot().tracks[0].midi_clips.front().id;
  const auto second_clip = session->snapshot().tracks[1].midi_clips.front().id;
  require(session->notes().add_note(first_clip, {0}, 60) &&
              session->notes().add_note(second_clip, {1'000'000}, 60),
          "paired VST3 paths could not save independent MIDI schedules");
  const float second_level = 0.25F;
  std::vector<std::uint8_t> state(sizeof(second_level));
  std::memcpy(state.data(), &second_level, state.size());
  require(session->instruments().set_vst3_instrument_state(
              session->snapshot().revision, second_track,
              instrument_on(session->snapshot().tracks[1]), std::move(state), {{1, 0.25}}) &&
              session->saving().save(),
          "paired VST3 state or project save failed");
  session.reset();
  session = take(app::ProjectSession::open(root));
  const auto saved = session->snapshot();
  require(saved.tracks.size() == 2 && saved.tracks[0].id == first_track &&
              saved.tracks[1].id == second_track &&
              instrument_on(saved.tracks[0]).id == first_source &&
              instrument_on(saved.tracks[1]).id == second_source &&
              effect_on(saved.tracks[0]).id == first_effect &&
              effect_on(saved.tracks[1]).id == second_effect &&
              instrument_on(saved.tracks[1]).component_state.size() == sizeof(second_level),
          "paired VST3 paths lost source, effect or state identity on reopen");
  for (const auto frames : {64U, 128U}) {
    OutputEvidence evidence{.count_allocations = true, .caller = std::this_thread::get_id()};
    app::PlaybackOutputFactory factory = [&evidence](audio::StereoRenderSource source,
                                                     audio::TransportCursor& cursor) {
      return std::make_unique<FakeOutput>(source, cursor, evidence, false);
    };
    auto run = quick::RoutedPlayback::prepare_and_start(
        root, saved, take(session->reserve_playback_start()), factory, std::nullopt, frames);
    if (!run)
      throw std::runtime_error(run.error().message);
    const float first = 2.0F * 0.5F * 100.0F / 127.0F;
    const float second = 2.0F * 0.25F * 100.0F / 127.0F;
    require(evidence.first_channel[6] == 0.0F &&
                std::fabs(evidence.first_channel[7] - first) < 0.00001F &&
                std::fabs(evidence.first_channel[31] - first) < 0.00001F &&
                std::fabs(evidence.first_channel[32] - first - second) < 0.00001F &&
                evidence.callback_allocation_count == 0 &&
                run.value()->status().active_graph_revision == saved.revision &&
                !run.value()->processor_fault(),
            "paired VST3 effects did not process independent instrument routes with PDC");
    (void)run.value()->stop_and_retire();
  }
  const auto connected_scan = scanner.scan(VST3_CONNECTED_BUNDLE);
  require(connected_scan.ok(), "paired effect manual-control fixture scan failed");
  const auto connected_class =
      std::ranges::find_if(connected_scan.metadata.classes, [](const auto& klass) {
        return klass.category == "Audio Module Class";
      });
  require(connected_class != connected_scan.metadata.classes.end(),
          "paired effect manual-control class missing");
  auto mixed_qualification = saved;
  auto& first_mixed_effect = effect_on(mixed_qualification.tracks[0]);
  first_mixed_effect.class_id = connected_class->id;
  first_mixed_effect.vendor = connected_class->vendor;
  first_mixed_effect.name = connected_class->name;
  first_mixed_effect.version = connected_class->version;
  first_mixed_effect.bundle_path = connected_scan.binary.canonical_bundle.string();
  first_mixed_effect.binary_sha256 = connected_scan.binary.sha256;
  quick::Vst3LiveQualification only_first_qualified;
  only_first_qualified.fixed_latency_binaries.push_back(connected_scan.binary.sha256);
  OutputEvidence manual_evidence{.caller = std::this_thread::get_id()};
  app::PlaybackOutputFactory manual_factory = [&manual_evidence](audio::StereoRenderSource source,
                                                                 audio::TransportCursor& cursor) {
    return std::make_unique<FakeOutput>(source, cursor, manual_evidence, false);
  };
  auto mixed_run = quick::RoutedPlayback::prepare_and_start(
      root, std::move(mixed_qualification), take(session->reserve_playback_start()), manual_factory,
      std::nullopt, 64, 0, std::move(only_first_qualified));
  if (!mixed_run)
    throw std::runtime_error(mixed_run.error().message);
  require(mixed_run.value()->status().active_graph_revision == saved.revision,
          "mixed-qualification paired graph did not activate");
  const auto first_binding = mixed_run.value()->active_vst3_effect_manual_events(
      first_track, saved.tracks[0].effects.front().id, first_effect);
  const auto second_binding = mixed_run.value()->active_vst3_effect_manual_events(
      second_track, saved.tracks[1].effects.front().id, second_effect);
  require(first_binding && second_binding && first_binding->slot && second_binding->slot &&
              first_binding->slot->admission(first_binding->identity, saved.revision + 1,
                                             {101, 0, 0.75}) ==
                  plugins::Vst3ManualEventStatus::accepted &&
              second_binding->slot->admission(second_binding->identity, saved.revision + 1,
                                              {101, 0, 0.75}) !=
                  plugins::Vst3ManualEventStatus::accepted,
          "paired effect control evidence was lost or leaked across saved instances");
  (void)mixed_run.value()->stop_and_retire();
  auto stepped = std::make_shared<SteppedEvidence>();
  stepped->count_allocations = true;
  app::PlaybackOutputFactory stepped_factory = [stepped](audio::StereoRenderSource source,
                                                         audio::TransportCursor& cursor) {
    return std::make_unique<SteppedOutput>(source, cursor, stepped);
  };
  auto live = take(quick::RoutedPlayback::prepare_and_start(
      root, saved, take(session->reserve_playback_start()), stepped_factory, std::nullopt, 64));
  stepped->permit(1);
  stepped->await_blocks(1);
  require(live->status().active_graph_revision == saved.revision,
          "paired VST3 graph did not become active before editing");
  require(session->tracks().set_track_level_pan(first_track, 0.5, 0.0) &&
              live->submit_revision(session->snapshot()) && live->edit_pending(),
          "paired VST3 paths could not accept a held live gain edit");
  stepped->permit(1);
  stepped->await_blocks(2);
  (void)live->status();
  stepped->permit(1);
  stepped->await_blocks(3);
  (void)live->status();
  stepped->permit(2);
  stepped->await_blocks(5);
  const auto active = live->status();
  float settled_sample = 0.0F;
  {
    std::lock_guard lock(stepped->mutex);
    require(stepped->samples.size() == std::size_t{5} * 64 * 2,
            "paired VST3 graph lost an audio block at its held edit");
    require(stepped->callback_allocation_count == 0,
            "paired VST3 callback allocated across its held edit");
    settled_sample = stepped->samples[4 * 64 * 2 + 40 * 2];
  }
  const float expected_settled = 2.0F * (0.5F * 0.5F + 0.25F) * 100.0F / 127.0F;
  require(active.active_graph_revision == session->snapshot().revision &&
              active.application == app::PlaybackApplication::active &&
              std::fabs(settled_sample - expected_settled) < 0.00002F && !live->processor_fault(),
          "paired VST3 held edit lost a source, effect or active revision");
  (void)live->stop_and_retire();
  bool output_created = false;
  app::PlaybackOutputFactory should_not_start = [&output_created](audio::StereoRenderSource,
                                                                  audio::TransportCursor&) {
    output_created = true;
    return std::unique_ptr<app::PlaybackOutput>{};
  };
  auto changed = saved;
  effect_on(changed.tracks[1]).binary_sha256.assign(64, '0');
  auto refused = quick::RoutedPlayback::prepare_and_start(root, std::move(changed),
                                                          take(session->reserve_playback_start()),
                                                          should_not_start, std::nullopt, 64);
  require(!refused && !output_created &&
              refused.error().message.find("binary changed") != std::string::npos,
          "changed second effect binary reached the output device");
  auto third_effect = saved;
  third_effect.tracks[1].effects.push_back(third_effect.tracks[0].effects.front());
  third_effect.tracks[1].effects.back().id = take(project::InsertSlotId::from_bits({0, 8'001}));
  std::get<project::Vst3PluginInstance>(third_effect.tracks[1].effects.back().processor).id =
      take(project::PluginInstanceId::from_bits({0, 8'002}));
  // Any chain plays (ADR-0050): a second effect on the second path doubles it
  // again and moves both paths to 14 frames of latency, and an extra empty
  // track changes nothing audible.
  const auto play_exact = [&](project::Snapshot snapshot,
                              std::span<const std::pair<std::size_t, float>> expected,
                              std::string_view message) {
    OutputEvidence evidence{.caller = std::this_thread::get_id()};
    app::PlaybackOutputFactory factory = [&evidence](audio::StereoRenderSource source,
                                                     audio::TransportCursor& cursor) {
      return std::make_unique<FakeOutput>(source, cursor, evidence, false);
    };
    auto run = quick::RoutedPlayback::prepare_and_start(root, std::move(snapshot),
                                                        take(session->reserve_playback_start()),
                                                        factory, std::nullopt, 64);
    if (!run)
      throw std::runtime_error(std::string(message) + ": " + run.error().message);
    for (const auto& [frame, value] : expected)
      require(std::fabs(evidence.first_channel[frame] - value) < 0.00001F, message);
    require(!run.value()->processor_fault(), message);
    (void)run.value()->stop_and_retire();
  };
  const float first_path = 2.0F * 0.5F * 100.0F / 127.0F;
  const float second_path = 2.0F * 0.25F * 100.0F / 127.0F;
  const std::array<std::pair<std::size_t, float>, 3> third_expected{
      {{13, 0.0F}, {14, first_path}, {39, first_path + 2.0F * second_path}}};
  play_exact(std::move(third_effect), third_expected,
             "second effect on the second VST3 instrument path did not play in order");
  auto extra_track = saved;
  extra_track.tracks.emplace_back(take(project::TrackId::from_bits({0, 8'003})));
  const std::array<std::pair<std::size_t, float>, 3> extra_expected{
      {{6, 0.0F}, {7, first_path}, {32, first_path + second_path}}};
  play_exact(std::move(extra_track), extra_expected,
             "an extra empty track changed the paired VST3 instrument paths");
#endif
}

void native_synth_and_vst3_instrument_sum_in_quick_playback() {
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  Directory directory;
  const auto root = directory.path() / "native-and-vst3";
  auto session = take(app::ProjectSession::create(root));
  plugins::Vst3Scanner scanner(OSCILLATE_VST3_HELPER_PATH);
  const auto scan = scanner.scan(VST3_INSTRUMENT_BUNDLE);
  require(scan.ok() && scan.metadata.classes.size() == 1, "mixed instrument fixture scan failed");
  const auto& klass = scan.metadata.classes.front();
  const app::Vst3InsertIdentity selection{.class_id = klass.id,
                                          .vendor = klass.vendor,
                                          .name = klass.name,
                                          .version = klass.version,
                                          .canonical_bundle_path =
                                              scan.binary.canonical_bundle.string(),
                                          .binary_sha256 = scan.binary.sha256};
  require(session->tracks().add_instrument_track() && session->tracks().add_instrument_track(),
          "mixed instrument tracks could not be created");
  const auto native_track = session->snapshot().tracks[0].id;
  const auto vst3_track = session->snapshot().tracks[1].id;
  require(session->instruments().select_native_instrument(native_track, "prism") &&
              session->effects().insert_vst3_plugin(vst3_track, 0, selection) &&
              session->midi_clips().add_midi_clip(native_track, {0}) &&
              session->midi_clips().add_midi_clip(vst3_track, {0}),
          "mixed instrument sources could not be bound");
  const auto native_clip = session->snapshot().tracks[0].midi_clips.front().id;
  const auto vst3_clip = session->snapshot().tracks[1].midi_clips.front().id;
  require(session->notes().add_note(native_clip, {0}, 60) &&
              session->notes().add_note(vst3_clip, {0}, 60) && session->saving().save(),
          "mixed instrument MIDI or save failed");
  session.reset();
  session = take(app::ProjectSession::open(root));
  const auto both = session->snapshot();
  require(
      std::holds_alternative<project::NativeInstrumentPatch>(both.tracks[0].instrument_source) &&
          instrument_on(both.tracks[1]).class_id == selection.class_id,
      "mixed source identities did not survive reopen");
  auto native_only = both;
  native_only.tracks[1].midi_clips.front().notes.clear();
  auto vst3_only = both;
  vst3_only.tracks[0].midi_clips.front().notes.clear();
  for (const auto frames : {64U, 128U}) {
    const auto render = [&](project::Snapshot snapshot) {
      OutputEvidence evidence{.caller = std::this_thread::get_id()};
      app::PlaybackOutputFactory factory = [&evidence](audio::StereoRenderSource source,
                                                       audio::TransportCursor& cursor) {
        return std::make_unique<FakeOutput>(source, cursor, evidence, false);
      };
      auto run = quick::RoutedPlayback::prepare_and_start(root, std::move(snapshot),
                                                          take(session->reserve_playback_start()),
                                                          factory, std::nullopt, frames);
      if (!run)
        throw std::runtime_error(run.error().message);
      require(!run.value()->processor_fault(), "mixed instrument callback faulted");
      (void)run.value()->stop_and_retire();
      auto released = session->reserve_playback_start();
      require(released.has_value(), "mixed playback retained the transport reservation");
      released.value().finish_after_join();
      return evidence.first_channel;
    };
    const auto mixed = render(both);
    const auto native = render(native_only);
    const auto vst3 = render(vst3_only);
    bool native_audible = false;
    bool vst3_audible = false;
    for (std::size_t frame = 0; frame < frames; ++frame) {
      native_audible = native_audible || std::fabs(native[frame]) > 0.000001F;
      vst3_audible = vst3_audible || std::fabs(vst3[frame]) > 0.000001F;
      require(std::fabs(mixed[frame] - native[frame] - vst3[frame]) < 0.00002F,
              "Quick mixed instrument sources did not sum independently");
    }
    require(native_audible && vst3_audible, "one mixed instrument source was silent");
  }
#endif
}

void instrument_owned_vst3_effect_plays_after_its_source() {
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  Directory directory;
  const auto root = directory.path() / "instrument-effect-chain";
  auto session = take(app::ProjectSession::create(root));
  plugins::Vst3Scanner scanner(OSCILLATE_VST3_HELPER_PATH);
  const auto source_scan = scanner.scan(VST3_INSTRUMENT_BUNDLE);
  const auto effect_scan = scanner.scan(VST3_GAIN_BUNDLE);
  require(source_scan.ok() && effect_scan.ok(), "instrument-chain plugin scans failed");
  const auto source_class = source_scan.metadata.classes.front();
  const auto effect_class = effect_scan.metadata.classes.front();
  const auto track = take(session->instruments().create_vst3_instrument_track(
      {.class_id = source_class.id,
       .vendor = source_class.vendor,
       .name = source_class.name,
       .version = source_class.version,
       .canonical_bundle_path = source_scan.binary.canonical_bundle.string(),
       .binary_sha256 = source_scan.binary.sha256}));
  require(session->midi_clips().add_midi_clip(track, {0}).has_value(),
          "instrument-chain MIDI clip failed");
  const auto clip = session->snapshot().tracks.front().midi_clips.front().id;
  require(session->notes().add_note(clip, {0}, 60).has_value(), "instrument-chain note failed");
  const auto effect = take(session->effects().insert_vst3_plugin(
      track, 0,
      {.class_id = effect_class.id,
       .vendor = effect_class.vendor,
       .name = effect_class.name,
       .version = effect_class.version,
       .canonical_bundle_path = effect_scan.binary.canonical_bundle.string(),
       .binary_sha256 = effect_scan.binary.sha256}));
  session.reset();
  session = take(app::ProjectSession::open(root));
  require(session->snapshot().tracks.front().effects.size() == 1 &&
              effect_on(session->snapshot().tracks.front()).id == effect,
          "instrument-chain effect did not reopen in its saved slot");
  for (const auto frames : {64U, 128U}) {
    OutputEvidence evidence{.caller = std::this_thread::get_id()};
    app::PlaybackOutputFactory factory = [&evidence](audio::StereoRenderSource source,
                                                     audio::TransportCursor& cursor) {
      return std::make_unique<FakeOutput>(source, cursor, evidence, false);
    };
    auto run = quick::RoutedPlayback::prepare_and_start(root, session->snapshot(),
                                                        take(session->reserve_playback_start()),
                                                        factory, std::nullopt, frames);
    if (!run)
      throw std::runtime_error(run.error().message);
    require(evidence.first_channel[0] == 0.0F &&
                std::fabs(evidence.first_channel[7] - 2.0F * 0.5F * 100.0F / 127.0F) < 0.00001F &&
                !run.value()->processor_fault(),
            "instrument-owned effect was not applied after the MIDI source");
    require(run.value()->stop_and_retire() ==
                quick::RoutedPlayback::StopDrainOutcome::not_requested,
            "instrument-chain playback did not stop cleanly");
  }
#endif
}

void instrument_owned_vst3_effects_preserve_order_and_play_a_third() {
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  Directory directory;
  const auto root = directory.path() / "instrument-two-effects";
  auto session = take(app::ProjectSession::create(root));
  plugins::Vst3Scanner scanner(OSCILLATE_VST3_HELPER_PATH);
  const auto source_scan = scanner.scan(VST3_INSTRUMENT_BUNDLE);
  const auto effect_scan = scanner.scan(VST3_GAIN_BUNDLE);
  require(source_scan.ok() && effect_scan.ok(), "two-stage instrument scans failed");
  const auto& source_class = source_scan.metadata.classes.front();
  const auto& effect_class = effect_scan.metadata.classes.front();
  const auto track = take(session->instruments().create_vst3_instrument_track(
      {.class_id = source_class.id,
       .vendor = source_class.vendor,
       .name = source_class.name,
       .version = source_class.version,
       .canonical_bundle_path = source_scan.binary.canonical_bundle.string(),
       .binary_sha256 = source_scan.binary.sha256}));
  require(session->midi_clips().add_midi_clip(track, {0}).has_value(),
          "two-stage MIDI clip failed");
  const auto clip = session->snapshot().tracks.front().midi_clips.front().id;
  require(session->notes().add_note(clip, {0}, 60).has_value(), "two-stage MIDI note failed");
  const app::Vst3InsertIdentity effect_selection{.class_id = effect_class.id,
                                                 .vendor = effect_class.vendor,
                                                 .name = effect_class.name,
                                                 .version = effect_class.version,
                                                 .canonical_bundle_path =
                                                     effect_scan.binary.canonical_bundle.string(),
                                                 .binary_sha256 = effect_scan.binary.sha256};
  const auto first = take(session->effects().insert_vst3_plugin(track, 0, effect_selection));
  const auto second = take(session->effects().insert_vst3_plugin(track, 1, effect_selection));
  require(first != second && session->saving().save().has_value(), "two-stage effect save failed");
  session.reset();
  session = take(app::ProjectSession::open(root));
  const auto saved = session->snapshot();
  require(saved.tracks.front().effects.size() == 2 &&
              std::get<project::Vst3PluginInstance>(saved.tracks.front().effects[0].processor).id ==
                  first &&
              std::get<project::Vst3PluginInstance>(saved.tracks.front().effects[1].processor).id ==
                  second,
          "two-stage instrument chain lost its saved order or identities");
  for (const auto frames : {64U, 128U}) {
    OutputEvidence evidence{.caller = std::this_thread::get_id()};
    app::PlaybackOutputFactory factory = [&evidence](audio::StereoRenderSource source,
                                                     audio::TransportCursor& cursor) {
      return std::make_unique<FakeOutput>(source, cursor, evidence, false);
    };
    auto run = quick::RoutedPlayback::prepare_and_start(
        root, saved, take(session->reserve_playback_start()), factory, std::nullopt, frames);
    if (!run)
      throw std::runtime_error(run.error().message);
    require(evidence.first_channel[13] == 0.0F &&
                std::fabs(evidence.first_channel[14] - 4.0F * 0.5F * 100.0F / 127.0F) < 0.00001F &&
                run.value()->status().active_graph_revision == saved.revision &&
                !run.value()->processor_fault(),
            "two instrument-owned effects were not serial and audible");
    require(run.value()->stop_and_retire() ==
                quick::RoutedPlayback::StopDrainOutcome::not_requested,
            "two-stage instrument playback did not release its MIDI source");
  }
  auto excess = saved;
  auto third = excess.tracks.front().effects.back();
  third.id = take(project::InsertSlotId::from_bits({0, 777}));
  std::get<project::Vst3PluginInstance>(third.processor).id =
      take(project::PluginInstanceId::from_bits({0, 778}));
  excess.tracks.front().effects.push_back(std::move(third));
  OutputEvidence excess_evidence{.caller = std::this_thread::get_id()};
  app::PlaybackOutputFactory excess_factory = [&excess_evidence](audio::StereoRenderSource source,
                                                                 audio::TransportCursor& cursor) {
    return std::make_unique<FakeOutput>(source, cursor, excess_evidence, false);
  };
  auto three = quick::RoutedPlayback::prepare_and_start(root, std::move(excess),
                                                        take(session->reserve_playback_start()),
                                                        excess_factory, std::nullopt, 64);
  if (!three)
    throw std::runtime_error("third instrument-owned effect: " + three.error().message);
  require(excess_evidence.first_channel[20] == 0.0F &&
              std::fabs(excess_evidence.first_channel[21] - 8.0F * 0.5F * 100.0F / 127.0F) <
                  0.00001F &&
              !three.value()->processor_fault(),
          "third instrument-owned effect was not serial and audible");
  require(three.value()->stop_and_retire() ==
              quick::RoutedPlayback::StopDrainOutcome::not_requested,
          "three-stage instrument playback did not release its MIDI source");
  auto released = session->reserve_playback_start();
  require(released.has_value(), "three-stage instrument playback retained transport");
  released.value().finish_after_join();
#endif
}

void bus_vst3_effect_processes_routed_audio() {
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  for (const auto kind : {project::TrackKind::group_bus, project::TrackKind::return_bus}) {
    Directory directory;
    const auto root = directory.path() / "bus-effect-song";
    const auto wav = make_wav(directory.path());
    auto session = take(app::ProjectSession::create(root));
    const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
    require(fd >= 0, "bus-effect source open failed");
    const auto audio_track = take(session->imports().import_wav_audio(fd));
    (void)::close(fd);
    const auto bus = take(session->tracks().add_bus_track(kind));
    if (kind == project::TrackKind::group_bus)
      require(session->routing().set_main_output(audio_track, bus).has_value(),
              "bus-effect main route failed");
    else
      require(session->routing().add_send(audio_track, bus, 0.5).has_value(),
              "bus-effect send route failed");
    plugins::Vst3Scanner scanner(OSCILLATE_VST3_HELPER_PATH);
    const auto scan = scanner.scan(VST3_GAIN_BUNDLE);
    require(scan.ok(), "bus-effect fixture scan failed");
    const auto& klass = scan.metadata.classes.front();
    const auto effect = take(session->effects().insert_vst3_plugin(
        bus, 0,
        {.class_id = klass.id,
         .vendor = klass.vendor,
         .name = klass.name,
         .version = klass.version,
         .canonical_bundle_path = scan.binary.canonical_bundle.string(),
         .binary_sha256 = scan.binary.sha256}));
    session.reset();
    session = take(app::ProjectSession::open(root));
    require(session->snapshot().tracks[1].id == bus &&
                effect_on(session->snapshot().tracks[1]).id == effect,
            "bus effect identity or chain position did not reopen");
    for (const auto frames : {64U, 128U}) {
      OutputEvidence evidence{.caller = std::this_thread::get_id()};
      app::PlaybackOutputFactory factory = [&evidence](audio::StereoRenderSource source,
                                                       audio::TransportCursor& cursor) {
        return std::make_unique<FakeOutput>(source, cursor, evidence, false);
      };
      auto run = quick::RoutedPlayback::prepare_and_start(root, session->snapshot(),
                                                          take(session->reserve_playback_start()),
                                                          factory, std::nullopt, frames);
      if (!run)
        throw std::runtime_error(run.error().message);
      require(evidence.first_channel[0] == 0.0F &&
                  std::fabs(evidence.first_channel[7] - 0.5F) < 0.00001F &&
                  !run.value()->processor_fault(),
              "VST3 bus effect did not process its routed audio");
      require(run.value()->stop_and_retire() ==
                  quick::RoutedPlayback::StopDrainOutcome::not_requested,
              "VST3 bus effect did not stop cleanly");
    }
  }
#endif
}

void instrument_and_bus_vst3_effect_play_in_one_graph() {
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  Directory directory;
  const auto root = directory.path() / "instrument-bus-effect";
  auto session = take(app::ProjectSession::create(root));
  plugins::Vst3Scanner scanner(OSCILLATE_VST3_HELPER_PATH);
  const auto source_scan = scanner.scan(VST3_INSTRUMENT_BUNDLE);
  const auto effect_scan = scanner.scan(VST3_GAIN_BUNDLE);
  require(source_scan.ok() && effect_scan.ok(), "instrument/bus live scans failed");
  const auto& source_class = source_scan.metadata.classes.front();
  const auto& effect_class = effect_scan.metadata.classes.front();
  const auto instrument = take(session->instruments().create_vst3_instrument_track(
      {.class_id = source_class.id,
       .vendor = source_class.vendor,
       .name = source_class.name,
       .version = source_class.version,
       .canonical_bundle_path = source_scan.binary.canonical_bundle.string(),
       .binary_sha256 = source_scan.binary.sha256}));
  require(session->midi_clips().add_midi_clip(instrument, {0}).has_value(),
          "instrument/bus MIDI clip failed");
  const auto clip = session->snapshot().tracks.front().midi_clips.front().id;
  require(session->notes().add_note(clip, {0}, 60).has_value(), "instrument/bus note failed");
  const auto bus = take(session->tracks().add_bus_track(project::TrackKind::return_bus));
  require(session->routing().add_send(instrument, bus, 0.5).has_value(),
          "instrument/bus send failed");
  const auto effect = take(session->effects().insert_vst3_plugin(
      bus, 0,
      {.class_id = effect_class.id,
       .vendor = effect_class.vendor,
       .name = effect_class.name,
       .version = effect_class.version,
       .canonical_bundle_path = effect_scan.binary.canonical_bundle.string(),
       .binary_sha256 = effect_scan.binary.sha256}));
  session.reset();
  session = take(app::ProjectSession::open(root));
  require(session->snapshot().tracks[1].id == bus &&
              effect_on(session->snapshot().tracks[1]).id == effect,
          "instrument/bus effect did not reopen on its bus path");
  for (const auto frames : {64U, 128U}) {
    OutputEvidence evidence{.caller = std::this_thread::get_id()};
    app::PlaybackOutputFactory factory = [&evidence](audio::StereoRenderSource source,
                                                     audio::TransportCursor& cursor) {
      return std::make_unique<FakeOutput>(source, cursor, evidence, false);
    };
    auto run = quick::RoutedPlayback::prepare_and_start(root, session->snapshot(),
                                                        take(session->reserve_playback_start()),
                                                        factory, std::nullopt, frames);
    if (!run)
      throw std::runtime_error(run.error().message);
    require(evidence.first_channel[0] == 0.0F &&
                std::fabs(evidence.first_channel[7] - 2.0F * 0.5F * 100.0F / 127.0F) < 0.00001F &&
                !run.value()->processor_fault(),
            "instrument/bus graph omitted its verified send effect or delay compensation");
    require(run.value()->stop_and_retire() ==
                quick::RoutedPlayback::StopDrainOutcome::not_requested,
            "instrument/bus graph did not stop cleanly");
  }
#endif
}

void saved_instrument_and_separate_native_chain_play_exact_saved_order() {
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  for (const bool instrument_first : {false, true}) {
    Directory directory;
    const auto root = directory.path() / "instrument-native-song";
    auto session = take(app::ProjectSession::create(root));
    plugins::Vst3Scanner scanner(OSCILLATE_VST3_HELPER_PATH);
    const auto scan = scanner.scan(VST3_INSTRUMENT_BUNDLE);
    require(scan.ok() && scan.metadata.classes.size() == 1,
            "instrument/native fixture scan failed");
    const auto& klass = scan.metadata.classes.front();
    const app::Vst3InsertIdentity identity{.class_id = klass.id,
                                           .vendor = klass.vendor,
                                           .name = klass.name,
                                           .version = klass.version,
                                           .canonical_bundle_path =
                                               scan.binary.canonical_bundle.string(),
                                           .binary_sha256 = scan.binary.sha256};
    const auto wav = make_wav(directory.path());
    const auto add_audio = [&] {
      const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
      require(fd >= 0, "instrument/native WAV open failed");
      auto result = session->imports().import_wav_audio(fd);
      (void)::close(fd);
      return take(std::move(result));
    };
    std::optional<project::TrackId> audio_track;
    std::optional<project::TrackId> instrument_track;
    if (instrument_first) {
      instrument_track = take(session->instruments().create_vst3_instrument_track(identity));
      audio_track = add_audio();
    } else {
      audio_track = add_audio();
      instrument_track = take(session->instruments().create_vst3_instrument_track(identity));
    }
    require(session->midi_clips().add_midi_clip(*instrument_track, {0}).has_value(),
            "instrument/native MIDI clip creation failed");
    const auto& tracks = session->snapshot().tracks;
    const auto instrument = std::ranges::find(tracks, *instrument_track, &project::Track::id);
    require(instrument != tracks.end() && !instrument->midi_clips.empty(),
            "instrument/native MIDI track was not saved");
    require(session->notes().add_note(instrument->midi_clips.front().id, {0}, 60).has_value(),
            "instrument/native note creation failed");
    const Band band{shape_kind::high_pass, 1'000.0, 0.707, 0.0};
    const auto eq_slot =
        take(session->effects().add_native_effect(*audio_track, 0, shape_band(band)));
    Compression settings;
    settings.threshold_db = -30.0;
    settings.ratio = 20.0;
    settings.knee_db = 0.0;
    settings.attack_ms = 0.1;
    settings.release_ms = 1.0;
    const auto compressor_slot =
        take(session->effects().add_native_effect(*audio_track, 1, press(settings)));
    require(session->saving().save().has_value(), "instrument/native chain save failed");
    session.reset();
    session = take(app::ProjectSession::open(root));
    const auto audio =
        std::ranges::find(session->snapshot().tracks, *audio_track, &project::Track::id);
    require(audio != session->snapshot().tracks.end() && audio->effects.size() == 2 &&
                audio->effects[0].id == eq_slot && audio->effects[1].id == compressor_slot,
            "instrument/native chain changed order or slot IDs on reopen");
    const std::array eq_settings{oscillate::native_dsp::EqBandSettings{
        oscillate::native_dsp::EqBandKind::high_pass, band.frequency_hz, band.shape, band.gain_db}};
    const auto prepared_eq = take(oscillate::native_dsp::prepare_eq(eq_settings, 48'000));
    const auto prepared_compressor = take(oscillate::native_dsp::prepare_compressor(
        {settings.threshold_db, settings.ratio, settings.knee_db, settings.attack_ms,
         settings.release_ms, settings.makeup_db},
        48'000));
    const auto verify = [&](bool compressor_first) {
      for (const auto frames : {64U, 128U}) {
        oscillate::native_dsp::EqRuntime eq(prepared_eq);
        oscillate::native_dsp::CompressorRuntime compressor(prepared_compressor);
        std::array<engine::StereoFrame, 128> expected{};
        expected.fill({0.25F, 0.25F});
        if (compressor_first) {
          compressor.process(std::span(expected).first(frames), {0, 1});
          eq.process(std::span(expected).first(frames), {0, 1});
        } else {
          eq.process(std::span(expected).first(frames), {0, 1});
          compressor.process(std::span(expected).first(frames), {0, 1});
        }
        require(std::fabs(expected.front().left - 0.25F) > 0.001F,
                "native fixture did not differ measurably from dry audio");
        OutputEvidence evidence{.caller = std::this_thread::get_id()};
        app::PlaybackOutputFactory factory = [&evidence](audio::StereoRenderSource source,
                                                         audio::TransportCursor& cursor) {
          return std::make_unique<FakeOutput>(source, cursor, evidence, false);
        };
        auto run = quick::RoutedPlayback::prepare_and_start(root, session->snapshot(),
                                                            take(session->reserve_playback_start()),
                                                            factory, std::nullopt, frames);
        if (!run)
          throw std::runtime_error(run.error().message);
        constexpr auto instrument_level = 0.5F * 100.0F / 127.0F;
        for (std::size_t frame = 0; frame < frames; ++frame)
          require(std::fabs(evidence.first_channel[frame] -
                            (expected[frame].left + instrument_level)) < 0.00001F,
                  "Quick instrument/native route omitted a stage, changed order or read audio "
                  "as instrument input");
        require(run.value()->status().active_graph_revision == session->snapshot().revision &&
                    !run.value()->processor_fault() &&
                    run.value()->stop_and_retire() ==
                        quick::RoutedPlayback::StopDrainOutcome::not_requested,
                "Quick instrument/native graph or instrument release failed");
      }
    };
    verify(false);
    auto dry_snapshot = session->snapshot();
    const auto dry_audio =
        std::ranges::find(dry_snapshot.tracks, *audio_track, &project::Track::id);
    require(dry_audio != dry_snapshot.tracks.end(), "dry comparison lost its audio path");
    for (auto& effect : dry_audio->effects)
      effect.enabled = false;
    for (const auto frames : {64U, 128U}) {
      OutputEvidence evidence{.caller = std::this_thread::get_id()};
      app::PlaybackOutputFactory factory = [&evidence](audio::StereoRenderSource source,
                                                       audio::TransportCursor& cursor) {
        return std::make_unique<FakeOutput>(source, cursor, evidence, false);
      };
      auto run = quick::RoutedPlayback::prepare_and_start(root, dry_snapshot,
                                                          take(session->reserve_playback_start()),
                                                          factory, std::nullopt, frames);
      if (!run)
        throw std::runtime_error(run.error().message);
      constexpr auto dry_level = 0.25F + 0.5F * 100.0F / 127.0F;
      require(std::fabs(evidence.first_channel[0] - dry_level) < 0.00001F &&
                  std::fabs(evidence.first_channel[frames - 1] - dry_level) < 0.00001F,
              "bypassed native path was not exactly dry beside the MIDI instrument");
      run.value()->stop_and_retire();
    }
    require(session->effects().move_effect_insert(*audio_track, compressor_slot, 0).has_value() &&
                session->saving().save().has_value(),
            "instrument/native reorder save failed");
    session.reset();
    session = take(app::ProjectSession::open(root));
    verify(true);
    require(session->effects().remove_effect_insert(*audio_track, eq_slot).has_value() &&
                session->effects().remove_effect_insert(*audio_track, compressor_slot).has_value(),
            "instrument-owned fixture could not clear its separate native path");
    const auto own_eq =
        take(session->effects().add_native_effect(*instrument_track, 0, shape_band(band)));
    const auto own_compressor =
        take(session->effects().add_native_effect(*instrument_track, 1, press(settings)));
    require(session->saving().save().has_value(), "instrument-owned native chain save failed");
    session.reset();
    session = take(app::ProjectSession::open(root));
    const auto owner =
        std::ranges::find(session->snapshot().tracks, *instrument_track, &project::Track::id);
    require(owner != session->snapshot().tracks.end() && owner->effects.size() == 2 &&
                owner->effects[0].id == own_eq && owner->effects[1].id == own_compressor,
            "instrument-owned native chain lost saved order or slot identity");
    for (const auto frames : {64U, 128U}) {
      oscillate::native_dsp::EqRuntime own_eq_runtime(prepared_eq);
      oscillate::native_dsp::CompressorRuntime own_compressor_runtime(prepared_compressor);
      std::array<engine::StereoFrame, 128> expected{};
      constexpr auto instrument_level = 0.5F * 100.0F / 127.0F;
      expected.fill({instrument_level, instrument_level});
      own_eq_runtime.process(std::span(expected).first(frames), {0, 1});
      own_compressor_runtime.process(std::span(expected).first(frames), {0, 1});
      OutputEvidence evidence{.caller = std::this_thread::get_id()};
      app::PlaybackOutputFactory factory = [&evidence](audio::StereoRenderSource source,
                                                       audio::TransportCursor& cursor) {
        return std::make_unique<FakeOutput>(source, cursor, evidence, false);
      };
      auto run = quick::RoutedPlayback::prepare_and_start(root, session->snapshot(),
                                                          take(session->reserve_playback_start()),
                                                          factory, std::nullopt, frames);
      if (!run)
        throw std::runtime_error(run.error().message);
      for (std::size_t frame = 0; frame < frames; ++frame)
        require(std::fabs(evidence.first_channel[frame] - (0.25F + expected[frame].left)) <
                    0.00001F,
                "Quick instrument-owned chain omitted or reordered a native stage");
      require(!run.value()->processor_fault() &&
                  run.value()->stop_and_retire() ==
                      quick::RoutedPlayback::StopDrainOutcome::not_requested,
              "Quick instrument-owned native chain did not stop cleanly");
    }
  }
#endif
}

void native_synth_switch_rings_out_and_promotes_without_stopping() {
  Directory directory;
  const auto root = directory.path() / "live-native-switch";
  auto session = take(app::ProjectSession::create(root));
  const auto track = take(session->instruments().add_synth_track());
  require(session->midi_clips().add_midi_clip(track, {0}).has_value(),
          "synth MIDI clip setup failed");
  const auto clip = session->snapshot().tracks.front().midi_clips.front().id;
  require(session->notes().add_note(clip, {0}, 60).has_value() &&
              session->saving().save().has_value(),
          "synth held note setup failed");
  auto evidence = std::make_shared<SteppedEvidence>();
  app::PlaybackOutputFactory factory = [evidence](audio::StereoRenderSource source,
                                                  audio::TransportCursor& cursor) {
    return std::make_unique<SteppedOutput>(source, cursor, evidence);
  };
  auto run = take(quick::RoutedPlayback::prepare_and_start(root, session->snapshot(),
                                                           take(session->reserve_playback_start()),
                                                           factory, std::nullopt, 64));
  evidence->permit(1);
  evidence->await_blocks(1);
  const auto old_revision = run->status().active_graph_revision;
  require(session->instruments().select_native_instrument(track, "ember").has_value(),
          "saved Ember selection failed");
  const auto new_revision = session->snapshot().revision;
  require(run->submit_revision(session->snapshot()).has_value() && run->edit_pending(),
          "native switch did not start source preparation");
  require(run->status().application == app::PlaybackApplication::pending &&
              run->status().active_graph_revision == old_revision,
          "switch was reported audible while the first note was held");

  std::size_t blocks = 1;
  bool activated = false;
  for (std::size_t attempt = 0; attempt < 400; ++attempt) {
    evidence->permit(8);
    blocks += 8;
    evidence->await_blocks(blocks);
    const auto state = run->status();
    if (state.application == app::PlaybackApplication::failed)
      throw std::runtime_error("native source switch failed: " +
                               (state.failure ? state.failure->message : "unknown"));
    if (state.active_graph_revision == new_revision && !run->edit_pending()) {
      activated = true;
      break;
    }
    if (attempt == 0) {
      std::lock_guard lock(evidence->mutex);
      require(std::ranges::any_of(evidence->samples, [](float sample) { return sample != 0.0F; }),
              "the held old synth note was silenced while its replacement prepared");
    }
  }
  require(activated && run->output_status().state == audio::OutputState::streaming &&
              run->completed_sample() >= std::uint64_t{64} * 8,
          "Ember did not become active at a quiet boundary without stopping transport");
  auto values = take(session->instruments().native_instrument_values(track));
  const auto* ember = oscillate::native_dsp::synth::find_engine("ember");
  require(ember != nullptr, "Ember engine was not registered");
  const auto cutoff =
      oscillate::native_dsp::find_parameter(*ember->descriptor, "ember.filter.cutoff");
  require(cutoff.has_value(), "Ember cutoff is absent");
  values[*cutoff] = 400.0;
  require(session->instruments().set_native_instrument_values(track, values).has_value() &&
              run->submit_revision(session->snapshot()).has_value(),
          "replacement synth did not accept a live panel edit");
  evidence->permit(1);
  evidence->await_blocks(++blocks);
  require(run->status().active_graph_revision == session->snapshot().revision,
          "replacement synth runtime was not rebound for live editing");
  // Inside a loop the switch is live as well (ADR-0098).
  require(run->set_loop(0, 128) &&
              session->instruments().select_native_instrument(track, "prism").has_value(),
          "looped engine switch fixture did not prepare");
  const auto looped_revision = session->snapshot().revision;
  require(run->submit_revision(session->snapshot()).has_value(),
          "looped native switch was refused");
  bool looped = false;
  for (std::size_t attempt = 0; attempt < 400 && !looped; ++attempt) {
    evidence->permit(8);
    blocks += 8;
    evidence->await_blocks(blocks);
    const auto state = run->status();
    require(state.application != app::PlaybackApplication::failed, "looped native switch failed");
    looped = state.active_graph_revision == looped_revision && !run->edit_pending();
  }
  require(looped && run->output_status().state == audio::OutputState::streaming,
          "looped native switch did not become active without stopping transport");
  run->clear_loop();
  (void)run->stop_and_retire();
}

void native_synth_accepts_a_live_native_insert() {
  for (const auto frames : {64U, 128U}) {
    Directory directory;
    const auto root = directory.path() / "live-synth-native-insert";
    auto session = take(app::ProjectSession::create(root));
    const auto track = take(session->instruments().add_synth_track());
    require(session->midi_clips().add_midi_clip(track, {0}).has_value(), "synth clip setup failed");
    const auto clip = session->snapshot().tracks.front().midi_clips.front().id;
    require(session->notes().add_note(clip, {0}, 60).has_value(), "synth note setup failed");
    auto evidence = std::make_shared<SteppedEvidence>();
    app::PlaybackOutputFactory factory = [evidence](audio::StereoRenderSource source,
                                                    audio::TransportCursor& cursor) {
      return std::make_unique<SteppedOutput>(source, cursor, evidence);
    };
    auto run = take(quick::RoutedPlayback::prepare_and_start(
        root, session->snapshot(), take(session->reserve_playback_start()), factory, std::nullopt,
        frames));
    evidence->permit(1);
    evidence->await_blocks(1);
    const auto before = run->status().active_graph_revision;
    require(before == session->snapshot().revision, "synth graph did not start");
    const Band band{shape_kind::low_pass, 2'000.0, 0.707, 0.0};
    require(session->effects().add_native_effect(track, 0, shape_band(band)).has_value(),
            "native synth effect edit failed");
    const auto after = session->snapshot().revision;
    require(run->submit_revision(session->snapshot()).has_value(),
            "native synth effect was refused during playback");
    auto blocks = std::size_t{1};
    auto state = run->status();
    for (std::size_t attempt = 0;
         attempt < 16 && state.application == app::PlaybackApplication::pending; ++attempt) {
      evidence->permit(1);
      evidence->await_blocks(++blocks);
      state = run->status();
    }
    if (state.application != app::PlaybackApplication::active ||
        state.active_graph_revision != after ||
        run->output_status().state != audio::OutputState::streaming)
      throw std::runtime_error("native synth effect did not become audible: " +
                               (state.failure ? state.failure->message : "no reported failure"));
    evidence->permit(1);
    evidence->await_blocks(++blocks);
    {
      std::lock_guard lock(evidence->mutex);
      require(evidence->samples.size() >= std::size_t{frames} * 2 &&
                  std::ranges::any_of(std::span(evidence->samples).last(std::size_t{frames} * 2),
                                      [](float value) { return value != 0.0F; }),
              "native synth effect silenced the active source");
    }
    (void)run->stop_and_retire();
    require(session->saving().save().has_value(), "native synth effect save failed");
    session.reset();
    session = take(app::ProjectSession::open(root));
    require(session->snapshot().tracks.front().effects.size() == 1,
            "native synth effect did not survive reopen");
  }
}

void run_native_graph() {
  routed_controller_reserves_stopped_export_and_retires_in_order();
  routed_pause_and_seek_keep_runtime_and_exact_completed_sample();
  pause_preserves_master_eq_history();
  live_routed_level_edit_becomes_audible_without_restarting_output();
  live_send_gain_edit_reaches_running_quick_output(64);
  live_send_gain_edit_reaches_running_quick_output(128);
  live_clip_move_overlay_keeps_source_timing(64);
  live_clip_move_overlay_keeps_source_timing(128);
  live_clip_move_overlay_keeps_source_timing(64, true);
  live_clip_move_overlay_keeps_source_timing(128, true);
  live_clip_overlay_keeps_master_eq_history(64);
  live_clip_overlay_keeps_master_eq_history(128);
  live_clip_trim_overlay_keeps_playing(64);
  live_clip_trim_overlay_keeps_playing(128);
  live_clip_split_overlay_keeps_playing(64);
  live_clip_split_overlay_keeps_playing(128);
  live_clip_split_overlay_keeps_playing(64, true);
  live_clip_split_overlay_keeps_playing(128, true);
  multi_track_clip_overlay_preserves_native_processing(64);
  multi_track_clip_overlay_preserves_native_processing(128);
  multi_track_clip_overlay_preserves_native_processing(64, true);
  multi_track_clip_overlay_preserves_native_processing(128, true);
  failed_live_clip_preparation_keeps_old_audio_audible();
  live_clip_preparation_survives_pause_and_seek();
  pause_waits_for_live_source_switch();
  audio_edit_beside_a_synth_keeps_its_voice();
  structural_edit_never_holds_the_transport();
  unsupported_shape_and_device_failure_release_reservation();
}

void run_native_processors() {
  live_native_eq_revision_keeps_the_output_stream();
  live_native_compressor_revision_changes_running_output();
  live_native_reverb_revision_changes_running_output();
  limiter_stop_renders_exact_lookahead_before_transport_release();
  routed_track_limiter_stop_drains_before_releasing_transport();
  saved_native_eq_is_audible_from_quick();
}

void run_vst3_chains() {
  saved_vst3_is_not_silently_omitted();
  missing_bypassed_vst3_between_native_effects_is_dry();
  qualified_manual_vst3_edits_become_live();
  two_saved_effects_on_distinct_audio_tracks();
  three_saved_effects_on_one_audio_track();
  bus_vst3_effect_processes_routed_audio();
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  dry_graph_accepts_backend_block_larger_than_request();
  live_zero_latency_vst3_insert_uses_worker_and_keeps_source(64);
  live_zero_latency_vst3_insert_uses_worker_and_keeps_source(128);
  live_zero_latency_vst3_insert_uses_worker_and_keeps_source(64, 256);
  live_zero_latency_vst3_insert_uses_worker_and_keeps_source(64, 0, true);
  live_zero_latency_vst3_insert_uses_worker_and_keeps_source(128, 0, true);
  live_zero_latency_vst3_insert_uses_worker_and_keeps_source(64, 256, true);
  live_zero_latency_vst3_insert_uses_worker_and_keeps_source(64, 0, true, true);
  live_zero_latency_vst3_insert_uses_worker_and_keeps_source(128, 0, true, true);
  live_latent_vst3_insert_switches_without_holding();
#endif
}

void run_instrument_sources() {
  saved_vst3_instrument_plays_midi_and_fails_closed();
  two_bare_vst3_instruments_keep_distinct_midi_and_stop_together();
  two_vst3_instrument_effect_paths_render_independently();
  native_synth_and_vst3_instrument_sum_in_quick_playback();
  instrument_owned_vst3_effect_plays_after_its_source();
  instrument_owned_vst3_effects_preserve_order_and_play_a_third();
  instrument_and_bus_vst3_effect_play_in_one_graph();
  saved_instrument_and_separate_native_chain_play_exact_saved_order();
  native_synth_switch_rings_out_and_promotes_without_stopping();
  native_synth_accepts_a_live_native_insert();
}
} // namespace

int main(int argc, char** argv) {
  try {
    if (argc == 1) {
      run_native_graph();
      run_native_processors();
      run_vst3_chains();
      run_instrument_sources();
    } else if (argc == 2 && std::string_view(argv[1]) == "--native-graph") {
      run_native_graph();
    } else if (argc == 2 && std::string_view(argv[1]) == "--native-processors") {
      run_native_processors();
    } else if (argc == 2 && std::string_view(argv[1]) == "--vst3-chains") {
      run_vst3_chains();
    } else if (argc == 2 && std::string_view(argv[1]) == "--instrument-sources") {
      run_instrument_sources();
    } else if (argc == 2 && std::string_view(argv[1]) == "--two-vst3-instruments") {
      two_bare_vst3_instruments_keep_distinct_midi_and_stop_together();
    } else {
      std::fprintf(stderr, "Unknown routed playback scenario group\n");
      return 2;
    }
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
  return 0;
}
