#include "oscillate/audio_io/stereo_block.hpp"
#include "oscillate/plugins/vst3_scan.hpp"
#include "oscillate/test/check.hpp"
#include "project_bridge.hpp"
#include "track_automation_controller.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
namespace app = oscillate::application;
namespace quick = oscillate::quick;

// How long a dropped file's import may take before the test reports it lost.
constexpr int import_wait_ms = 15'000;

struct BlockPermits {
  std::mutex mutex;
  std::condition_variable wake;
  std::size_t count = 0;
  bool stopping = false;
  void release(std::size_t blocks = 1) {
    {
      std::lock_guard lock(mutex);
      count = std::min<std::size_t>(8, count + blocks);
    }
    wake.notify_all();
  }
};

class PermittedOutput final : public app::PlaybackOutput {
public:
  PermittedOutput(oscillate::audio_io::StereoRenderSource source,
                  oscillate::audio_io::TransportCursor& cursor,
                  std::shared_ptr<BlockPermits> permits)
      : source_(source), cursor_(cursor), permits_(std::move(permits)) {}
  ~PermittedOutput() override { stop(); }
  // PlaybackOutput fixes the adjacent sample-rate and block-size argument order.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  [[nodiscard]] oscillate::foundation::Result<void> start(std::uint32_t, std::uint32_t frames,
                                                          std::optional<std::uint64_t>) override {
    {
      std::lock_guard lock(permits_->mutex);
      permits_->stopping = false;
      permits_->count = 0;
    }
    state_.store(oscillate::audio_io::OutputState::streaming, std::memory_order_release);
    worker_ = std::thread([this, frames] {
      std::array<oscillate::engine::StereoFrame, 128> rendered{};
      std::array<float, 256> interleaved{};
      for (;;) {
        {
          std::unique_lock lock(permits_->mutex);
          permits_->wake.wait(lock, [&] { return permits_->stopping || permits_->count != 0; });
          if (permits_->stopping)
            break;
          --permits_->count;
        }
        (void)oscillate::audio_io::render_transport_block(
            source_, cursor_, true, std::span(rendered).first(frames),
            std::span(interleaved).first(static_cast<std::size_t>(frames) * 2));
      }
    });
    return oscillate::foundation::Result<void>::success();
  }
  void stop() noexcept override {
    {
      std::lock_guard lock(permits_->mutex);
      permits_->stopping = true;
    }
    permits_->wake.notify_all();
    if (worker_.joinable())
      worker_.join();
    state_.store(oscillate::audio_io::OutputState::stopped, std::memory_order_release);
  }
  [[nodiscard]] oscillate::audio_io::OutputStatus status() const noexcept override {
    oscillate::audio_io::OutputStatus result;
    result.state = state_.load(std::memory_order_acquire);
    return result;
  }

private:
  oscillate::audio_io::StereoRenderSource source_;
  oscillate::audio_io::TransportCursor& cursor_;
  std::shared_ptr<BlockPermits> permits_;
  std::thread worker_;
  std::atomic<oscillate::audio_io::OutputState> state_{oscillate::audio_io::OutputState::stopped};
};

using oscillate::test::require;

QQuickItem* find_item(QQuickItem* parent, const QString& name) {
  if (parent->objectName() == name)
    return parent;
  for (auto* child : parent->childItems()) {
    if (auto* found = find_item(child, name))
      return found;
  }
  return nullptr;
}

void send_drop(QQuickWindow& window, const QMimeData& mime, std::optional<QPoint> at = {}) {
  const QPoint position = at.value_or(QPoint{window.width() / 2, window.height() / 2});
  QDragEnterEvent enter(position, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
  QCoreApplication::sendEvent(&window, &enter);
  QDropEvent drop(position, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
  QCoreApplication::sendEvent(&window, &drop);
  require(enter.isAccepted() && drop.isAccepted(), "new-track drop was not accepted by the UI");
}

void put16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value));
  bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void put32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
  put16(bytes, static_cast<std::uint16_t>(value));
  put16(bytes, static_cast<std::uint16_t>(value >> 16U));
}

// A one-track Standard MIDI File: one quarter-note C4 at tick 0.
void write_midi_source(const std::filesystem::path& path) {
  const std::vector<std::uint8_t> smf{
      'M',  'T',  'h',  'd', 0, 0, 0, 6,  0,    0,    0,  1,   0x01, 0xe0, // format 0, 480 PPQ
      'M',  'T',  'r',  'k', 0, 0, 0, 13, 0x00, 0x90, 60, 100,             // note on
      0x83, 0x60, 0x80, 60,  0,                                            // 480 ticks, note off
      0x00, 0xff, 0x2f, 0x00};                                             // end of track
  std::ofstream(path, std::ios::binary)
      .write(reinterpret_cast<const char*>(smf.data()), static_cast<std::streamsize>(smf.size()));
}

std::vector<QQuickItem*> find_items(QQuickItem* parent, const QString& name) {
  std::vector<QQuickItem*> found;
  if (parent->objectName() == name)
    found.push_back(parent);
  for (auto* child : parent->childItems())
    std::ranges::move(find_items(child, name), std::back_inserter(found));
  return found;
}

QPoint scene_point(QQuickItem& item, double fx, double fy) {
  return item.mapToScene({item.width() * fx, item.height() * fy}).toPoint();
}

void write_audio_source(const std::filesystem::path& path) {
  constexpr std::uint32_t frames = 48'000;
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
  for (std::uint32_t frame = 0; frame < frames; ++frame)
    put16(wav, 8192);
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char*>(wav.data()), static_cast<std::streamsize>(wav.size()));
  require(output.good(), "WAV fixture write failed");
}

void require_track_chrome(QQuickWindow& window, bool has_track) {
  auto* creation_row = find_item(window.contentItem(), QStringLiteral("trackCreationRow"));
  auto* add_button = find_item(window.contentItem(), QStringLiteral("addTrackButton"));
  auto* master = find_item(window.contentItem(), QStringLiteral("masterTrackRow"));
  auto* track_list = find_item(window.contentItem(), QStringLiteral("trackList"));
  require(creation_row && add_button && master && track_list && creation_row->isVisible() &&
              add_button->isVisible() && master->isVisible() && track_list->isVisible(),
          "project did not show the add control and pinned master");
  // TRACKS and + Track head the list; the master is pinned below it.
  const auto list_y = track_list->mapToScene({0, 0}).y();
  require(list_y == creation_row->mapToScene({0, 0}).y() + creation_row->height() &&
              master->mapToScene({0, 0}).y() == list_y + track_list->height() &&
              (find_item(window.contentItem(), QStringLiteral("trackRow")) != nullptr) == has_track,
          "track list header and pinned master are out of place");
}

void run() {
  QTemporaryDir directory;
  require(directory.isValid(), "temporary project directory unavailable");
  qputenv("XDG_CONFIG_HOME", (directory.path() + QStringLiteral("/config")).toUtf8());
  const auto root = std::filesystem::path(directory.path().toStdString()) / "project";
  auto created = app::ProjectSession::create(root);
  require(created.has_value(), "empty project creation failed");

  {
    quick::ProjectBridge bridge(directory.path().toStdString());
    bridge.set_project(std::move(created).value(), root);
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("projectBridge", &bridge);
    engine.load(QUrl(QStringLiteral("qrc:/DawShell.qml")));
    require(!engine.rootObjects().isEmpty(), "Quick shell did not load");
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().front());
    require(window != nullptr, "Quick shell has no window");
    require_track_chrome(*window, false);

    bridge.vst3_browser()->add_vst3_search_path(QString::fromUtf8(VST3_INSTRUMENT_BUNDLE));
    const auto entry = [&bridge]() -> QVariantMap {
      const auto entries = bridge.vst3_browser()->entries();
      const auto found = std::ranges::find_if(entries, [](const auto& candidate) {
        const auto row = candidate.toMap();
        return row.value(QStringLiteral("kind")) == QStringLiteral("instrument") &&
               row.value(QStringLiteral("state")) == QStringLiteral("ready");
      });
      return found == entries.end() ? QVariantMap{} : found->toMap();
    };
    require(QTest::qWaitFor([&] { return !bridge.vst3_browser()->busy() && !entry().isEmpty(); },
                            15'000),
            "VST3 instrument fixture was not scanned");
    window->setProperty("pluginBrowserOpen", true);
    QCoreApplication::processEvents();
    auto* browser = find_item(window->contentItem(), QStringLiteral("vst3BrowserPanel"));
    auto* toggle = find_item(window->contentItem(), QStringLiteral("vst3BrowserKindToggle"));
    require(browser && toggle && toggle->isVisible(), "instrument browser toggle is unavailable");
    toggle->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_Return);
    QCoreApplication::processEvents();
    auto* drag_handle =
        find_item(window->contentItem(), QStringLiteral("vst3InstrumentDragHandle"));
    require(browser->property("showInstruments").toBool() && drag_handle &&
                drag_handle->isVisible(),
            "verified instrument cannot be dragged from the browser");
    const auto choice = entry();
    QMimeData forged;
    forged.setData(
        "application/x-oscillate-vst3-instrument",
        QJsonDocument(QJsonObject{{"index", choice.value(QStringLiteral("index")).toInt()},
                                  {"classId", QStringLiteral("wrong class")}})
            .toJson(QJsonDocument::Compact));
    const auto before = bridge.revision();
    send_drop(*window, forged);
    require(QTest::qWaitFor([&] { return !bridge.vst3_browser()->busy(); }, 15'000) &&
                bridge.revision() == before && bridge.tracks().isEmpty(),
            "forged instrument drop left an empty track");
    QMimeData mime;
    mime.setData(
        "application/x-oscillate-vst3-instrument",
        QJsonDocument(QJsonObject{{"index", choice.value(QStringLiteral("index")).toInt()},
                                  {"classId", choice.value(QStringLiteral("classId")).toString()}})
            .toJson(QJsonDocument::Compact));
    send_drop(*window, mime);
    require(
        QTest::qWaitFor(
            [&] { return !bridge.vst3_browser()->busy() && bridge.revision() > before; }, 15'000),
        "verified instrument drop did not create a track");
    require(
        bridge.revision() == before + 1 && bridge.tracks().size() == 1 &&
            bridge.tracks().front().toMap().value(QStringLiteral("kind")) ==
                QStringLiteral("instrument") &&
            bridge.tracks().front().toMap().value(QStringLiteral("vst3Plugins")).toList().size() ==
                1,
        "instrument track and insert were not one project edit");
    bridge.undo();
    require(bridge.tracks().isEmpty(), "one undo did not remove instrument and track");
    bridge.redo();
    require(bridge.tracks().size() == 1, "one redo did not restore instrument and track");
    const auto instrument_id =
        bridge.tracks().front().toMap().value(QStringLiteral("id")).toString();
    bridge.add_effect_engine(instrument_id, 0, QStringLiteral("shape"));
    require(
        bridge.insert_chains().front().toMap().value(QStringLiteral("effects")).toList().size() ==
            1,
        "VST3 instrument did not expose the common native insert chain");
    bridge.save();
  }
  auto reopened = app::ProjectSession::open(root);
  require(reopened && reopened.value()->snapshot().tracks.size() == 1 &&
              std::holds_alternative<oscillate::project::Vst3PluginInstance>(
                  reopened.value()->snapshot().tracks.front().instrument_source) &&
              reopened.value()->snapshot().tracks.front().effects.size() == 1,
          "instrument source and native effect did not save and reopen");
  quick::ProjectBridge reopened_bridge(directory.path().toStdString());
  reopened_bridge.set_project(std::move(reopened).value(), root);
  QQmlApplicationEngine reopened_engine;
  reopened_engine.rootContext()->setContextProperty("projectBridge", &reopened_bridge);
  reopened_engine.load(QUrl(QStringLiteral("qrc:/DawShell.qml")));
  require(!reopened_engine.rootObjects().isEmpty(), "reopened Quick shell did not load");
  auto* reopened_window = qobject_cast<QQuickWindow*>(reopened_engine.rootObjects().front());
  require(reopened_window != nullptr, "reopened Quick shell has no window");
  // Channel controls live in the context panel, which starts closed.
  reopened_window->setProperty("contextPanelOpen", true);
  QCoreApplication::processEvents();
  require_track_chrome(*reopened_window, true);
  const auto track_id =
      reopened_bridge.tracks().front().toMap().value(QStringLiteral("id")).toString();
  auto* mute = find_item(reopened_window->contentItem(), QStringLiteral("trackMuteButton"));
  auto* solo = find_item(reopened_window->contentItem(), QStringLiteral("trackSoloButton"));
  auto* remove = find_item(reopened_window->contentItem(), QStringLiteral("trackDeleteButton"));
  require(mute && solo && remove && mute->isVisible() && solo->isVisible() && remove->isVisible(),
          "reopened track lacks visible mute, solo and delete controls");
  solo->forceActiveFocus();
  QTest::keyClick(reopened_window, Qt::Key_Return);
  require(reopened_bridge.tracks().front().toMap().value(QStringLiteral("soloed")).toBool(),
          "keyboard solo did not change project state");
  mute = find_item(reopened_window->contentItem(), QStringLiteral("trackMuteButton"));
  require(mute && mute->isVisible(), "track mute control vanished after solo");
  mute->forceActiveFocus();
  QTest::keyClick(reopened_window, Qt::Key_Return);
  require(reopened_bridge.tracks().front().toMap().value(QStringLiteral("muted")).toBool(),
          "keyboard mute did not change project state");
  auto* level = find_item(reopened_window->contentItem(), QStringLiteral("trackLevelControl"));
  require(level && level->isVisible(), "selected track has no channel level control");
  level->forceActiveFocus();
  QTest::keyClick(reopened_window, Qt::Key_Right);
  require(reopened_bridge.tracks().front().toMap().value(QStringLiteral("level")).toDouble() > 1.0,
          "keyboard channel level did not change project state");
  auto* pan = find_item(reopened_window->contentItem(), QStringLiteral("trackPanControl"));
  require(pan && pan->isVisible(), "selected track has no balance control");
  pan->forceActiveFocus();
  QTest::keyClick(reopened_window, Qt::Key_Right);
  require(reopened_bridge.tracks().front().toMap().value(QStringLiteral("pan")).toDouble() > 0.0,
          "keyboard balance did not change project state");
  auto* inspector_scroll =
      find_item(reopened_window->contentItem(), QStringLiteral("inspectorScroll"));
  require(inspector_scroll != nullptr, "channel inspector scroll is missing");
  inspector_scroll->setProperty("contentY", 240.0);
  QCoreApplication::processEvents();
  level = find_item(reopened_window->contentItem(), QStringLiteral("trackLevelControl"));
  require(level && level->isVisible(), "level bar vanished before pointer drag");
  const auto level_from = level->mapToScene({level->width() * 0.84, level->height() / 2}).toPoint();
  const auto level_to = level->mapToScene({level->width() * 0.65, level->height() / 2}).toPoint();
  const auto level_revision = reopened_bridge.revision();
  const auto original_level =
      reopened_bridge.tracks().front().toMap().value(QStringLiteral("level")).toDouble();
  // A drag is heard live and saves nothing until its release: one edit.
  QTest::mousePress(reopened_window, Qt::LeftButton, Qt::NoModifier, level_from);
  QTest::mouseMove(reopened_window, level_to);
  require(reopened_bridge.revision() == level_revision,
          "level pointer drag saved an edit before its release");
  QTest::mouseRelease(reopened_window, Qt::LeftButton, Qt::NoModifier, level_to);
  require(reopened_bridge.revision() == level_revision + 1 &&
              reopened_bridge.tracks().front().toMap().value(QStringLiteral("level")).toDouble() <
                  1.0,
          "level pointer drag did not apply its final value");
  reopened_bridge.undo();
  require(reopened_bridge.tracks().front().toMap().value(QStringLiteral("level")).toDouble() ==
              original_level,
          "level pointer drag required more than one Undo step");
  reopened_bridge.redo();
  pan = find_item(reopened_window->contentItem(), QStringLiteral("trackPanControl"));
  require(pan && pan->isVisible(), "balance bar vanished before pointer drag");
  const auto pan_from = pan->mapToScene({pan->width() * 0.55, pan->height() / 2}).toPoint();
  const auto pan_to = pan->mapToScene({pan->width() * 0.2, pan->height() / 2}).toPoint();
  const auto pan_revision = reopened_bridge.revision();
  const auto original_pan =
      reopened_bridge.tracks().front().toMap().value(QStringLiteral("pan")).toDouble();
  // A drag is heard live and saves nothing until its release: one edit.
  QTest::mousePress(reopened_window, Qt::LeftButton, Qt::NoModifier, pan_from);
  QTest::mouseMove(reopened_window, pan_to);
  require(reopened_bridge.revision() == pan_revision,
          "balance pointer drag saved an edit before its release");
  QTest::mouseRelease(reopened_window, Qt::LeftButton, Qt::NoModifier, pan_to);
  require(reopened_bridge.revision() == pan_revision + 1 &&
              reopened_bridge.tracks().front().toMap().value(QStringLiteral("pan")).toDouble() <
                  0.0,
          "balance pointer drag did not apply its final value");
  reopened_bridge.undo();
  require(reopened_bridge.tracks().front().toMap().value(QStringLiteral("pan")).toDouble() ==
              original_pan,
          "balance pointer drag required more than one Undo step");
  reopened_bridge.redo();
  inspector_scroll->setProperty("contentY", 600.0);
  QCoreApplication::processEvents();
  auto* channel_add =
      find_item(reopened_window->contentItem(), QStringLiteral("channelAutomationAddPoint"));
  require(channel_add && channel_add->isVisible(),
          "selected track lacks a channel automation editor");
  const auto channel_revision = reopened_bridge.revision();
  channel_add->forceActiveFocus();
  QTest::keyClick(reopened_window, Qt::Key_Return);
  require(reopened_bridge.revision() == channel_revision + 1 &&
              reopened_bridge.tracks()
                      .front()
                      .toMap()
                      .value(QStringLiteral("channelAutomation"))
                      .toMap()
                      .value(QStringLiteral("pointCount"))
                      .toInt() == 1,
          "keyboard channel automation did not create one point");
  const auto channel_track_id =
      reopened_bridge.tracks().front().toMap().value(QStringLiteral("id")).toString();
  auto* automation =
      static_cast<quick::TrackAutomationController*>(reopened_bridge.track_automation());
  for (const int zero_based_bar : {1, 2, 3, 4, 16})
    automation->set_point(channel_track_id, zero_based_bar, 0.5);
  const auto channel_lane =
      reopened_bridge.tracks().front().toMap().value(QStringLiteral("channelAutomation")).toMap();
  const auto projected_points = channel_lane.value(QStringLiteral("points")).toList();
  require(channel_lane.value(QStringLiteral("pointCount")).toInt() == 6 &&
              projected_points.size() == 6 &&
              projected_points.back().toMap().value(QStringLiteral("position")).toString() ==
                  QStringLiteral("BAR 17 · BEAT 1"),
          "channel editor projection truncated a longer automation lane");
  // The editor opens on its graph; exact positions and segment shapes are in
  // the point list behind its view toggle.
  auto* view_toggle =
      find_item(reopened_window->contentItem(), QStringLiteral("channelAutomationViewToggle"));
  require(view_toggle && view_toggle->isVisible(), "channel editor has no point-list toggle");
  view_toggle->forceActiveFocus();
  QTest::keyClick(reopened_window, Qt::Key_Return);
  QCoreApplication::processEvents();
  auto* channel_list =
      find_item(reopened_window->contentItem(), QStringLiteral("channelAutomationPointList"));
  require(channel_list && channel_list->property("count").toInt() == 6,
          "channel editor did not expose the full scrollable point list");
  const auto lane_id = channel_lane.value(QStringLiteral("laneId")).toString();
  const auto first_id = projected_points.front().toMap().value(QStringLiteral("id")).toString();
  auto* next_shape = find_item(channel_list, QStringLiteral("automationShapeNext"));
  require(next_shape && next_shape->isVisible(), "channel segment shape control is missing");
  const auto before_shape = reopened_bridge.revision();
  next_shape->forceActiveFocus();
  QTest::keyClick(reopened_window, Qt::Key_Return);
  const auto shape = automation->segment_curve(lane_id, first_id, reopened_bridge.revision());
  require(reopened_bridge.revision() == before_shape + 1 &&
              shape.value(QStringLiteral("shapeIndex")).toInt() == 2,
          "keyboard curve action did not save a quadratic segment");
  automation->set_segment_curve(
      lane_id, first_id, {{"shapeIndex", 5}, {"x1", 0.9}, {"y1", 0.1}, {"x2", 0.2}, {"y2", 0.8}});
  require(reopened_bridge.revision() == before_shape + 1,
          "invalid Bézier handle order changed the project");
  automation->set_segment_curve(
      lane_id, first_id, {{"shapeIndex", 5}, {"x1", 0.1}, {"y1", 0.2}, {"x2", 0.8}, {"y2", 0.9}});
  const auto custom = automation->segment_curve(lane_id, first_id, reopened_bridge.revision());
  require(reopened_bridge.revision() == before_shape + 2 &&
              custom.value(QStringLiteral("shapeIndex")).toInt() == 5 &&
              custom.value(QStringLiteral("x1")).toDouble() == 0.1 &&
              custom.value(QStringLiteral("y2")).toDouble() == 0.9,
          "Bézier handles did not round-trip through the Quick controller");
  auto* x1_handle = find_item(channel_list, QStringLiteral("automationHandleX1"));
  auto* apply_handles = find_item(channel_list, QStringLiteral("automationApplyHandles"));
  require(x1_handle && apply_handles && apply_handles->isVisible() &&
              x1_handle->setProperty("text", QStringLiteral("0.125")),
          "Bézier handle editor is missing");
  apply_handles->forceActiveFocus();
  QTest::keyClick(reopened_window, Qt::Key_Return);
  require(reopened_bridge.revision() == before_shape + 3 &&
              automation->segment_curve(lane_id, first_id, reopened_bridge.revision())
                      .value(QStringLiteral("x1"))
                      .toDouble() == 0.125,
          "keyboard Bézier Apply did not save the edited handle");
  auto* timebase =
      find_item(reopened_window->contentItem(), QStringLiteral("channelAutomationTimebase"));
  require(timebase && timebase->isVisible(), "channel automation timebase control is missing");
  const auto timebase_revision = reopened_bridge.revision();
  timebase->forceActiveFocus();
  QTest::keyClick(reopened_window, Qt::Key_Return);
  const auto changed_lane =
      reopened_bridge.tracks().front().toMap().value(QStringLiteral("channelAutomation")).toMap();
  require(reopened_bridge.revision() == timebase_revision + 1 &&
              changed_lane.value(QStringLiteral("timebase")) !=
                  channel_lane.value(QStringLiteral("timebase")) &&
              changed_lane.value(QStringLiteral("pointCount")).toInt() == 6,
          "keyboard timebase edit did not retain the channel automation points");
  auto* master_row = find_item(reopened_window->contentItem(), QStringLiteral("masterTrackRow"));
  require(master_row && master_row->isVisible(), "pinned master row disappeared");
  master_row->forceActiveFocus();
  QTest::keyClick(reopened_window, Qt::Key_Return);
  auto* master_level =
      find_item(reopened_window->contentItem(), QStringLiteral("masterLevelControl"));
  require(master_level && master_level->isVisible(), "master has no output level control");
  const auto master_revision = reopened_bridge.revision();
  master_level->forceActiveFocus();
  QTest::keyClick(reopened_window, Qt::Key_Left);
  require(reopened_bridge.revision() == master_revision + 1 && reopened_bridge.master_level() < 1.0,
          "keyboard master level was not one project edit");
  auto* track_row = find_item(reopened_window->contentItem(), QStringLiteral("trackRow"));
  require(track_row && track_row->isVisible(), "track row vanished after master selection");
  track_row->forceActiveFocus();
  QTest::keyClick(reopened_window, Qt::Key_Return);
  reopened_bridge.save();
  require(reopened_bridge.status().contains(QStringLiteral("saved"), Qt::CaseInsensitive),
          "muted and soloed track was not saved");
  auto reader = oscillate::persistence::ProjectStore::open_reader(root);
  require(reader.has_value(), "saved track controls reader did not open");
  auto saved_controls = reader.value().load_current();
  require(saved_controls.has_value(), "saved track controls did not reopen");
  require(saved_controls.value().snapshot.tracks.front().muted,
          "mute did not survive project reopen");
  require(saved_controls.value().snapshot.tracks.front().soloed,
          "solo did not survive project reopen");
  require(saved_controls.value().snapshot.tracks.front().level < 1.0 &&
              saved_controls.value().snapshot.tracks.front().pan < 0.0,
          "channel level and balance did not survive project reopen");
  require(saved_controls.value().snapshot.tracks.front().automation.size() == 1 &&
              saved_controls.value().snapshot.tracks.front().automation.front().points.size() ==
                  6 &&
              std::get<oscillate::project::TrackControlTarget>(
                  saved_controls.value().snapshot.tracks.front().automation.front().target)
                      .key == oscillate::project::TrackControlTarget::Key::channel_level,
          "channel automation did not survive project reopen");
  require(saved_controls.value().snapshot.master_level < 1.0,
          "master output level did not survive project reopen");
  remove = find_item(reopened_window->contentItem(), QStringLiteral("trackDeleteButton"));
  require(remove && remove->isVisible(), "track delete control vanished after mute");
  remove->forceActiveFocus();
  QTest::keyClick(reopened_window, Qt::Key_Return);
  require(reopened_bridge.tracks().isEmpty(), "keyboard delete did not remove selected track");
  reopened_bridge.undo();
  require(reopened_bridge.tracks().size() == 1 &&
              reopened_bridge.tracks().front().toMap().value(QStringLiteral("id")).toString() ==
                  track_id &&
              reopened_bridge.tracks().front().toMap().value(QStringLiteral("muted")).toBool() &&
              reopened_bridge.tracks().front().toMap().value(QStringLiteral("soloed")).toBool(),
          "one undo did not restore deleted muted and soloed track");
  reopened_bridge.set_track_soloed(track_id, false);
  reopened_bridge.set_track_level_pan(track_id, 1.0, 0.0);

  const auto audio_source = std::filesystem::path(directory.path().toStdString()) / "drop.wav";
  write_audio_source(audio_source);
  QMimeData audio_drop;
  audio_drop.setUrls({QUrl::fromLocalFile(QString::fromStdString(audio_source.string()))});
  const auto before_import = reopened_bridge.revision();
  send_drop(*reopened_window, audio_drop);
  require(QTest::qWaitFor(
              [&] {
                return reopened_bridge.revision() > before_import &&
                       !reopened_bridge.imports()->import_busy();
              },
              import_wait_ms),
          "dropped WAV did not create an audio track");
  require(reopened_bridge.tracks().size() == 2 &&
              reopened_bridge.tracks().front().toMap().value(QStringLiteral("id")).toString() ==
                  track_id &&
              reopened_bridge.tracks()[1].toMap().value(QStringLiteral("kind")).toString() ==
                  QStringLiteral("audio"),
          "audio drop did not append an audio track");
  auto* creation_row =
      find_item(reopened_window->contentItem(), QStringLiteral("trackCreationRow"));
  auto* track_list = find_item(reopened_window->contentItem(), QStringLiteral("trackList"));
  require(creation_row && track_list &&
              track_list->mapToScene({0, 0}).y() ==
                  creation_row->mapToScene({0, 0}).y() + creation_row->height(),
          "track list header moved after an audio drop");

  // A MIDI file dropped on an instrument track's lane plays on that track
  // from the bar under the pointer; on its header, from bar 1.
  const auto midi_source = std::filesystem::path(directory.path().toStdString()) / "idea.mid";
  write_midi_source(midi_source);
  QMimeData midi_drop;
  midi_drop.setUrls({QUrl::fromLocalFile(QString::fromStdString(midi_source.string()))});
  reopened_bridge.create_native_instrument_track(QStringLiteral("nova"), -1);
  QCoreApplication::processEvents();
  const auto midi_clips = [&](int row) {
    const auto clips =
        reopened_bridge.tracks()[row].toMap().value(QStringLiteral("clips")).toList();
    std::vector<QVariantMap> midi;
    for (const auto& clip : clips)
      if (clip.toMap().value(QStringLiteral("kind")).toString() == QStringLiteral("midi"))
        midi.push_back(clip.toMap());
    return midi;
  };
  require(reopened_bridge.tracks().size() == 3 &&
              reopened_bridge.tracks()[2].toMap().value(QStringLiteral("kind")).toString() ==
                  QStringLiteral("instrument") &&
              midi_clips(2).empty(),
          "an empty instrument track was not created for the MIDI drop");
  const auto lanes = find_items(reopened_window->contentItem(), QStringLiteral("arrangementRow"));
  const auto headers = find_items(reopened_window->contentItem(), QStringLiteral("trackRow"));
  require(lanes.size() == 3 && headers.size() == 3, "arrangement rows unavailable");
  // The file is read on a worker and lands when the import controller polls
  // it, so each drop waits for the import to finish before looking.
  const auto drop_midi = [&](QPoint at) {
    const auto before = reopened_bridge.revision();
    send_drop(*reopened_window, midi_drop, at);
    require(QTest::qWaitFor(
                [&] {
                  return reopened_bridge.revision() > before &&
                         !reopened_bridge.imports()->import_busy();
                },
                import_wait_ms),
            "the dropped MIDI file was not imported");
  };
  drop_midi(scene_point(*lanes[2], 0.75, 0.5));
  require(reopened_bridge.tracks().size() == 3 && midi_clips(2).size() == 1 &&
              midi_clips(2).front().value(QStringLiteral("start")).toDouble() > 0.0,
          "MIDI dropped on an instrument lane did not land on it at the bar under the pointer");
  drop_midi(scene_point(*headers[2], 0.5, 0.5));
  require(reopened_bridge.tracks().size() == 3 && midi_clips(2).size() == 2 &&
              std::ranges::any_of(midi_clips(2),
                                  [](const auto& clip) {
                                    return clip.value(QStringLiteral("start")).toDouble() == 0.0;
                                  }),
          "MIDI dropped on an instrument header did not land on it at bar 1");
  // Double-clicking a MIDI clip opens its notes, whichever dock tab was showing.
  auto* dock = find_item(reopened_window->contentItem(), QStringLiteral("deviceChainDock"));
  require(dock != nullptr, "device dock unavailable");
  dock->setProperty("activeTab", 0);
  QCoreApplication::processEvents();
  const auto handles =
      find_items(reopened_window->contentItem(), QStringLiteral("midiClipMoveHandle"));
  const auto lane_top = lanes[2]->mapToScene({0, 0}).y();
  const auto handle = std::ranges::find_if(handles, [&](QQuickItem* item) {
    const auto y = item->mapToScene({0, item->height() / 2}).y();
    return item->isEnabled() && item->isVisible() && y > lane_top &&
           y < lane_top + lanes[2]->height();
  });
  require(handle != handles.end(), "no MIDI clip to double-click");
  QTest::mouseDClick(reopened_window, Qt::LeftButton, Qt::NoModifier,
                     scene_point(**handle, 0.5, 0.5));
  QCoreApplication::processEvents();
  require(dock->property("activeTab").toInt() == 2 &&
              reopened_window->property("selectedTrack").toInt() == 2,
          "double-clicking a MIDI clip did not open its track's notes");
  // Not on an instrument track (here the audio track): new tracks.
  drop_midi(scene_point(*lanes[1], 0.5, 0.5));
  require(reopened_bridge.tracks().size() == 4 && !midi_clips(3).empty(),
          "MIDI dropped off an instrument track did not make a new track");
}

void paired_vst3_browser_insertion() {
  QTemporaryDir directory;
  require(directory.isValid(), "paired VST3 project directory unavailable");
  qputenv("XDG_CONFIG_HOME", (directory.path() + QStringLiteral("/config")).toUtf8());
  const auto root = std::filesystem::path(directory.path().toStdString()) / "paired-project";
  oscillate::plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  const auto source_scan = scanner.scan(VST3_INSTRUMENT_BUNDLE);
  require(source_scan.ok(), "paired VST3 source fixture scan failed");
  const auto& source_class = source_scan.metadata.classes.front();
  auto session = std::move(app::ProjectSession::create(root)).value();
  const auto first = session->instruments().create_vst3_instrument_track(
      {source_class.id, source_class.vendor, source_class.name, source_class.version,
       source_scan.binary.canonical_bundle.string(), source_scan.binary.sha256});
  require(first.has_value(), "initial VST3 source track could not be created");
  const auto permits = std::make_shared<BlockPermits>();
  {
    quick::ProjectBridge bridge(
        directory.path().toStdString(), nullptr,
        [permits](oscillate::audio_io::StereoRenderSource source,
                  oscillate::audio_io::TransportCursor& cursor) {
          return std::make_unique<PermittedOutput>(source, cursor, permits);
        },
        [] {
          return oscillate::foundation::Result<
              std::vector<oscillate::audio_io::PipeWireSink>>::success({});
        },
        {}, {},
        quick::ProjectBridge::Dependencies{.fixed_latency_vst3_binaries =
                                               std::vector{source_scan.binary.sha256}});
    bridge.set_project(std::move(session), root);
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("projectBridge", &bridge);
    engine.load(QUrl(QStringLiteral("qrc:/DawShell.qml")));
    require(!engine.rootObjects().isEmpty(), "paired VST3 shell did not load");
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().front());
    require(window != nullptr, "paired VST3 shell has no window");
    window->setProperty("pluginBrowserOpen", true);
    QCoreApplication::processEvents();
    auto* browser = find_item(window->contentItem(), QStringLiteral("vst3BrowserPanel"));
    require(browser && browser->property("newInstrumentEligible").toBool(),
            "one direct VST3 source could not add the second from the browser");

    bridge.vst3_browser()->add_vst3_search_path(QString::fromUtf8(VST3_GAIN_BUNDLE));
    require(QTest::qWaitFor([&] { return !bridge.vst3_browser()->busy(); }, 15'000),
            "paired VST3 effect scan timed out");
    bridge.vst3_browser()->add_vst3_search_path(QString::fromUtf8(VST3_INSTRUMENT_BUNDLE));
    require(QTest::qWaitFor([&] { return !bridge.vst3_browser()->busy(); }, 15'000),
            "paired VST3 instrument scan timed out");
    const auto entry = [&bridge](const QString& kind) -> QVariantMap {
      const auto entries = bridge.vst3_browser()->entries();
      const auto found = std::ranges::find_if(entries, [&](const auto& value) {
        const auto row = value.toMap();
        const auto expected_bundle = kind == QStringLiteral("effect")
                                         ? QString::fromUtf8(VST3_GAIN_BUNDLE)
                                         : QString::fromUtf8(VST3_INSTRUMENT_BUNDLE);
        return row.value(QStringLiteral("kind")) == kind &&
               row.value(QStringLiteral("bundle")) == expected_bundle &&
               row.value(QStringLiteral("state")) == QStringLiteral("ready");
      });
      return found == entries.end() ? QVariantMap{} : found->toMap();
    };
    const auto effect = entry(QStringLiteral("effect"));
    const auto instrument = entry(QStringLiteral("instrument"));
    require(!effect.isEmpty() && !instrument.isEmpty(), "paired VST3 catalogue entries missing");
    browser->setProperty("showInstruments", false);
    QCoreApplication::processEvents();
    require(browser->property("trackEligible").toBool(),
            "first direct VST3 source was not an effect target");
    const auto first_revision = bridge.revision();
    bridge.vst3_browser()->insert_vst3_effect(effect.value(QStringLiteral("index")).toInt(),
                                              effect.value(QStringLiteral("classId")).toString(),
                                              QString::fromStdString(first.value().to_string()));
    require(QTest::qWaitFor(
                [&] {
                  return !bridge.vst3_browser()->busy() && bridge.revision() == first_revision + 1;
                },
                15'000),
            "browser did not verify and save first source effect");
    require(browser->property("newInstrumentEligible").toBool(),
            "first source effect incorrectly blocked a second direct source");

    bridge.audio_output()->select_buffer_frames(64);
    QTimer callback_pump;
    QObject::connect(&callback_pump, &QTimer::timeout, [&] { permits->release(); });
    callback_pump.start(2);
    bridge.toggle_playback();
    require(QTest::qWaitFor([&] { return bridge.playback_active(); }, 15'000),
            "paired VST3 insertion playback did not start");
    require(QTest::qWaitFor([&] { return bridge.routed_graph_current(); }, 15'000),
            "initial source effect was not active before live insertion");
    const auto before_second = bridge.revision();
    bridge.vst3_browser()->create_vst3_instrument_track(
        instrument.value(QStringLiteral("index")).toInt(),
        instrument.value(QStringLiteral("classId")).toString());
    require(QTest::qWaitFor(
                [&] {
                  return !bridge.vst3_browser()->busy() && bridge.revision() == before_second + 1;
                },
                15'000) &&
                bridge.playback_active(),
            "browser did not save second source while playback continued");
    require(QTest::qWaitFor([&] { return bridge.playback_revision_inactive(); }, 15'000),
            "asymmetric source/effect pair was incorrectly reported audible");
    window->setProperty("selectedTrack", 1);
    browser->setProperty("showInstruments", false);
    QCoreApplication::processEvents();
    require(browser->property("trackEligible").toBool() &&
                browser->property("savedInsertState")
                    .toString()
                    .contains(QStringLiteral("saved, not yet audible")),
            "second source target or saved versus audible browser status is wrong");
    const auto second_id = bridge.tracks()[1].toMap().value(QStringLiteral("id")).toString();
    const auto before_second_effect = bridge.revision();
    bridge.vst3_browser()->insert_vst3_effect(effect.value(QStringLiteral("index")).toInt(),
                                              effect.value(QStringLiteral("classId")).toString(),
                                              second_id);
    require(QTest::qWaitFor(
                [&] {
                  return !bridge.vst3_browser()->busy() &&
                         bridge.revision() == before_second_effect + 1;
                },
                15'000) &&
                bridge.playback_active(),
            "browser did not save second source effect while playback continued");
    // Its track joined while playing, so it plays from the next Play too.
    require(QTest::qWaitFor([&] { return bridge.playback_revision_inactive(); }, 15'000) &&
                bridge.playback_active(),
            "an effect on a not-yet-audible track was reported live");
    // A chain takes any number of effects (ADR-0050).
    const auto full_revision = bridge.revision();
    bridge.vst3_browser()->insert_vst3_effect(effect.value(QStringLiteral("index")).toInt(),
                                              effect.value(QStringLiteral("classId")).toString(),
                                              second_id);
    require(QTest::qWaitFor(
                [&] {
                  return !bridge.vst3_browser()->busy() && bridge.revision() == full_revision + 1;
                },
                15'000),
            "browser refused a further effect on the chain");
    bridge.toggle_playback();
    require(QTest::qWaitFor([&] { return !bridge.playback_active(); }, 15'000),
            "paired VST3 playback did not stop");
    bridge.toggle_playback();
    require(QTest::qWaitFor([&] { return bridge.routed_graph_current(); }, 15'000),
            "saved paired VST3 layout did not become audible after Stop and Play");
    bridge.toggle_playback();
    require(QTest::qWaitFor([&] { return !bridge.playback_active(); }, 15'000),
            "restarted paired VST3 playback did not stop");
  }
  const auto reopened = app::ProjectSession::open(root);
  if (!reopened)
    throw std::runtime_error("paired VST3 reopen failed: " + reopened.error().message);
  if (reopened.value()->snapshot().tracks.size() != 2 ||
      reopened.value()->snapshot().tracks[0].effects.size() != 1 ||
      reopened.value()->snapshot().tracks[1].effects.size() != 2)
    throw std::runtime_error("paired VST3 source/effect layout did not survive save and reopen: " +
                             std::to_string(reopened.value()->snapshot().tracks.size()) +
                             " tracks");
}

void existing_vst3_target_browser_insertion() {
  QTemporaryDir directory;
  require(directory.isValid(), "existing VST3 target project directory unavailable");
  qputenv("XDG_CONFIG_HOME", (directory.path() + QStringLiteral("/config")).toUtf8());
  const auto root = std::filesystem::path(directory.path().toStdString()) / "existing-target";
  oscillate::plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  const auto scanned = scanner.scan(VST3_INSTRUMENT_BUNDLE);
  require(scanned.ok(), "existing-target VST3 source fixture scan failed");
  const auto& klass = scanned.metadata.classes.front();
  auto session = std::move(app::ProjectSession::create(root)).value();
  require(session->instruments().create_vst3_instrument_track(
              {klass.id, klass.vendor, klass.name, klass.version,
               scanned.binary.canonical_bundle.string(), scanned.binary.sha256}) &&
              session->tracks().add_instrument_track() && session->saving().save(),
          "existing empty instrument target was not saved");
  {
    quick::ProjectBridge bridge(directory.path().toStdString());
    bridge.set_project(std::move(session), root);
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("projectBridge", &bridge);
    engine.load(QUrl(QStringLiteral("qrc:/DawShell.qml")));
    require(!engine.rootObjects().isEmpty(), "existing-target Quick shell did not load");
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().front());
    require(window != nullptr, "existing-target Quick shell has no window");
    window->setProperty("selectedTrack", 1);
    window->setProperty("pluginBrowserOpen", true);
    QCoreApplication::processEvents();
    auto* browser = find_item(window->contentItem(), QStringLiteral("vst3BrowserPanel"));
    require(browser && browser->property("trackEligible").toBool(),
            "browser did not offer insertion into the exact empty second instrument track");
    bridge.vst3_browser()->add_vst3_search_path(QString::fromUtf8(VST3_INSTRUMENT_BUNDLE));
    require(QTest::qWaitFor([&] { return !bridge.vst3_browser()->busy(); }, 15'000),
            "existing-target VST3 scan timed out");
    const auto entries = bridge.vst3_browser()->entries();
    const auto found = std::ranges::find_if(entries, [](const auto& value) {
      const auto row = value.toMap();
      return row.value(QStringLiteral("kind")) == QStringLiteral("instrument") &&
             row.value(QStringLiteral("state")) == QStringLiteral("ready") &&
             row.value(QStringLiteral("bundle")) == QString::fromUtf8(VST3_INSTRUMENT_BUNDLE);
    });
    require(found != entries.end(), "existing-target browser source entry missing");
    const auto second_id = bridge.tracks()[1].toMap().value(QStringLiteral("id")).toString();
    const auto before = bridge.revision();
    bridge.vst3_browser()->insert_vst3_instrument(
        found->toMap().value(QStringLiteral("index")).toInt(),
        found->toMap().value(QStringLiteral("classId")).toString(), second_id);
    require(QTest::qWaitFor(
                [&] { return !bridge.vst3_browser()->busy() && bridge.revision() == before + 1; },
                15'000) &&
                bridge.tracks().size() == 2 &&
                bridge.tracks()[1].toMap().value(QStringLiteral("vst3SourceReady")).toBool(),
            "verified browser insertion did not bind the existing empty second track");
    require(!browser->property("trackEligible").toBool(),
            "browser offered a third source after filling the second track");
  }
  const auto reopened = app::ProjectSession::open(root);
  require(reopened && reopened.value()->snapshot().tracks.size() == 2 &&
              std::holds_alternative<oscillate::project::Vst3PluginInstance>(
                  reopened.value()->snapshot().tracks[1].instrument_source),
          "existing-target VST3 insertion did not reopen with the saved source identity");
}
} // namespace

int main(int argc, char** argv) {
  QApplication application(argc, argv);
  try {
    run();
    paired_vst3_browser_insertion();
    existing_vst3_target_browser_insertion();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Quick track creation: " << error.what() << '\n';
    return 1;
  }
}
