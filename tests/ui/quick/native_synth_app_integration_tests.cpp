#include "arrangement_automation_controller.hpp"
#include "native_synth_automation_controller.hpp"
#include "native_synth_device_controller.hpp"
#include "panel_fit_checks.hpp"
#include "panel_kit_checks.hpp"
#include "panel_text_checks.hpp"
#include "project_bridge.hpp"

#include "../../native_dsp/bow_test_library.hpp"
#include "../../native_dsp/concert_test_library.hpp"
#include "../../native_dsp/fret_test_library.hpp"
#include "../../native_dsp/wren_test_library.hpp"
#include "oscillate/application/project_session.hpp"
#include "oscillate/audio_io/stereo_block.hpp"
#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/native_dsp/synth/knock.hpp"
#include "oscillate/test/check.hpp"

#include <QColor>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

namespace {
namespace app = oscillate::application;
namespace native = oscillate::native_dsp::synth;
namespace project = oscillate::project;
namespace quick = oscillate::quick;
namespace audio = oscillate::audio_io;
namespace engine = oscillate::engine;

using oscillate::test::require;

// The scene point at the centre of grid step `step` on the piano roll's row
// for `key`: each row is one hit area ("pianoRollRow-<key>") that works the
// step out from x. Empty when the row is missing or hidden.
// The arguments are named at every call.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::optional<QPointF> piano_roll_cell(QQuickItem* root, int step, int key) {
  const auto name = QStringLiteral("pianoRollRow-%1").arg(key);
  QQuickItem* row = nullptr;
  std::function<void(QQuickItem*)> visit = [&](QQuickItem* item) {
    if (item->objectName() == name)
      row = item;
    if (row)
      return;
    for (auto* child : item->childItems())
      visit(child);
  };
  visit(root);
  if (!row || !row->isVisible())
    return std::nullopt;
  const auto columns = row->property("columns").toInt();
  if (step < 0 || step >= columns)
    return std::nullopt;
  return row->mapToScene({row->width() * (step + 0.5) / columns, row->height() / 2});
}

struct ConcertProbe {
  std::atomic<int> blocks{0};
  std::atomic<float> peak{0.0F};
};

class ConcertOutput final : public app::PlaybackOutput {
public:
  ConcertOutput(audio::StereoRenderSource source, audio::TransportCursor& cursor,
                std::shared_ptr<ConcertProbe> probe)
      : source_(source), cursor_(cursor), probe_(std::move(probe)) {}

  // The PlaybackOutput interface's rate and block size.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  [[nodiscard]] oscillate::foundation::Result<void> start(std::uint32_t rate, std::uint32_t frames,
                                                          std::optional<std::uint64_t>) override {
    if (frames == 0 || frames > app::max_playback_buffer_frames)
      return oscillate::foundation::Result<void>::failure(
          {oscillate::foundation::ErrorCode::invalid_argument, "Invalid preview block size"});
    std::array<engine::StereoFrame, app::max_playback_buffer_frames> scratch{};
    std::array<float, 2 * std::size_t{app::max_playback_buffer_frames}> samples{};
    float peak = 0.0F;
    for (int block = 0; block < 64; ++block) {
      if (audio::render_transport_block(source_, cursor_, true, std::span(scratch).first(frames),
                                        std::span(samples).first(std::size_t{frames} * 2)) !=
          audio::BlockResult::rendered)
        return oscillate::foundation::Result<void>::failure(
            {oscillate::foundation::ErrorCode::internal, "Concert callback failed"});
      for (std::size_t i = 0; i < static_cast<std::size_t>(frames) * 2; ++i)
        peak = std::max(peak, std::fabs(samples[i]));
    }
    // A note drawn in the piano roll may start a monitor run first (live
    // notes play through the graph); keep the loudest start.
    probe_->peak.store(std::max(peak, probe_->peak.load(std::memory_order_acquire)),
                       std::memory_order_release);
    probe_->blocks.store(64, std::memory_order_release);
    status_.state = audio::OutputState::streaming;
    status_.requested_client_rate = rate;
    return oscillate::foundation::Result<void>::success();
  }
  void stop() noexcept override { status_.state = audio::OutputState::stopped; }
  [[nodiscard]] audio::OutputStatus status() const noexcept override { return status_; }

private:
  audio::StereoRenderSource source_;
  audio::TransportCursor& cursor_;
  std::shared_ptr<ConcertProbe> probe_;
  audio::OutputStatus status_;
};

void concert_note_plays_from_the_quick_shell() {
  QTemporaryDir directory;
  require(directory.isValid(), "Concert scratch directory unavailable");
  const auto root = std::filesystem::path(directory.path().toStdString()) / "concert-playback";
  const auto existing_project = qEnvironmentVariable("OSCILLATE_CONCERT_PROJECT");
  if (!existing_project.isEmpty())
    std::filesystem::copy(existing_project.toStdString(), root,
                          std::filesystem::copy_options::recursive);
  auto created = existing_project.isEmpty() ? app::ProjectSession::create(root)
                                            : app::ProjectSession::open(root);
  require(created.has_value(), "Concert project setup failed");
  const bool real_output = qEnvironmentVariableIsSet("OSCILLATE_CONCERT_REAL_OUTPUT");
  auto probe = std::make_shared<ConcertProbe>();
  app::PlaybackOutputFactory output;
  if (!real_output)
    output = [probe](audio::StereoRenderSource source, audio::TransportCursor& cursor) {
      return std::make_unique<ConcertOutput>(source, cursor, probe);
    };
  quick::ProjectBridge bridge(directory.path().toStdString(), nullptr, output);
  bridge.set_project(std::move(created).value(), root);
  if (real_output) {
    bridge.set_master_level(0.12);
    const auto serial = qEnvironmentVariable("OSCILLATE_CONCERT_OUTPUT_SERIAL");
    if (!serial.isEmpty()) {
      require(QTest::qWaitFor(
                  [&] {
                    return !bridge.audio_output()->audio_device_busy() &&
                           std::ranges::any_of(
                               bridge.audio_output()->audio_devices(), [&](const auto& device) {
                                 return device.toMap().value(QStringLiteral("serial")).toString() ==
                                        serial;
                               });
                  },
                  5'000),
              "Requested physical output was not discovered");
      bridge.audio_output()->select_audio_device(serial);
      require(bridge.audio_output()->selected_audio_device() == serial,
              "Physical output selection failed");
    }
  }
  if (existing_project.isEmpty())
    require(QMetaObject::invokeMethod(bridge.track_commands(), "add_synth_track"),
            "Concert track action failed");
  std::size_t track_index = 0;
  if (!existing_project.isEmpty()) {
    bool found = false;
    for (int i = 0; i < bridge.tracks().size(); ++i) {
      if (bridge.tracks().at(i).toMap().value("nativeEngineId").toString() ==
          QStringLiteral("concert")) {
        track_index = static_cast<std::size_t>(i);
        found = true;
      }
    }
    require(found, "Existing Concert track is missing");
  }
  const auto track =
      bridge.tracks().at(static_cast<int>(track_index)).toMap().value("id").toString();
  require(!track.isEmpty(), "Concert track has no ID");
  QQmlApplicationEngine qml;
  qml.rootContext()->setContextProperty("projectBridge", &bridge);
  qml.load(QUrl(QStringLiteral("qrc:/DawShell.qml")));
  require(!qml.rootObjects().isEmpty(), "Concert shell failed to load");
  auto* window = qobject_cast<QQuickWindow*>(qml.rootObjects().front());
  require(window != nullptr, "Concert shell has no window");
  auto* play =
      qobject_cast<QQuickItem*>(window->findChild<QObject*>(QStringLiteral("togglePlayback")));
  require(play != nullptr, "Concert shell has no Play control");
  if (existing_project.isEmpty())
    bridge.select_native_instrument(track, QStringLiteral("concert"));
  else
    bridge.open_native_synth_editor(track);
  require(bridge.tracks()
                  .at(static_cast<int>(track_index))
                  .toMap()
                  .value("nativeEngineId")
                  .toString() == QStringLiteral("concert"),
          "Concert was not selected in the app");
  bridge.add_midi_clip(track, 0);
  const auto clips =
      bridge.tracks().at(static_cast<int>(track_index)).toMap().value("clips").toList();
  require(clips.size() == 1, "Concert MIDI clip action failed");
  const auto clip = clips.front().toMap().value("id").toString();
  window->setProperty("selectedClipId", clip);
  QCoreApplication::processEvents();
  const auto note_cell = piano_roll_cell(window->contentItem(), 0, 60);
  if (!note_cell) {
    throw std::runtime_error("Concert note cell is unavailable");
  }
  QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, note_cell->toPoint());
  require(bridge.tracks()
                  .at(static_cast<int>(track_index))
                  .toMap()
                  .value("clips")
                  .toList()
                  .front()
                  .toMap()
                  .value("notes")
                  .toList()
                  .size() == 1,
          "Concert note action failed");
  require(
      QTest::qWaitFor(
          [&] { return window->findChild<QObject*>(QStringLiteral("concertPanel")) != nullptr; },
          2'000),
      "Concert editor did not load");
  QCoreApplication::processEvents();
  QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                    play->mapToScene(QPointF(40, play->height() / 2)).toPoint());
  if (real_output) {
    require(QTest::qWaitFor([&] { return bridge.routed_graph_current(); }, 10'000),
            "Concert audio device did not start");
    QTest::qWait(2'000);
    require(bridge.playback_active() &&
                !bridge.playback_status().contains(QStringLiteral("Audio output errors")),
            "Concert physical playback stopped or reported an output error");
  } else {
    if (!QTest::qWaitFor(
            [&] {
              return bridge.playback_active() &&
                     probe->peak.load(std::memory_order_acquire) > 0.0001F;
            },
            10'000) &&
        probe->blocks.load(std::memory_order_acquire) != 64)
      throw std::runtime_error("Concert callback did not start: " +
                               bridge.playback_status().toStdString());
    if (probe->peak.load(std::memory_order_acquire) <= 0.0001F)
      throw std::runtime_error("Concert note was silent: peak=" +
                               std::to_string(probe->peak.load(std::memory_order_acquire)) +
                               " status=" + bridge.playback_status().toStdString());
  }
  // The docked editor sits in the device chain; bring it back from the MIDI
  // editor before reading its live view.
  if (auto* dock = window->findChild<QObject*>(QStringLiteral("deviceChainDock")))
    dock->setProperty("activeTab", 0);
  QCoreApplication::processEvents();
  auto* panel = window->findChild<QObject*>(QStringLiteral("concertPanel"));
  require(
      QTest::qWaitFor(
          [&] { return panel->property("live").toMap().value(QStringLiteral("live")).toBool(); },
          2'000),
      "Concert editor did not receive live telemetry");
  bridge.toggle_playback();
  require(QTest::qWaitFor([&] { return !bridge.playback_active(); }, 10'000),
          "Concert playback did not stop");
}

// The arguments are named at every call.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void engine_is_reachable_and_saves_from_the_app(std::string_view engine_id,
                                                std::string_view cutoff_id,
                                                const QString& panel_name) {
  QTemporaryDir directory;
  require(directory.isValid(), "scratch directory unavailable");
  const auto root =
      std::filesystem::path(directory.path().toStdString()) / (std::string(engine_id) + "-app");
  auto created = app::ProjectSession::create(root);
  require(created.has_value(), "project creation failed");
  std::string track_id;
  {
    quick::ProjectBridge bridge(directory.path().toStdString());
    bridge.set_project(std::move(created).value(), root);
    require(QMetaObject::invokeMethod(bridge.track_commands(), "add_synth_track"),
            "add MIDI track action unavailable");
    require(bridge.tracks().size() == 1 &&
                bridge.tracks().front().toMap().value("nativeEngineId").toString() ==
                    QStringLiteral("prism"),
            "new MIDI track did not start with the default synth");
    track_id = bridge.tracks().front().toMap().value("id").toString().toStdString();
    QQmlApplicationEngine qml;
    qml.rootContext()->setContextProperty("projectBridge", &bridge);
    qml.load(QUrl(QStringLiteral("qrc:/DawShell.qml")));
    require(!qml.rootObjects().isEmpty(), "DAW shell did not load");
    auto* window = qobject_cast<QQuickWindow*>(qml.rootObjects().front());
    require(window != nullptr, "DAW shell has no window");
    QCoreApplication::processEvents();
    require(window->findChild<QObject*>(QStringLiteral("nativeSynthSourceCard")) != nullptr,
            "built-in synth source card is missing");
    if (engine_id != "prism") {
      // Track details are optional in the default workspace; open them before
      // choosing a different built-in instrument through the source card.
      auto* details = qobject_cast<QQuickItem*>(
          window->findChild<QObject*>(QStringLiteral("deviceContextButton")));
      require(details != nullptr, "track details action is missing");
      QTest::mouseClick(
          window, Qt::LeftButton, Qt::NoModifier,
          details->mapToScene({details->width() / 2, details->height() / 2}).toPoint());
      QCoreApplication::processEvents();
      QQuickItem* switch_button = nullptr;
      std::function<void(QQuickItem*)> find_switch = [&](QQuickItem* item) {
        if (item->objectName() == QStringLiteral("switchNativeSynthButton") && item->isVisible() &&
            item->parent()->property("modelData").toMap().value("id").toString() ==
                QString::fromStdString(std::string(engine_id))) {
          switch_button = item;
        }
        for (auto* child : item->childItems()) {
          find_switch(child);
        }
      };
      find_switch(window->contentItem());
      require(switch_button != nullptr && switch_button->isVisible(),
              "built-in synth switch is not available in the inspector");
      QTest::mouseClick(
          window, Qt::LeftButton, Qt::NoModifier,
          switch_button
              ->mapToScene(QPointF(switch_button->width() / 2, switch_button->height() / 2))
              .toPoint());
    } else {
      bridge.open_native_synth_editor(QString::fromStdString(track_id));
    }
    QCoreApplication::processEvents();
    auto* panel = window->findChild<QObject*>(panel_name);
    require(panel != nullptr && panel->property("visible").toBool(),
            "selected synth panel did not open inside the DAW");
    // Panels dock by default; this checks the floating, movable surface.
    if (auto* overlay = window->findChild<QObject*>(QStringLiteral("nativeSynthEditorOverlay")))
      overlay->setProperty("docked", false);
    QCoreApplication::processEvents();
    auto* floating = qobject_cast<QQuickItem*>(
        window->findChild<QObject*>(QStringLiteral("nativeSynthFloatingFrame")));
    require(floating != nullptr && floating->isVisible() && floating->width() < window->width(),
            "synth editor does not use a floating, modeless surface");
    auto* drag_handle = qobject_cast<QQuickItem*>(
        window->findChild<QObject*>(QStringLiteral("nativeSynthDragHandle")));
    require(drag_handle != nullptr && drag_handle->isVisible(),
            "shared editor drag handle is missing");
    const auto start_x = floating->x();
    const QPoint drag_start =
        drag_handle->mapToScene({drag_handle->width() / 2, drag_handle->height() / 2}).toPoint();
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, drag_start);
    QTest::mouseMove(window, drag_start + QPoint(12, 0));
    QTest::mouseMove(window, drag_start + QPoint(40, 0));
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, drag_start + QPoint(40, 0));
    QCoreApplication::processEvents();
    require(floating->x() > start_x + 20, "synth editor could not be moved within the DAW");
    auto* tempo = qobject_cast<QQuickItem*>(
        window->findChild<QObject*>(QStringLiteral("projectInitialTempo")));
    require(tempo != nullptr && tempo->isVisible(), "project timing is not accessible");
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                      tempo->mapToScene({tempo->width() / 2, tempo->height() / 2}).toPoint());
    QCoreApplication::processEvents();
    auto* tempo_entry = window->findChild<QObject*>(QStringLiteral("projectInitialTempoEntry"));
    require(tempo_entry != nullptr && tempo_entry->property("visible").toBool() &&
                bridge.native_synth_controller() != nullptr,
            "parent DAW controls cannot be used while the synth editor remains open");
    QTest::keyClick(window, Qt::Key_1);
    QTest::keyClick(window, Qt::Key_2);
    QTest::keyClick(window, Qt::Key_8);
    QTest::keyClick(window, Qt::Key_Return);
    QCoreApplication::processEvents();
    require(std::fabs(bridge.project_timing().value("projectBpm").toDouble() - 128.0) < 1e-9 &&
                bridge.native_synth_controller() != nullptr && panel->property("visible").toBool(),
            "changing project tempo closed the synth editor or failed to update the DAW");
    const auto* engine = native::find_engine(engine_id);
    require(engine != nullptr, "selected engine is not registered");
    const auto track = bridge.tracks().front().toMap();
    require(track.value("nativeEngineId").toString() ==
                    QString::fromStdString(std::string(engine_id)) &&
                bridge.insert_chains().front().toMap().value("sourceName").toString() ==
                    QString::fromStdString(std::string(engine->descriptor->name)),
            "selected synth is missing from the track and chain projections");
    auto* controller =
        qobject_cast<quick::NativeSynthDeviceController*>(bridge.native_synth_controller());
    require(controller != nullptr && controller->editable() &&
                controller->engine_id() == QString::fromStdString(std::string(engine_id)),
            "selected synth editor is not editable");
    // The panel keeps no automation form: Automate shows the synth's lane
    // on the arrangement, where it is drawn (ADR-0051).
    auto* automation =
        qobject_cast<quick::NativeSynthAutomationController*>(bridge.native_synth_automation());
    require(automation != nullptr && automation->track_id() == track.value("id").toString(),
            "the synth's arrangement lane target is unavailable");
    const auto first_key =
        QStringLiteral("synth:") +
        QString::fromStdString(
            std::string(std::ranges::find_if(engine->descriptor->parameters, [](const auto& item) {
                          return item.automatable;
                        })->id));
    require(automation->lane_key() == first_key,
            "an unautomated synth did not offer its first automatable parameter");
    require(window->findChild<QObject*>(QStringLiteral("nativeSynthAutomationEditor")) == nullptr &&
                window->findChild<QObject*>(QStringLiteral("nativeSynthAutomationToggle")) ==
                    nullptr,
            "the synth panel still carries an automation form");
    auto* show_automation = qobject_cast<QQuickItem*>(
        window->findChild<QObject*>(QStringLiteral("nativeSynthShowAutomation")));
    require(show_automation != nullptr && show_automation->isVisible(),
            "the synth panel has no Automate action");
    const auto before_show = bridge.revision();
    QTest::mouseClick(
        window, Qt::LeftButton, Qt::NoModifier,
        show_automation
            ->mapToScene(QPointF(show_automation->width() / 2, show_automation->height() / 2))
            .toPoint());
    QCoreApplication::processEvents();
    auto* overlay = qobject_cast<QQuickItem*>(
        window->findChild<QObject*>(QStringLiteral("arrangementAutomationOverlay")));
    require(bridge.revision() == before_show && window->property("currentView") == "arrange" &&
                overlay != nullptr && overlay->isVisible() &&
                overlay->property("laneKey").toString() == first_key,
            "Automate did not show the synth's lane over its clips, without an edit");
    const auto parameter = QString::fromStdString(std::string(cutoff_id));
    controller->set_normalised(parameter, 0.62);
    require(std::fabs(controller->normalised(parameter) - 0.62) < 1e-9,
            "synth cutoff did not change");
    bridge.undo();
    require(std::fabs(controller->normalised(parameter) - 0.62) > 0.01,
            "undo did not refresh the open synth editor");
    bridge.redo();
    require(std::fabs(controller->normalised(parameter) - 0.62) < 1e-9,
            "redo did not restore the synth edit");
    // The cutoff's lane on the arrangement: added, typed and cleared there.
    auto* lanes =
        qobject_cast<quick::ArrangementAutomationController*>(bridge.arrangement_automation());
    const auto track_key = track.value("id").toString();
    const auto cutoff_key = QStringLiteral("synth:") + parameter;
    const auto cutoff_lane = [&] {
      for (const auto& entry : lanes->tracks()) {
        for (const auto& item : entry.toMap().value("lanes").toList()) {
          if (item.toMap().value("key").toString() == cutoff_key)
            return item.toMap();
        }
      }
      return QVariantMap{};
    };
    constexpr double quarter_travel = 0.25;
    constexpr double three_quarter_travel = 0.75;
    const auto revision = bridge.revision();
    const auto point_id = lanes->add_point(track_key, cutoff_key, 0.0, quarter_travel);
    require(!point_id.isEmpty() && bridge.revision() == revision + 1 &&
                cutoff_lane().value("points").toList().size() == 1,
            "the synth cutoff lane was not created on the arrangement");
    require(automation->lane_key() == cutoff_key,
            "Automate does not lead to the synth's automated parameter");
    const auto lane_id = cutoff_lane().value("laneId").toString();
    const auto typed = lanes->value_from_text(
        track_key, cutoff_key, lanes->value_text(track_key, cutoff_key, three_quarter_travel));
    // The text is rounded to the display's decimals.
    constexpr double display_rounding = 0.01;
    require(std::fabs(typed - three_quarter_travel) < display_rounding,
            "a synth value did not read back from its own text");
    lanes->set_point_value(lane_id, point_id, three_quarter_travel);
    require(
        std::fabs(cutoff_lane().value("points").toList().front().toMap().value("value").toDouble() -
                  three_quarter_travel) < 1e-9,
        "edited synth automation value was not projected");
    lanes->toggle_timebase(lane_id);
    require(cutoff_lane().value("timebase").toString() == QStringLiteral("Elapsed"),
            "synth automation timebase did not change");
    lanes->toggle_timebase(lane_id);
    lanes->clear_lane(lane_id);
    require(cutoff_lane().isEmpty(), "synth automation lane could not be cleared");
    bridge.undo();
    require(cutoff_lane().value("points").toList().size() == 1, "clearing the lane did not undo");
    bridge.close_native_synth_editor();
    QCoreApplication::processEvents();
    require(bridge.native_synth_controller() == nullptr, "synth editor did not close");
  }
  auto reopened = app::ProjectSession::open(root);
  require(reopened.has_value(), "synth project did not reopen");
  const auto track = project::TrackId::parse(track_id);
  require(track.has_value(), "created track ID is invalid");
  const auto* saved = std::get_if<project::NativeInstrumentPatch>(
      &reopened.value()->snapshot().tracks.front().instrument_source);
  require(saved && saved->engine == engine_id, "selected synth identity did not reopen");
  const auto values = reopened.value()->instruments().native_instrument_values(track.value());
  require(values.has_value(), "saved synth patch is missing");
  const auto* engine = native::find_engine(engine_id);
  const auto index = oscillate::native_dsp::find_parameter(*engine->descriptor, cutoff_id);
  if (!index) {
    throw std::runtime_error("the cutoff parameter is missing");
  }
  require(std::fabs(values.value()[*index] - oscillate::native_dsp::from_normalised(
                                                 engine->descriptor->parameters[*index], 0.62)) <
              1e-9,
          "edited synth cutoff did not survive save and reopen");
  const auto& lanes = reopened.value()->snapshot().tracks.front().automation;
  const auto lane = std::ranges::find_if(lanes, [&](const project::AutomationLane& item) {
    const auto* target = std::get_if<project::NativeParameterTarget>(&item.target);
    return target && target->parameter_id == cutoff_id;
  });
  require(lane != lanes.end() && lane->points.size() == 1 &&
              std::fabs(oscillate::native_dsp::to_normalised(engine->descriptor->parameters[*index],
                                                             lane->points.front().value) -
                        0.75) < 1e-9,
          "synth automation did not survive save and reopen");
}

void all_registered_panels_open_in_the_daw() {
  QTemporaryDir directory;
  require(directory.isValid(), "synth panel scratch directory unavailable");
  const auto root = std::filesystem::path(directory.path().toStdString()) / "panels";
  auto created = app::ProjectSession::create(root);
  require(created.has_value(), "synth panel project creation failed");
  quick::ProjectBridge bridge(directory.path().toStdString());
  bridge.set_project(std::move(created).value(), root);
  require(QMetaObject::invokeMethod(bridge.track_commands(), "add_synth_track"),
          "synth panel track creation failed");
  const auto track_id = bridge.tracks().front().toMap().value("id").toString();
  QQmlApplicationEngine qml;
  qml.rootContext()->setContextProperty("projectBridge", &bridge);
  qml.load(QUrl(QStringLiteral("qrc:/DawShell.qml")));
  require(!qml.rootObjects().isEmpty(), "synth panel shell did not load");
  auto* window = qobject_cast<QQuickWindow*>(qml.rootObjects().front());
  require(window != nullptr, "synth panel shell has no window");
  window->resize(1600, 1000);
  for (const auto* engine : native::engines()) {
    const auto id = QString::fromStdString(std::string(engine->descriptor->id));
    bridge.select_native_instrument(track_id, id);
    QCoreApplication::processEvents();
    const auto panel_name = id + QStringLiteral("Panel");
    auto* panel = qobject_cast<QQuickItem*>(window->findChild<QObject*>(panel_name));
    require(panel != nullptr && panel->property("visible").toBool() &&
                bridge.native_synth_controller() != nullptr,
            ("registered synth panel did not open in the DAW: " + id.toStdString()).c_str());
    // Every value the panel shows follows the display formatter.
    oscillate::test::panel_text::require_formatted_values(panel, id.toStdString());
    // As the effects' app contract checks: every label shows whole, and no
    // text is drawn over other text or clipped.
    oscillate::test::panel_fit::require_text_fits(panel, id.toStdString());
    // The panel is built from the kit's shared parts: one header with its
    // own readout and the one status badge; inactive and linked controls
    // say why. A touched control's readout fits the header too.
    require(!engine->descriptor->parameters.empty(), "the instrument has parameters");
    const auto touched =
        QString::fromStdString(std::string(engine->descriptor->parameters.front().id));
    oscillate::test::panel_kit::require_device_header(panel, id.toStdString(), touched);
    oscillate::test::panel_kit::require_control_states(panel, id.toStdString());
    oscillate::test::panel_kit::require_plot_frames(panel, id.toStdString());
    oscillate::test::panel_kit::require_pointer_controls(panel, id.toStdString());
    oscillate::test::panel_kit::with_touched(panel, touched, [&] {
      oscillate::test::panel_fit::require_text_fits(panel,
                                                    id.toStdString() + " (a control touched)");
    });
    oscillate::test::panel_kit::require_panel_zoom(*window, id.toStdString(),
                                                   QStringLiteral("nativeSynthFloatingFrame"),
                                                   QStringLiteral("nativeSynthPanelHost"));
  }
}

// The visible item named `name` under `root`, or null.
QQuickItem* find_visible(QQuickItem* root, const QString& name) {
  if (root->objectName() == name && root->isVisible())
    return root;
  for (auto* child : root->childItems()) {
    if (auto* found = find_visible(child, name))
      return found;
  }
  return nullptr;
}

// ADR-0076: the floating instrument window resizes from its corner grip and
// Nova reflows to it (its laid-out width follows the window); the window's
// panel zoom chips (ADR-0103) scale the panel. Both are kept in the workspace preferences, which
// this test returns to their defaults so the other tests see a fresh window.
void nova_window_resizes_and_zooms_in_the_daw() {
  QTemporaryDir directory;
  require(directory.isValid(), "nova window scratch directory unavailable");
  const auto root = std::filesystem::path(directory.path().toStdString()) / "nova-window";
  auto created = app::ProjectSession::create(root);
  require(created.has_value(), "nova window project creation failed");
  quick::ProjectBridge bridge(directory.path().toStdString());
  bridge.set_project(std::move(created).value(), root);
  auto* layout = bridge.workspace_layout();
  const auto reset_layout = [layout] {
    layout->setProperty("instrumentWindowWidth", 0);
    layout->setProperty("instrumentWindowHeight", 0);
    layout->setProperty("panelZoom", 1.0);
  };
  reset_layout();
  struct Restore {
    std::function<void()> undo;
    ~Restore() { undo(); }
  } restore{reset_layout};
  require(QMetaObject::invokeMethod(bridge.track_commands(), "add_synth_track"),
          "nova window track creation failed");
  const auto track_id = bridge.tracks().front().toMap().value("id").toString();
  QQmlApplicationEngine qml;
  qml.rootContext()->setContextProperty("projectBridge", &bridge);
  qml.load(QUrl(QStringLiteral("qrc:/DawShell.qml")));
  require(!qml.rootObjects().isEmpty(), "nova window shell did not load");
  auto* window = qobject_cast<QQuickWindow*>(qml.rootObjects().front());
  require(window != nullptr, "nova window shell has no window");
  bridge.select_native_instrument(track_id, QStringLiteral("nova"));
  QCoreApplication::processEvents();
  auto* overlay = window->findChild<QObject*>(QStringLiteral("nativeSynthEditorOverlay"));
  require(overlay != nullptr, "the instrument overlay is missing");
  overlay->setProperty("docked", false);
  QCoreApplication::processEvents();
  auto* panel = qobject_cast<QQuickItem*>(window->findChild<QObject*>(QStringLiteral("novaPanel")));
  auto* host = qobject_cast<QQuickItem*>(
      window->findChild<QObject*>(QStringLiteral("nativeSynthPanelHost")));
  auto* frame = qobject_cast<QQuickItem*>(
      window->findChild<QObject*>(QStringLiteral("nativeSynthFloatingFrame")));
  require(panel != nullptr && panel->isVisible() && host != nullptr && frame != nullptr,
          "Nova did not open in a floating window");
  // Nova's window wears its own neutral graphite, not the frame's blue-grey.
  auto* chrome = overlay->property("chrome").value<QObject*>();
  require(chrome != nullptr &&
              overlay->property("titleBarColor").value<QColor>() ==
                  chrome->property("card").value<QColor>() &&
              overlay->property("frameColor").value<QColor>() ==
                  chrome->property("plateMid").value<QColor>(),
          "Nova's window chrome is graphite");
  require(find_visible(panel, QStringLiteral("novaKeyStrip")) != nullptr, "Nova has its key strip");
  const auto click_named = [&](QQuickItem* under, const QString& name) {
    auto* target = find_visible(under, name);
    require(target != nullptr, "a Nova window control is missing");
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                      target->mapToScene({target->width() / 2, target->height() / 2}).toPoint());
    QCoreApplication::processEvents();
  };

  click_named(frame, QStringLiteral("panelZoom:100"));
  const double full = host->scale();
  click_named(frame, QStringLiteral("panelZoom:90"));
  require(host->scale() < full - 0.05 && std::fabs(host->scale() - 0.9) < 0.01,
          "the 90% chip draws the panel at 90%");
  require(std::fabs(layout->property("panelZoom").toDouble() - 0.9) < 1e-9,
          "the zoom is kept in the workspace preferences");
  click_named(frame, QStringLiteral("panelZoom:100"));
  require(std::fabs(host->scale() - full) < 1e-9, "100% restores the scale");

  // Narrow the window from its grip: Nova lays itself out narrower, at the
  // same scale, instead of shrinking.
  auto* grip = find_visible(frame, QStringLiteral("floatingDeviceResizeGrip"));
  require(grip != nullptr, "the floating window has no resize grip");
  const double before_width = panel->width();
  const double before_frame = frame->width();
  const double scale = host->scale();
  const QPoint from = grip->mapToScene({grip->width() / 2, grip->height() / 2}).toPoint();
  const QPoint to = from - QPoint(100, 0);
  QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, from);
  for (int step = 1; step <= 5; ++step)
    QTest::mouseMove(window, from + (to - from) * step / 5);
  QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, to);
  QCoreApplication::processEvents();
  require(std::fabs(frame->width() - (before_frame - 100)) < 2, "the grip narrows the window");
  require(std::fabs(host->scale() - scale) < 1e-9 &&
              std::fabs(panel->width() - (before_width - 100 / scale)) < 2,
          "Nova reflows to the narrower window at the same scale");
  require(layout->property("instrumentWindowWidth").toInt() ==
              static_cast<int>(std::lround(frame->width())),
          "the window size is kept in the workspace preferences");
}

void pulse_drum_lanes_share_the_piano_roll_and_saved_notes() {
  QTemporaryDir directory;
  require(directory.isValid(), "drum-lane scratch directory unavailable");
  const auto root = std::filesystem::path(directory.path().toStdString()) / "drum-lanes";
  auto created = app::ProjectSession::create(root);
  require(created.has_value(), "drum-lane project creation failed");
  {
    quick::ProjectBridge bridge(directory.path().toStdString());
    bridge.set_project(std::move(created).value(), root);
    const auto pad_lanes = bridge.native_synth_note_lanes(QStringLiteral("pulse"));
    require(pad_lanes.size() == 12 && pad_lanes.front().toMap().value("key").toInt() == 36 &&
                pad_lanes.back().toMap().value("key").toInt() == 47 &&
                !pad_lanes.front().toMap().value("name").toString().isEmpty(),
            "Pulse pad lanes do not match its registered MIDI note map");
    require(bridge.native_synth_note_lanes(QStringLiteral("prism")).isEmpty() &&
                bridge.native_synth_note_lanes(QStringLiteral("unknown")).isEmpty(),
            "non-drum instruments were given Pulse lanes");
    // Knock's pads, named "Pad 1".. without a track to read their sounds from.
    const auto knock_lanes = bridge.native_synth_note_lanes(QStringLiteral("knock"));
    require(knock_lanes.size() == static_cast<qsizetype>(native::knock::pad_count) &&
                knock_lanes.front().toMap().value("key").toInt() == native::knock::first_key &&
                knock_lanes.front().toMap().value("name").toString() == QStringLiteral("Pad 1"),
            "Knock pad lanes do not match its MIDI note map");
    require(QMetaObject::invokeMethod(bridge.track_commands(), "add_synth_track"),
            "drum-lane track creation failed");
    const auto track_id = bridge.tracks().front().toMap().value("id").toString();
    bridge.select_native_instrument(track_id, QStringLiteral("pulse"));
    bridge.close_native_synth_editor();
    bridge.add_midi_clip(track_id, 0);
    const auto clip_id = bridge.tracks()
                             .front()
                             .toMap()
                             .value("clips")
                             .toList()
                             .front()
                             .toMap()
                             .value("id")
                             .toString();
    require(!clip_id.isEmpty(), "drum-lane MIDI clip was not created");

    {
      QQmlApplicationEngine qml;
      qml.rootContext()->setContextProperty("projectBridge", &bridge);
      qml.load(QUrl(QStringLiteral("qrc:/DawShell.qml")));
      require(!qml.rootObjects().isEmpty(), "drum-lane DAW shell did not load");
      auto* window = qobject_cast<QQuickWindow*>(qml.rootObjects().front());
      require(window != nullptr, "drum-lane DAW shell has no window");
      window->setProperty("selectedClipId", clip_id);
      QCoreApplication::processEvents();
      auto* roll = window->findChild<QObject*>(QStringLiteral("pianoRoll"));
      require(roll != nullptr && roll->property("drumView").toBool(),
              "Pulse MIDI clip did not open the named drum view");
      const auto find_visual_item = [&](const QString& name) -> QQuickItem* {
        QQuickItem* found = nullptr;
        std::function<void(QQuickItem*)> visit = [&](QQuickItem* item) {
          if (item->objectName() == name)
            found = item;
          if (found)
            return;
          for (auto* child : item->childItems())
            visit(child);
        };
        visit(window->contentItem());
        return found;
      };
      const auto kick_cell = piano_roll_cell(window->contentItem(), 0, 36);
      if (!kick_cell) {
        throw std::runtime_error("Pulse's first pad cell was not rendered or is hidden");
      }
      QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, kick_cell->toPoint());
      QCoreApplication::processEvents();
      auto notes = bridge.tracks()
                       .front()
                       .toMap()
                       .value("clips")
                       .toList()
                       .front()
                       .toMap()
                       .value("notes")
                       .toList();
      require(notes.size() == 1 && notes.front().toMap().value("key").toInt() == 36,
              "drum-grid click did not create a normal MIDI note");
      bridge.add_note(clip_id, 4, 60);
      QCoreApplication::processEvents();
      require(roll->property("unmappedNotes").toInt() == 1,
              "notes outside Pulse's pads were silently hidden");
      auto* all_notes = find_visual_item(QStringLiteral("pianoFocusNotes"));
      require(all_notes != nullptr && all_notes->isVisible(),
              "the path from drum pads to all MIDI notes is missing");
      QTest::mouseClick(
          window, Qt::LeftButton, Qt::NoModifier,
          all_notes->mapToScene({all_notes->width() / 2, all_notes->height() / 2}).toPoint());
      QCoreApplication::processEvents();
      require(!roll->property("drumView").toBool() &&
                  roll->property("unmappedNotes").toInt() == 0 &&
                  roll->property("lowestKey").toInt() <= 60 &&
                  roll->property("lowestKey").toInt() + 12 > 60,
              "All Notes did not reveal unmapped pitches in the piano roll");
      const auto note_id = bridge.tracks()
                               .front()
                               .toMap()
                               .value("clips")
                               .toList()
                               .front()
                               .toMap()
                               .value("notes")
                               .toList()
                               .back()
                               .toMap()
                               .value("id")
                               .toString();
      auto* note_item = find_visual_item(QStringLiteral("pianoRollNote-") + note_id);
      require(note_item != nullptr && note_item->isVisible(),
              "unmapped note was not visible in the piano view");
      const auto before_wheel = roll->property("lowestKey").toInt();
      const auto wheel_at =
          note_item->mapToScene({note_item->width() / 2, note_item->height() / 2});
      QWheelEvent wheel(wheel_at, window->mapToGlobal(wheel_at.toPoint()), QPoint{},
                        QPoint{0, -120}, Qt::NoButton, Qt::NoModifier, Qt::ScrollUpdate, false);
      QCoreApplication::sendEvent(window, &wheel);
      QCoreApplication::processEvents();
      require(roll->property("lowestKey").toInt() == before_wheel - 3,
              "mouse wheel over a note did not scroll piano pitches");
      auto* drum_button = find_visual_item(QStringLiteral("midiDrumViewButton"));
      require(drum_button != nullptr && drum_button->isVisible(),
              "drum view cannot be selected again");
      QTest::mouseClick(
          window, Qt::LeftButton, Qt::NoModifier,
          drum_button->mapToScene({drum_button->width() / 2, drum_button->height() / 2}).toPoint());
      QCoreApplication::processEvents();
      require(roll->property("drumView").toBool(), "Piano and drum views do not toggle");
      bridge.save();
    }
  }
  const auto reopened = app::ProjectSession::open(root);
  require(reopened.has_value(), "drum-lane project did not reopen");
  const auto& track = reopened.value()->snapshot().tracks.front();
  const auto* source = std::get_if<project::NativeInstrumentPatch>(&track.instrument_source);
  require(source && source->engine == "pulse" && track.midi_clips.size() == 1 &&
              track.midi_clips.front().notes.size() == 2,
          "Pulse identity or edited MIDI notes were lost on reopen");
}

// The panel's Automate leads to the synth's automated parameter, else to its
// first automatable one, as the arrangement keys its lanes.
void automate_leads_to_the_automated_parameter() {
  QTemporaryDir directory;
  require(directory.isValid(), "synth automation target scratch directory unavailable");
  const auto root = std::filesystem::path(directory.path().toStdString()) / "automation-target";
  auto session = app::ProjectSession::create(root);
  require(session.has_value(), "synth automation target project creation failed");
  const auto track = session.value()->instruments().add_synth_track();
  require(track.has_value(), "synth automation target track creation failed");
  const auto* engine = native::find_engine("prism");
  require(engine != nullptr, "Prism engine is unavailable");
  quick::NativeSynthAutomationController target(*session.value(), track.value(), *engine);
  const auto first = std::ranges::find_if(engine->descriptor->parameters,
                                          [](const auto& item) { return item.automatable; });
  require(target.track_id() == QString::fromStdString(track.value().to_string()) &&
              target.lane_key() ==
                  QStringLiteral("synth:") + QString::fromStdString(std::string(first->id)),
          "an unautomated synth did not lead to its first automatable parameter");
  constexpr std::string_view cutoff = "prism.filter.cutoff";
  const auto index = oscillate::native_dsp::find_parameter(*engine->descriptor, cutoff);
  require(index.has_value(), "Prism has no cutoff");
  const auto cutoff_default = engine->descriptor->parameters[*index].default_value;
  require(session.value()
              ->automation()
              .add_automation_point(
                  track.value(), project::NativeParameterTarget{track.value(), std::string(cutoff)},
                  0, cutoff_default)
              .has_value(),
          "the cutoff lane fixture failed");
  QSignalSpy updates(&target, &quick::NativeSynthAutomationController::changed);
  target.refresh();
  require(updates.count() == 1 &&
              target.lane_key() ==
                  QStringLiteral("synth:") + QString::fromStdString(std::string(cutoff)),
          "Automate did not lead to the synth's automated parameter");
}
} // namespace

// A device the test drives block by block, recording starts and stops.
struct PulledOutput final : app::PlaybackOutput {
  struct Probe {
    audio::StereoRenderSource source{};
    audio::TransportCursor* cursor = nullptr;
    int starts = 0;
    int stops = 0;
    bool streaming = false;
  };
  PulledOutput(audio::StereoRenderSource source, audio::TransportCursor& cursor,
               std::shared_ptr<Probe> probe)
      : probe_(std::move(probe)) {
    probe_->source = source;
    probe_->cursor = &cursor;
  }
  [[nodiscard]] oscillate::foundation::Result<void> start(std::uint32_t rate, std::uint32_t,
                                                          std::optional<std::uint64_t>) override {
    ++probe_->starts;
    probe_->streaming = true;
    status_.state = audio::OutputState::streaming;
    status_.requested_client_rate = rate;
    return oscillate::foundation::Result<void>::success();
  }
  void stop() noexcept override {
    if (probe_->streaming)
      ++probe_->stops;
    probe_->streaming = false;
    status_.state = audio::OutputState::stopped;
  }
  [[nodiscard]] audio::OutputStatus status() const noexcept override { return status_; }

private:
  std::shared_ptr<Probe> probe_;
  audio::OutputStatus status_;
};

// Notes played while stopped run through the project's own graph (a monitor
// run with the clips muted), so the master meter hears them; Play then takes
// the device from the monitor.
void live_notes_monitor_through_the_graph_while_stopped() {
  QTemporaryDir directory;
  require(directory.isValid(), "monitor scratch directory unavailable");
  const auto root = std::filesystem::path(directory.path().toStdString()) / "monitor";
  auto created = app::ProjectSession::create(root);
  require(created.has_value(), "monitor project setup failed");
  auto probe = std::make_shared<PulledOutput::Probe>();
  app::PlaybackOutputFactory output = [probe](audio::StereoRenderSource source,
                                              audio::TransportCursor& cursor) {
    return std::make_unique<PulledOutput>(source, cursor, probe);
  };
  quick::ProjectBridge bridge(directory.path().toStdString(), nullptr, output);
  bridge.set_project(std::move(created).value(), root);
  require(QMetaObject::invokeMethod(bridge.track_commands(), "add_synth_track"),
          "monitor synth track failed");
  const auto track = bridge.tracks().front().toMap().value("id").toString();
  bridge.add_midi_clip(track, 0);
  bridge.play_note(track, 60, 110);
  require(QTest::qWaitFor([&] { return probe->streaming; }, 10'000),
          "a live note started the monitor graph");
  require(!bridge.playback_active(), "monitoring is not transport playback");
  std::array<engine::StereoFrame, 128> scratch{};
  std::array<float, 256> samples{};
  const auto pull = [&](int blocks) {
    float peak = 0.0F;
    for (int block = 0; block < blocks; ++block) {
      require(audio::render_transport_block(probe->source, *probe->cursor, true, scratch,
                                            samples) == audio::BlockResult::rendered,
              "the monitor renders");
      for (const float sample : samples)
        peak = std::max(peak, std::fabs(sample));
    }
    return peak;
  };
  // The device starts on the worker; the bridge hands the queued note to the
  // graph on its next poll.
  require(QTest::qWaitFor([&] { return pull(20) > 0.01F; }, 10'000),
          "the held note sounds through the graph");
  require(bridge.master_meter().value("valid").toBool(), "the master meter hears live notes");
  bridge.release_note(60);
  (void)pull(800);
  require(pull(50) < 1e-4F, "the released note falls silent");
  bridge.toggle_pause();
  require(QTest::qWaitFor([&] { return probe->stops >= 1 && bridge.playback_active(); }, 10'000),
          "Play takes the device from the monitor");
  bridge.stop_playback();
  require(QTest::qWaitFor([&] { return !bridge.playback_active(); }, 10'000),
          "playback stops after monitoring");
}

int main(int argc, char** argv) {
  QGuiApplication application(argc, argv);
  try {
    oscillate::test::panel_text::rules_catch_known_mistakes();
    // Concert plays the synthetic library unless a manual run points at an
    // existing project, which plays the installed one.
    if (!qEnvironmentVariableIsSet("OSCILLATE_CONCERT_PROJECT")) {
      (void)concert_test::install(concert_test::resident_note_seconds);
    }
    (void)fret_test::install(fret_test::resident_note_seconds);
    (void)wren_test::install();
    (void)bow_test::install();
    live_notes_monitor_through_the_graph_while_stopped();
    concert_note_plays_from_the_quick_shell();
    engine_is_reachable_and_saves_from_the_app("prism", "prism.filter.cutoff",
                                               QStringLiteral("prismPanel"));
    engine_is_reachable_and_saves_from_the_app("ember", "ember.filter.cutoff",
                                               QStringLiteral("emberPanel"));
    all_registered_panels_open_in_the_daw();
    nova_window_resizes_and_zooms_in_the_daw();
    pulse_drum_lanes_share_the_piano_roll_and_saved_notes();
    automate_leads_to_the_automated_parameter();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
