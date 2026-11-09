#include "arrangement_automation_controller.hpp"
#include "automation_projection.hpp"
#include "loop_transport.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/test/check.hpp"
#include "project_bridge.hpp"
#include "workspace_layout_controller.hpp"

#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTest>

#include <cmath>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

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

void find_items(QQuickItem* parent, const QString& name, QList<QQuickItem*>& found) {
  if (parent->objectName() == name)
    found.push_back(parent);
  for (auto* child : parent->childItems())
    find_items(child, name, found);
}

void put16(std::vector<std::uint8_t>& output, std::uint16_t value) {
  output.push_back(static_cast<std::uint8_t>(value));
  output.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void put32(std::vector<std::uint8_t>& output, std::uint32_t value) {
  put16(output, static_cast<std::uint16_t>(value));
  put16(output, static_cast<std::uint16_t>(value >> 16U));
}

void write_source(const std::filesystem::path& path, std::uint32_t frames = 96'000) {
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

void run() {
  QTemporaryDir directory;
  require(directory.isValid(), "temporary project directory unavailable");
  qputenv("XDG_CONFIG_HOME", (directory.path() + QStringLiteral("/config")).toUtf8());
  const auto root = std::filesystem::path(directory.path().toStdString()) / "project";
  auto created = oscillate::application::ProjectSession::create(root);
  require(created.has_value(), "project creation failed");
  auto& session = *created.value();
  const auto source = std::filesystem::path(directory.path().toStdString()) / "source.wav";
  write_source(source);
  const int fd = ::open(source.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  require(fd >= 0, "WAV fixture open failed");
  const auto imported = session.imports().import_wav_audio(fd);
  (void)::close(fd);
  require(imported.has_value(), "audio import failed");
  const auto clip_id = session.snapshot().tracks.front().clips.front().id;
  const auto bar_21 = session.snapshot().meter.tick_at({21, 1, 0});
  require(bar_21.has_value(), "bar 21 unavailable");
  require(session.audio_clips().move_audio_clip(clip_id, bar_21.value()).has_value(),
          "long timeline clip move failed");

  {
    oscillate::quick::ProjectBridge bridge(directory.path().toStdString());
    bridge.set_project(std::move(created).value(), root);
    bridge.set_track_row_height(
        QString::fromStdString(session.snapshot().tracks.front().id.to_string()), 180);
    require(bridge.elapsed_seconds_at_bar(0) == 0.0,
            "elapsed ruler does not start at project zero");
    require(bridge.elapsed_seconds_at_bar(1) > 0.0,
            "elapsed ruler did not evaluate the project tempo map");
    require(bridge.timeline_bars() > 16, "clip after bar 16 did not extend timeline");
    const auto clips = bridge.tracks().front().toMap().value(QStringLiteral("clips")).toList();
    require(clips.size() == 1, "clip projection absent");
    const double normalized_start = clips.front().toMap().value(QStringLiteral("start")).toDouble();
    require(std::abs(normalized_start * bridge.timeline_bars() - 20.0) < 0.00001,
            "long timeline clip position was clamped");
    const auto before_trim = bridge.revision();
    bridge.trim_audio_clip(QString::fromStdString(clip_id.to_string()), false,
                           20.5 / bridge.timeline_bars(), false);
    require(bridge.revision() == before_trim + 1, "trim beyond bar 16 was rejected");
    const auto trimmed = bridge.tracks().front().toMap().value(QStringLiteral("clips")).toList();
    require(std::abs(trimmed.front().toMap().value(QStringLiteral("length")).toDouble() *
                         bridge.timeline_bars() -
                     0.5) < 0.00001,
            "long timeline trim changed the wrong amount");
    bridge.save();
    require(bridge.status().startsWith(QStringLiteral("Saved")), "long timeline save failed");
  }
  auto reopened = oscillate::application::ProjectSession::open(root);
  require(reopened.has_value(), "trimmed project did not reopen");
  // The bridge owns the session; this reads its saved state.
  const auto* project = reopened.value().get();
  oscillate::quick::ProjectBridge bridge(directory.path().toStdString());
  bridge.set_project(std::move(reopened).value(), root);
  const auto track_id = bridge.tracks().front().toMap().value(QStringLiteral("id")).toString();
  require(bridge.track_row_heights().value(track_id).toInt() == 180,
          "saved track height did not reopen");
  require(bridge.timeline_bars() > 16, "saved long timeline extent did not reopen");
  const auto saved_clip = bridge.tracks().front().toMap().value(QStringLiteral("clips")).toList();
  require(std::abs(saved_clip.front().toMap().value(QStringLiteral("length")).toDouble() *
                       bridge.timeline_bars() -
                   0.5) < 0.00001,
          "saved long timeline trim did not reopen");

  QQmlApplicationEngine engine;
  engine.rootContext()->setContextProperty("projectBridge", &bridge);
  engine.load(QUrl(QStringLiteral("qrc:/DawShell.qml")));
  require(!engine.rootObjects().isEmpty(), "Quick shell did not load");
  auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().front());
  require(window != nullptr, "Quick shell window unavailable");
  auto* dock = find_item(window->contentItem(), QStringLiteral("deviceChainDock"));
  auto* chain = find_item(window->contentItem(), QStringLiteral("insertChainPanel"));
  auto* context = find_item(window->contentItem(), QStringLiteral("inspectorPanel"));
  require(dock && chain && context && dock->isVisible() && chain->isVisible() &&
              !context->isVisible(),
          "selected device chain should occupy the bottom dock with context closed");
  auto* browse = find_item(window->contentItem(), QStringLiteral("dockedBrowserExpand"));
  auto* browser = find_item(window->contentItem(), QStringLiteral("vst3BrowserPanel"));
  require(browse && browse->isVisible() && browser,
          "the docked browser does not offer the full browser");
  QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                    browse->mapToScene({browse->width() / 2.0, browse->height() / 2.0}).toPoint());
  QCoreApplication::processEvents();
  auto* native_results = find_item(browser, QStringLiteral("nativeBrowserResults"));
  require(browser->isVisible() && browser->property("showBuiltIns").toBool() && native_results &&
              native_results->isVisible(),
          "Full browser did not open built-in effects for the selected track");
  window->setProperty("pluginBrowserOpen", false);
  QCoreApplication::processEvents();
  window->setProperty("insertChainHeight", 320);
  QCoreApplication::processEvents();
  require(std::abs(dock->height() - 320.0) < 1.0,
          "device chain dock did not resize with the shell");
  window->setProperty("contextPanelOpen", true);
  QCoreApplication::processEvents();
  require(context->isVisible() && dock->width() < window->width() - 100,
          "optional context panel did not appear beside the device dock");
  window->setProperty("contextPanelOpen", false);
  QCoreApplication::processEvents();
  oscillate::quick::WorkspaceLayoutController restored_layout;
  require(restored_layout.device_dock_height() == 320 && !restored_layout.context_panel_open(),
          "device dock and context visibility did not persist locally");
  window->setProperty("visibleBars", 8);
  window->setProperty("viewStartBar", 16);
  QCoreApplication::processEvents();
  auto* arrangement = find_item(window->contentItem(), QStringLiteral("arrangementView"));
  auto* clip = find_item(window->contentItem(), QStringLiteral("arrangementClip"));
  auto* master = find_item(window->contentItem(), QStringLiteral("masterArrangementRow"));
  auto* list_master = find_item(window->contentItem(), QStringLiteral("masterTrackRow"));
  auto* list_row = find_item(window->contentItem(), QStringLiteral("trackRow"));
  auto* arrangement_row = find_item(window->contentItem(), QStringLiteral("arrangementRow"));
  auto* resize_handle = find_item(window->contentItem(), QStringLiteral("trackHeightHandle"));
  auto* playhead = find_item(window->contentItem(), QStringLiteral("arrangementPlayhead"));
  require(arrangement && clip && clip->isVisible(), "clip after bar 16 is not visible when panned");
  require(master && list_master && playhead && list_row && arrangement_row && resize_handle,
          "resizable track, master or playhead line absent");
  require(std::abs(list_row->height() - 180.0) < 1.0 &&
              std::abs(arrangement_row->height() - list_row->height()) < 1.0,
          "saved track height did not align arrangement and controls");
  require(std::abs(list_master->y() - master->y()) < 1.0,
          "master rows are not pinned at the same height");
  // The elapsed-time ruler sits under the master row, at the bottom.
  auto* elapsed = find_item(window->contentItem(), QStringLiteral("arrangementElapsedRuler"));
  require(elapsed && elapsed->height() > 0.0 &&
              std::abs(master->y() + master->height() + elapsed->height() - arrangement->height()) <
                  1.0,
          "master and the elapsed-time ruler are not pinned to the bottom of the arrangement");
  require(std::abs(clip->x() - arrangement->width() / 2.0 - 2.0) < 2.0,
          "clip after bar 16 renders at the wrong bar");
  bridge.seek_to_bar(20.0);
  QCoreApplication::processEvents();
  require(playhead->isVisible(), "playhead line is not visible in the current ruler view");
  require(std::abs(playhead->x() - arrangement->width() / 2.0) < 2.0,
          "playhead line is not aligned with its bar");
  auto* ruler = find_item(window->contentItem(), QStringLiteral("arrangementRulerSeek"));
  auto* loop_band = find_item(window->contentItem(), QStringLiteral("arrangementLoopRange"));
  auto* loop = qobject_cast<oscillate::quick::LoopTransport*>(bridge.loop_transport());
  require(ruler && loop_band && loop, "arrangement loop controls unavailable");
  const auto ruler_at = [ruler](double fraction) {
    return ruler->mapToScene({ruler->width() * fraction, ruler->height() / 2.0}).toPoint();
  };
  QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, ruler_at(0.125));
  require(std::abs(bridge.playhead_bar() - 17.0) < 0.05 && !loop->enabled(),
          "clicking the ruler did not seek without starting a loop");
  QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, ruler_at(0.25));
  QTest::mouseMove(window, ruler_at(0.5));
  QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, ruler_at(0.5));
  QCoreApplication::processEvents();
  require(loop->enabled() && loop->start_bar() == 19 && loop->end_bar() == 22 &&
              loop_band->isVisible(),
          "dragging bars on the ruler did not create and show an active loop");
  loop->set_enabled(false);
  QCoreApplication::processEvents();
  require(!loop_band->isVisible(), "disabling the loop left the range highlighted");
  const auto start =
      resize_handle->mapToScene({resize_handle->width() / 2.0, resize_handle->height() / 2.0})
          .toPoint();
  QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start);
  QTest::mouseMove(window, start + QPoint(0, 48));
  QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, start + QPoint(0, 48));
  QCoreApplication::processEvents();
  require(std::abs(list_row->height() - 228.0) < 3.0 &&
              std::abs(arrangement_row->height() - list_row->height()) < 1.0,
          "dragging the track edge did not resize both rows");
  require(bridge.track_row_heights().value(track_id).toInt() >= 226,
          "dragged track height was not saved");
  const auto clip_position =
      clip->mapToScene({clip->width() / 2.0, clip->height() / 2.0}).toPoint();
  QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, clip_position);
  QCoreApplication::processEvents();
  auto* menu = find_item(window->contentItem(), QStringLiteral("audioClipContextMenu"));
  auto* rebuild = find_item(window->contentItem(), QStringLiteral("rebuildWaveformAction"));
  require(menu && menu->isVisible() && rebuild,
          "right-clicking an audio clip did not open its action menu");
  QTest::mouseClick(
      window, Qt::LeftButton, Qt::NoModifier,
      rebuild->mapToScene({rebuild->width() / 2.0, rebuild->height() / 2.0}).toPoint());
  require(QTest::qWaitFor(
              [&bridge] { return bridge.status().startsWith(QStringLiteral("Waveform rebuilt")); },
              10'000),
          "clip action did not rebuild the waveform from source audio");
  // Convert to sampler: Crate on a new track below, one undo step.
  QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, clip_position);
  QCoreApplication::processEvents();
  auto* convert =
      find_item(window->contentItem(), QStringLiteral("convertAudioClipToSamplerAction"));
  require(convert && convert->isVisible(), "the audio clip menu has no Convert to sampler");
  const auto tracks_before_convert = bridge.tracks().size();
  QTest::mouseClick(
      window, Qt::LeftButton, Qt::NoModifier,
      convert->mapToScene({convert->width() / 2.0, convert->height() / 2.0}).toPoint());
  QCoreApplication::processEvents();
  require(bridge.status().startsWith(QStringLiteral("Converted to Crate")) &&
              bridge.tracks().size() == tracks_before_convert + 1,
          "Convert to sampler did not add a Crate track");
  bridge.undo();
  QCoreApplication::processEvents();
  require(bridge.tracks().size() == tracks_before_convert,
          "one undo did not remove the converted sampler track");
  const auto clip_key = QString::fromStdString(clip_id.to_string());
  window->setProperty("selectedClipId", clip_key);
  window->setProperty("contextPanelOpen", true);
  QCoreApplication::processEvents();
  // The clip's level envelope is edited on the arrangement only (ADR-0051).
  require(find_item(window->contentItem(), QStringLiteral("clipChannelAutomationEditor")) ==
              nullptr,
          "the inspector still shows a clip level envelope form");
  const auto envelope_revision = bridge.revision();
  constexpr double clip_start = 0.0;
  constexpr double unity_level = 1.0;
  require(QMetaObject::invokeMethod(bridge.clip_automation(), "set_point_at_fraction",
                                    Q_ARG(QString, clip_key), Q_ARG(double, clip_start),
                                    Q_ARG(double, unity_level)),
          "clip envelope point at the clip start was not callable");
  const auto envelope = bridge.tracks()
                            .front()
                            .toMap()
                            .value(QStringLiteral("clips"))
                            .toList()
                            .front()
                            .toMap()
                            .value(QStringLiteral("channelAutomation"))
                            .toMap();
  require(bridge.revision() == envelope_revision + 1 &&
              envelope.value(QStringLiteral("pointCount")).toInt() == 1 &&
              envelope.value(QStringLiteral("points"))
                      .toList()
                      .front()
                      .toMap()
                      .value(QStringLiteral("position"))
                      .toString() == QStringLiteral("BAR 21 · BEAT 1"),
          "clip envelope keyboard edit did not retain its absolute bar display");
  bridge.save();
  auto envelope_reader = oscillate::persistence::ProjectStore::open_reader(root);
  require(envelope_reader.has_value(), "clip envelope reader failed");
  auto reopened_envelope = envelope_reader.value().load_current();
  require(reopened_envelope.has_value() &&
              reopened_envelope.value().snapshot.tracks.front().clips.front().automation.size() ==
                  1 &&
              reopened_envelope.value()
                      .snapshot.tracks.front()
                      .clips.front()
                      .automation.front()
                      .points.front()
                      .coordinate == 0,
          "clip envelope did not save its local bar anchor");
  auto* edit_level = find_item(window->contentItem(), QStringLiteral("arrangementLevelEditToggle"));
  require(edit_level != nullptr, "arrangement level edit mode is missing");
  QTest::mouseClick(
      window, Qt::LeftButton, Qt::NoModifier,
      edit_level->mapToScene({edit_level->width() / 2.0, edit_level->height() / 2.0}).toPoint());
  QCoreApplication::processEvents();
  auto* surface = find_item(window->contentItem(), QStringLiteral("clipLevelAddSurface"));
  require(surface && surface->isVisible(), "selected clip has no arrangement level surface");
  const auto before_arrangement_edit = bridge.revision();
  QTest::mouseClick(
      window, Qt::LeftButton, Qt::NoModifier,
      surface->mapToScene({surface->width() / 2.0, surface->height() / 2.0}).toPoint());
  QCoreApplication::processEvents();
  const auto revised = bridge.tracks()
                           .front()
                           .toMap()
                           .value(QStringLiteral("clips"))
                           .toList()
                           .front()
                           .toMap()
                           .value(QStringLiteral("channelAutomation"))
                           .toMap();
  require(bridge.revision() == before_arrangement_edit + 1 &&
              revised.value(QStringLiteral("pointCount")).toInt() == 2 &&
              revised.value(QStringLiteral("points"))
                      .toList()[1]
                      .toMap()
                      .value(QStringLiteral("clipFraction"))
                      .toDouble() > 0.4,
          "arrangement click did not add a point inside the half-bar clip");
  QList<QQuickItem*> handles;
  find_items(window->contentItem(), QStringLiteral("clipLevelPointHandle"), handles);
  require(handles.size() == 2, "arrangement level handles are missing");
  const auto drag_start =
      handles[1]->mapToScene({handles[1]->width() / 2.0, handles[1]->height() / 2.0}).toPoint();
  const auto before_drag = bridge.revision();
  QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, drag_start);
  // The first move crosses Qt Quick's drag threshold. Deliver another move
  // after activation so the point receives an actual position update.
  QTest::mouseMove(window, drag_start + QPoint(8, -12));
  QCoreApplication::processEvents();
  QTest::mouseMove(window, drag_start + QPoint(16, -24));
  const auto preview_moved = [&] {
    return handles[1]->property("previewFraction").toDouble() > 0.55;
  };
  if (!QTest::qWaitFor(preview_moved, 150)) {
    // Under concurrent test load Qt may coalesce the threshold-crossing move.
    // Require an actual preview update before releasing the pointer.
    QTest::mouseMove(window, drag_start + QPoint(17, -24));
    require(QTest::qWaitFor(preview_moved, 1'000),
            "dragging the clip point never updated its position preview");
  }
  QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, drag_start + QPoint(16, -24));
  QCoreApplication::processEvents();
  const auto moved_points = bridge.tracks()
                                .front()
                                .toMap()
                                .value(QStringLiteral("clips"))
                                .toList()
                                .front()
                                .toMap()
                                .value(QStringLiteral("channelAutomation"))
                                .toMap()
                                .value(QStringLiteral("points"))
                                .toList();
  require(bridge.revision() == before_drag + 1 && moved_points.size() == 2 &&
              moved_points[1].toMap().value(QStringLiteral("clipFraction")).toDouble() > 0.55,
          "dragging a clip point did not commit one position edit");
  const auto graphical_lane = bridge.tracks()
                                  .front()
                                  .toMap()
                                  .value(QStringLiteral("clips"))
                                  .toList()
                                  .front()
                                  .toMap()
                                  .value(QStringLiteral("channelAutomation"))
                                  .toMap();
  const auto lane_id = graphical_lane.value(QStringLiteral("laneId")).toString();
  const auto first_point = moved_points.front().toMap().value(QStringLiteral("id")).toString();
  auto* graph = find_item(window->contentItem(), QStringLiteral("clipLevelCurve"));
  auto* envelope_view = graph ? graph->parentItem() : nullptr;
  require(graph && envelope_view && graph->isVisible(),
          "selected clip has no visible graphical automation curve");
  auto progress_at = [&](int shape, double u, const QVariantMap& handles) {
    QVariant result;
    require(
        QMetaObject::invokeMethod(envelope_view, "segmentProgress", Q_RETURN_ARG(QVariant, result),
                                  Q_ARG(QVariant, QVariant(shape)), Q_ARG(QVariant, QVariant(u)),
                                  Q_ARG(QVariant, QVariant(handles))),
        "graphical curve evaluator was not callable");
    return result.toDouble();
  };
  require(std::abs(progress_at(0, 0.5, {}) - 0.0) < 1e-8 &&
              std::abs(progress_at(1, 0.5, {}) - 0.5) < 1e-8 &&
              std::abs(progress_at(2, 0.5, {}) - 0.25) < 1e-8 &&
              std::abs(progress_at(3, 0.5, {}) - std::expm1(2.0) / std::expm1(4.0)) < 1e-8 &&
              std::abs(progress_at(4, 0.25, {}) - 0.15625) < 1e-8 &&
              std::abs(progress_at(5, 0.5, {{"x1", 0.0}, {"y1", 1.0}, {"x2", 1.0}, {"y2", 1.0}}) -
                       0.875) < 1e-6,
          "graphical segment shapes diverged from the shared evaluator's equations");
  // Ctrl-drag on the lane bows a segment: a Bezier with its handles on the
  // thirds, lifted together (ArrangementAutomationController::bend).
  auto* lanes = qobject_cast<oscillate::quick::ArrangementAutomationController*>(
      bridge.arrangement_automation());
  require(lanes != nullptr, "arrangement automation controller missing");
  constexpr double segment_bow = 0.3;
  constexpr double third = 1.0 / 3.0;
  constexpr double handle_tolerance = 1e-6;
  const auto saved_first = [&] {
    return project->snapshot().tracks.front().clips.front().automation.front().points.front();
  };
  const auto curve_revision = bridge.revision();
  lanes->bend(lane_id, first_point, segment_bow);
  require(bridge.revision() == curve_revision + 1 &&
              saved_first().to_next == oscillate::project::AutomationShape::bezier &&
              std::abs(saved_first().handles.x1 - third) < handle_tolerance &&
              std::abs(saved_first().handles.y1 - (third + segment_bow)) < handle_tolerance,
          "bowing the clip segment did not save one undoable Bezier");
  bridge.undo();
  require(saved_first().to_next != oscillate::project::AutomationShape::bezier,
          "the clip segment bow did not undo as one operation");
  bridge.redo();
  const auto dragged_x1 = saved_first().handles.x1;

  // Pressing a point selects it; its exact level is typed in the tools row.
  QList<QQuickItem*> typed_handles;
  find_items(window->contentItem(), QStringLiteral("clipLevelPointHandle"), typed_handles);
  require(typed_handles.size() == 2, "clip level handles are missing before the value field");
  const auto typed_at =
      typed_handles[1]
          ->mapToScene({typed_handles[1]->width() / 2.0, typed_handles[1]->height() / 2.0})
          .toPoint();
  const auto before_select = bridge.revision();
  QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, typed_at);
  QCoreApplication::processEvents();
  require(bridge.revision() == before_select, "selecting a clip level point changed it");
  auto* level_field = find_item(window->contentItem(), QStringLiteral("clipLevelValueField"));
  auto* level_input = find_item(window->contentItem(), QStringLiteral("clipLevelValueInput"));
  require(level_field && level_input && level_field->isVisible(),
          "a selected clip level point offered no exact value field");
  constexpr double typed_decibels = -6.0;
  constexpr double decibels_per_decade = 20.0;
  constexpr double level_tolerance = 1e-6;
  const auto before_typed = bridge.revision();
  level_input->setProperty("text", QStringLiteral("-6 dB"));
  require(QMetaObject::invokeMethod(level_input, "commit"), "typed level commit not callable");
  const auto typed_point =
      project->snapshot().tracks.front().clips.front().automation.front().points[1];
  require(bridge.revision() == before_typed + 1 &&
              std::abs(typed_point.value - std::pow(10.0, typed_decibels / decibels_per_decade)) <
                  level_tolerance,
          "a typed clip level was not saved as one edit");
  bridge.undo();
  require(
      std::abs(project->snapshot().tracks.front().clips.front().automation.front().points[1].value -
               std::pow(10.0, typed_decibels / decibels_per_decade)) > level_tolerance,
      "a typed clip level did not undo");
  bridge.redo();

  // The envelope's tools also switch its timebase and clear it.
  auto* timebase = find_item(window->contentItem(), QStringLiteral("clipLevelTimebase"));
  require(timebase && timebase->isVisible(), "the clip envelope offers no timebase switch");
  const auto timebase_before =
      project->snapshot().tracks.front().clips.front().automation.front().timebase;
  const auto before_timebase = bridge.revision();
  require(QMetaObject::invokeMethod(timebase, "activate"), "timebase action not callable");
  require(bridge.revision() == before_timebase + 1 &&
              project->snapshot().tracks.front().clips.front().automation.front().timebase !=
                  timebase_before,
          "the envelope's timebase did not switch as one edit");
  bridge.undo();
  require(project->snapshot().tracks.front().clips.front().automation.front().timebase ==
              timebase_before,
          "the envelope's timebase switch did not undo");
  bridge.save();
  auto arrangement_reader = oscillate::persistence::ProjectStore::open_reader(root);
  require(arrangement_reader.has_value(), "arrangement envelope reader failed");
  auto reopened_arrangement = arrangement_reader.value().load_current();
  require(reopened_arrangement &&
              reopened_arrangement.value()
                      .snapshot.tracks.front()
                      .clips.front()
                      .automation.front()
                      .points.size() == 2 &&
              reopened_arrangement.value()
                      .snapshot.tracks.front()
                      .clips.front()
                      .automation.front()
                      .points.front()
                      .handles.x1 > dragged_x1 - 0.000001 &&
              reopened_arrangement.value()
                      .snapshot.tracks.front()
                      .clips.front()
                      .automation.front()
                      .points.front()
                      .handles.x1 < dragged_x1 + 0.000001,
          "arrangement points and graphical handles did not survive save and reopen");
  surface = find_item(window->contentItem(), QStringLiteral("clipLevelAddSurface"));
  require(surface && surface->isVisible(), "keyboard clip level surface disappeared");
  const auto before_keyboard = bridge.revision();
  surface->forceActiveFocus();
  QTest::keyClick(window, Qt::Key_Return);
  const auto keyboard_lane = bridge.tracks()
                                 .front()
                                 .toMap()
                                 .value(QStringLiteral("clips"))
                                 .toList()
                                 .front()
                                 .toMap()
                                 .value(QStringLiteral("channelAutomation"))
                                 .toMap();
  require(bridge.revision() == before_keyboard + 1 &&
              keyboard_lane.value(QStringLiteral("pointCount")).toInt() == 3,
          "keyboard level edit did not add a point inside the selected clip");

  // Clear removes the whole envelope as one edit.
  auto* clear = find_item(window->contentItem(), QStringLiteral("clipLevelClear"));
  require(clear && clear->isVisible(), "the clip envelope offers no Clear");
  const auto before_clear = bridge.revision();
  require(QMetaObject::invokeMethod(clear, "activate"), "clear action not callable");
  require(bridge.revision() == before_clear + 1 &&
              project->snapshot().tracks.front().clips.front().automation.empty(),
          "Clear did not remove the clip's envelope as one edit");
  bridge.undo();
  require(project->snapshot().tracks.front().clips.front().automation.size() == 1,
          "clearing the clip envelope did not undo");
}

void placeholder_inserts_above_itself() {
  QTemporaryDir directory;
  require(directory.isValid(), "temporary project directory unavailable");
  const auto root = std::filesystem::path(directory.path().toStdString()) / "ordered-project";
  auto created = oscillate::application::ProjectSession::create(root);
  require(created.has_value() && created.value()->tracks().add_instrument_track().has_value() &&
              created.value()
                  ->tracks()
                  .add_bus_track(oscillate::project::TrackKind::group_bus)
                  .has_value(),
          "initial track creation failed");
  oscillate::quick::ProjectBridge bridge(directory.path().toStdString());
  bridge.set_project(std::move(created).value(), root);
  const auto first_id = bridge.tracks().front().toMap().value(QStringLiteral("id")).toString();
  const auto second_id = bridge.tracks()[1].toMap().value(QStringLiteral("id")).toString();
  QQmlApplicationEngine engine;
  engine.rootContext()->setContextProperty("projectBridge", &bridge);
  engine.load(QUrl(QStringLiteral("qrc:/DawShell.qml")));
  require(!engine.rootObjects().isEmpty(), "Quick shell did not load for insertion test");
  auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().front());
  require(window != nullptr, "Quick shell window unavailable for insertion test");
  QCoreApplication::processEvents();
  auto* add_button = find_item(window->contentItem(), QStringLiteral("addTrackButton"));
  auto* creation_row = find_item(window->contentItem(), QStringLiteral("trackCreationRow"));
  auto* list = find_item(window->contentItem(), QStringLiteral("trackList"));
  require(add_button && creation_row && list, "track creation controls are missing");
  window->setProperty("selectedTrack", -2);
  QTest::mouseClick(
      window, Qt::LeftButton, Qt::NoModifier,
      add_button->mapToScene({add_button->width() / 2, add_button->height() / 2}).toPoint());
  QCoreApplication::processEvents();
  auto* menu = find_item(window->contentItem(), QStringLiteral("addTrackMenu"));
  require(menu && menu->isVisible(), "add-track menu did not open");
  QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, menu->mapToScene({25, 22}).toPoint());
  QCoreApplication::processEvents();
  require(bridge.tracks().size() == 3 &&
              bridge.tracks()[0].toMap().value(QStringLiteral("id")).toString() == first_id &&
              bridge.tracks()[1].toMap().value(QStringLiteral("id")).toString() == second_id,
          "placeholder action did not append above the add row");
  const auto created_id = bridge.tracks()[2].toMap().value(QStringLiteral("id")).toString();
  require(window->property("selectedTrack").toInt() == 2, "newly created track was not selected");
  // The tempo lane's header (ADR-0112) sits between the header row and the tracks.
  auto* tempo_header = find_item(window->contentItem(), QStringLiteral("tempoLaneHeader"));
  require(tempo_header != nullptr &&
              std::abs(tempo_header->mapToScene({0, 0}).y() -
                       (creation_row->mapToScene({0, 0}).y() + creation_row->height())) < 1.0 &&
              std::abs(list->mapToScene({0, 0}).y() -
                       (tempo_header->mapToScene({0, 0}).y() + tempo_header->height())) < 1.0,
          "track list header did not stay above the scrolling tracks");
  auto* master = find_item(window->contentItem(), QStringLiteral("masterTrackRow"));
  require(master && master->mapToScene({0, 0}).y() == list->mapToScene({0, 0}).y() + list->height(),
          "master was not pinned below the scrolling tracks");

  QList<QQuickItem*> rows;
  find_items(window->contentItem(), QStringLiteral("trackRow"), rows);
  require(rows.size() == 3, "track rows unavailable for reordering");
  const auto source = rows[0]->mapToScene({100, 40}).toPoint();
  const auto target = rows[2]->mapToScene({100, 40}).toPoint();
  QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, source);
  QTest::mouseMove(window, target);
  QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, target);
  QCoreApplication::processEvents();
  require(bridge.tracks()[0].toMap().value(QStringLiteral("id")).toString() == second_id &&
              bridge.tracks()[1].toMap().value(QStringLiteral("id")).toString() == created_id &&
              bridge.tracks()[2].toMap().value(QStringLiteral("id")).toString() == first_id,
          "dragging a track did not reorder the saved project");
  bridge.undo();
  require(bridge.tracks()[0].toMap().value(QStringLiteral("id")).toString() == first_id,
          "one undo did not restore the dragged track order");
  bridge.redo();
  require(bridge.tracks()[2].toMap().value(QStringLiteral("id")).toString() == first_id,
          "redo did not restore the dragged track order");
  bridge.save();
  require(bridge.status().startsWith(QStringLiteral("Saved")),
          "reordered track project did not save");
  auto reader = oscillate::persistence::ProjectStore::open_reader(root);
  require(reader.has_value(), "saved project reader failed to open");
  auto saved = reader.value().load_current();
  require(saved && saved.value().snapshot.tracks.size() == 3 &&
              saved.value().snapshot.tracks[2].id.to_string() == first_id.toStdString(),
          "reordered track order did not survive save and reopen");

  const auto pinned_y = creation_row->mapToScene({0, 0}).y();
  const auto pinned_master_y = master->mapToScene({0, 0}).y();
  for (int index = 0; index < 6; ++index)
    bridge.add_bus_track(QStringLiteral("group"));
  QCoreApplication::processEvents();
  require(list->property("contentHeight").toDouble() > list->height(),
          "track list did not become scrollable");
  list->setProperty("contentY", list->property("contentHeight").toDouble() - list->height());
  QCoreApplication::processEvents();
  require(creation_row->mapToScene({0, 0}).y() == pinned_y &&
              master->mapToScene({0, 0}).y() == pinned_master_y,
          "track list header or master scrolled away with ordinary tracks");

  list->setProperty("contentY", 0);
  QCoreApplication::processEvents();
  QList<QQuickItem*> long_rows;
  find_items(window->contentItem(), QStringLiteral("trackRow"), long_rows);
  require(long_rows.size() == bridge.tracks().size(), "scrollable rows were not rendered");
  const auto dragged_id = bridge.tracks().front().toMap().value(QStringLiteral("id")).toString();
  const auto drag_start = long_rows[0]->mapToScene({100, 40}).toPoint();
  const auto drag_edge = list->mapToScene({100, list->height() - 4}).toPoint();
  QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, drag_start);
  QTest::mouseMove(window, drag_edge);
  const auto scroll_limit = list->property("contentHeight").toDouble() - list->height();
  require(QTest::qWaitFor([&] { return list->property("contentY").toDouble() >= scroll_limit - 1; },
                          2'000),
          "dragging near the viewport edge did not scroll to lower tracks");
  QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, drag_edge);
  QCoreApplication::processEvents();
  require(bridge.tracks().back().toMap().value(QStringLiteral("id")).toString() == dragged_id,
          "dragging through the scrolled list did not move the track to the end");
  require(creation_row->mapToScene({0, 0}).y() == pinned_y &&
              master->mapToScene({0, 0}).y() == pinned_master_y,
          "drag scrolling moved either pinned row");
}

void clip_curve_x_follows_shared_tempo_map() {
  QTemporaryDir directory;
  require(directory.isValid(), "tempo projection directory unavailable");
  const auto root = std::filesystem::path(directory.path().toStdString()) / "ramped-project";
  auto created = oscillate::application::ProjectSession::create(root);
  require(created.has_value(), "tempo projection project creation failed");
  auto& session = *created.value();
  constexpr auto quarter = oscillate::timeline::ticks_per_quarter;
  require(
      session.song().set_tempo_event({8 * quarter}, 60.0, oscillate::timeline::TempoShape::step) &&
          session.song().set_tempo_event({0}, 120.0, oscillate::timeline::TempoShape::linear_bpm),
      "tempo projection ramp setup failed");
  const auto source = std::filesystem::path(directory.path().toStdString()) / "source.wav";
  const auto bar_two_sample =
      session.snapshot().tempo.tick_to_sample({4 * quarter}, session.snapshot().sample_rate);
  require(bar_two_sample.has_value() && bar_two_sample.value().value > 0 &&
              bar_two_sample.value().value < 200'000,
          "tempo projection musical clip boundary unavailable");
  write_source(source, static_cast<std::uint32_t>(bar_two_sample.value().value));
  const int fd = ::open(source.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  require(fd >= 0, "tempo projection WAV open failed");
  const auto imported = session.imports().import_wav_audio(fd);
  (void)::close(fd);
  require(imported.has_value(), "tempo projection WAV import failed");
  const auto clip_id = session.snapshot().tracks.front().clips.front().id;
  require(
      session.automation().set_audio_clip_channel_level_point_at_fraction(clip_id, {0.1}, 0.5) &&
          session.automation().set_audio_clip_channel_level_point_at_fraction(clip_id, {0.9}, 1.5),
      "tempo projection clip points failed");
  const auto lane_id = session.snapshot().tracks.front().clips.front().automation.front().id;
  require(session.automation().toggle_automation_lane_timebase(lane_id).has_value(),
          "tempo projection lane timebase switch failed");
  const auto& snapshot = session.snapshot();
  const auto& clip = snapshot.tracks.front().clips.front();
  require(std::holds_alternative<oscillate::project::ElapsedInterval>(clip.placement) &&
              clip.automation.front().timebase == oscillate::project::AutomationTimebase::musical,
          "tempo projection fixture did not retain mixed timebases");
  const auto projected = oscillate::quick::detail::clip_channel_automation_projection(
      snapshot, snapshot.tracks.front(), clip);
  const auto axis = projected.value(QStringLiteral("axisMap")).toList();
  const auto points = projected.value(QStringLiteral("points")).toList();
  require(axis.size() == 65 && points.size() == 2,
          "mixed-timebase projection omitted its bounded axis map");
  const auto& interval = std::get<oscillate::project::ElapsedInterval>(clip.placement);
  const auto first_tick = snapshot.tempo.sample_to_tick(interval.start, snapshot.sample_rate);
  const auto last_tick = snapshot.tempo.sample_to_tick(interval.end, snapshot.sample_rate);
  require(first_tick && last_tick, "tempo projection clip tick bounds unavailable");
  const auto middle_tick = oscillate::timeline::TickPosition{
      first_tick.value().value + (last_tick.value().value - first_tick.value().value) / 2};
  const auto middle_sample = snapshot.tempo.tick_to_sample(middle_tick, snapshot.sample_rate);
  require(middle_sample.has_value(), "tempo projection midpoint conversion failed");
  const double oracle = static_cast<double>(middle_sample.value().value - interval.start.value) /
                        static_cast<double>(interval.end.value - interval.start.value);
  const double linear = (points[0].toMap().value(QStringLiteral("clipFraction")).toDouble() +
                         points[1].toMap().value(QStringLiteral("clipFraction")).toDouble()) /
                        2.0;
  require(std::abs(axis[32].toDouble() - oracle) < 0.00002 &&
              std::abs(axis[32].toDouble() - linear) > 0.002,
          "clip curve x did not follow the shared ramp instead of a straight line");
  require(session.saving().save().has_value(), "tempo projection project save failed");
  created.value().reset();
  auto reopened = oscillate::application::ProjectSession::open(root);
  require(reopened.has_value(), "tempo projection project reopen failed");
  const auto& restored = reopened.value()->snapshot();
  const auto restored_axis =
      oscillate::quick::detail::clip_channel_automation_projection(
          restored, restored.tracks.front(), restored.tracks.front().clips.front())
          .value(QStringLiteral("axisMap"))
          .toList();
  require(restored_axis.size() == axis.size() &&
              std::abs(restored_axis[32].toDouble() - oracle) < 0.00002,
          "saved mixed-timebase curve x did not reopen at the same sample");
  {
    oscillate::quick::ProjectBridge bridge(directory.path().toStdString());
    bridge.set_project(std::move(reopened).value(), root);
    const auto clip_data =
        bridge.tracks().front().toMap().value(QStringLiteral("clips")).toList().front().toMap();
    QQmlEngine engine;
    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/ClipLevelEnvelope.qml")));
    auto* envelope = component.createWithInitialProperties(
        {{QStringLiteral("bridge"), QVariant::fromValue(static_cast<QObject*>(&bridge))},
         {QStringLiteral("clipData"), clip_data},
         {QStringLiteral("accentColor"), QColor(QStringLiteral("#84dca4"))},
         {QStringLiteral("lineColor"), QColor(QStringLiteral("#202e27"))}});
    require(envelope != nullptr, "projected clip envelope QML did not load");
    QVariant drawn_midpoint;
    require(QMetaObject::invokeMethod(envelope, "projectedFraction",
                                      Q_RETURN_ARG(QVariant, drawn_midpoint),
                                      Q_ARG(QVariant, QVariant(0.5))) &&
                std::abs(drawn_midpoint.toDouble() - oracle) < 0.00002,
            "QML curve axis did not use the authoritative projected midpoint");
    delete envelope;
  }

  auto reverse = oscillate::application::ProjectSession::open(root);
  require(reverse.has_value(), "reverse mixed-timebase project reopen failed");
  const auto clip_switched = reverse.value()->audio_clips().toggle_audio_clip_timebase(clip_id);
  require(clip_switched.has_value(), "reverse mixed-timebase clip placement switch failed");
  const auto lane_switched = reverse.value()->automation().toggle_automation_lane_timebase(lane_id);
  require(lane_switched.has_value(), "reverse mixed-timebase lane switch failed");
  const auto& reverse_snapshot = reverse.value()->snapshot();
  const auto& reverse_clip = reverse_snapshot.tracks.front().clips.front();
  require(std::holds_alternative<oscillate::project::MusicalInterval>(reverse_clip.placement) &&
              reverse_clip.automation.front().timebase ==
                  oscillate::project::AutomationTimebase::elapsed,
          "reverse mixed-timebase fixture did not switch both axes");
  const auto reverse_axis = oscillate::quick::detail::clip_channel_automation_projection(
                                reverse_snapshot, reverse_snapshot.tracks.front(), reverse_clip)
                                .value(QStringLiteral("axisMap"))
                                .toList();
  const auto& musical = std::get<oscillate::project::MusicalInterval>(reverse_clip.placement);
  const auto start_sample =
      reverse_snapshot.tempo.tick_to_sample(musical.start, reverse_snapshot.sample_rate);
  const auto end_sample =
      reverse_snapshot.tempo.tick_to_sample(musical.end, reverse_snapshot.sample_rate);
  require(start_sample && end_sample && reverse_axis.size() == 65,
          "reverse mixed-timebase axis map unavailable");
  const auto middle_sample_position = oscillate::timeline::SamplePosition{
      start_sample.value().value + (end_sample.value().value - start_sample.value().value) / 2};
  const auto middle_musical =
      reverse_snapshot.tempo.sample_to_tick(middle_sample_position, reverse_snapshot.sample_rate);
  require(middle_musical.has_value() &&
              std::abs(reverse_axis[32].toDouble() -
                       static_cast<double>(middle_musical.value().value - musical.start.value) /
                           static_cast<double>(musical.end.value - musical.start.value)) < 0.00002,
          "reverse mixed-timebase curve x diverged from shared elapsed-to-musical conversion");
}

// The lanes a lane-count fixture saves: none, one on its clip, or that and
// two on the track (level with two points, pan with one).
enum class Lanes : std::uint8_t { none, clip, clip_and_track };

// A track says how many automation lanes it holds, its own and its clips',
// before any are shown: the projection counts them and the A button says
// so in its tooltip and accessible description (issue #203).
void lane_count_reads(Lanes lanes, int expected, const QString& text) {
  QTemporaryDir directory;
  require(directory.isValid(), "lane count directory unavailable");
  const auto root = std::filesystem::path(directory.path().toStdString()) / "lane-count";
  auto created = oscillate::application::ProjectSession::create(root);
  require(created.has_value(), "lane count project creation failed");
  auto& session = *created.value();
  const auto source = std::filesystem::path(directory.path().toStdString()) / "source.wav";
  write_source(source);
  const int fd = ::open(source.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  require(fd >= 0, "lane count WAV open failed");
  const auto imported = session.imports().import_wav_audio(fd);
  (void)::close(fd);
  require(imported.has_value(), "lane count WAV import failed");
  const auto track_id = session.snapshot().tracks.front().id;
  const auto clip_id = session.snapshot().tracks.front().clips.front().id;
  constexpr double clip_level = 0.5;
  constexpr double clip_fraction = 0.5;
  constexpr std::int64_t first_bar = 0;
  constexpr std::int64_t second_bar = 2;
  constexpr double level = 0.7;
  constexpr double pan = 0.25;
  using Key = oscillate::project::TrackControlTarget::Key;
  if (lanes != Lanes::none) {
    require(
        session.automation()
            .set_audio_clip_channel_level_point_at_fraction(clip_id, {clip_fraction}, clip_level)
            .has_value(),
        "clip lane point failed");
  }
  if (lanes == Lanes::clip_and_track) {
    auto& automation = session.parameter_automation();
    require(
        automation.set_channel_control_point(track_id, Key::channel_level, first_bar, level)
                .has_value() &&
            automation.set_channel_control_point(track_id, Key::channel_level, second_bar, level)
                .has_value() &&
            automation.set_channel_control_point(track_id, Key::channel_pan, first_bar, pan)
                .has_value(),
        "track lane points failed");
  }
  oscillate::quick::ProjectBridge bridge(directory.path().toStdString());
  bridge.set_project(std::move(created).value(), root);
  require(bridge.tracks().front().toMap().value(QStringLiteral("automationLaneCount")).toInt() ==
              expected,
          "the track projects " + std::to_string(expected) + " automation lanes");
  QQmlApplicationEngine engine;
  engine.rootContext()->setContextProperty("projectBridge", &bridge);
  engine.load(QUrl(QStringLiteral("qrc:/DawShell.qml")));
  require(!engine.rootObjects().isEmpty(), "Quick shell did not load for the lane count");
  auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().front());
  require(window != nullptr, "Quick shell window unavailable for the lane count");
  QCoreApplication::processEvents();
  auto* button = find_item(window->contentItem(), QStringLiteral("trackAutomationButton"));
  auto* mark = find_item(window->contentItem(), QStringLiteral("trackAutomationMark"));
  require(button != nullptr && mark != nullptr, "the track's A button is missing");
  require(button->property("laneText").toString() == text,
          "the A button says \"" + button->property("laneText").toString().toStdString() +
              "\", not \"" + text.toStdString() + "\"");
  require(mark->isVisible() == (expected > 0), "the A button is marked only with lanes");
}

void automation_lane_count_marks_the_track() {
  lane_count_reads(Lanes::none, 0, QStringLiteral("No automation"));
  lane_count_reads(Lanes::clip, 1, QStringLiteral("1 automation lane"));
  lane_count_reads(Lanes::clip_and_track, 3, QStringLiteral("3 automation lanes"));
}

// Automation is edited on the arrangement (ADR-0051): the inspector's and
// the device editors' automation forms are gone from the app's resources.
void retired_automation_forms_are_gone() {
  QQmlEngine engine;
  for (const auto* form :
       {"ChannelAutomationEditor", "ChannelAutomationGraph", "PluginAutomationEditor",
        "NativeSynthAutomationEditor", "AutomationSegmentControls", "AutomationTimebaseAction"}) {
    const auto url = QUrl(QStringLiteral("qrc:/%1.qml").arg(QString::fromLatin1(form)));
    QQmlComponent component(&engine, url);
    require(component.isError(), std::string("a retired automation form still ships: ") + form);
  }
}

} // namespace

// A save requested while a background save is still writing the project
// runs after it and saves the latest state, instead of being refused as a
// save already in progress (the store allows one writer at a time).
void save_during_background_save_runs_after_it() {
  constexpr int background_save_wait_ms = 10'000;
  QTemporaryDir directory;
  require(directory.isValid(), "temporary project directory unavailable");
  const auto root = std::filesystem::path(directory.path().toStdString()) / "queued-save";
  auto created = oscillate::application::ProjectSession::create(root);
  require(created.has_value(), "queued-save project creation failed");
  oscillate::quick::ProjectBridge bridge(directory.path().toStdString());
  bridge.set_project(std::move(created).value(), root);
  const auto tracks_before = bridge.tracks().size();
  // The edit schedules a background save; wait until it is writing.
  bridge.create_native_instrument_track(QStringLiteral("prism"), -1);
  require(QTest::qWaitFor([&] { return bridge.save_in_flight(); }, background_save_wait_ms),
          "the edit did not start a background save");
  // A second edit while that save runs: only the manual save can hold it.
  bridge.create_native_instrument_track(QStringLiteral("prism"), -1);
  require(bridge.tracks().size() == tracks_before + 2, "second edit was rejected");
  const auto latest = bridge.revision();
  bridge.save();
  require(!bridge.save_in_flight(), "save returned with the background save still writing");
  require(bridge.status().startsWith(QStringLiteral("Saved")),
          "a save during a background save was refused");
  auto reader = oscillate::persistence::ProjectStore::open_reader(root);
  require(reader.has_value(), "queued-save reader failed to open");
  auto saved = reader.value().load_current();
  require(saved && saved.value().snapshot.revision == latest &&
              std::cmp_equal(saved.value().snapshot.tracks.size(), tracks_before + 2),
          "the save after the background save did not hold the latest state");
}

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  try {
    run();
    placeholder_inserts_above_itself();
    clip_curve_x_follows_shared_tempo_map();
    automation_lane_count_marks_the_track();
    retired_automation_forms_are_gone();
    save_during_background_save_runs_after_it();
  } catch (const std::exception& error) {
    qCritical("Timeline extent test: %s", error.what());
    return 1;
  }
  return 0;
}
