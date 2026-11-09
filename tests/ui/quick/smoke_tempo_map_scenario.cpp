#include "smoke_tempo_map_scenario.hpp"

#include "project_bridge.hpp"
#include "smoke_import_scenario.hpp"
#include "tempo_lane_controller.hpp"

#include <QCoreApplication>
#include <QQuickItem>
#include <QQuickWindow>
#include <QVariantMap>
#ifdef OSCILLATE_QUICK_TEST_INPUT
#include <QTest>
#endif

#include <cmath>
#include <iostream>
#include <limits>

namespace quick_smoke {
namespace {

// The song's opening tempo, and the meter and tempo the project controls set.
constexpr double opening_bpm = 120.0;
constexpr double edited_opening_bpm = 105.0;
constexpr int edited_meter_numerator = 3;
constexpr int invalid_meter_denominator = 3;
constexpr int edited_meter_denominator = 4;
// The ramp drawn with the pointer: two bars, starting a bar into the view.
constexpr double ramp_lead_bars = 1.0;
constexpr double ramp_bars = 2.0;
constexpr int drag_steps = 6;
// Positions and tempi read back from the lane.
constexpr double bar_tolerance = 1e-6;
constexpr double bpm_tolerance = 0.051;

[[nodiscard]] QVariantList lane_points(oscillate::quick::ProjectBridge& bridge) {
  return bridge.tempo_lane()->property("points").toList();
}

[[nodiscard]] QVariantMap lane_point(oscillate::quick::ProjectBridge& bridge, qsizetype index) {
  const auto points = lane_points(bridge);
  return index < points.size() ? points[index].toMap() : QVariantMap{};
}

#ifdef OSCILLATE_QUICK_TEST_INPUT
// The window point of `bar` and `bpm` on the lane, as the lane draws them.
[[nodiscard]] QPoint lane_point_at(QQuickItem* lane, double bar, double bpm) {
  const auto start = lane->property("viewStartBar").toDouble();
  const auto bars = lane->property("visibleBars").toDouble();
  const auto padding = lane->property("padding").toDouble();
  const auto low = lane->property("low").toDouble();
  const auto high = lane->property("high").toDouble();
  const auto x = (bar - start) / bars * lane->width();
  const auto y = padding + (high - bpm) / (high - low) * (lane->height() - 2 * padding);
  return lane->mapToScene({x, y}).toPoint();
}
#endif

} // namespace

bool edit_tempo_map(oscillate::quick::ProjectBridge& bridge, QQuickWindow* window,
                    VisibleItemLookup find_item) {
#ifdef OSCILLATE_QUICK_TEST_INPUT
  auto* controller = qobject_cast<oscillate::quick::TempoLaneController*>(bridge.tempo_lane());
  auto* header =
      window ? find_item(window->contentItem(), QStringLiteral("tempoLaneHeader")) : nullptr;
  auto* lane = window ? find_item(window->contentItem(), QStringLiteral("tempoLane")) : nullptr;
  if (!controller || !header || !lane || lane_points(bridge).size() != 1) {
    std::cerr << "Tempo lane smoke: the lane, its header or the opening tempo is missing\n";
    return false;
  }
  // The header expands the lane from the keyboard.
  header->forceActiveFocus();
  QTest::keyClick(window, Qt::Key_Return);
  QCoreApplication::processEvents();
  if (!lane->property("expanded").toBool()) {
    std::cerr << "Tempo lane smoke: the header did not expand the lane\n";
    return false;
  }

  const auto initial = bridge.revision();
  controller->add_point(ramp_bars, 0.0);
  controller->ramp(ramp_bars, ramp_lead_bars, opening_bpm, QStringLiteral("s_curve"));
  controller->set_shape(QStringLiteral("0"), QStringLiteral("sideways"));
  controller->remove_point(QStringLiteral("0"));
  if (bridge.revision() != initial) {
    std::cerr << "Tempo lane smoke: an invalid edit changed the project\n";
    return false;
  }

  // Drag across two bars at the lane's top: a ramp to the fastest tempo
  // on its scale, one edit, previewed until release.
  const auto view_start = std::ceil(lane->property("viewStartBar").toDouble());
  const auto from_bar = view_start + ramp_lead_bars;
  const auto to_bar = from_bar + ramp_bars;
  const auto top = controller->high_bpm();
  const auto press = lane_point_at(lane, from_bar, top);
  const auto release = lane_point_at(lane, to_bar, top);
  QTest::mousePress(window, Qt::LeftButton, {}, press);
  for (int step = 1; step <= drag_steps; ++step)
    QTest::mouseMove(window, press + (release - press) * step / drag_steps);
  if (bridge.revision() != initial) {
    std::cerr << "Tempo lane smoke: the ramp saved before release\n";
    return false;
  }
  QTest::mouseRelease(window, Qt::LeftButton, {}, release);
  QCoreApplication::processEvents();
  const auto start = lane_point(bridge, 1);
  const auto end = lane_point(bridge, 2);
  if (bridge.revision() != initial + 1 || lane_points(bridge).size() != 3 ||
      std::abs(start.value(QStringLiteral("bar")).toDouble() - from_bar) > bar_tolerance ||
      start.value(QStringLiteral("shape")).toString() != QStringLiteral("s_curve") ||
      std::abs(start.value(QStringLiteral("bpm")).toDouble() - opening_bpm) > bpm_tolerance ||
      std::abs(end.value(QStringLiteral("bar")).toDouble() - to_bar) > bar_tolerance ||
      std::abs(end.value(QStringLiteral("bpm")).toDouble() - top) > bpm_tolerance) {
    std::cerr << "Tempo lane smoke: the drawn ramp is wrong: " << bridge.status().toStdString()
              << '\n';
    return false;
  }
  const auto middle = controller->bpm_at((from_bar + to_bar) / 2);
  if (!(middle > opening_bpm && middle < top)) {
    std::cerr << "Tempo lane smoke: the tempo does not follow the ramp\n";
    return false;
  }

  // Right-click the ramp's start for its shape: Straight is one edit.
  const auto at_start = lane_point_at(lane, from_bar, opening_bpm);
  QTest::mouseClick(window, Qt::RightButton, {}, at_start);
  QCoreApplication::processEvents();
  auto* straight = find_item(window->contentItem(), QStringLiteral("tempoShape_straight"));
  if (!straight) {
    std::cerr << "Tempo lane smoke: the shape menu did not open\n";
    return false;
  }
  const auto choice =
      straight->mapToScene({straight->width() / 2, straight->height() / 2}).toPoint();
  QTest::mouseClick(window, Qt::LeftButton, {}, choice);
  QCoreApplication::processEvents();
  if (bridge.revision() != initial + 2 ||
      lane_point(bridge, 1).value(QStringLiteral("shape")).toString() !=
          QStringLiteral("straight")) {
    std::cerr << "Tempo lane smoke: the shape menu did not make one edit: "
              << bridge.status().toStdString() << " revision " << bridge.revision() - initial
              << " shape "
              << lane_point(bridge, 1).value(QStringLiteral("shape")).toString().toStdString()
              << '\n';
    return false;
  }

  // Dragging the ramp's end down to the opening tempo is one edit; undo.
  const auto end_point = lane_point_at(lane, to_bar, top);
  const auto lower = lane_point_at(lane, to_bar, opening_bpm);
  QTest::mousePress(window, Qt::LeftButton, {}, end_point);
  for (int step = 1; step <= drag_steps; ++step)
    QTest::mouseMove(window, end_point + (lower - end_point) * step / drag_steps);
  QTest::mouseRelease(window, Qt::LeftButton, {}, lower);
  QCoreApplication::processEvents();
  if (bridge.revision() != initial + 3 ||
      std::abs(lane_point(bridge, 2).value(QStringLiteral("bpm")).toDouble() - opening_bpm) >
          bpm_tolerance) {
    std::cerr << "Tempo lane smoke: dragging a point did not make one edit\n";
    return false;
  }
  bridge.undo();
  if (std::abs(lane_point(bridge, 2).value(QStringLiteral("bpm")).toDouble() - top) >
      bpm_tolerance) {
    std::cerr << "Tempo lane smoke: undo did not restore the dragged point\n";
    return false;
  }

  // Double-click empty lane adds a point; double-click it again removes it.
  const auto added_bar = to_bar + ramp_bars;
  const auto add_at = lane_point_at(lane, added_bar, top);
  const auto before_add = bridge.revision();
  QTest::mouseDClick(window, Qt::LeftButton, {}, add_at);
  QCoreApplication::processEvents();
  if (bridge.revision() != before_add + 1 || lane_points(bridge).size() != 4) {
    std::cerr << "Tempo lane smoke: double-click did not add one point\n";
    return false;
  }
  QTest::mouseDClick(window, Qt::LeftButton, {}, lane_point_at(lane, added_bar, top));
  QCoreApplication::processEvents();
  if (bridge.revision() != before_add + 2 || lane_points(bridge).size() != 3) {
    std::cerr << "Tempo lane smoke: double-click did not remove the point\n";
    return false;
  }

  // The project controls edit the opening tempo and meter, keeping the ramps.
  const auto before_global_edit = bridge.revision();
  bridge.set_initial_tempo_bpm(std::numeric_limits<double>::quiet_NaN());
  if (bridge.revision() != before_global_edit) {
    std::cerr << "Tempo lane smoke: an invalid opening tempo changed the project\n";
    return false;
  }
  bridge.set_initial_tempo_bpm(edited_opening_bpm);
  if (bridge.revision() != before_global_edit + 1 ||
      bridge.project_timing().value(QStringLiteral("projectBpm")).toDouble() !=
          edited_opening_bpm ||
      lane_point(bridge, 1).value(QStringLiteral("shape")).toString() !=
          QStringLiteral("straight")) {
    std::cerr << "Tempo lane smoke: the opening tempo did not edit only the first point\n";
    return false;
  }
  bridge.undo();
  if (bridge.project_timing().value(QStringLiteral("projectBpm")).toDouble() != opening_bpm) {
    std::cerr << "Tempo lane smoke: undo did not restore the opening tempo\n";
    return false;
  }
  const auto before_meter = bridge.revision();
  bridge.set_initial_meter(edited_meter_numerator, invalid_meter_denominator);
  if (bridge.revision() != before_meter) {
    std::cerr << "Tempo lane smoke: an invalid meter changed the project\n";
    return false;
  }
  bridge.set_initial_meter(edited_meter_numerator, edited_meter_denominator);
  if (bridge.revision() != before_meter + 1 ||
      bridge.project_timing().value(QStringLiteral("projectMeter")).toString() !=
          QStringLiteral("3/4")) {
    std::cerr << "Tempo lane smoke: the meter did not commit one undoable change\n";
    return false;
  }
  bridge.undo();
  bridge.save();
  if (!bridge.status().startsWith(QStringLiteral("Saved"))) {
    std::cerr << "Tempo lane smoke: save failed\n";
    return false;
  }
  std::cout << "Qt Quick tempo lane drawn, shaped, undone and saved\n";
  return true;
#else
  (void)bridge;
  (void)window;
  (void)find_item;
  std::cerr << "Tempo lane smoke requires Qt Test input support\n";
  return false;
#endif
}

bool verify_tempo_map(oscillate::quick::ProjectBridge& bridge, QQuickWindow* window) {
  const auto points = lane_points(bridge);
  if (!window || points.size() != 3 ||
      points[1].toMap().value(QStringLiteral("shape")).toString() != QStringLiteral("straight") ||
      std::abs(points[1].toMap().value(QStringLiteral("bpm")).toDouble() - opening_bpm) >
          bpm_tolerance ||
      !(points[2].toMap().value(QStringLiteral("bpm")).toDouble() > opening_bpm) ||
      bridge.project_timing().value(QStringLiteral("projectMeter")).toString() !=
          QStringLiteral("4/4")) {
    std::cerr << "Tempo lane smoke: the saved ramp did not reopen\n";
    return false;
  }
  std::cout << "Qt Quick tempo lane reopened\n";
  return true;
}

} // namespace quick_smoke
