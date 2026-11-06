#include "oscillate/application/control_transport_gate.hpp"
#include "oscillate/test/check.hpp"

#include <atomic>
#include <barrier>
#include <iostream>
#include <memory>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {
using oscillate::application::ControlTransportGate;
using oscillate::foundation::ErrorCode;
using oscillate::project::StoppedTransportLease;

using oscillate::test::require;

void stopped_lease_excludes_start() {
  ControlTransportGate gate;
  auto first = gate.acquire_stopped();
  auto second = gate.acquire_stopped();
  REQUIRE_TRUE(first, "concurrent stopped operations should hold leases");
  REQUIRE_TRUE(second, "concurrent stopped operations should hold leases");
  auto denied = gate.try_start();
  REQUIRE_TRUE(!denied, "playback started while export or rate lease was held");
  REQUIRE_EQ(denied.error().code, ErrorCode::conflict,
             "playback started while export or rate lease was held");
  first.value().reset();
  require(!gate.try_start(), "releasing one of two leases allowed playback");
  second.value().reset();
  auto playing = gate.try_start();
  REQUIRE_OK(playing, "playback did not start after final lease released");
  require(!gate.acquire_stopped(), "stopped operation started during playback");
  require(!gate.try_start(), "second playback token started concurrently");
  playing.value().finish_after_join();
  REQUIRE_OK(gate.acquire_stopped(), "stopped operation did not resume after join acknowledgement");
}

void stop_request_does_not_release_running_authority() {
  ControlTransportGate gate;
  auto playing = gate.try_start();
  REQUIRE_OK(playing, "playback setup failed");
  std::binary_semaphore worker_exit{0};
  std::atomic<bool> callback_running{false};
  std::thread callback([&] {
    callback_running.store(true, std::memory_order_release);
    worker_exit.acquire();
  });
  while (!callback_running.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  // The caller has requested stop; the callback has not yet joined.
  require(!gate.acquire_stopped(), "stopped lease acquired before callback joined");
  worker_exit.release();
  callback.join();
  require(!gate.acquire_stopped(), "join without explicit finish released playback authority");
  playing.value().finish_after_join();
  REQUIRE_OK(gate.acquire_stopped(), "finish after join did not allow stopped operation");
}

void unfinished_token_fails_closed() {
  ControlTransportGate gate;
  {
    auto playing = gate.try_start();
    REQUIRE_OK(playing, "fail-closed fixture did not start");
  }
  REQUIRE_TRUE(!gate.try_start(),
               "dropping unfinished playback token silently marked transport stopped");
  REQUIRE_TRUE(!gate.acquire_stopped(),
               "dropping unfinished playback token silently marked transport stopped");
}

void owner_destruction_keeps_lease_alive() {
  std::unique_ptr<StoppedTransportLease> outstanding;
  {
    auto gate = std::make_unique<ControlTransportGate>();
    auto lease = gate->acquire_stopped();
    REQUIRE_OK(lease, "lifetime fixture could not acquire lease");
    outstanding = std::move(lease.value());
  }
  outstanding.reset();
  std::unique_ptr<ControlTransportGate::PlaybackToken> playing;
  {
    auto gate = std::make_unique<ControlTransportGate>();
    auto started = gate->try_start();
    REQUIRE_OK(started, "token lifetime fixture could not start");
    playing = std::make_unique<ControlTransportGate::PlaybackToken>(std::move(started.value()));
  }
  playing->finish_after_join();
}

void simultaneous_start_and_lease_are_exclusive() {
  for (int iteration = 0; iteration < 64; ++iteration) {
    ControlTransportGate gate;
    std::barrier arrive(3);
    std::barrier release(3);
    std::atomic<bool> started{false};
    std::atomic<bool> leased{false};
    std::thread player([&] {
      arrive.arrive_and_wait();
      auto token = gate.try_start();
      started.store(token.has_value(), std::memory_order_release);
      release.arrive_and_wait();
      if (token) {
        token.value().finish_after_join();
      }
    });
    std::thread exporter([&] {
      arrive.arrive_and_wait();
      auto lease = gate.acquire_stopped();
      leased.store(lease.has_value(), std::memory_order_release);
      release.arrive_and_wait();
    });
    arrive.arrive_and_wait();
    release.arrive_and_wait();
    player.join();
    exporter.join();
    REQUIRE_NE(started.load(std::memory_order_acquire), leased.load(std::memory_order_acquire),
               "start/export race admitted both or neither operation");
  }
}

} // namespace

int main() {
  try {
    stopped_lease_excludes_start();
    stop_request_does_not_release_running_authority();
    unfinished_token_fails_closed();
    owner_destruction_keeps_lease_alive();
    simultaneous_start_and_lease_are_exclusive();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
