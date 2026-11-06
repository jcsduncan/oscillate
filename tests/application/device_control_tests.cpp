#include "oscillate/test/check.hpp"

#include <oscillate/application/device_control.hpp>
#include <oscillate/application/playback_gate.hpp>

#include <atomic>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {
using oscillate::application::DeviceEvent;
using oscillate::application::DeviceEventKind;
using oscillate::application::DeviceLossReason;
using oscillate::application::DevicePlaybackControl;
using oscillate::application::DevicePlaybackState;

using oscillate::test::require;

void loss_requires_explicit_reselection_and_priming() {
  DevicePlaybackControl control;
  REQUIRE_EQ(control.status().state, DevicePlaybackState::stopped, "initial transport is stopped");
  REQUIRE_TRUE(!control.transport_enabled(), "initial transport is stopped");
  require(!control.select_target(0), "zero is not a valid device target");
  const auto first = control.select_target(42).value_or(0);
  REQUIRE_EQ(first, 1, "selection starts a fresh device attempt");
  REQUIRE_EQ(control.status().state, DevicePlaybackState::awaiting_device,
             "selection starts a fresh device attempt");
  require(!control.primed(first), "source priming cannot precede device readiness");
  require(!control.observe({DeviceEventKind::ready, 43, first}),
          "other device readiness cannot start playback");
  require(control.observe({DeviceEventKind::ready, 42, first}),
          "selected device readiness starts source priming");
  REQUIRE_EQ(control.status().state, DevicePlaybackState::priming,
             "device readiness alone cannot advance transport");
  REQUIRE_TRUE(!control.transport_enabled(), "device readiness alone cannot advance transport");
  REQUIRE_TRUE(control.primed(first), "matching priming completion enables transport");
  REQUIRE_TRUE(control.transport_enabled(), "matching priming completion enables transport");
  require(!control.observe({DeviceEventKind::ready, 42, first}),
          "duplicate readiness cannot restart a playing stream");

  require(control.observe({DeviceEventKind::target_lost, 42, first}),
          "loss of selected target is accepted");
  REQUIRE_EQ(control.status().state, DevicePlaybackState::needs_reselection,
             "target loss immediately closes callback transport gate");
  REQUIRE_EQ(control.status().loss_reason, DeviceLossReason::target_lost,
             "target loss immediately closes callback transport gate");
  REQUIRE_TRUE(!control.transport_enabled(),
               "target loss immediately closes callback transport gate");
  REQUIRE_TRUE(!control.observe({DeviceEventKind::ready, 42, first}),
               "auto reconnect cannot resume a lost selection");
  REQUIRE_TRUE(!control.primed(first), "auto reconnect cannot resume a lost selection");

  const auto second = control.select_target(42).value_or(0);
  REQUIRE_GT(second, first, "retrying the same target has a new epoch and needs priming");
  REQUIRE_TRUE(!control.transport_enabled(),
               "retrying the same target has a new epoch and needs priming");
  REQUIRE_TRUE(!control.observe({DeviceEventKind::ready, 42, first}),
               "stale events from the old selection cannot enable transport");
  REQUIRE_TRUE(!control.primed(first),
               "stale events from the old selection cannot enable transport");
  REQUIRE_TRUE(control.observe({DeviceEventKind::ready, 42, second}),
               "current selection can resume only after both transitions");
  REQUIRE_TRUE(control.primed(second), "current selection can resume only after both transitions");
  require(control.transport_enabled(), "reselected transport is enabled");

  require(control.observe({DeviceEventKind::format_invalid, 42, second}),
          "format loss while playing is accepted");
  REQUIRE_EQ(control.status().state, DevicePlaybackState::needs_reselection,
             "invalid format immediately closes callback transport gate");
  REQUIRE_EQ(control.status().loss_reason, DeviceLossReason::invalid_format,
             "invalid format immediately closes callback transport gate");
  REQUIRE_TRUE(!control.transport_enabled(),
               "invalid format immediately closes callback transport gate");

  control.stop();
  REQUIRE_EQ(control.status().state, DevicePlaybackState::stopped,
             "explicit stop clears selected target and callback gate");
  REQUIRE_TRUE(!control.status().target_serial,
               "explicit stop clears selected target and callback gate");
  REQUIRE_TRUE(!control.transport_enabled(),
               "explicit stop clears selected target and callback gate");
  require(!control.observe({DeviceEventKind::ready, 42, second}),
          "late readiness after stop is ignored");
}

void invalid_format_and_loss_during_priming() {
  DevicePlaybackControl control;
  const auto first = control.select_target(10).value_or(0);
  REQUIRE_NE(first, 0, "first device enters priming");
  REQUIRE_TRUE(control.observe({DeviceEventKind::ready, 10, first}), "first device enters priming");
  require(control.observe({DeviceEventKind::format_invalid, 10, first}),
          "format invalidation during priming is handled");
  REQUIRE_EQ(control.status().state, DevicePlaybackState::needs_reselection,
             "invalid format cannot be rescued by late priming completion");
  REQUIRE_EQ(control.status().loss_reason, DeviceLossReason::invalid_format,
             "invalid format cannot be rescued by late priming completion");
  REQUIRE_TRUE(!control.primed(first),
               "invalid format cannot be rescued by late priming completion");
  REQUIRE_TRUE(!control.transport_enabled(),
               "invalid format cannot be rescued by late priming completion");

  const auto second = control.select_target(11).value_or(0);
  REQUIRE_NE(second, 0, "new selection clears old loss reason");
  REQUIRE_EQ(control.status().loss_reason, DeviceLossReason::none,
             "new selection clears old loss reason");
  require(!control.observe({DeviceEventKind::target_lost, 10, first}),
          "stale target loss cannot interrupt current attempt");
  require(control.observe({DeviceEventKind::ready, 11, second}), "second device enters priming");
  require(control.observe({DeviceEventKind::target_lost, 11, second}),
          "current target can be lost during priming");
  REQUIRE_EQ(control.status().state, DevicePlaybackState::needs_reselection,
             "loss during connection still requires a new selection");
  REQUIRE_TRUE(!control.transport_enabled(),
               "loss during connection still requires a new selection");
}

void callback_gate_closes_after_control_event() {
  DevicePlaybackControl control;
  const auto epoch = control.select_target(77).value_or(0);
  REQUIRE_NE(epoch, 0, "fixture begins in playing state");
  REQUIRE_TRUE(control.observe({DeviceEventKind::ready, 77, epoch}),
               "fixture begins in playing state");
  REQUIRE_TRUE(control.primed(epoch), "fixture begins in playing state");
  std::atomic<bool> event_applied{false};
  std::atomic<bool> callback_observed_enabled{true};
  std::thread callback([&] {
    while (!event_applied.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    callback_observed_enabled.store(control.transport_enabled(), std::memory_order_release);
  });
  const auto lost = control.observe({DeviceEventKind::target_lost, 77, epoch});
  event_applied.store(true, std::memory_order_release);
  callback.join();
  require(lost, "control thread applies device loss");
  require(!callback_observed_enabled.load(std::memory_order_acquire),
          "callback observes closed gate after loss event");
}

void source_priming_also_blocks_transport() {
  oscillate::application::PlaybackReadiness readiness;
  oscillate::application::PlaybackGate default_gate(readiness);
  require(!default_gate.enabled(), "unprimed default device cannot advance transport");
  readiness.set_steady(true);
  require(default_gate.enabled(), "primed default device may advance transport");
  readiness.set_steady(false);
  require(!default_gate.enabled(), "seek priming closes default-device transport gate");

  DevicePlaybackControl device;
  oscillate::application::PlaybackGate selected_gate(readiness, &device);
  const auto epoch = device.select_target(50).value_or(0);
  readiness.set_steady(true);
  require(!selected_gate.enabled(), "source readiness cannot bypass unready selected device");
  REQUIRE_TRUE(device.observe({DeviceEventKind::ready, 50, epoch}),
               "selected device becomes ready after priming");
  REQUIRE_TRUE(device.primed(epoch), "selected device becomes ready after priming");
  require(selected_gate.enabled(), "both ready sources allow transport");
  readiness.set_steady(false);
  require(!selected_gate.enabled(), "seek priming stops selected-device transport");
  readiness.set_steady(true);
  require(selected_gate.enabled(), "completed seek priming may resume selected device");
  REQUIRE_TRUE(device.observe({DeviceEventKind::target_lost, 50, epoch}),
               "source readiness cannot bypass target loss");
  REQUIRE_TRUE(!selected_gate.enabled(), "source readiness cannot bypass target loss");
}
} // namespace

int main() {
  try {
    loss_requires_explicit_reselection_and_priming();
    invalid_format_and_loss_during_priming();
    callback_gate_closes_after_control_event();
    source_priming_also_blocks_transport();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
