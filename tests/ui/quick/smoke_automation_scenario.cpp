#include "smoke_automation_scenario.hpp"

#include "arrangement_automation_controller.hpp"
#include "project_bridge.hpp"

#include <QCoreApplication>
#include <QQuickItem>
#include <QQuickWindow>
#include <QVariant>
#ifdef OSCILLATE_QUICK_TEST_INPUT
#include <QTest>
#endif

#include <cmath>
#include <iostream>

namespace quick_smoke {

#ifdef OSCILLATE_QUICK_TEST_INPUT
namespace {

QVariantList level_points(oscillate::quick::ProjectBridge& bridge, const QString& track_id) {
  auto* controller = qobject_cast<oscillate::quick::ArrangementAutomationController*>(
      bridge.arrangement_automation());
  for (const auto& track : controller->tracks()) {
    const auto entry = track.toMap();
    if (entry.value(QStringLiteral("trackId")).toString() != track_id)
      continue;
    for (const auto& lane : entry.value(QStringLiteral("lanes")).toList()) {
      if (lane.toMap().value(QStringLiteral("key")).toString() == QStringLiteral("level"))
        return lane.toMap().value(QStringLiteral("points")).toList();
    }
  }
  return {};
}

// The window position of a bar and normalised value on the overlay.
QPoint lane_point(QQuickItem* lane, double bar, double value) {
  QVariant x;
  QVariant y;
  QMetaObject::invokeMethod(lane, "xAt", Q_RETURN_ARG(QVariant, x), Q_ARG(QVariant, bar));
  QMetaObject::invokeMethod(lane, "yAt", Q_RETURN_ARG(QVariant, y), Q_ARG(QVariant, value));
  return lane->mapToScene(QPointF(x.toDouble(), y.toDouble())).toPoint();
}

bool near(double actual, double expected, double tolerance) {
  return std::abs(actual - expected) <= tolerance;
}

} // namespace
#endif

int edit_arrangement_automation(oscillate::quick::ProjectBridge& bridge, QQuickWindow* window,
                                AutomationItemLookup find_item) {
#ifdef OSCILLATE_QUICK_TEST_INPUT
  const auto tracks = bridge.tracks();
  if (!window || tracks.size() != 1) {
    std::cerr << "Arrangement automation smoke: expected one track\n";
    return 1;
  }
  const auto track_id = tracks.front().toMap().value(QStringLiteral("id")).toString();
  window->requestActivate();
  if (!QTest::qWaitForWindowActive(window)) {
    std::cerr << "Arrangement automation smoke: window never became active\n";
    return 1;
  }
  QTest::keyClick(window, Qt::Key_A);
  QCoreApplication::processEvents();
  auto* lane = find_item(window->contentItem(), QStringLiteral("arrangementAutomationOverlay"));
  if (!lane || lane->width() < 100) {
    std::cerr << "Arrangement automation smoke: A did not lay automation over the clips\n";
    return 1;
  }
  // A click adds a point, snapped to the beat.
  const auto start = bridge.revision();
  QTest::mouseClick(window, Qt::LeftButton, {}, lane_point(lane, 1.02, 0.5));
  QCoreApplication::processEvents();
  auto points = level_points(bridge, track_id);
  if (bridge.revision() != start + 1 || points.size() != 1 ||
      !near(points.front().toMap().value(QStringLiteral("bar")).toDouble(), 1.0, 1e-9) ||
      !near(points.front().toMap().value(QStringLiteral("value")).toDouble(), 0.5, 0.02)) {
    std::cerr << "Arrangement automation smoke: click did not add one Level point at bar 2: "
              << bridge.status().toStdString() << '\n';
    return 1;
  }
  const auto point_id = points.front().toMap().value(QStringLiteral("id")).toString();
  // Dragging the point moves it in one step, keeping its identity.
  QTest::mousePress(window, Qt::LeftButton, {}, lane_point(lane, 1.0, 0.5));
  QTest::mouseMove(window, lane_point(lane, 1.5, 0.6));
  QTest::mouseMove(window, lane_point(lane, 2.0, 0.75));
  QTest::mouseRelease(window, Qt::LeftButton, {}, lane_point(lane, 2.0, 0.75));
  QCoreApplication::processEvents();
  points = level_points(bridge, track_id);
  if (bridge.revision() != start + 2 || points.size() != 1 ||
      points.front().toMap().value(QStringLiteral("id")).toString() != point_id ||
      !near(points.front().toMap().value(QStringLiteral("bar")).toDouble(), 2.0, 1e-9) ||
      !near(points.front().toMap().value(QStringLiteral("value")).toDouble(), 0.75, 0.02)) {
    std::cerr << "Arrangement automation smoke: drag did not move the point in one step\n";
    return 1;
  }
  bridge.undo();
  points = level_points(bridge, track_id);
  if (points.size() != 1 ||
      !near(points.front().toMap().value(QStringLiteral("bar")).toDouble(), 1.0, 1e-9)) {
    std::cerr << "Arrangement automation smoke: undo did not restore the dragged point\n";
    return 1;
  }
  bridge.redo();
  // Clicking the point selects it; Delete removes it (and the empty lane).
  QTest::mouseClick(window, Qt::LeftButton, {}, lane_point(lane, 2.0, 0.75));
  QTest::keyClick(window, Qt::Key_Delete);
  QCoreApplication::processEvents();
  if (!level_points(bridge, track_id).isEmpty()) {
    std::cerr << "Arrangement automation smoke: Delete did not remove the selected point\n";
    return 1;
  }
  // B, then a drag, paints the envelope as one step.
  const auto before_draw = bridge.revision();
  QTest::keyClick(window, Qt::Key_B);
  QTest::mousePress(window, Qt::LeftButton, {}, lane_point(lane, 3.0, 0.2));
  for (int step = 1; step <= 8; ++step)
    QTest::mouseMove(window, lane_point(lane, 3.0 + step / 8.0, 0.2 + 0.6 * step / 8.0));
  QTest::mouseRelease(window, Qt::LeftButton, {}, lane_point(lane, 4.0, 0.8));
  QTest::keyClick(window, Qt::Key_B);
  QCoreApplication::processEvents();
  points = level_points(bridge, track_id);
  if (bridge.revision() != before_draw + 1 || points.size() < 4 ||
      !near(points.front().toMap().value(QStringLiteral("bar")).toDouble(), 3.0, 1e-9) ||
      !near(points.back().toMap().value(QStringLiteral("value")).toDouble(), 0.8, 0.02)) {
    std::cerr << "Arrangement automation smoke: drawing did not paint the envelope in one step ("
              << points.size() << " points)\n";
    return 1;
  }
  bridge.undo();
  if (!level_points(bridge, track_id).isEmpty()) {
    std::cerr << "Arrangement automation smoke: undo did not remove the drawn envelope\n";
    return 1;
  }
  // Two points join with a straight line, on the lane as it plays.
  QTest::mouseClick(window, Qt::LeftButton, {}, lane_point(lane, 5.0, 0.2));
  QTest::mouseClick(window, Qt::LeftButton, {}, lane_point(lane, 7.0, 0.8));
  QCoreApplication::processEvents();
  auto* controller = qobject_cast<oscillate::quick::ArrangementAutomationController*>(
      bridge.arrangement_automation());
  const auto lane_id = [&] {
    for (const auto& track : controller->tracks())
      for (const auto& entry : track.toMap().value(QStringLiteral("lanes")).toList())
        if (entry.toMap().value(QStringLiteral("key")).toString() == QStringLiteral("level"))
          return entry.toMap().value(QStringLiteral("laneId")).toString();
    return QString{};
  }();
  const auto middle = [&] { return controller->curve(lane_id, 5.5, 6.5, 3).at(1).toDouble(); };
  points = level_points(bridge, track_id);
  if (points.size() != 2 || !near(middle(), 0.5, 0.02)) {
    std::cerr << "Arrangement automation smoke: two points did not join with a straight line ("
              << middle() << ")\n";
    return 1;
  }
  // Ctrl-drag between them bows the segment, as one step.
  const auto before_bend = bridge.revision();
  QTest::mousePress(window, Qt::LeftButton, Qt::ControlModifier, lane_point(lane, 6.0, 0.5));
  QTest::mouseMove(window, lane_point(lane, 6.0, 0.6));
  QTest::mouseMove(window, lane_point(lane, 6.0, 0.7));
  QTest::mouseRelease(window, Qt::LeftButton, Qt::ControlModifier, lane_point(lane, 6.0, 0.7));
  QCoreApplication::processEvents();
  points = level_points(bridge, track_id);
  if (bridge.revision() != before_bend + 1 || points.size() != 2 ||
      points.front().toMap().value(QStringLiteral("shape")).toString() !=
          QStringLiteral("bezier") ||
      points.front().toMap().value(QStringLiteral("bend")).toDouble() <= 0.1 || middle() <= 0.6) {
    std::cerr << "Arrangement automation smoke: Ctrl-drag did not bow the segment upwards ("
              << middle() << ")\n";
    return 1;
  }
  // Back to where it started, it is a straight line again.
  QTest::mousePress(window, Qt::LeftButton, Qt::ControlModifier, lane_point(lane, 6.0, 0.6));
  QTest::mouseMove(window, lane_point(lane, 6.0, 0.5));
  QTest::mouseMove(window, lane_point(lane, 6.0, 0.4));
  QTest::mouseRelease(window, Qt::LeftButton, Qt::ControlModifier, lane_point(lane, 6.0, 0.4));
  QCoreApplication::processEvents();
  points = level_points(bridge, track_id);
  if (points.front().toMap().value(QStringLiteral("shape")).toString() !=
          QStringLiteral("linear") ||
      !near(middle(), 0.5, 0.02)) {
    std::cerr << "Arrangement automation smoke: dragging a bow back did not straighten it\n";
    return 1;
  }
  QTest::keyClick(window, Qt::Key_A);
  QCoreApplication::processEvents();
  if (find_item(window->contentItem(), QStringLiteral("arrangementAutomationOverlay"))) {
    std::cerr << "Arrangement automation smoke: A did not hide the overlay\n";
    return 1;
  }
  std::cout
      << "Qt Quick arrangement automation added, dragged, deleted, drawn and bent on the clips\n";
  return 0;
#else
  std::cerr << "Automation smoke requires Qt Test input support\n";
  return 1;
#endif
}

} // namespace quick_smoke
