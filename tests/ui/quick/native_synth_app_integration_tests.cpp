#include "native_synth_automation_controller.hpp"
#include "native_synth_device_controller.hpp"
#include "panel_fit_checks.hpp"
#include "panel_text_checks.hpp"
#include "project_bridge.hpp"

#include "../../native_dsp/concert_test_library.hpp"
#include "../../native_dsp/fret_test_library.hpp"
#include "../../native_dsp/wren_test_library.hpp"
#include "oscillate/application/project_session.hpp"
#include "oscillate/audio_io/stereo_block.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/native_dsp/synth/knock.hpp"
#include "oscillate/native_dsp/synth/parameters.hpp"
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
    auto* automation =
        qobject_cast<quick::NativeSynthAutomationController*>(bridge.native_synth_automation());
    require(automation != nullptr && !automation->parameters().empty(),
            "native synth automation editor is unavailable");
    require(window->findChild<QObject*>(QStringLiteral("nativeSynthAutomationToggle")) != nullptr &&
                window->findChild<QObject*>(QStringLiteral("nativeSynthAutomationEditor")) !=
                    nullptr,
            "synth automation controls are absent from the in-app editor");
    auto* automation_toggle = qobject_cast<QQuickItem*>(
        window->findChild<QObject*>(QStringLiteral("nativeSynthAutomationToggle")));
    auto* automation_editor =
        window->findChild<QObject*>(QStringLiteral("nativeSynthAutomationEditor"));
    QTest::mouseClick(
        window, Qt::LeftButton, Qt::NoModifier,
        automation_toggle
            ->mapToScene(QPointF(automation_toggle->width() / 2, automation_toggle->height() / 2))
            .toPoint());
    require(automation_editor->property("visible").toBool(),
            "synth automation editor did not open in the DAW");
    auto* automation_status =
        window->findChild<QObject*>(QStringLiteral("nativeSynthAutomationPlaybackStatus"));
    require(automation_status != nullptr && automation_status->property("text").toString().contains(
                                                QStringLiteral("Ready for next playback")),
            "stopped synth automation editor did not show its application state");
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
    automation->select_parameter(parameter);
    require(automation->selected_parameter() == parameter,
            "continuous synth parameter cannot be selected for automation");
    auto* add_point_button = qobject_cast<QQuickItem*>(
        window->findChild<QObject*>(QStringLiteral("nativeSynthAutomationAddPoint")));
    require(add_point_button != nullptr, "synth automation add control is absent");
    QTest::mouseClick(
        window, Qt::LeftButton, Qt::NoModifier,
        add_point_button
            ->mapToScene(QPointF(add_point_button->width() / 2, add_point_button->height() / 2))
            .toPoint());
    require(automation->lane().value("present").toBool(),
            "synth automation UI did not create a lane");
    require(automation->remove_lane(), "UI-created synth lane could not be removed");
    const auto discrete =
        std::ranges::find_if(engine->descriptor->parameters, [](const auto& item) {
          return item.automatable && native::is_discrete(item);
        });
    require(discrete != engine->descriptor->parameters.end(), "synth has no discrete parameter");
    const auto discrete_id = QString::fromStdString(std::string(discrete->id));
    automation->select_parameter(discrete_id);
    require(automation->selected_parameter() == discrete_id && automation->set_point(0, 1.0) &&
                automation->lane().value("present").toBool() && automation->remove_lane(),
            "stepped synth parameter could not be automated");
    automation->select_parameter(parameter);
    require(!automation->set_point(-1, 0.5) && !automation->set_point(0, 1.01),
            "invalid automation values changed the project");
    const auto revision = bridge.revision();
    require(automation->set_point(0, 0.25), "synth automation lane could not be created");
    const auto lane = automation->lane();
    require(lane.value("present").toBool() && lane.value("points").toList().size() == 1 &&
                bridge.revision() > revision,
            "synth automation lane was not projected");
    const auto point_id = lane.value("points").toList().front().toMap().value("id").toString();
    require(automation->set_value(point_id, 0.75), "synth automation point could not be edited");
    require(
        std::fabs(
            automation->lane().value("points").toList().front().toMap().value("value").toDouble() -
            0.75) < 1e-9,
        "edited synth automation value was not projected");
    require(automation->toggle_timebase() &&
                automation->lane().value("timebase").toString() == QStringLiteral("Elapsed"),
            "synth automation timebase did not change");
    require(automation->toggle_timebase(), "synth automation timebase could not be restored");
    require(!automation->remove_point(QStringLiteral("not-an-id")),
            "invalid synth automation point was accepted");
    require(automation->remove_point(point_id) && automation->lane().value("present").toBool() &&
                automation->lane().value("points").toList().empty(),
            "removing a synth point did not leave an empty lane");
    require(automation->remove_lane() && !automation->lane().value("present").toBool(),
            "synth automation lane could not be cleared");
    require(automation->set_point(0, 0.75), "synth lane could not be recreated");
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
  const auto index = native::find_parameter(*engine->descriptor, cutoff_id);
  if (!index) {
    throw std::runtime_error("the cutoff parameter is missing");
  }
  require(std::fabs(values.value()[*index] -
                    native::from_normalised(engine->descriptor->parameters[*index], 0.62)) < 1e-9,
          "edited synth cutoff did not survive save and reopen");
  const auto& lanes = reopened.value()->snapshot().tracks.front().automation;
  const auto lane = std::ranges::find_if(lanes, [&](const project::AutomationLane& item) {
    const auto* target = std::get_if<project::NativeParameterTarget>(&item.target);
    return target && target->parameter_id == cutoff_id;
  });
  require(lane != lanes.end() && lane->points.size() == 1 &&
              std::fabs(native::to_normalised(engine->descriptor->parameters[*index],
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
// Nova reflows to it (its laid-out width follows the window); Nova's zoom
// chips scale the panel. Both are kept in the workspace preferences, which
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

  click_named(panel, QStringLiteral("novaZoom:100"));
  const double full = host->scale();
  click_named(panel, QStringLiteral("novaZoom:90"));
  require(host->scale() < full - 0.05 && std::fabs(host->scale() - 0.9) < 0.01,
          "the 90% chip draws the panel at 90%");
  require(std::fabs(layout->property("panelZoom").toDouble() - 0.9) < 1e-9,
          "the zoom is kept in the workspace preferences");
  click_named(panel, QStringLiteral("novaZoom:100"));
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

void automation_edits_report_saved_and_audible_revisions() {
  QTemporaryDir directory;
  require(directory.isValid(), "synth automation status scratch directory unavailable");
  const auto root = std::filesystem::path(directory.path().toStdString()) / "automation-status";
  auto session = app::ProjectSession::create(root);
  require(session.has_value(), "synth automation status project creation failed");
  const auto track = session.value()->instruments().add_synth_track();
  require(track.has_value(), "synth automation status track creation failed");
  const auto* engine = native::find_engine("prism");
  require(engine != nullptr, "Prism engine is unavailable");
  std::optional<app::RevisionStatus> active;
  std::uint64_t submitted = 0;
  quick::NativeSynthAutomationController editor(
      *session.value(), track.value(), *engine,
      [&](std::uint64_t revision) {
        submitted = revision;
        if (active) {
          active->model_revision = revision;
          active->application = app::PlaybackApplication::pending;
        }
      },
      [&] { return active; });
  require(editor.playback_status().contains(QStringLiteral("Ready for next playback")),
          "stopped synth automation status is unclear");
  editor.select_parameter(QStringLiteral("prism.filter.cutoff"));
  active.emplace();
  active->model_revision = session.value()->snapshot().revision;
  active->active_graph_revision = active->model_revision;
  active->application = app::PlaybackApplication::active;
  require(editor.set_point(0, 0.25) && submitted == session.value()->snapshot().revision &&
              active->active_graph_revision < submitted &&
              editor.playback_status().contains(QStringLiteral("Applying project edit")),
          "live synth automation lane edit did not report pending application");
  const auto point_id =
      editor.lane().value("points").toList().front().toMap().value("id").toString();
  require(editor.set_value(point_id, 0.75) &&
              editor.playback_status().contains(QStringLiteral("Applying project edit")) &&
              editor.toggle_timebase() &&
              editor.playback_status().contains(QStringLiteral("Applying project edit")),
          "point or timebase edit lost the pending status");
  QSignalSpy lane_updates(&editor, &quick::NativeSynthAutomationController::changed);
  QSignalSpy status_updates(&editor,
                            &quick::NativeSynthAutomationController::playbackStatusChanged);
  active->active_graph_revision = session.value()->snapshot().revision;
  active->application = app::PlaybackApplication::active;
  editor.refresh_playback_status();
  require(editor.playback_status().contains(QStringLiteral("Active in playback")) &&
              status_updates.count() == 1 && lane_updates.count() == 0,
          "accepted synth automation revision was not reported active");
  editor.refresh_playback_status();
  require(status_updates.count() == 1 && lane_updates.count() == 0,
          "playback polling rebuilt the lane editor or repeated an unchanged status");
  require(editor.remove_lane() &&
              editor.playback_status().contains(QStringLiteral("Applying project edit")),
          "lane removal was falsely reported audible");
  active->application = app::PlaybackApplication::failed;
  editor.refresh_playback_status();
  require(editor.playback_status().contains(QStringLiteral("could not apply")),
          "failed synth automation edit was not reported");
  active.reset();
  editor.refresh();
  require(editor.playback_status().contains(QStringLiteral("Ready for next playback")),
          "stopping playback did not clear the stale application status");
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
    live_notes_monitor_through_the_graph_while_stopped();
    concert_note_plays_from_the_quick_shell();
    engine_is_reachable_and_saves_from_the_app("prism", "prism.filter.cutoff",
                                               QStringLiteral("prismPanel"));
    engine_is_reachable_and_saves_from_the_app("ember", "ember.filter.cutoff",
                                               QStringLiteral("emberPanel"));
    all_registered_panels_open_in_the_daw();
    nova_window_resizes_and_zooms_in_the_daw();
    pulse_drum_lanes_share_the_piano_roll_and_saved_notes();
    automation_edits_report_saved_and_audible_revisions();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
