// Quick warp editor contract: mode, settings, transpose and marker commands
// as single undo steps that save and reopen; input validation; keyboard and
// drag input through the real QML editor; disposable, generation-checked
// suggestions; saved versus audible playback state and render backing;
// loop wraps (primed and missed with the seek fallback); and exporting a
// processed clip through the UI.
#include "../../../apps/daw_quick/src/clip_warp_controller.hpp"
#include "../../../apps/daw_quick/src/loop_transport.hpp"
#include "../../../apps/daw_quick/src/project_bridge.hpp"

#include "oscillate/audio_io/stereo_block.hpp"
#include "oscillate/test/check.hpp"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QTest>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <numbers>
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
using namespace std::chrono_literals;

using oscillate::test::require;

class Directory {
public:
  Directory() {
    std::string pattern = (fs::temp_directory_path() / "oscillate-quick-warp-XXXXXX").string();
    require(::mkdtemp(pattern.data()) != nullptr, "temporary directory failed");
    root_ = pattern;
  }
  ~Directory() {
    std::error_code ignored;
    fs::remove_all(root_, ignored);
  }
  Directory(const Directory&) = delete;
  Directory& operator=(const Directory&) = delete;
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

// A 440 Hz tone with a click every quarter second, or (`drums`) a quiet tone
// with a 20 ms decaying burst every half second: a 120 BPM pulse the tempo
// analyser can follow.
fs::path source_wav(const fs::path& root, std::uint32_t frames, bool drums) {
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
  for (std::uint32_t index = 0; index < frames; ++index) {
    double value =
        (drums ? 0.05 : 0.3) * std::sin(2.0 * std::numbers::pi * 440.0 * index / 48'000.0);
    if (!drums && index % 12'000 == 6'000)
      value += 0.6;
    const auto beat = (index + 18'000) % 24'000; // bursts at 6,000 + k * 24,000
    if (drums && beat < 960)
      value += 0.8 * std::exp(-static_cast<double>(beat) / 160.0) *
               std::sin(2.0 * std::numbers::pi * 180.0 * beat / 48'000.0);
    put16(wav, static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lround(value * 32'767))));
  }
  const auto path = root / "source.wav";
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char*>(wav.data()), static_cast<std::streamsize>(wav.size()));
  require(output.good(), "fixture WAV write failed");
  return path;
}

// A fake device that keeps rendering 128-frame blocks on its own thread
// while its gate is open, like a real callback, pausing `pause_us` between
// blocks (500 µs is about five times real time; 0 outruns any worker).
class ThreadOutput final : public app::PlaybackOutput {
public:
  ThreadOutput(audio::StereoRenderSource source, audio::TransportCursor& cursor,
               std::shared_ptr<std::atomic<bool>> gate, std::shared_ptr<std::atomic<int>> pause_us)
      : source_(source), cursor_(cursor), gate_(std::move(gate)), pause_us_(std::move(pause_us)) {}
  ~ThreadOutput() override { stop(); }
  ThreadOutput(const ThreadOutput&) = delete;
  ThreadOutput& operator=(const ThreadOutput&) = delete;

  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  [[nodiscard]] foundation::Result<void> start(std::uint32_t rate, std::uint32_t,
                                               std::optional<std::uint64_t>) override {
    rate_ = rate;
    state_.store(audio::OutputState::streaming, std::memory_order_release);
    thread_ = std::jthread([this](const std::stop_token& stop) {
      std::array<engine::StereoFrame, 128> frames{};
      std::array<float, 256> samples{};
      while (!stop.stop_requested()) {
        if (gate_->load(std::memory_order_acquire))
          (void)audio::render_transport_block(source_, cursor_, true, frames, samples);
        if (const auto pause = pause_us_->load(std::memory_order_acquire); pause > 0)
          std::this_thread::sleep_for(std::chrono::microseconds(pause));
        else
          std::this_thread::yield();
      }
    });
    return foundation::Result<void>::success();
  }
  void stop() noexcept override {
    if (thread_.joinable()) {
      thread_.request_stop();
      thread_.join();
    }
    state_.store(audio::OutputState::stopped, std::memory_order_release);
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
  std::shared_ptr<std::atomic<bool>> gate_;
  std::shared_ptr<std::atomic<int>> pause_us_;
  std::jthread thread_;
  std::atomic<audio::OutputState> state_{audio::OutputState::stopped};
  std::uint32_t rate_ = 0;
};

struct Fixture {
  Directory directory;
  fs::path root = directory.root() / "song";
  std::shared_ptr<std::atomic<bool>> gate = std::make_shared<std::atomic<bool>>(true);
  std::shared_ptr<std::atomic<int>> pause_us = std::make_shared<std::atomic<int>>(500);
  std::unique_ptr<quick::ProjectBridge> bridge;
  QString clip_id;

  // relative_open opens the project as a relative path with a trailing
  // separator, as a recent-projects entry or a shell argument may name it.
  explicit Fixture(std::uint32_t frames = 48'000, bool drums = false, bool relative_open = false) {
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       QString::fromStdString((directory.root() / "settings").string()));
    auto session = app::ProjectSession::create(root);
    require(session.has_value(), "project create failed");
    const auto source = source_wav(directory.root(), frames, drums);
    const int fd = ::open(source.c_str(), O_RDONLY | O_CLOEXEC);
    require(fd >= 0, "source WAV open failed");
    const auto imported = session.value()->imports().import_wav_audio(fd);
    (void)::close(fd);
    require(imported.has_value() && session.value()->saving().save().has_value(),
            "fixture import failed");
    clip_id = QString::fromStdString(
        session.value()->snapshot().tracks.front().clips.front().id.to_string());
    app::PlaybackOutputFactory factory = [gate = gate,
                                          pause = pause_us](audio::StereoRenderSource source,
                                                            audio::TransportCursor& cursor) {
      return std::make_unique<ThreadOutput>(source, cursor, gate, pause);
    };
    bridge =
        std::make_unique<quick::ProjectBridge>(directory.root(), nullptr, std::move(factory), [] {
          return foundation::Result<std::vector<audio::PipeWireSink>>::success({});
        });
    bridge->set_project(std::move(session.value()), relative_open ? fs::relative(root) / "" : root);
    bridge->select_audio_clip(clip_id);
  }
  [[nodiscard]] quick::ClipWarpController& warp() const {
    auto* controller = qobject_cast<quick::ClipWarpController*>(bridge->clip_warp());
    require(controller != nullptr, "bridge has no warp controller");
    return *controller;
  }
  [[nodiscard]] quick::LoopTransport& loop() const {
    auto* controller = qobject_cast<quick::LoopTransport*>(bridge->loop_transport());
    require(controller != nullptr, "bridge has no loop transport");
    return *controller;
  }
  [[nodiscard]] QVariantMap state() const { return warp().state(); }
  void stop_playback() {
    if (!bridge->playback_active())
      return;
    bridge->toggle_playback();
    require(QTest::qWaitFor([this] { return !bridge->playback_active(); }, 10'000),
            "playback did not stop");
  }
};

// --- Commands, validation, undo and reopen ---------------------------------------

void mode_settings_and_transpose_are_single_undo_steps() {
  Fixture fixture;
  auto& warp = fixture.warp();
  auto state = fixture.state();
  require(state.value("available").toBool() && state.value("modeIndex").toInt() == 4 &&
              !state.value("overridden").toBool() && !state.value("processed").toBool(),
          "a fresh clip is not shown as unoverridden Complex playing its source");
  const auto start = fixture.bridge->revision();

  warp.set_mode(5); // Complex Pro
  state = fixture.state();
  require(fixture.bridge->revision() == start + 1 && state.value("modeIndex").toInt() == 5 &&
              state.value("overridden").toBool() && state.value("formantsActive").toBool(),
          "choosing Complex Pro was not one overriding edit");
  warp.set_transpose(7, 4);
  state = fixture.state();
  require(fixture.bridge->revision() == start + 2 && state.value("semitones").toInt() == 7 &&
              state.value("cents").toInt() == 4 && state.value("processed").toBool(),
          "transpose was not saved as semitones plus cents");
  // A remembered setting of another mode survives switching modes.
  warp.set_setting("textureGrain", 250.0);
  warp.set_mode(2); // Texture
  state = fixture.state();
  require(fixture.bridge->revision() == start + 4 &&
              state.value("textureGrain").toDouble() == 250.0,
          "Texture settings were not remembered across a mode switch");
  warp.set_mode(3); // Re-Pitch
  state = fixture.state();
  require(!state.value("transposeActive").toBool() && !state.value("formantsActive").toBool() &&
              state.value("semitones").toInt() == 7,
          "Re-Pitch did not mark transpose and formants inactive while keeping them");

  // Invalid input changes nothing.
  const auto before = fixture.bridge->revision();
  warp.set_transpose(48, 10);
  warp.set_transpose(0, 60);
  warp.set_setting("tonesGrain", 3.0);
  warp.set_setting("unknown", 1.0);
  warp.set_mode(9);
  warp.set_beats_loop(4);
  require(fixture.bridge->revision() == before, "invalid warp input created a revision");

  // One undo per edit, in order.
  fixture.bridge->undo();
  require(fixture.state().value("modeIndex").toInt() == 2, "undo did not revert one mode edit");
  fixture.bridge->undo();
  require(fixture.state().value("modeIndex").toInt() == 5 &&
              fixture.state().value("textureGrain").toDouble() == 250.0,
          "undo did not revert exactly the mode switch");
  fixture.bridge->redo();
  fixture.bridge->redo();
  warp.set_preserve_formants(false);
  warp.set_beats_loop(2);
  fixture.bridge->save();
  const auto saved = fixture.state();
  const auto root = fixture.root;
  fixture.bridge.reset();
  auto reopened = app::ProjectSession::open(root);
  require(reopened.has_value(), "project did not reopen");
  const auto& processing = reopened.value()->snapshot().tracks.front().clips.front().processing;
  require(processing.warp.mode == project::WarpMode::repitch &&
              std::lround(processing.pitch_semitones * 100) == 704 &&
              !processing.preserve_formants &&
              processing.warp.beats.loop == project::WarpBeatsLoop::back_and_forth &&
              processing.warp.texture.grain_ms == 250.0 && saved.value("modeIndex").toInt() == 3,
          "warp edits did not save and reopen");
}

void markers_and_suggestions_are_reviewed_edits() {
  Fixture fixture;
  auto& warp = fixture.warp();
  const auto start = fixture.bridge->revision();
  warp.add_marker(0.25);
  auto markers = fixture.state().value("markers").toList();
  require(fixture.bridge->revision() == start + 1 && markers.size() == 1 &&
              std::fabs(markers[0].toMap().value("fraction").toDouble() - 0.25) < 0.001 &&
              fixture.state().value("processed").toBool(),
          "adding a marker was not one edit making the clip processed");
  warp.add_marker(0.75);
  warp.move_marker(0, 0.5);
  markers = fixture.state().value("markers").toList();
  require(markers.size() == 2 &&
              std::fabs(markers[0].toMap().value("fraction").toDouble() - 0.5) < 0.001 &&
              std::fabs(markers[1].toMap().value("fraction").toDouble() - 0.75) < 0.001,
          "moving a marker changed its neighbour");
  const auto before = fixture.bridge->revision();
  warp.move_marker(0, 0.9); // past its neighbour
  warp.move_marker(1, 1.2); // outside the clip
  warp.add_marker(0.0);     // on the clip start
  warp.remove_marker(7);    // missing
  require(fixture.bridge->revision() == before, "invalid marker input created a revision");
  warp.remove_marker(1);
  require(fixture.state().value("markers").toList().size() == 1, "marker was not removed");

  // A project edit while analysis runs makes its result stale: nothing is stored.
  warp.request_suggestions();
  require(fixture.state().value("suggestionState").toString() == "running",
          "suggestion analysis did not start");
  warp.set_mode(0);
  const auto edited = fixture.bridge->revision();
  require(
      QTest::qWaitFor(
          [&] { return fixture.state().value("suggestionState").toString() == "stale"; }, 20'000),
      "a result for an edited project was not discarded as stale");
  require(fixture.state().value("suggestions").toList().isEmpty() &&
              fixture.bridge->revision() == edited,
          "stale suggestions were shown or stored");

  // Fresh analysis is stored with the project as one revision.
  const auto analysed = fixture.bridge->revision();
  warp.request_suggestions();
  require(
      QTest::qWaitFor(
          [&] { return fixture.state().value("suggestionState").toString() == "ready"; }, 20'000),
      "suggestion analysis did not finish");
  const auto suggested = fixture.state().value("suggestions").toList().size();
  require(suggested >= 3 && fixture.bridge->revision() == analysed + 1,
          "suggestions were missing or not stored as one revision");
  // Dismissing one is a saved correction that reanalysis keeps.
  warp.dismiss_suggestion(0);
  require(fixture.bridge->revision() == analysed + 2 &&
              fixture.state().value("suggestions").toList().size() == suggested - 1,
          "a dismissal was not one saved edit");
  warp.request_suggestions();
  require(QTest::qWaitFor([&] { return fixture.bridge->revision() == analysed + 3; }, 20'000) &&
              fixture.state().value("suggestions").toList().size() == suggested - 1,
          "reanalysis brought back a dismissed suggestion");
  warp.apply_suggestions();
  const auto applied = fixture.state().value("markers").toList().size();
  require(fixture.bridge->revision() == analysed + 4 && applied > 1 &&
              fixture.state().value("suggestions").toList().isEmpty(),
          "reviewed suggestions were not applied as one edit");
  fixture.bridge->undo();
  require(fixture.state().value("markers").toList().size() == 1 &&
              fixture.state().value("suggestions").toList().size() == suggested - 1,
          "one undo did not remove every applied suggestion");
}

void auto_warp_and_conform_are_reviewed_proposals() {
  Fixture fixture(48'000 * 6, true);
  auto& warp = fixture.warp();
  // Every source is analysed in the background (the clip strip's tempo).
  require(QTest::qWaitFor(
              [&] {
                const auto entry = fixture.bridge->clip_tempo()
                                       ->property("clips")
                                       .toMap()
                                       .value(fixture.clip_id)
                                       .toMap();
                return !entry.isEmpty() && !entry.value("analysing").toBool() &&
                       entry.value("bpm").toDouble() > 0.0;
              },
              30'000),
          "tempo analysis did not finish");
  const auto elapsed = fixture.bridge->revision();
  warp.propose_auto_warp();
  require(!fixture.state().contains("proposal") && fixture.bridge->revision() == elapsed,
          "an elapsed clip got an auto-warp proposal or changed the project");
  fixture.bridge->toggle_audio_clip_timebase(fixture.clip_id);
  require(fixture.state().value("musical").toBool(), "the clip did not become musical");
  const auto before = fixture.bridge->revision();
  warp.propose_auto_warp();
  // The verified tempo analysis is stored first, as its own revision; the
  // proposal itself changes nothing.
  const auto proposed = fixture.bridge->revision();
  require(proposed == before + 1 && fixture.state().contains("proposal") &&
              fixture.state().value("proposal").toMap().value("markers").toList().size() >= 2,
          "auto-warp did not propose markers from the stored tempo review: " +
              fixture.bridge->status().toStdString());
  require(fixture.state().value("markers").toList().isEmpty(),
          "a proposal changed the clip before it was applied");
  warp.apply_proposal();
  require(fixture.bridge->revision() == proposed + 1 && !fixture.state().contains("proposal") &&
              fixture.state().value("markers").toList().size() >= 2,
          "auto-warp was not applied as one edit");
  fixture.bridge->undo();
  require(fixture.state().value("markers").toList().isEmpty(), "auto-warp did not undo");
  // A proposal made before a later edit is not applied.
  warp.propose_tempo_conform();
  require(fixture.state().contains("proposal"), "conform did not propose");
  warp.set_mode(1);
  require(!fixture.state().contains("proposal"), "a stale proposal was kept");
  const auto unchanged = fixture.bridge->revision();
  warp.apply_proposal();
  require(fixture.bridge->revision() == unchanged, "a stale proposal was applied");
}

// --- The real QML editor: keyboard and drag ----------------------------------------

// Searches the visual item tree, which includes Repeater delegates.
QQuickItem* search(QQuickItem* item, const QString& name) {
  if (item->objectName() == name)
    return item;
  for (auto* child : item->childItems())
    if (auto* found = search(child, name))
      return found;
  return nullptr;
}

QQuickItem* find_item(QQuickWindow& window, const QString& name) {
  auto* item = search(window.contentItem(), name);
  require(item != nullptr, "QML item missing: " + name.toStdString());
  return item;
}

void editor_keyboard_and_drag_commit_single_edits() {
  Fixture fixture;
  QQmlApplicationEngine engine;
  engine.rootContext()->setContextProperty("projectBridge", fixture.bridge.get());
  engine.loadData(R"(
    import QtQuick
    import QtQuick.Window
    Window {
        width: 420; height: 900; visible: true; color: "#11151b"
        WarpEditor {
            objectName: "editorUnderTest"
            x: 10; y: 10; width: 400
            bridge: projectBridge
            controller: projectBridge.clipWarp
        }
    }
  )",
                  QUrl(QStringLiteral("qrc:/warp_editor_test.qml")));
  require(!engine.rootObjects().isEmpty(), "warp editor QML did not load");
  auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().front());
  require(window != nullptr && QTest::qWaitForWindowExposed(window), "test window not exposed");

  // Mode pill by keyboard.
  auto* pro = find_item(*window, "warpModeComplexPro");
  pro->forceActiveFocus();
  QTest::keyClick(window, Qt::Key_Return);
  require(fixture.state().value("modeIndex").toInt() == 5, "keyboard did not choose Complex Pro");

  // Semitone stepper: Right commits +1, Page Up +10, each one edit.
  const auto start = fixture.bridge->revision();
  auto* semitones = find_item(*window, "warpStepperSemitones");
  semitones->forceActiveFocus();
  QTest::keyClick(window, Qt::Key_Right);
  QTest::keyClick(window, Qt::Key_PageUp);
  require(fixture.bridge->revision() == start + 2 &&
              fixture.state().value("semitones").toInt() == 11,
          "keyboard transpose steps were not single edits");

  // Re-Pitch keeps transpose but labels it inactive.
  find_item(*window, "warpModeRePitch")->forceActiveFocus();
  QTest::keyClick(window, Qt::Key_Space);
  require(!fixture.state().value("transposeActive").toBool(),
          "Re-Pitch did not deactivate transpose");

  // Optional review aid: OSCILLATE_WARP_EDITOR_SCREENSHOT=<png> saves the
  // rendered editor.
  if (const auto* shot = std::getenv("OSCILLATE_WARP_EDITOR_SCREENSHOT")) {
    fixture.warp().set_mode(5);
    fixture.warp().add_marker(0.3);
    fixture.warp().add_marker(0.7);
    QCoreApplication::processEvents();
    QTest::qWait(200);
    (void)window->grabWindow().save(QString::fromLocal8Bit(shot));
    fixture.bridge->undo();
    fixture.bridge->undo();
    fixture.bridge->undo();
  }

  // Marker lane: Enter adds at the cursor; dragging a handle is one edit.
  auto* lane = find_item(*window, "warpMarkerLane");
  lane->forceActiveFocus();
  QTest::keyClick(window, Qt::Key_Return);
  QCoreApplication::processEvents();
  require(fixture.state().value("markers").toList().size() == 1, "Enter did not add a marker");
  find_item(*window, "warpSnap")->forceActiveFocus();
  QTest::keyClick(window, Qt::Key_Space); // snap off, so the drag lands where released
  require(
      QTest::qWaitFor(
          [&] { return search(window->contentItem(), QStringLiteral("warpMarker0")) != nullptr; },
          5'000),
      "marker handle did not appear");
  auto* handle = find_item(*window, "warpMarker0");
  const auto before = fixture.bridge->revision();
  const auto from =
      handle->mapToScene(QPointF(handle->width() / 2, handle->height() / 2)).toPoint();
  const auto lane_origin = lane->mapToScene(QPointF(0, 0));
  const auto to = QPoint(static_cast<int>(lane_origin.x() + lane->width() * 0.3), from.y());
  QTest::mousePress(window, Qt::LeftButton, {}, from);
  for (int step = 1; step <= 5; ++step)
    QTest::mouseMove(window, from + (to - from) * step / 5);
  require(fixture.bridge->revision() == before, "a marker drag saved before release");
  QTest::mouseRelease(window, Qt::LeftButton, {}, to);
  const auto markers = fixture.state().value("markers").toList();
  require(fixture.bridge->revision() == before + 1 && markers.size() == 1 &&
              std::fabs(markers[0].toMap().value("fraction").toDouble() - 0.3) < 0.02,
          "a marker drag was not one edit to its release position");
  // Delete removes the selected marker.
  lane->forceActiveFocus();
  QTest::keyClick(window, Qt::Key_BracketRight);
  QTest::keyClick(window, Qt::Key_Delete);
  require(fixture.state().value("markers").toList().isEmpty(), "Delete did not remove the marker");
}

// --- Playback state, backing and loops ------------------------------------------------

void playback_status_never_claims_a_pending_edit_is_audible() {
  Fixture fixture;
  auto& warp = fixture.warp();
  warp.add_marker(0.5); // processed: routed playback renders it live
  fixture.bridge->toggle_playback();
  require(QTest::qWaitFor(
              [&] {
                return warp.playback().value("state").toString() == "active" &&
                       warp.playback().value("backing").toString() == "live";
              },
              15'000),
          "processed clip was not shown audible from a live render");
  require(warp.playback().value("tier").toString() == "Q0" &&
              warp.playback().value("text").toString().contains("live render at Q0"),
          "live render tier was not shown");

  // A saved edit is pending until the replacement source is accepted.
  warp.set_mode(0);
  const auto saved = fixture.bridge->revision();
  const auto pending = warp.playback();
  require(pending.value("state").toString() != "active" ||
              pending.value("activeRevision").toULongLong() == saved,
          "an edit was reported audible before its source was accepted");
  require(QTest::qWaitFor(
              [&] {
                const auto view = warp.playback();
                return view.value("state").toString() == "active" &&
                       view.value("activeRevision").toULongLong() == saved;
              },
              15'000),
          "the edit never became audible through the routed handoff");
  fixture.stop_playback();
  require(warp.playback().value("state").toString() == "stopped",
          "stopped playback is not shown as stopped");
}

void processed_clip_plays_when_opened_by_relative_path() {
  Fixture fixture(48'000, false, true);
  auto& warp = fixture.warp();
  warp.add_marker(0.5);
  fixture.bridge->toggle_playback();
  require(QTest::qWaitFor(
              [&] {
                return warp.playback().value("state").toString() == "active" &&
                       warp.playback().value("backing").toString() == "live";
              },
              15'000),
          "a processed clip did not play from a project opened by a relative path");
  fixture.stop_playback();
}

void loop_reprimes_wraps_exactly_and_withdraws_on_disable() {
  Fixture fixture(48'000 * 3); // longer than a one-bar loop
  auto& loop = fixture.loop();
  bool reprimed = false;
  QObject::connect(&loop, &quick::LoopTransport::changed,
                   [&] { reprimed |= loop.status().contains("re-priming"); });
  // Loop playback is a routed-path feature; a processed clip plays routed.
  fixture.warp().add_marker(0.5);
  fixture.bridge->toggle_playback();
  require(
      QTest::qWaitFor(
          [&] { return fixture.bridge->playback_status().startsWith("Playing routed"); }, 15'000),
      "routed playback did not start");
  require(QTest::qWaitFor([&] { return fixture.bridge->playhead_sample() >= 40'000; }, 15'000),
          "playback did not advance");
  // Arm the loop only after the worker streamed past the loop end: the
  // queued frames are discarded by an in-place restart (a short hold, no
  // jump) instead of a seek at the loop end, and the next pass wraps exactly.
  fixture.gate->store(false, std::memory_order_release);
  std::this_thread::sleep_for(300ms); // let the worker fill ahead past bar 2
  loop.set_range(1, 2);
  loop.set_enabled(true);
  fixture.gate->store(true, std::memory_order_release);
  require(QTest::qWaitFor([&] { return loop.gapless_wraps() >= 1; }, 20'000),
          "the loop did not wrap once primed: " + loop.status().toStdString());
  require(reprimed && loop.seek_restarts() == 0 && loop.late_wraps() == 0,
          "a late-armed loop was not re-primed in place: " + loop.status().toStdString());
  // Rapid toggling withdraws every queued wrap; playback then runs straight
  // through the old loop end (bar 2 starts at sample 96,000).
  require(QTest::qWaitFor([&] { return loop.status().contains("wraps exactly"); }, 15'000),
          "the next pass was not scheduled: " + loop.status().toStdString());
  const auto wraps = loop.gapless_wraps();
  loop.set_enabled(false);
  loop.set_enabled(true);
  loop.set_enabled(false);
  require(QTest::qWaitFor([&] { return fixture.bridge->playhead_sample() > 110'000; }, 20'000),
          "playback did not continue past the withdrawn loop: " + loop.status().toStdString());
  // A wrap the callback had already applied at the instant of the first
  // disable is legitimate; none may apply after it.
  require(loop.gapless_wraps() <= wraps + 1 && loop.status() == "Loop off",
          "a withdrawn loop still wrapped: " + loop.status().toStdString());
  fixture.stop_playback();
}

// A live render that cannot keep up steps down Q0 → Q1 → Q2, and the editor
// says so: the tier heard and how many times it stepped down, never Q0.
void step_down_is_shown_as_the_tier_heard() {
  Fixture fixture(48'000 * 20);
  auto& warp = fixture.warp();
  warp.add_marker(0.5);
  warp.set_mode(5);      // Complex Pro, the heaviest live render
  *fixture.pause_us = 0; // the fake device outruns the prefetch worker
  fixture.bridge->toggle_playback();
  const bool stepped = QTest::qWaitFor(
      [&] {
        const auto view = warp.playback();
        return view.value("tier").toString() == "Q2" && view.value("stepDowns").toULongLong() == 2;
      },
      30'000);
  const auto view = warp.playback();
  require(stepped, "a starved live render did not step down to Q2: " +
                       view.value("text").toString().toStdString());
  require(view.value("backing").toString() == "live" &&
              view.value("text").toString().contains("live render at Q2") &&
              view.value("text").toString().contains("stepped down 2×") &&
              !view.value("text").toString().contains("Q0"),
          "the step-down was not reported as the tier heard: " +
              view.value("text").toString().toStdString());
  *fixture.pause_us = 500;
  fixture.stop_playback();
}

// --- Arrangement waveform -----------------------------------------------------

// The clip's arrangement waveform once its summary is ready, and the
// fractions (of the clip) where its clicks start (the tone peaks at 0.3, each
// click reaches 0.6).
std::vector<double> arrangement_clicks(const Fixture& fixture) {
  for (const auto& track : fixture.bridge->tracks()) {
    for (const auto& value : track.toMap().value("clips").toList()) {
      const auto clip = value.toMap();
      if (clip.value("id").toString() != fixture.clip_id ||
          clip.value("waveformState").toString() != "ready")
        continue;
      const auto columns = fixture.bridge->clip_waveform(fixture.clip_id);
      std::vector<double> clicks;
      bool inside = false;
      for (qsizetype index = 0; index < columns.size(); ++index) {
        const bool peak = columns[index].toMap().value("max").toDouble() > 0.45;
        if (peak && !inside)
          clicks.push_back(static_cast<double>(index) / static_cast<double>(columns.size()));
        inside = peak;
      }
      return clicks;
    }
  }
  return {};
}

std::string describe(const std::vector<double>& clicks) {
  std::string text;
  for (const auto click : clicks)
    text += std::to_string(click) + " ";
  return text.empty() ? std::string("none") : text;
}

bool clicks_at(const std::vector<double>& clicks, const std::vector<double>& expected) {
  if (clicks.size() != expected.size())
    return false;
  for (std::size_t index = 0; index < clicks.size(); ++index)
    if (std::fabs(clicks[index] - expected[index]) > 0.01)
      return false;
  return true;
}

void arrangement_waveform_follows_the_warp_map() {
  Fixture fixture; // clicks at source 6,000 + k * 12,000 of 48,000
  fixture.bridge->set_arrangement_viewport(0, 1, 3'840);
  const bool unwarped = QTest::qWaitFor(
      [&] { return clicks_at(arrangement_clicks(fixture), {0.125, 0.375, 0.625, 0.875}); }, 20'000);
  require(unwarped, "the unwarped arrangement waveform does not show the source clicks: " +
                        describe(arrangement_clicks(fixture)));
  // Pin source 24,000 at output 12,000: the first half plays twice as fast,
  // the second at two-thirds speed. The clicks move with the audio.
  fixture.warp().add_marker(0.5);
  fixture.warp().move_marker(0, 0.25);
  const std::vector<double> warped{3'000.0 / 48'000, 9'000.0 / 48'000, 21'000.0 / 48'000,
                                   39'000.0 / 48'000};
  const bool follows =
      QTest::qWaitFor([&] { return clicks_at(arrangement_clicks(fixture), warped); }, 20'000);
  require(follows, "the arrangement waveform does not follow the warp map: " +
                       describe(arrangement_clicks(fixture)));
}

// --- Editing processed clips --------------------------------------------------

void marked_clip_splits_through_the_ui_as_one_undo_step() {
  Fixture fixture;
  auto& warp = fixture.warp();
  warp.add_marker(0.5); // output 24,000 of 48,000
  const auto revision = fixture.bridge->revision();
  fixture.bridge->seek_to_sample(12'000);
  fixture.bridge->split_audio_clip_at_playhead(fixture.clip_id);
  require(fixture.bridge->revision() == revision + 1 &&
              fixture.bridge->status().startsWith("Audio clip split") &&
              !fixture.bridge->status().contains("removed"),
          "a marked clip did not split: " + fixture.bridge->status().toStdString());
  // The selected left half ends before the marker, so it is an unstretched
  // clip with no markers and plays raw; the right half keeps the marker.
  require(fixture.state().value("markers").toList().isEmpty() &&
              !fixture.state().value("processed").toBool(),
          "the left half of a split warped clip has the wrong warp state");
  fixture.bridge->undo();
  require(fixture.bridge->revision() == revision + 2 &&
              fixture.state().value("markers").toList().size() == 1,
          "undo did not rejoin the warped clip with its marker");
}

// --- Export ----------------------------------------------------------------------------

void processed_clip_exports_through_the_ui() {
  Fixture fixture;
  auto& warp = fixture.warp();
  warp.add_marker(0.5);
  warp.move_marker(0, 0.4);
  warp.set_mode(4);
  const auto target = fixture.directory.root() / "processed.wav";
  fixture.bridge->exports()->set_export_target(QString::fromStdString(target.string()));
  fixture.bridge->exports()->export_master();
  require(QTest::qWaitFor([&] { return !fixture.bridge->exports()->export_busy(); }, 30'000) &&
              fs::exists(target) && fs::file_size(target) > 44,
          "processed clip did not export: " +
              fixture.bridge->exports()->export_status().toStdString());
  require(
      !fixture.bridge->exports()->export_status().contains("warp", Qt::CaseInsensitive) ||
          !fixture.bridge->exports()->export_status().contains("unsupported", Qt::CaseInsensitive),
      "export still claims warp is unsupported");
}

void cancelling_onset_analysis_does_not_block_the_ui() {
  Directory directory;
  const auto root = directory.root() / "song";
  auto created = app::ProjectSession::create(root);
  require(created.has_value(), "project create failed");
  auto session = std::move(created.value());
  const auto source = source_wav(directory.root(), 48'000, false);
  const int fd = ::open(source.c_str(), O_RDONLY | O_CLOEXEC);
  require(fd >= 0, "source WAV open failed");
  const auto imported = session->imports().import_wav_audio(fd);
  (void)::close(fd);
  require(imported.has_value(), "fixture import failed");
  const auto clip_id =
      QString::fromStdString(session->snapshot().tracks.front().clips.front().id.to_string());

  struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool started = false;
    bool released = false;
    bool returned = false;
    bool finished = false;
    bool timed_out = false;
    bool cancel_seen = false;
  } gate;
  quick::ClipWarpController controller(
      [&] { return session.get(); }, [root] { return root; }, [] { return true; }, [](QString) {},
      [] {}, [](const project::Clip&, std::size_t) { return QVariantList{}; },
      [] { return quick::WarpPlaybackObservation{}; }, {}, nullptr,
      [&](const fs::path&, const project::Snapshot&, project::SourceId,
          const std::atomic<bool>* cancelled) {
        std::unique_lock lock(gate.mutex);
        gate.started = true;
        gate.changed.notify_all();
        gate.changed.wait(lock, [&] { return gate.released; });
        gate.cancel_seen = cancelled->load(std::memory_order_acquire);
        gate.finished = true;
        gate.changed.notify_all();
        return foundation::Result<app::SourceOnsets>::success({});
      });
  controller.select_clip(clip_id);
  controller.request_suggestions();
  {
    std::unique_lock lock(gate.mutex);
    require(gate.changed.wait_for(lock, 5s, [&] { return gate.started; }),
            "onset analysis did not start");
  }
  // If selection tries to join the blocked analyzer, this watchdog releases
  // it and makes the regression fail instead of hanging the test suite.
  std::jthread watchdog([&] {
    std::unique_lock lock(gate.mutex);
    if (!gate.changed.wait_for(lock, 500ms, [&] { return gate.returned; }))
      gate.timed_out = true;
    gate.released = true;
    gate.changed.notify_all();
  });
  controller.select_clip({});
  {
    std::lock_guard lock(gate.mutex);
    gate.returned = true;
    gate.changed.notify_all();
  }
  watchdog.join();
  {
    std::unique_lock lock(gate.mutex);
    require(gate.changed.wait_for(lock, 5s, [&] { return gate.finished; }),
            "cancelled analysis did not finish");
    require(!gate.timed_out, "selecting another clip blocked on onset analysis");
    require(gate.cancel_seen, "superseded analysis did not receive cancellation");
  }
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication application(argc, argv);
  try {
    arrangement_waveform_follows_the_warp_map();
    mode_settings_and_transpose_are_single_undo_steps();
    markers_and_suggestions_are_reviewed_edits();
    auto_warp_and_conform_are_reviewed_proposals();
    editor_keyboard_and_drag_commit_single_edits();
    playback_status_never_claims_a_pending_edit_is_audible();
    processed_clip_plays_when_opened_by_relative_path();
    loop_reprimes_wraps_exactly_and_withdraws_on_disable();
    step_down_is_shown_as_the_tier_heard();
    marked_clip_splits_through_the_ui_as_one_undo_step();
    processed_clip_exports_through_the_ui();
    cancelling_onset_analysis_does_not_block_the_ui();
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
  return 0;
}
