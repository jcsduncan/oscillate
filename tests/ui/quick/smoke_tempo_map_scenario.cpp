#include "smoke_tempo_map_scenario.hpp"

#include "project_bridge.hpp"
#include "smoke_import_scenario.hpp"

#include <QCoreApplication>
#include <QQuickItem>
#include <QQuickWindow>
#ifdef OSCILLATE_QUICK_TEST_INPUT
#include <QTest>
#endif

#include <cmath>
#include <iostream>
#include <limits>

namespace quick_smoke {

bool edit_tempo_map(oscillate::quick::ProjectBridge& bridge, QQuickWindow* window,
                    VisibleItemLookup find_item) {
#ifdef OSCILLATE_QUICK_TEST_INPUT
  if (window) {
    window->setProperty("contextPanelOpen", true);
    QCoreApplication::processEvents();
  }
  auto* bar = window ? find_item(window->contentItem(), QStringLiteral("tempoBarInput")) : nullptr;
  auto* beat =
      window ? find_item(window->contentItem(), QStringLiteral("tempoBeatInput")) : nullptr;
  auto* bpm = window ? find_item(window->contentItem(), QStringLiteral("tempoBpmInput")) : nullptr;
  auto* shape =
      window ? find_item(window->contentItem(), QStringLiteral("tempoShapeButton")) : nullptr;
  auto* apply =
      window ? find_item(window->contentItem(), QStringLiteral("tempoApplyButton")) : nullptr;
  if (!bar || !beat || !bpm || !shape || !apply ||
      !find_item(window->contentItem(), QStringLiteral("tempoMapEditor")) ||
      bridge.tempo_map().value(QStringLiteral("eventCount")).toInt() != 1) {
    std::cerr << "Tempo map smoke: editor controls or initial event missing\n";
    return false;
  }
  const auto initial = bridge.revision();
  bridge.add_tempo_event(3, 5, 100.0, 0);
  if (bridge.revision() != initial || !bridge.status().contains(QStringLiteral("position"))) {
    std::cerr << "Tempo map smoke: invalid meter beat changed the project\n";
    return false;
  }
  bridge.add_tempo_event(3, 1, 0.0, 0);
  if (bridge.revision() != initial || !bridge.status().contains(QStringLiteral("BPM"))) {
    std::cerr << "Tempo map smoke: invalid BPM changed the project\n";
    return false;
  }
  bar->setProperty("text", QStringLiteral("5"));
  beat->setProperty("text", QStringLiteral("1"));
  bpm->setProperty("text", QStringLiteral("90"));
  apply->forceActiveFocus();
  QTest::keyClick(window, Qt::Key_Return);
  QCoreApplication::processEvents();
  auto map = bridge.tempo_map();
  auto events = map.value(QStringLiteral("events")).toList();
  if (bridge.revision() != initial + 1 || events.size() != 2 ||
      events[1].toMap().value(QStringLiteral("position")).toString() !=
          QStringLiteral("BAR 5 · BEAT 1") ||
      std::abs(events[1].toMap().value(QStringLiteral("elapsedSeconds")).toDouble() - 8.0) >
          0.0001) {
    std::cerr << "Tempo map smoke: keyboard add or authoritative elapsed time failed: "
              << bridge.status().toStdString() << '\n';
    return false;
  }
  bar->setProperty("text", QStringLiteral("3"));
  bpm->setProperty("text", QStringLiteral("100"));
  shape->forceActiveFocus();
  QTest::keyClick(window, Qt::Key_Return);
  apply->forceActiveFocus();
  QTest::keyClick(window, Qt::Key_Return);
  QCoreApplication::processEvents();
  map = bridge.tempo_map();
  events = map.value(QStringLiteral("events")).toList();
  if (bridge.revision() != initial + 2 || events.size() != 3 ||
      events[1].toMap().value(QStringLiteral("shapeIndex")).toInt() != 1 ||
      events[2].toMap().value(QStringLiteral("elapsedSeconds")).toDouble() <= 8.5) {
    std::cerr << "Tempo map smoke: ramp and timing projection failed: "
              << bridge.status().toStdString() << '\n';
    return false;
  }
  const auto bar_three_tick = events[1].toMap().value(QStringLiteral("tick")).toString();
  const auto bar_five_tick = events[2].toMap().value(QStringLiteral("tick")).toString();
  const auto before_edit = bridge.revision();
  bridge.update_tempo_event(bar_three_tick, 80.0, 1);
  map = bridge.tempo_map();
  events = map.value(QStringLiteral("events")).toList();
  if (bridge.revision() != before_edit + 1 ||
      events[2].toMap().value(QStringLiteral("elapsedSeconds")).toDouble() <= 9.0) {
    std::cerr << "Tempo map smoke: BPM edit did not change authoritative timing\n";
    return false;
  }
  bridge.update_tempo_event(bar_three_tick, 80.0, 1);
  if (bridge.revision() != before_edit + 1) {
    std::cerr << "Tempo map smoke: unchanged event made an undo step\n";
    return false;
  }
  bridge.update_tempo_event(bar_five_tick, 90.0, 1);
  bridge.remove_tempo_event(QStringLiteral("0"));
  if (bridge.revision() != before_edit + 1) {
    std::cerr << "Tempo map smoke: invalid final shape or initial removal changed the map\n";
    return false;
  }
  bridge.update_tempo_event(bar_three_tick, 100.0, 1);
  const auto before_blocked_remove = bridge.revision();
  bridge.remove_tempo_event(bar_five_tick);
  if (bridge.revision() != before_blocked_remove ||
      !bridge.status().contains(QStringLiteral("preceding event"))) {
    std::cerr << "Tempo map smoke: final ramp removal lacked an actionable error\n";
    return false;
  }
  bridge.add_tempo_event(7, 1, 110.0, 0);
  bridge.add_tempo_event(9, 1, 120.0, 0);
  map = bridge.tempo_map();
  if (map.value(QStringLiteral("eventCount")).toInt() != 5 ||
      map.value(QStringLiteral("pageCount")).toInt() != 2 ||
      map.value(QStringLiteral("page")).toInt() != 1 ||
      map.value(QStringLiteral("events")).toList().size() != 1) {
    std::cerr << "Tempo map smoke: bounded paging failed\n";
    return false;
  }
  auto* previous = find_item(window->contentItem(), QStringLiteral("tempoPreviousPage"));
  if (!previous) {
    std::cerr << "Tempo map smoke: previous page action missing\n";
    return false;
  }
  previous->forceActiveFocus();
  QTest::keyClick(window, Qt::Key_Return);
  QCoreApplication::processEvents();
  if (bridge.tempo_map().value(QStringLiteral("page")).toInt() != 0 ||
      count_visible_items(window->contentItem(), QStringLiteral("tempoEventRow")) != 4) {
    std::cerr << "Tempo map smoke: keyboard previous page failed\n";
    return false;
  }
  auto* next = find_item(window->contentItem(), QStringLiteral("tempoNextPage"));
  if (!next) {
    std::cerr << "Tempo map smoke: next page action missing\n";
    return false;
  }
  next->forceActiveFocus();
  QTest::keyClick(window, Qt::Key_Return);
  QCoreApplication::processEvents();
  if (bridge.tempo_map().value(QStringLiteral("page")).toInt() != 1) {
    std::cerr << "Tempo map smoke: keyboard next page failed\n";
    return false;
  }
  bridge.set_tempo_event_page(0);
  auto* edit = find_item(window->contentItem(), QStringLiteral("tempoEditEvent"));
  if (!edit) {
    std::cerr << "Tempo map smoke: edit action missing\n";
    return false;
  }
  edit->forceActiveFocus();
  QTest::keyClick(window, Qt::Key_Return);
  bpm->setProperty("text", QStringLiteral("110"));
  const auto before_ui_edit = bridge.revision();
  apply->forceActiveFocus();
  QTest::keyClick(window, Qt::Key_Return);
  QCoreApplication::processEvents();
  events = bridge.tempo_map().value(QStringLiteral("events")).toList();
  if (bridge.revision() != before_ui_edit + 1 ||
      events.front().toMap().value(QStringLiteral("bpm")).toDouble() != 110.0) {
    std::cerr << "Tempo map smoke: keyboard edit did not commit one step\n";
    return false;
  }
  bridge.undo();
  events = bridge.tempo_map().value(QStringLiteral("events")).toList();
  if (events.front().toMap().value(QStringLiteral("bpm")).toDouble() != 120.0) {
    std::cerr << "Tempo map smoke: one undo did not restore BPM\n";
    return false;
  }
  auto* remove = find_item(window->contentItem(), QStringLiteral("tempoRemoveEvent"));
  if (!remove) {
    std::cerr << "Tempo map smoke: remove action missing\n";
    return false;
  }
  const auto before_remove = bridge.revision();
  remove->forceActiveFocus();
  QTest::keyClick(window, Qt::Key_Return);
  QCoreApplication::processEvents();
  if (bridge.revision() != before_remove + 1 ||
      bridge.tempo_map().value(QStringLiteral("eventCount")).toInt() != 4) {
    std::cerr << "Tempo map smoke: keyboard removal did not commit one step\n";
    return false;
  }
  bridge.undo();
  if (bridge.tempo_map().value(QStringLiteral("eventCount")).toInt() != 5) {
    std::cerr << "Tempo map smoke: one undo did not restore removed event\n";
    return false;
  }
  const auto before_global_edit = bridge.revision();
  const auto initial_shape = bridge.tempo_map()
                                 .value(QStringLiteral("events"))
                                 .toList()
                                 .front()
                                 .toMap()
                                 .value(QStringLiteral("shapeIndex"))
                                 .toInt();
  bridge.set_tempo_event_page(1);
  bridge.set_initial_tempo_bpm(std::numeric_limits<double>::quiet_NaN());
  if (bridge.revision() != before_global_edit) {
    std::cerr << "Tempo map smoke: invalid global BPM changed the project\n";
    return false;
  }
  bridge.set_initial_tempo_bpm(105.0);
  if (bridge.revision() != before_global_edit + 1 ||
      bridge.project_timing().value(QStringLiteral("projectBpm")).toDouble() != 105.0 ||
      bridge.project_timing().value(QStringLiteral("projectMeter")).toString() !=
          QStringLiteral("4/4")) {
    std::cerr << "Tempo map smoke: global BPM did not edit the initial event while paged\n";
    return false;
  }
  bridge.undo();
  bridge.set_tempo_event_page(0);
  if (bridge.project_timing().value(QStringLiteral("projectBpm")).toDouble() != 120.0 ||
      bridge.tempo_map()
              .value(QStringLiteral("events"))
              .toList()
              .front()
              .toMap()
              .value(QStringLiteral("shapeIndex"))
              .toInt() != initial_shape) {
    std::cerr << "Tempo map smoke: undo failed or global BPM changed the ramp shape\n";
    return false;
  }
  const auto before_meter = bridge.revision();
  bridge.set_initial_meter(3, 3);
  if (bridge.revision() != before_meter) {
    std::cerr << "Tempo map smoke: invalid meter changed the project\n";
    return false;
  }
  bridge.set_initial_meter(3, 4);
  if (bridge.revision() != before_meter + 1 ||
      bridge.project_timing().value(QStringLiteral("projectMeter")).toString() !=
          QStringLiteral("3/4")) {
    std::cerr << "Tempo map smoke: global meter did not commit one undoable change\n";
    return false;
  }
  bridge.undo();
  if (bridge.project_timing().value(QStringLiteral("projectMeter")).toString() !=
      QStringLiteral("4/4")) {
    std::cerr << "Tempo map smoke: undo did not restore global meter\n";
    return false;
  }
  bridge.save();
  if (!bridge.status().startsWith(QStringLiteral("Saved"))) {
    std::cerr << "Tempo map smoke: save failed\n";
    return false;
  }
  std::cout << "Qt Quick tempo map edited, paged, undone and saved\n";
  return true;
#else
  std::cerr << "Tempo map smoke requires Qt Test input support\n";
  return false;
#endif
}

bool verify_tempo_map(oscillate::quick::ProjectBridge& bridge, QQuickWindow* window) {
  if (window) {
    window->setProperty("contextPanelOpen", true);
    QCoreApplication::processEvents();
  }
  auto map = bridge.tempo_map();
  auto events = map.value(QStringLiteral("events")).toList();
  if (!window || map.value(QStringLiteral("eventCount")).toInt() != 5 ||
      map.value(QStringLiteral("pageCount")).toInt() != 2 || events.size() != 4 ||
      events[1].toMap().value(QStringLiteral("position")).toString() !=
          QStringLiteral("BAR 3 · BEAT 1") ||
      events[1].toMap().value(QStringLiteral("bpm")).toDouble() != 100.0 ||
      events[1].toMap().value(QStringLiteral("shapeIndex")).toInt() != 1 ||
      events[2].toMap().value(QStringLiteral("elapsedSeconds")).toDouble() <= 8.5 ||
      count_visible_items(window->contentItem(), QStringLiteral("tempoEventRow")) != 4) {
    std::cerr << "Tempo map smoke: saved events did not reopen\n";
    return false;
  }
  bridge.set_tempo_event_page(1);
  events = bridge.tempo_map().value(QStringLiteral("events")).toList();
  if (events.size() != 1 || events.front().toMap().value(QStringLiteral("position")).toString() !=
                                QStringLiteral("BAR 9 · BEAT 1")) {
    std::cerr << "Tempo map smoke: saved later events are inaccessible\n";
    return false;
  }
  std::cout << "Qt Quick tempo map reopened\n";
  return true;
}

} // namespace quick_smoke
