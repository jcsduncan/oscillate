#include "smoke_routing_scenario.hpp"

#include "project_bridge.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QQuickItem>
#include <QQuickWindow>
#ifdef OSCILLATE_QUICK_TEST_INPUT
#include <QTest>
#endif

#include <algorithm>
#include <cmath>
#include <iostream>
#include <thread>

namespace quick_smoke {

namespace {

// The routing controls live in the inspector, whose open state is a saved
// preference; a fresh profile starts with it closed.
void open_inspector(QQuickWindow* window) {
  if (!window)
    return;
  window->setProperty("contextPanelOpen", true);
  QCoreApplication::processEvents();
}

} // namespace

int run_routing(QApplication& app, oscillate::quick::ProjectBridge& bridge, QQuickWindow* window,
                const RoutingPlaybackProbe& probe, RoutingItemLookup find_item,
                RoutingLabeledItemLookup find_labeled_item) {
#ifdef OSCILLATE_QUICK_TEST_INPUT
  open_inspector(window);
  auto* group =
      window ? find_item(window->contentItem(), QStringLiteral("createGroupBus")) : nullptr;
  auto* returns =
      window ? find_item(window->contentItem(), QStringLiteral("createReturnBus")) : nullptr;
  if (!group || !returns || bridge.tracks().size() != 1) {
    std::cerr << "Routing smoke: creation controls missing\n";
    return 1;
  }
  const auto initial = bridge.revision();
  group->forceActiveFocus();
  QTest::keyClick(window, Qt::Key_Return);
  returns->forceActiveFocus();
  QTest::keyClick(window, Qt::Key_Return);
  QCoreApplication::processEvents();
  auto tracks = bridge.tracks();
  if (tracks.size() != 3 || bridge.revision() != initial + 2 ||
      tracks[1].toMap().value(QStringLiteral("kind")).toString() != QStringLiteral("group") ||
      tracks[2].toMap().value(QStringLiteral("kind")).toString() != QStringLiteral("return") ||
      !bridge.routing_active()) {
    std::cerr << "Routing smoke: bus creation did not persist in model: "
              << bridge.status().toStdString() << '\n';
    return 1;
  }
  const auto group_id = tracks[1].toMap().value(QStringLiteral("id")).toString();
  const auto return_id = tracks[2].toMap().value(QStringLiteral("id")).toString();
  window->setProperty("selectedTrack", 0);
  QCoreApplication::processEvents();
  auto* main_route = find_item(window->contentItem(), QStringLiteral("mainRouteButton"));
  if (!main_route) {
    std::cerr << "Routing smoke: main destination control missing\n";
    return 1;
  }
  main_route->forceActiveFocus();
  QTest::keyClick(window, Qt::Key_Return);
  QCoreApplication::processEvents();
  auto* group_choice = find_labeled_item(window->contentItem(), QStringLiteral("mainRouteChoice"),
                                         QStringLiteral("Group 1"));
  if (!group_choice) {
    std::cerr << "Routing smoke: group destination not listed\n";
    return 1;
  }
  group_choice->forceActiveFocus();
  QTest::keyClick(window, Qt::Key_Return);
  QCoreApplication::processEvents();
  tracks = bridge.tracks();
  if (tracks.front().toMap().value(QStringLiteral("mainDestinationId")).toString() != group_id ||
      bridge.revision() != initial + 3) {
    std::cerr << "Routing smoke: main output command failed: " << bridge.status().toStdString()
              << '\n';
    return 1;
  }
  auto* send_destination =
      find_item(window->contentItem(), QStringLiteral("sendDestinationButton"));
  if (!send_destination) {
    std::cerr << "Routing smoke: send destination control missing\n";
    return 1;
  }
  send_destination->forceActiveFocus();
  QTest::keyClick(window, Qt::Key_Return);
  QCoreApplication::processEvents();
  auto* return_choice = find_labeled_item(window->contentItem(), QStringLiteral("sendRouteChoice"),
                                          QStringLiteral("Return 1"));
  if (!return_choice) {
    std::cerr << "Routing smoke: return destination not listed\n";
    return 1;
  }
  return_choice->forceActiveFocus();
  QTest::keyClick(window, Qt::Key_Return);
  QCoreApplication::processEvents();
  auto* add_send = find_item(window->contentItem(), QStringLiteral("addSendButton"));
  if (!add_send || !add_send->isEnabled()) {
    std::cerr << "Routing smoke: add send control disabled\n";
    return 1;
  }
  add_send->forceActiveFocus();
  QTest::keyClick(window, Qt::Key_Return);
  QCoreApplication::processEvents();
  auto sends = bridge.tracks().front().toMap().value(QStringLiteral("sends")).toList();
  if (bridge.revision() != initial + 4 || sends.size() != 1 ||
      sends.front().toMap().value(QStringLiteral("destinationId")).toString() != return_id ||
      sends.front().toMap().value(QStringLiteral("gain")).toDouble() != 0.5) {
    std::cerr << "Routing smoke: send was not added: " << bridge.status().toStdString() << '\n';
    return 1;
  }
  const auto send_id = sends.front().toMap().value(QStringLiteral("id")).toString();
  auto* gain = find_item(window->contentItem(), QStringLiteral("sendGainInput"));
  if (!gain) {
    std::cerr << "Routing smoke: gain editor missing\n";
    return 1;
  }
  gain->forceActiveFocus();
  QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
  QTest::keyClick(window, Qt::Key_0);
  QTest::keyClick(window, Qt::Key_Period);
  QTest::keyClick(window, Qt::Key_2);
  QTest::keyClick(window, Qt::Key_5);
  QTest::keyClick(window, Qt::Key_Return);
  QCoreApplication::processEvents();
  sends = bridge.tracks().front().toMap().value(QStringLiteral("sends")).toList();
  if (bridge.revision() != initial + 5 || sends.size() != 1 ||
      sends.front().toMap().value(QStringLiteral("id")).toString() != send_id ||
      sends.front().toMap().value(QStringLiteral("gain")).toDouble() != 0.25) {
    std::cerr << "Routing smoke: gain edit failed: " << bridge.status().toStdString() << '\n';
    return 1;
  }
  bridge.undo();
  sends = bridge.tracks().front().toMap().value(QStringLiteral("sends")).toList();
  if (sends.size() != 1 || sends.front().toMap().value(QStringLiteral("gain")).toDouble() != 0.5) {
    std::cerr << "Routing smoke: one undo did not restore gain\n";
    return 1;
  }
  bridge.redo();
  auto* remove = find_item(window->contentItem(), QStringLiteral("removeSendButton"));
  if (!remove) {
    std::cerr << "Routing smoke: remove send control missing\n";
    return 1;
  }
  remove->forceActiveFocus();
  QTest::keyClick(window, Qt::Key_Return);
  QCoreApplication::processEvents();
  if (!bridge.tracks().front().toMap().value(QStringLiteral("sends")).toList().empty()) {
    std::cerr << "Routing smoke: remove send failed\n";
    return 1;
  }
  bridge.undo();
  sends = bridge.tracks().front().toMap().value(QStringLiteral("sends")).toList();
  if (sends.size() != 1 ||
      sends.front().toMap().value(QStringLiteral("id")).toString() != send_id) {
    std::cerr << "Routing smoke: remove undo lost send identity\n";
    return 1;
  }
  bridge.save();
  bridge.toggle_playback();
  auto wait_for = [&app](auto&& ready) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 5'000) {
      app.processEvents();
      if (ready())
        return true;
      std::this_thread::yield();
    }
    return false;
  };
  if (!bridge.status().startsWith(QStringLiteral("Saved")) || !wait_for([&bridge] {
        return bridge.playback_status().contains(QStringLiteral("Playing routed revision"));
      }) ||
      !bridge.playback_active() || bridge.exports()->export_enabled() ||
      std::fabs(probe.first_sample.load(std::memory_order_acquire) - 0.3125F) > 0.00001F ||
      probe.first_completed_sample.load(std::memory_order_acquire) != 64) {
    std::cerr << "Routing smoke: saved bus/send did not render or reserve playback: "
              << bridge.playback_status().toStdString() << '\n';
    return 1;
  }
  // Playback holds the project: an export asked for now is refused.
  bridge.exports()->export_master();
  if (bridge.exports()->export_busy() ||
      !bridge.exports()->export_status().contains(QStringLiteral("Stop playback"))) {
    std::cerr << "Routing smoke: export was not refused during routed playback\n";
    return 1;
  }
  auto* meter_display = find_item(window->contentItem(), QStringLiteral("masterMeter"));
  const auto first_meter = bridge.master_meter();
  if (!meter_display || !first_meter.value(QStringLiteral("valid")).toBool() ||
      std::fabs(first_meter.value(QStringLiteral("leftPeak")).toDouble() - 0.3125) > 0.00001 ||
      std::fabs(first_meter.value(QStringLiteral("leftRms")).toDouble() - 0.3125) > 0.00001 ||
      !wait_for([meter_display] {
        const auto* reading = meter_display->property("reading").value<QObject*>();
        return reading && reading->property("valid").toBool() &&
               reading->property("leftPeak").toDouble() > 0.3;
      })) {
    std::cerr << "Routing smoke: master meter did not show routed output\n";
    return 1;
  }
  // The source track's own post-fader node meter, keyed by its track ID.
  const auto source_track_id =
      bridge.tracks().front().toMap().value(QStringLiteral("id")).toString();
  const auto source_meter = [&bridge, &source_track_id] {
    return bridge.track_meters().value(source_track_id).toMap();
  };
  if (!wait_for([&source_meter] {
        const auto meter = source_meter();
        return meter.value(QStringLiteral("valid")).toBool() &&
               meter.value(QStringLiteral("leftPeak")).toDouble() > 0.0 &&
               meter.value(QStringLiteral("leftRms")).toDouble() > 0.0;
      }) ||
      !bridge.cpu_load().isValid() || bridge.cpu_load().toDouble() < 0.0 ||
      bridge.cpu_load().toDouble() > 1.0) {
    std::cerr << "Routing smoke: routed track meter or DSP load did not go live\n";
    return 1;
  }
  const auto audible_revision = bridge.revision();
  bridge.set_send_gain(send_id, 0.5);
  if (bridge.revision() != audible_revision + 1 ||
      !bridge.playback_status().contains(QStringLiteral("not active")) ||
      !bridge.playback_status().contains(QString::number(audible_revision)) ||
      std::fabs(probe.first_sample.load(std::memory_order_acquire) - 0.3125F) > 0.00001F) {
    std::cerr << "Routing smoke: accepted edit was misreported as audible\n";
    return 1;
  }
  bridge.save();
  bridge.toggle_playback();
  if (!wait_for([&bridge] { return !bridge.playback_active(); }) ||
      !probe.stopped_off_ui.load(std::memory_order_acquire) ||
      !bridge.exports()->export_enabled() ||
      bridge.master_meter().value(QStringLiteral("valid")).toBool() ||
      source_meter().value(QStringLiteral("valid")).toBool() || bridge.cpu_load().isValid()) {
    std::cerr << "Routing smoke: output did not retire off UI thread\n";
    return 1;
  }
  bridge.toggle_playback();
  if (!wait_for([&bridge] {
        return bridge.playback_status().contains(QStringLiteral("Playing routed revision"));
      }) ||
      !bridge.playback_status().contains(QString::number(bridge.revision())) ||
      std::fabs(probe.first_sample.load(std::memory_order_acquire) - 0.375F) > 0.00001F ||
      std::fabs(bridge.master_meter().value(QStringLiteral("leftPeak")).toDouble() - 0.375) >
          0.00001) {
    std::cerr << "Routing smoke: saved revision was not audible after Stop and Play: "
              << bridge.playback_status().toStdString() << '\n';
    return 1;
  }
  bridge.toggle_playback();
  if (!wait_for([&bridge] { return !bridge.playback_active(); })) {
    std::cerr << "Routing smoke: final routed output did not stop\n";
    return 1;
  }
  probe.delay_start.store(true, std::memory_order_release);
  bridge.toggle_playback();
  if (!bridge.playback_active() || !bridge.playback_enabled()) {
    std::cerr << "Routing smoke: preparation did not expose Stop\n";
    return 1;
  }
  bridge.toggle_playback();
  if (!wait_for([&bridge] { return !bridge.playback_active(); }) ||
      !bridge.playback_status().contains(QStringLiteral("preparation stopped")) ||
      !bridge.exports()->export_enabled()) {
    std::cerr << "Routing smoke: Stop during preparation did not retire the device: "
              << bridge.playback_status().toStdString() << '\n';
    return 1;
  }
  probe.delay_start.store(false, std::memory_order_release);
  bridge.add_instrument_track();
  const auto unsupported_tracks = bridge.tracks();
  if (unsupported_tracks.size() != 4) {
    std::cerr << "Routing smoke: unsupported instrument fixture failed\n";
    return 1;
  }
  // The new instrument sits above the return bus.
  const auto instrument = std::ranges::find_if(unsupported_tracks, [](const QVariant& track) {
    return track.toMap().value(QStringLiteral("kind")).toString() == QStringLiteral("instrument");
  });
  if (instrument == unsupported_tracks.end()) {
    std::cerr << "Routing smoke: unsupported instrument fixture failed\n";
    return 1;
  }
  bridge.add_midi_clip(instrument->toMap().value(QStringLiteral("id")).toString(), 0);
  bridge.toggle_playback();
  if (!wait_for([&bridge] {
        return bridge.playback_status().contains(QStringLiteral("verified bound MIDI instrument"));
      }) ||
      bridge.playback_active() || !bridge.exports()->export_enabled()) {
    std::cerr << "Routing smoke: unsupported instrument was not rejected or gate retained: "
              << bridge.playback_status().toStdString() << '\n';
    return 1;
  }
  bridge.undo();
  bridge.undo();
  bridge.save();
  if (bridge.tracks().size() != 3) {
    std::cerr << "Routing smoke: unsupported fixture undo changed saved tracks\n";
    return 1;
  }
  std::cout << "Qt Quick routed audio played, edited, stopped and reopened\n";
  return 0;
#else
  std::cerr << "Routing smoke requires Qt Test input support\n";
  return 1;
#endif
}

int verify_routing(oscillate::quick::ProjectBridge& bridge, QQuickWindow* window,
                   RoutingVisibleCount visible_count) {
  open_inspector(window);
  const auto tracks = bridge.tracks();
  const auto source = tracks.empty() ? QVariantMap{} : tracks.front().toMap();
  const auto sends = source.value(QStringLiteral("sends")).toList();
  if (tracks.size() != 3 ||
      tracks[1].toMap().value(QStringLiteral("kind")).toString() != QStringLiteral("group") ||
      tracks[2].toMap().value(QStringLiteral("kind")).toString() != QStringLiteral("return") ||
      source.value(QStringLiteral("mainDestinationId")).toString() !=
          tracks[1].toMap().value(QStringLiteral("id")).toString() ||
      sends.size() != 1 ||
      sends.front().toMap().value(QStringLiteral("destinationId")).toString() !=
          tracks[2].toMap().value(QStringLiteral("id")).toString() ||
      sends.front().toMap().value(QStringLiteral("gain")).toDouble() != 0.5 ||
      !bridge.routing_active() || !window ||
      visible_count(window->contentItem(), QStringLiteral("routingSendRow")) != 1) {
    std::cerr << "Routing smoke: saved signal flow did not reopen in inspector\n";
    return 1;
  }
  std::cout << "Qt Quick routing reopened\n";
  return 0;
}

} // namespace quick_smoke
