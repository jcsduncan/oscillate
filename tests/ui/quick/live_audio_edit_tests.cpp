#include "project_bridge.hpp"

#include "oscillate/application/project_session.hpp"
#include "oscillate/audio_io/render_ahead.hpp"
#include "oscillate/audio_io/stereo_block.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/wav.hpp"

#include <QGuiApplication>
#include <QTemporaryDir>
#include <QTest>
#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// A clip moved while the direct (unrouted) path plays: only the media
// preparation runs on a worker. The handoff that replaces the playing run, and
// the hook that may edit the session, run on the UI thread, which reads both
// between polls; Stop requested before the handoff wins and never installs it.
namespace {
namespace app = oscillate::application;
namespace audio = oscillate::audio_io;
namespace engine = oscillate::engine;
namespace foundation = oscillate::foundation;
namespace quick = oscillate::quick;
namespace test = oscillate::test;

using test::require;

// How long a UI wait may take before it counts as a hang (not a timing claim).
constexpr int ui_wait_ms = 10'000;
// Two seconds of a steady stereo tone: still playing when the edit lands.
constexpr std::size_t source_seconds = 2;
constexpr std::size_t source_frames = source_seconds * test::sample_rate_48k;
constexpr std::int16_t source_level = 6'000;
// Where the clip moves, as a fraction of the visible arrangement.
constexpr double edited_start = 1.0 / 16.0;
constexpr double start_tolerance = 1e-5;
constexpr std::size_t block_frames = 128;
constexpr auto block_period = std::chrono::milliseconds(2);

// Static initialisation runs on the thread that later runs main().
const std::thread::id ui_thread = std::this_thread::get_id();

// A device rendering blocks on its own thread until stopped. It counts the
// stops made off the UI thread: each one replaced the run the UI was reading.
class ThreadedOutput final : public app::PlaybackOutput {
public:
  ThreadedOutput(audio::StereoRenderSource source, audio::TransportCursor& cursor)
      : source_(source), cursor_(cursor) {}
  ~ThreadedOutput() override { stop(); }

  foundation::Result<void> start(std::uint32_t, std::uint32_t,
                                 std::optional<std::uint64_t>) override {
    running_.store(true, std::memory_order_release);
    state_.store(audio::OutputState::streaming, std::memory_order_release);
    thread_ = std::thread([this] {
      std::array<engine::StereoFrame, block_frames> frames{};
      std::array<float, block_frames * test::stereo_channels> samples{};
      while (running_.load(std::memory_order_acquire)) {
        (void)audio::render_device_block(source_, cursor_, true, frames, samples);
        std::this_thread::sleep_for(block_period);
      }
    });
    return foundation::Result<void>::success();
  }
  void stop() noexcept override {
    if (std::this_thread::get_id() != ui_thread)
      off_ui_stops.fetch_add(1, std::memory_order_relaxed);
    running_.store(false, std::memory_order_release);
    if (thread_.joinable())
      thread_.join();
    state_.store(audio::OutputState::stopped, std::memory_order_release);
  }
  [[nodiscard]] audio::OutputStatus status() const noexcept override {
    audio::OutputStatus result;
    result.state = state_.load(std::memory_order_acquire);
    return result;
  }
  static inline std::atomic<int> off_ui_stops{0};

private:
  audio::StereoRenderSource source_;
  audio::TransportCursor& cursor_;
  std::atomic<bool> running_{false};
  std::atomic<audio::OutputState> state_{audio::OutputState::stopped};
  std::thread thread_;
};

struct Fixture {
  QTemporaryDir directory;
  std::filesystem::path root;
  std::unique_ptr<app::ProjectSession> session;
};

// A saved project with one audio track holding the imported tone.
Fixture make_fixture() {
  Fixture fixture;
  require(fixture.directory.isValid(), "fixture directory unavailable");
  const auto folder = std::filesystem::path(fixture.directory.path().toStdString());
  qputenv("XDG_CONFIG_HOME", (fixture.directory.path() + QStringLiteral("/config")).toUtf8());
  fixture.root = folder / "song";
  auto created = app::ProjectSession::create(fixture.root);
  require(created.has_value(), "project creation failed");
  fixture.session = std::move(created).value();
  const auto source = folder / "tone.wav";
  const std::vector<std::int16_t> samples(source_frames * test::stereo_channels, source_level);
  test::write_wav_pcm16(source, test::sample_rate_48k, test::stereo_channels, samples);
  const int descriptor = ::open(source.c_str(), O_RDONLY | O_CLOEXEC);
  require(descriptor >= 0, "WAV fixture open failed");
  const auto imported = fixture.session->imports().import_wav_audio(descriptor);
  (void)::close(descriptor);
  require(imported.has_value(), "audio import failed");
  require(fixture.session->saving().save().has_value(), "project save failed");
  return fixture;
}

std::unique_ptr<quick::ProjectBridge> make_bridge(Fixture& fixture,
                                                  quick::ProjectBridge::Dependencies dependencies) {
  auto bridge = std::make_unique<quick::ProjectBridge>(
      fixture.directory.path().toStdString(), nullptr,
      [](audio::StereoRenderSource source, audio::TransportCursor& cursor) {
        return std::make_unique<ThreadedOutput>(source, cursor);
      },
      [] { return foundation::Result<std::vector<audio::PipeWireSink>>::success({}); },
      app::PlaybackOutputFactory{}, std::function<bool()>{}, std::move(dependencies));
  bridge->set_project(std::move(fixture.session), fixture.root);
  return bridge;
}

QVariantMap only_clip(const quick::ProjectBridge& bridge) {
  const auto clips = bridge.tracks().front().toMap().value(QStringLiteral("clips")).toList();
  require(clips.size() == 1, "the fixture holds one audio clip");
  return clips.front().toMap();
}

// Starts direct playback and waits for its graph to stream, then moves the
// clip: the edit is accepted as pending while the old audio plays on.
void play_and_move(quick::ProjectBridge& bridge) {
  bridge.toggle_playback();
  require(QTest::qWaitFor(
              [&] {
                return bridge.playback_status().contains(QStringLiteral("Playing revision")) &&
                       bridge.audio_edit_enabled();
              },
              ui_wait_ms),
          "direct playback did not start: " + bridge.playback_status().toStdString());
  const auto before = bridge.revision();
  bridge.move_audio_clip(only_clip(bridge).value(QStringLiteral("id")).toString(), edited_start,
                         true);
  require(bridge.revision() == before + 1 &&
              bridge.playback_status().contains(QStringLiteral("pending")),
          "the move was not accepted as a pending live edit: " +
              bridge.playback_status().toStdString());
}

void wait_stopped(quick::ProjectBridge& bridge) {
  require(QTest::qWaitFor([&] { return !bridge.playback_active(); }, ui_wait_ms),
          "playback did not stop: " + bridge.playback_status().toStdString());
}

void handoff_runs_on_the_ui_thread() {
  auto fixture = make_fixture();
  std::thread::id hook_thread;
  quick::ProjectBridge::Dependencies dependencies;
  dependencies.before_live_edit_handoff = [&hook_thread](app::ProjectSession&,
                                                         app::PreparedInstrumentPlayback&) {
    hook_thread = std::this_thread::get_id();
  };
  const auto bridge = make_bridge(fixture, std::move(dependencies));
  ThreadedOutput::off_ui_stops.store(0);
  play_and_move(*bridge);
  require(QTest::qWaitFor(
              [&] {
                const auto status = bridge->playback_status();
                return status.contains(QStringLiteral("Playing revision")) &&
                       status.contains(QString::number(bridge->revision()));
              },
              ui_wait_ms),
          "the live edit never became audible: " + bridge->playback_status().toStdString());
  require(hook_thread == ui_thread, "the handoff hook ran off the UI thread");
  require(ThreadedOutput::off_ui_stops.load() == 0,
          "the handoff stopped the playing output off the UI thread");
  // The workers the edit replaced retire while playback carries on.
  require(QTest::qWaitFor([&] { return bridge->audio_edit_enabled(); }, ui_wait_ms),
          "a second live edit stayed disabled after the first: " +
              bridge->playback_status().toStdString());
  bridge->toggle_playback();
  wait_stopped(*bridge);
  require(ThreadedOutput::off_ui_stops.load() == 0, "Stop ran off the UI thread");
}

void stop_wins_over_a_prepared_edit() {
  auto fixture = make_fixture();
  bool handed_off = false;
  quick::ProjectBridge::Dependencies dependencies;
  // Inline: the edit is prepared when the move returns; only a poll installs it.
  dependencies.audio_edit_runner = [](const std::function<void()>& job) {
    job();
    return std::jthread{};
  };
  dependencies.before_live_edit_handoff =
      [&handed_off](app::ProjectSession&, app::PreparedInstrumentPlayback&) { handed_off = true; };
  const auto bridge = make_bridge(fixture, std::move(dependencies));
  ThreadedOutput::off_ui_stops.store(0);
  play_and_move(*bridge);
  bridge->toggle_playback();
  wait_stopped(*bridge);
  require(!handed_off, "a prepared edit was installed after Stop");
  require(ThreadedOutput::off_ui_stops.load() == 0, "Stop ran off the UI thread");
  require(bridge->playback_status().contains(QStringLiteral("Playback stopped")),
          "Stop was not reported: " + bridge->playback_status().toStdString());
  require(std::abs(only_clip(*bridge).value(QStringLiteral("start")).toDouble() - edited_start) <
              start_tolerance,
          "Stop lost the accepted move");
}
} // namespace

int main(int argc, char** argv) {
  QGuiApplication application(argc, argv);
  try {
    handoff_runs_on_the_ui_thread();
    stop_wins_over_a_prepared_edit();
  } catch (const std::exception& error) {
    qCritical("FAILED: %s", error.what());
    return 1;
  }
  return 0;
}
