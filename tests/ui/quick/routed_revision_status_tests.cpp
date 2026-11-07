#include "../../../apps/daw_quick/src/loop_transport.hpp"
#include "../../../apps/daw_quick/src/project_bridge.hpp"
#include "../../../apps/daw_quick/src/recording_controller.hpp"

#include "oscillate/audio_io/stereo_block.hpp"
#include "oscillate/test/check.hpp"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QSettings>
#include <QTest>
#include <QUrl>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
namespace fs = std::filesystem;
namespace app = oscillate::application;
namespace audio = oscillate::audio_io;
namespace engine = oscillate::engine;
namespace foundation = oscillate::foundation;
namespace project = oscillate::project;
namespace quick = oscillate::quick;

using oscillate::test::require;

void transport_seek_policy_checks() {
  quick::TransportController controller;
  using Action = quick::TransportController::SeekAction;
  quick::TransportController::SeekState state{.project_open = true};
  require(controller.seek_action(512, state) == Action::park_playhead,
          "stopped transport should park the playhead");
  state.direct_playing = true;
  require(controller.seek_action(512, state) == Action::direct_unavailable,
          "direct playback should report unavailable seek");
  state.routed_playback = true;
  require(controller.seek_action(512, state) == Action::seek_routed,
          "routed playback should own live seek");
  state.transport_busy = true;
  require(controller.seek_action(512, state) == Action::wait_for_transport,
          "an in-flight transport change must keep its seek gate");
  require(controller.seek_action(std::numeric_limits<std::uint64_t>::max(), state) ==
              Action::ignore,
          "invalid sample must not enter the transport gate");
  require(!controller.sample_at_bar(nullptr, 1.0) &&
              !controller.sample_at_bar(nullptr, std::numeric_limits<double>::quiet_NaN()),
          "missing project or invalid bar must not produce a seek");
}

void transport_projection_and_command_checks() {
  quick::TransportController controller;
  quick::TransportController::PlaybackView view;
  view.project_open = true;
  view.model_revision = 7;
  view.audible_revision = 7;
  view.active = true;
  view.routed_present = true;
  view.routed_output_streaming = true;
  view.graph.emplace();
  view.graph->active_graph_revision = 7;
  view.graph->application = app::PlaybackApplication::active;
  require(controller.routed_graph_current(view) && !controller.playback_revision_inactive(view) &&
              controller.playback_state(view).value(QStringLiteral("phase")) ==
                  QStringLiteral("playing"),
          "current routed graph projection does not identify audible playback");
  view.model_revision = 8;
  view.audible_revision = 8;
  require(!controller.routed_graph_current(view) && controller.playback_revision_inactive(view),
          "saved revision was represented as the active graph");
  view.routed_paused = true;
  require(controller.playback_paused(view) && controller.playback_state(view).value(QStringLiteral(
                                                  "phase")) == QStringLiteral("paused"),
          "paused routed graph projection changed precedence");
  view.routed_stopping = true;
  require(!controller.playback_paused(view) && controller.playback_state(view).value(QStringLiteral(
                                                   "phase")) == QStringLiteral("stopping"),
          "stop phase did not take precedence over pause");
  view.routed_stopping = false;
  view.routed_paused = false;
  view.plugin_fault = foundation::Error{foundation::ErrorCode::internal, "processor failure"};
  require(controller.playback_state(view).value(QStringLiteral("phase")) ==
                  QStringLiteral("fault") &&
              controller.playback_state(view).value(QStringLiteral("failure")) ==
                  QStringLiteral("processor failure"),
          "plugin fault was not visible in typed transport state");

  using Pause = quick::TransportController::PauseAction;
  quick::TransportController::PauseState pause{.routed_present = true};
  require(controller.pause_action(nullptr, pause) == Pause::pause_routed,
          "playing routed transport did not plan a pause");
  pause.routed_paused = true;
  require(controller.pause_action(nullptr, pause) == Pause::resume_routed,
          "paused routed transport did not plan a resume");
  pause.deferred = true;
  require(controller.pause_action(nullptr, pause) == Pause::cancel_deferred,
          "deferred seek cancellation lost transport priority");
  pause.transport_pending = true;
  require(controller.pause_action(nullptr, pause) == Pause::none,
          "in-flight transport operation accepted another pause");

  using Stop = quick::TransportController::StopAction;
  quick::TransportController::StopState stop{.routed_present = true};
  require(controller.stop_action(stop) == Stop::stop_routed_gracefully,
          "playing routed transport did not plan graceful stop");
  stop.routed_paused = true;
  require(controller.stop_action(stop) == Stop::stop_routed_immediately,
          "paused routed transport incorrectly planned tail drain");
  stop.transport_pending = true;
  require(controller.stop_action(stop) == Stop::request_transport_stop,
          "in-flight transport stop lost priority");
}

void transport_poll_status_preserves_revision_and_error_priority() {
  quick::TransportController controller;
  app::RevisionStatus graph;
  graph.model_revision = 8;
  graph.active_graph_revision = 7;
  graph.application = app::PlaybackApplication::pending;
  audio::OutputStatus output;
  output.state = audio::OutputState::streaming;
  using OutputAction = quick::TransportController::OutputPollAction;
  require(controller.routed_output_action(output, 48'000, 128) == OutputAction::observe &&
              controller.direct_output_action(output) == OutputAction::observe,
          "healthy output incorrectly triggered recovery");
  output.negotiated_client_rate = 44'100;
  require(controller.routed_output_action(output, 48'000, 128) == OutputAction::stop_invalid_format,
          "routed poll ignored a mismatched client rate");
  output.negotiated_client_rate = 48'000;
  output.observed_block_frames = 256;
  require(controller.routed_output_action(output, 48'000, 128) == OutputAction::stop_invalid_format,
          "routed poll ignored an oversized device block");
  output.observed_block_frames = 128;
  output.state = audio::OutputState::disconnected;
  require(controller.routed_output_action(output, 48'000, 128) == OutputAction::recover_device &&
              controller.direct_output_action(output) == OutputAction::recover_device,
          "lost device did not take precedence over format checks");
  output.state = audio::OutputState::streaming;
  QString source_detail;
  quick::TransportController::RoutedPollView routed{.graph = graph,
                                                    .output = output,
                                                    .model_revision = 8,
                                                    .audible_revision = 8,
                                                    .completed_sample = 4096,
                                                    .source_missing_frames = 0,
                                                    .edit_pending = true,
                                                    .limiter_active = false,
                                                    .source_underrun_detail = source_detail};
  require(controller.routed_running_status(routed).contains(
              QStringLiteral("Edit revision 8 not active; playing routed revision 7")) &&
              controller.routed_running_status(routed).contains(
                  QStringLiteral("Pending at an audio block boundary")),
          "poll status represented a pending saved revision as audible");
  routed.edit_pending = false;
  graph.application = app::PlaybackApplication::failed;
  graph.failure = foundation::Error{foundation::ErrorCode::unsupported, "cannot build route"};
  require(controller.routed_running_status(routed).contains(QStringLiteral("cannot build route")),
          "failed revision lost its graph-build reason");

  graph.active_graph_revision = 8;
  graph.application = app::PlaybackApplication::active;
  graph.failure.reset();
  output.render_failures = 1;
  require(
      controller.routed_running_status(routed).startsWith(QStringLiteral("Routed output errors")),
      "output failure was hidden by the playing status");
  output.render_failures = 0;
  require(controller.routed_running_status(routed).contains(
              QStringLiteral("Playing routed revision 8, sample 4096")),
          "healthy routed poll did not report the audible revision and sample");

  QString edit_failure;
  quick::TransportController::DirectPollView direct{.graph = graph,
                                                    .output = output,
                                                    .completed_sample = 4096,
                                                    .source_underruns = 0,
                                                    .audio_edit_failure = edit_failure,
                                                    .source_underrun_detail = source_detail};
  output.state = audio::OutputState::stopped;
  require(!controller.direct_running_status(direct),
          "inactive direct output overwrote the preceding transport status");
}

class Directory {
public:
  Directory() {
    std::string pattern = (fs::temp_directory_path() / "oscillate-quick-revision-XXXXXX").string();
    require(::mkdtemp(pattern.data()) != nullptr, "temporary directory failed");
    root_ = pattern;
  }
  ~Directory() {
    std::error_code ignored;
    fs::remove_all(root_, ignored);
  }
  [[nodiscard]] const fs::path& root() const { return root_; }

private:
  fs::path root_;
};

void put16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value));
  bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void put32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
  put16(bytes, static_cast<std::uint16_t>(value));
  put16(bytes, static_cast<std::uint16_t>(value >> 16U));
}

fs::path source_wav(const fs::path& root) {
  constexpr std::uint32_t frames = 4096;
  std::vector<std::uint8_t> wav{'R', 'I', 'F', 'F'};
  put32(wav, 36 + frames * 2);
  wav.insert(wav.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  put32(wav, 16);
  put16(wav, 1);
  put16(wav, 1);
  put32(wav, 48'000);
  put32(wav, 96'000);
  put16(wav, 2);
  put16(wav, 16);
  wav.insert(wav.end(), {'d', 'a', 't', 'a'});
  put32(wav, frames * 2);
  for (std::uint32_t index = 0; index < frames; ++index)
    put16(wav, 8192);
  const auto path = root / "source.wav";
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char*>(wav.data()), static_cast<std::streamsize>(wav.size()));
  require(output.good(), "fixture WAV write failed");
  return path;
}

class FakeOutput;

struct OutputProbe {
  std::mutex mutex;
  std::vector<float> first_samples;
  std::atomic<std::uint32_t> starts{0};
  std::atomic<FakeOutput*> active{nullptr};
  // Set, stop() waits for release_stop, then renders one last block, as a
  // device callback can before its stop joins it.
  std::atomic<bool> hold_stop{false};
  std::atomic<bool> stop_held{false};
  std::atomic<bool> release_stop{false};
  std::atomic<bool> stop_block_rendered{false};
  std::atomic<float> stop_block_last_sample{0.0F};
};

class FakeOutput final : public app::PlaybackOutput {
public:
  FakeOutput(audio::StereoRenderSource source, audio::TransportCursor& cursor,
             std::shared_ptr<OutputProbe> probe)
      : source_(source), cursor_(cursor), probe_(std::move(probe)) {}

  // PlaybackOutput fixes the adjacent rate and requested-frame argument order.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  [[nodiscard]] foundation::Result<void> start(std::uint32_t rate, std::uint32_t requested_frames,
                                               std::optional<std::uint64_t>) override {
    if (!app::valid_playback_buffer_frames(requested_frames))
      return foundation::Result<void>::failure(
          {foundation::ErrorCode::invalid_argument, "fake output needs a playback buffer size"});
    std::array<engine::StereoFrame, app::max_playback_buffer_frames> frames{};
    std::array<float, 2 * app::max_playback_buffer_frames> samples{};
    if (audio::render_transport_block(
            source_, cursor_, true, std::span(frames).first(requested_frames),
            std::span(samples).first(static_cast<std::size_t>(requested_frames) * 2)) !=
        audio::BlockResult::rendered) {
      return foundation::Result<void>::failure(
          {foundation::ErrorCode::unavailable, "fake output could not render the routed graph"});
    }
    {
      std::lock_guard lock(probe_->mutex);
      probe_->first_samples.push_back(samples[0]);
    }
    probe_->starts.fetch_add(1, std::memory_order_release);
    rate_ = rate;
    block_frames_ = requested_frames;
    state_.store(audio::OutputState::streaming, std::memory_order_release);
    probe_->active.store(this, std::memory_order_release);
    return foundation::Result<void>::success();
  }

  void stop() noexcept override {
    if (probe_->hold_stop.load(std::memory_order_acquire)) {
      probe_->stop_held.store(true, std::memory_order_release);
      while (!probe_->release_stop.load(std::memory_order_acquire))
        std::this_thread::yield();
      const auto last = pump_last_sample();
      probe_->stop_block_last_sample.store(last.value_or(0.0F), std::memory_order_release);
      probe_->stop_block_rendered.store(last.has_value(), std::memory_order_release);
    }
    probe_->active.store(nullptr, std::memory_order_release);
    state_.store(audio::OutputState::stopped, std::memory_order_release);
  }

  [[nodiscard]] bool pump() noexcept {
    std::array<engine::StereoFrame, app::max_playback_buffer_frames> frames{};
    std::array<float, 2 * app::max_playback_buffer_frames> samples{};
    const auto result = audio::render_transport_block(
        source_, cursor_, true, std::span(frames).first(block_frames_),
        std::span(samples).first(static_cast<std::size_t>(block_frames_) * 2));
    return result == audio::BlockResult::rendered;
  }

  // The last left sample of one rendered block, if it rendered.
  [[nodiscard]] std::optional<float> pump_last_sample() noexcept {
    std::array<engine::StereoFrame, app::max_playback_buffer_frames> frames{};
    std::array<float, 2 * app::max_playback_buffer_frames> samples{};
    const auto interleaved = static_cast<std::size_t>(block_frames_) * 2;
    if (audio::render_transport_block(
            source_, cursor_, true, std::span(frames).first(block_frames_),
            std::span(samples).first(interleaved)) != audio::BlockResult::rendered)
      return std::nullopt;
    return samples[interleaved - 2];
  }

  [[nodiscard]] audio::OutputStatus status() const noexcept override {
    audio::OutputStatus status;
    status.state = state_.load(std::memory_order_acquire);
    status.requested_client_rate = rate_;
    return status;
  }

private:
  audio::StereoRenderSource source_;
  audio::TransportCursor& cursor_;
  std::shared_ptr<OutputProbe> probe_;
  std::atomic<audio::OutputState> state_{audio::OutputState::stopped};
  std::uint32_t rate_ = 0;
  std::uint32_t block_frames_ = 0;
};

float first_sample(OutputProbe& probe, std::size_t index) {
  std::lock_guard lock(probe.mutex);
  require(probe.first_samples.size() > index, "fake output did not render an expected start");
  return probe.first_samples[index];
}

void routed_revision_status_checks() {
  Directory directory;
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                     QString::fromStdString((directory.root() / "settings").string()));
  const auto root = directory.root() / "song";
  auto session = app::ProjectSession::create(root);
  require(session.has_value(), "project create failed");
  const auto source = source_wav(directory.root());
  const int fd = ::open(source.c_str(), O_RDONLY | O_CLOEXEC);
  require(fd >= 0, "source WAV open failed");
  auto imported = session.value()->imports().import_wav_audio(fd);
  (void)::close(fd);
  require(imported.has_value(), "source import failed");
  auto returns = session.value()->tracks().add_bus_track(project::TrackKind::return_bus);
  require(returns.has_value() && session.value()->saving().save().has_value(),
          "routed project fixture did not save");
  const auto audio_track = QString::fromStdString(imported.value().to_string());
  const auto clip_id = QString::fromStdString(
      session.value()->snapshot().tracks.front().clips.front().id.to_string());
  const auto return_track = QString::fromStdString(returns.value().to_string());
  session.value().reset();
  auto reopened = app::ProjectSession::open(root);
  require(reopened.has_value(), "routed project did not reopen");

  auto probe = std::make_shared<OutputProbe>();
  app::PlaybackOutputFactory factory = [probe](audio::StereoRenderSource source,
                                               audio::TransportCursor& cursor) {
    return std::make_unique<FakeOutput>(source, cursor, probe);
  };
  {
    quick::ProjectBridge bridge(directory.root(), nullptr, std::move(factory), [] {
      return foundation::Result<std::vector<audio::PipeWireSink>>::success({});
    });
    bridge.set_project(std::move(reopened.value()), root);
    const auto first_revision = bridge.revision();
    require(bridge.playback_status_is_default() &&
                bridge.playback_state().value(QStringLiteral("phase")) == QStringLiteral("stopped"),
            "fresh project did not expose the default stopped state");
    bridge.toggle_playback();
    require(QTest::qWaitFor(
                [&bridge] {
                  return bridge.playback_status().startsWith(
                      QStringLiteral("Playing routed revision"));
                },
                10'000) &&
                std::fabs(first_sample(*probe, 0) - 0.25F) < 0.00001F,
            "first routed revision was not audible on fake output");
    require(bridge.routed_graph_current() && !bridge.playback_revision_inactive(),
            "typed graph status did not identify the audible revision");
    const auto active_state = bridge.playback_state();
    require(
        active_state.value(QStringLiteral("phase")) == QStringLiteral("playing") &&
            active_state.value(QStringLiteral("application")) == QStringLiteral("active") &&
            active_state.value(QStringLiteral("modelRevision")).toULongLong() == first_revision &&
            active_state.value(QStringLiteral("activeRevision")).toULongLong() == first_revision &&
            !active_state.value(QStringLiteral("pluginFault")).toBool(),
        "machine-readable playback state did not match the audible graph");

    bridge.add_send_route(audio_track, return_track, 0.5);
    const auto saved_revision = bridge.revision();
    require(saved_revision == first_revision + 1 &&
                bridge.playback_status().contains(
                    QStringLiteral("Edit revision %1 not active; playing routed revision %2")
                        .arg(saved_revision)
                        .arg(first_revision)) &&
                std::fabs(first_sample(*probe, 0) - 0.25F) < 0.00001F,
            "route edit was represented as active before the graph changed");
    require(!bridge.routed_graph_current() && bridge.playback_revision_inactive(),
            "typed graph status represented the saved route as audible");
    const auto pending_state = bridge.playback_state();
    require(pending_state.value(QStringLiteral("modelRevision")).toULongLong() == saved_revision &&
                pending_state.value(QStringLiteral("activeRevision")).toULongLong() ==
                    first_revision,
            "machine-readable state hid the saved versus audible revision gap");
    bridge.save();
    require(bridge.status().contains(QStringLiteral("Saved")) &&
                bridge.status().contains(QString::number(saved_revision)),
            "inactive route edit did not save the model revision");
    bridge.exports()->export_master();
    require(!bridge.exports()->export_busy() &&
                bridge.exports()->export_status().contains(QStringLiteral("Stop playback")),
            "stopped-snapshot export was allowed while the old route was audible");

    bridge.toggle_playback();
    require(QTest::qWaitFor([&bridge] { return !bridge.playback_active(); }, 10'000),
            "routed output did not stop for graph re-prime");
    bridge.toggle_playback();
    require(QTest::qWaitFor(
                [&bridge, saved_revision] {
                  return bridge.playback_status().startsWith(
                             QStringLiteral("Playing routed revision %1").arg(saved_revision)) &&
                         bridge.playback_active();
                },
                10'000) &&
                std::fabs(first_sample(*probe, 1) - 0.375F) < 0.00001F,
            "saved send route did not become audible only after Stop and Play");
    require(bridge.routed_graph_current() && !bridge.playback_revision_inactive(),
            "typed graph status did not recover after graph re-prime");
    bridge.nudge_audio_clip(clip_id, 1);
    require(bridge.revision() == saved_revision + 1 &&
                bridge.playback_status().contains(QStringLiteral("not active")) &&
                bridge.playback_revision_inactive() && !bridge.routed_graph_current(),
            "clip move during routed playback was falsely reported as audible");
    bridge.toggle_playback();
    require(QTest::qWaitFor([&bridge] { return !bridge.playback_active(); }, 10'000),
            "second routed output did not stop");
  }

  auto persisted = app::ProjectSession::open(root);
  require(persisted.has_value() && persisted.value()->snapshot().tracks.front().sends.size() == 1,
          "saved route edit was lost after playback and reopen");
}

// A mixer gesture during Stop: the stop worker owns the run until it joins,
// so the previews must not reach it. The fake output holds its stop and
// renders one last block, which would carry a preview that got through.
void stop_during_mixer_preview_leaves_the_stopping_run_alone() {
  constexpr float fixture_level = 0.25F;
  constexpr float sample_tolerance = 0.00001F;
  constexpr double silent_level = 0.0;
  constexpr double centre_pan = 0.0;
  constexpr int wait_ms = 10'000;
  Directory directory;
  const auto root = directory.root() / "preview-stop-song";
  auto session = app::ProjectSession::create(root);
  require(session.has_value(), "preview-stop project create failed");
  const auto source = source_wav(directory.root());
  const int fd = ::open(source.c_str(), O_RDONLY | O_CLOEXEC);
  require(fd >= 0, "preview-stop WAV open failed");
  const auto imported = session.value()->imports().import_wav_audio(fd);
  (void)::close(fd);
  require(imported && session.value()->saving().save(), "preview-stop WAV import/save failed");
  const auto track = QString::fromStdString(imported.value().to_string());
  auto probe = std::make_shared<OutputProbe>();
  app::PlaybackOutputFactory factory = [probe](audio::StereoRenderSource render,
                                               audio::TransportCursor& cursor) {
    return std::make_unique<FakeOutput>(render, cursor, probe);
  };
  quick::ProjectBridge bridge(directory.root(), nullptr, std::move(factory), [] {
    return foundation::Result<std::vector<audio::PipeWireSink>>::success({});
  });
  bridge.set_project(std::move(session.value()), root);
  // Play/Pause starts the routed path whatever the project holds.
  bridge.toggle_pause();
  require(
      QTest::qWaitFor([&probe] { return probe->active.load(std::memory_order_acquire) != nullptr; },
                      wait_ms),
      "preview-stop fixture did not start routed playback");
  require(QTest::qWaitFor(
              [&probe, fixture_level, sample_tolerance] {
                auto* output = probe->active.load(std::memory_order_acquire);
                const auto last = output ? output->pump_last_sample() : std::nullopt;
                return last && std::fabs(*last - fixture_level) < sample_tolerance;
              },
              wait_ms),
          "preview-stop fixture did not stream its clip");
  require(QTest::qWaitFor([&bridge] { return bridge.routed_graph_current(); }, wait_ms),
          "preview-stop fixture did not install the routed run");
  // Declared after the bridge, so a failed check releases the held stop
  // before the bridge joins its worker.
  struct ReleaseStop {
    explicit ReleaseStop(OutputProbe& held) : probe(held) {}
    OutputProbe& probe;
    ReleaseStop(const ReleaseStop&) = delete;
    ReleaseStop& operator=(const ReleaseStop&) = delete;
    ~ReleaseStop() { probe.release_stop.store(true, std::memory_order_release); }
  } release_on_exit{*probe};
  probe->hold_stop.store(true, std::memory_order_release);
  bridge.stop_playback();
  require(QTest::qWaitFor([&probe] { return probe->stop_held.load(std::memory_order_acquire); },
                          wait_ms),
          "the stop worker did not reach the output");
  bridge.preview_master_level(silent_level);
  bridge.preview_track_level_pan(track, silent_level, centre_pan);
  probe->release_stop.store(true, std::memory_order_release);
  require(QTest::qWaitFor([&bridge] { return !bridge.playback_active(); }, wait_ms),
          "routed playback did not stop after the preview");
  require(probe->stop_block_rendered.load(std::memory_order_acquire),
          "the held stop did not render its last block");
  require(std::fabs(probe->stop_block_last_sample.load(std::memory_order_acquire) - fixture_level) <
              sample_tolerance,
          "a mixer preview reached the run while it was stopping");
}

// A project opens at bar 1, after any bars before it, so Play does not
// start with their silence.
void project_opens_with_the_playhead_at_bar_one() {
  Directory directory;
  const auto root = directory.root() / "lead-song";
  auto session = app::ProjectSession::create(root);
  require(session.has_value() && session.value()->song().insert_lead_bars(2).has_value(),
          "lead bar project create failed");
  quick::ProjectBridge bridge(directory.root(), nullptr, {}, [] {
    return foundation::Result<std::vector<audio::PipeWireSink>>::success({});
  });
  bridge.set_project(std::move(session.value()), root);
  // Two 4/4 bars at the default 120 BPM are four seconds.
  require(bridge.playhead_sample() == 4 * 48'000, "the playhead did not open at bar 1");
}

void transport_pause_seek_bridge_checks() {
  Directory directory;
  const auto root = directory.root() / "transport-song";
  auto session = app::ProjectSession::create(root);
  require(session.has_value(), "transport project create failed");
  const auto source = source_wav(directory.root());
  const int fd = ::open(source.c_str(), O_RDONLY | O_CLOEXEC);
  require(fd >= 0, "transport WAV open failed");
  const auto imported = session.value()->imports().import_wav_audio(fd);
  (void)::close(fd);
  require(imported && session.value()->saving().save(), "transport WAV import/save failed");
  auto probe = std::make_shared<OutputProbe>();
  app::PlaybackOutputFactory factory = [probe](audio::StereoRenderSource source,
                                               audio::TransportCursor& cursor) {
    return std::make_unique<FakeOutput>(source, cursor, probe);
  };
  quick::ProjectBridge bridge(directory.root(), nullptr, std::move(factory), [] {
    return foundation::Result<std::vector<audio::PipeWireSink>>::success({});
  });
  bridge.set_project(std::move(session.value()), root);
  bridge.seek_to_bar(0.01);
  require(bridge.playhead_sample() >= 900 && bridge.playhead_sample() <= 1000 &&
              std::fabs(bridge.playhead_bar() - 0.01) < 0.0001,
          "bar seek did not use the shared meter and tempo maps");
  const auto parked_sample = bridge.playhead_sample();
  bridge.seek_to_bar(std::numeric_limits<double>::quiet_NaN());
  bridge.seek_to_bar(static_cast<double>(std::numeric_limits<std::int64_t>::max()));
  require(bridge.playhead_sample() == parked_sample,
          "invalid bar seek changed the parked playhead");
  bridge.toggle_pause();
  require(QTest::qWaitFor(
              [&bridge, &probe] {
                return (bridge.playback_status().startsWith(QStringLiteral("Priming routed")) ||
                        bridge.playback_status().startsWith(QStringLiteral("Playing routed"))) &&
                       probe->starts.load(std::memory_order_acquire) == 1 &&
                       bridge.playhead_sample() >= 900;
              },
              10'000),
          "Play from a later bar did not start routed audio");
  bridge.toggle_pause();
  require(QTest::qWaitFor(
              [&bridge] {
                return bridge.playback_paused() &&
                       bridge.playback_status().startsWith(QStringLiteral("Paused"));
              },
              10'000),
          "Play/Pause did not enter a real paused state");
  const auto held = bridge.playhead_sample();
  require(held >= 900 && probe->starts.load(std::memory_order_acquire) == 1,
          "Pause did not retain the exact completed playhead");
  bridge.toggle_pause();
  require(QTest::qWaitFor(
              [&bridge, &probe] {
                return !bridge.playback_paused() &&
                       bridge.playback_status().startsWith(QStringLiteral("Playing")) &&
                       probe->starts.load(std::memory_order_acquire) == 2;
              },
              10'000) &&
              bridge.playhead_sample() >= held,
          "Resume recreated or rewound the routed transport");
  bridge.seek_to_sample(2048);
  require(QTest::qWaitFor(
              [&bridge, &probe] {
                return bridge.playhead_sample() >= 2048 &&
                       bridge.playback_status().startsWith(QStringLiteral("Playing")) &&
                       probe->starts.load(std::memory_order_acquire) == 3;
              },
              10'000),
          "live seek did not resume at the requested sample");
  bridge.toggle_pause();
  require(QTest::qWaitFor(
              [&bridge] {
                return bridge.playback_paused() &&
                       bridge.playback_status().startsWith(QStringLiteral("Paused"));
              },
              10'000),
          "second Pause did not complete");
  bridge.seek_to_sample(3072);
  require(QTest::qWaitFor(
              [&bridge] { return bridge.playback_paused() && bridge.playhead_sample() == 3072; },
              10'000),
          "paused seek moved neither the playhead nor source generation");
  // Stop goes back to bar 1 wherever the playhead was put; so does Stop
  // while stopped.
  bridge.stop_playback();
  require(QTest::qWaitFor([&bridge] { return !bridge.playback_active(); }, 10'000) &&
              bridge.playhead_sample() == 0,
          "explicit Stop did not release the graph and go back to bar 1");
  bridge.seek_to_sample(3072);
  bridge.stop_playback();
  require(bridge.playhead_sample() == 0, "Stop while stopped did not go back to bar 1");
  // With a loop on, Stop goes back to the loop's start instead; off again, bar 1.
  auto* loop = qobject_cast<quick::LoopTransport*>(bridge.loop_transport());
  require(loop != nullptr, "the bridge has no loop transport");
  constexpr int loop_start_bar = 3;
  constexpr int loop_end_bar = 5;
  // Two 4/4 bars at the default 120 BPM are four seconds.
  constexpr std::uint64_t loop_start_sample = 4 * 48'000;
  loop->set_range(loop_start_bar, loop_end_bar);
  loop->set_enabled(true);
  bridge.seek_to_sample(3072);
  bridge.stop_playback();
  require(bridge.playhead_sample() == loop_start_sample, "Stop did not go back to the loop start");
  loop->set_enabled(false);
  bridge.stop_playback();
  require(bridge.playhead_sample() == 0, "Stop with the loop off did not go back to bar 1");
}

// Record from stop (ADR-0086): a bar of count-in holds the song still, only
// clicks playing; then the song starts from where it was and the take is
// live. Stop ends it.
void record_counts_in_then_plays() {
  Directory directory;
  const auto root = directory.root() / "record-song";
  auto session = app::ProjectSession::create(root);
  require(session.has_value(), "record project create failed");
  const auto source = source_wav(directory.root());
  const int fd = ::open(source.c_str(), O_RDONLY | O_CLOEXEC);
  require(fd >= 0, "record WAV open failed");
  const auto imported = session.value()->imports().import_wav_audio(fd);
  (void)::close(fd);
  require(imported && session.value()->saving().save(), "record WAV import/save failed");
  auto probe = std::make_shared<OutputProbe>();
  app::PlaybackOutputFactory factory = [probe](audio::StereoRenderSource source,
                                               audio::TransportCursor& cursor) {
    return std::make_unique<FakeOutput>(source, cursor, probe);
  };
  quick::ProjectBridge bridge(directory.root(), nullptr, std::move(factory), [] {
    return foundation::Result<std::vector<audio::PipeWireSink>>::success({});
  });
  bridge.set_project(std::move(session.value()), root);
  auto* recording = qobject_cast<quick::RecordingController*>(bridge.recording());
  require(recording != nullptr, "the bridge has a recorder");
  recording->set_count_in(true);
  const auto start = bridge.playhead_sample();
  recording->toggle_record();
  require(QTest::qWaitFor(
              [&] {
                return probe->starts.load(std::memory_order_acquire) == 1 &&
                       probe->active.load(std::memory_order_acquire) != nullptr;
              },
              10'000),
          "Record from stop starts the song");
  require(recording->armed() && recording->counting_in() && !recording->recording(),
          "first, a count-in");
  // The default 120 BPM 4/4 bar is 96000 frames: pump blocks through it.
  std::size_t blocks = 0;
  while (recording->counting_in()) {
    auto* output = probe->active.load(std::memory_order_acquire);
    require(output != nullptr && output->pump(), "a count-in block renders");
    require(++blocks < 4'000, "the count-in ends");
  }
  QTest::qWait(100);
  require(bridge.playhead_sample() <= start + 4'096, "the song held still through the count-in");
  for (int block = 0; block < 8; ++block)
    require(probe->active.load(std::memory_order_acquire)->pump(), "the song renders");
  require(QTest::qWaitFor([&] { return bridge.playhead_sample() > start; }, 10'000),
          "after the count-in the song plays");
  require(QTest::qWaitFor([&] { return recording->recording(); }, 10'000), "and the take is live");
  bridge.stop_playback();
  require(!recording->armed(), "Stop ends the take");
  require(QTest::qWaitFor([&bridge] { return !bridge.playback_active(); }, 10'000),
          "playback stops");
}

void close_persists_clip_and_master_effect_edits() {
  Directory directory;
  const auto root = directory.root() / "close-song";
  auto session = app::ProjectSession::create(root);
  require(session.has_value(), "close project create failed");
  const auto source = source_wav(directory.root());
  const int fd = ::open(source.c_str(), O_RDONLY | O_CLOEXEC);
  require(fd >= 0, "close WAV open failed");
  const auto imported = session.value()->imports().import_wav_audio(fd);
  (void)::close(fd);
  require(imported && session.value()->saving().save(), "close WAV import/save failed");
  const auto clip_id = QString::fromStdString(
      session.value()->snapshot().tracks.front().clips.front().id.to_string());

  {
    quick::ProjectBridge bridge(directory.root(), nullptr, {}, [] {
      return foundation::Result<std::vector<audio::PipeWireSink>>::success({});
    });
    bridge.set_project(std::move(session.value()), root);
    require(QTest::qWaitFor([&bridge] { return bridge.insert_chain_edit_enabled(); }, 10'000),
            "insert editor did not become ready");
    bridge.add_effect_engine(QStringLiteral("master"), 0, QStringLiteral("ceiling"));
    const auto master = bridge.insert_chains().back().toMap();
    const auto effects = master.value(QStringLiteral("effects")).toList();
    require(effects.size() == 1, "master limiter was not added");
    const auto slot_id = effects.front().toMap().value(QStringLiteral("slotId")).toString();
    require(!slot_id.isEmpty(), "master limiter slot ID was missing");
    const auto before_move = bridge.revision();
    bridge.nudge_audio_clip(clip_id, 1);
    require(bridge.revision() > before_move, "clip nudge did not change the model revision");
    bridge.remove_effect_insert(QStringLiteral("master"), slot_id);
    require(
        bridge.insert_chains().back().toMap().value(QStringLiteral("effects")).toList().isEmpty(),
        "master limiter was not deleted in the UI model");
    require(bridge.request_close(), "normal close did not save the dirty project");
  }

  const auto reopened = app::ProjectSession::open(root);
  require(reopened && reopened.value()->snapshot().master_effects.empty(),
          "deleted master limiter returned after app close");
  const auto& placement = reopened.value()->snapshot().tracks.front().clips.front().placement;
  const bool moved = std::holds_alternative<project::MusicalInterval>(placement)
                         ? std::get<project::MusicalInterval>(placement).start.value > 0
                         : std::get<project::ElapsedInterval>(placement).start.value > 0;
  require(moved, "moved clip returned to its original position after app close");
}

void reopening_current_project_disk_checks() {
  Directory directory;
  const auto root = directory.root() / "reopen-song";
  auto session = app::ProjectSession::create(root);
  require(session.has_value(), "reopen project create failed");
  const auto source = source_wav(directory.root());
  const int fd = ::open(source.c_str(), O_RDONLY | O_CLOEXEC);
  require(fd >= 0, "reopen WAV open failed");
  const auto imported = session.value()->imports().import_wav_audio(fd);
  (void)::close(fd);
  require(imported && session.value()->saving().save(), "reopen WAV import/save failed");
  const auto clip_id = QString::fromStdString(
      session.value()->snapshot().tracks.front().clips.front().id.to_string());
  {
    quick::ProjectBridge bridge(directory.root(), nullptr, {}, [] {
      return foundation::Result<std::vector<audio::PipeWireSink>>::success({});
    });
    bridge.set_project(std::move(session.value()), root);
    const auto before_move = bridge.revision();
    bridge.nudge_audio_clip(clip_id, 1);
    require(bridge.revision() > before_move, "reopen fixture clip nudge did not change revision");
    bridge.open_project(QUrl::fromLocalFile(QString::fromStdString(root.string())));
    require(bridge.revision() > 1, "reopening current project loaded its stale disk revision");
  }
  const auto reopened = app::ProjectSession::open(root);
  require(reopened.has_value(), "reopened project could not be loaded");
  const auto& placement = reopened.value()->snapshot().tracks.front().clips.front().placement;
  const bool moved = std::holds_alternative<project::MusicalInterval>(placement)
                         ? std::get<project::MusicalInterval>(placement).start.value > 0
                         : std::get<project::ElapsedInterval>(placement).start.value > 0;
  require(moved, "reopening current project discarded its moved clip");
}

void failed_close_save_keeps_project_open_for_retry() {
  Directory directory;
  const auto root = directory.root() / "fault-song";
  auto initial = app::ProjectSession::create(root);
  require(initial.has_value(), "fault project create failed");
  initial.value().reset();
  bool fail_save = true;
  auto session =
      app::ProjectSession::open(root, [&fail_save](oscillate::persistence::SaveStage stage) {
        return fail_save && stage == oscillate::persistence::SaveStage::manifest_written;
      });
  require(session && session.value()->tracks().add_bus_track(project::TrackKind::group_bus),
          "fault fixture edit failed");
  quick::ProjectBridge bridge(directory.root(), nullptr, {}, [] {
    return foundation::Result<std::vector<audio::PipeWireSink>>::success({});
  });
  bridge.set_project(std::move(session.value()), root);
  const auto pending_revision = bridge.revision();
  require(!bridge.request_close() && bridge.revision() == pending_revision &&
              bridge.status().contains(QStringLiteral("could not be saved")),
          "failed close save did not preserve the open edit and report the failure");
  fail_save = false;
  require(bridge.request_close(), "close retry did not save the pending edit");
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication application(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("OscillateTest"));
  QCoreApplication::setApplicationName(QStringLiteral("QuickRoutedRevisionStatus"));
  try {
    transport_seek_policy_checks();
    transport_projection_and_command_checks();
    transport_poll_status_preserves_revision_and_error_priority();
    routed_revision_status_checks();
    project_opens_with_the_playhead_at_bar_one();
    transport_pause_seek_bridge_checks();
    stop_during_mixer_preview_leaves_the_stopping_run_alone();
    record_counts_in_then_plays();
    close_persists_clip_and_master_effect_edits();
    reopening_current_project_disk_checks();
    failed_close_save_keeps_project_open_for_retry();
  } catch (const std::exception& error) {
    std::fprintf(stderr, "Quick routed revision status: %s\n", error.what());
    return 1;
  }
  return 0;
}
