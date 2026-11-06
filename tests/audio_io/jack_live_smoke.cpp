#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"

#include <oscillate/audio_io/jack_output.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>

#include <jack/jack.h>

namespace {
using namespace std::chrono_literals;
using oscillate::audio_io::JackOutput;
using oscillate::audio_io::JackStartConfig;
using oscillate::audio_io::OutputState;
using oscillate::audio_io::TransportCursor;

struct RenderProbe {
  std::atomic<std::uint64_t> callbacks{0};
  static bool render(void* context, std::span<oscillate::engine::StereoFrame> frames,
                     oscillate::engine::BlockContext) noexcept {
    auto& self = *static_cast<RenderProbe*>(context);
    for (auto& frame : frames) {
      frame = {0.25F, -0.5F};
    }
    self.callbacks.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
};

struct Gate {
  std::atomic<bool> enabled{true};
  static bool allowed(const void* context) noexcept {
    return static_cast<const Gate*>(context)->enabled.load(std::memory_order_acquire);
  }
};

struct CaptureProbe {
  jack_client_t* client = nullptr;
  jack_port_t* left = nullptr;
  jack_port_t* right = nullptr;
  std::atomic<std::uint64_t> signal_blocks{0};
  std::atomic<std::uint64_t> silent_blocks{0};
  std::atomic<std::uint64_t> unexpected_blocks{0};

  static int process(jack_nframes_t frames, void* context) noexcept {
    auto& self = *static_cast<CaptureProbe*>(context);
    const auto* left = static_cast<const float*>(jack_port_get_buffer(self.left, frames));
    const auto* right = static_cast<const float*>(jack_port_get_buffer(self.right, frames));
    if (left == nullptr || right == nullptr || frames == 0) {
      self.unexpected_blocks.fetch_add(1, std::memory_order_relaxed);
      return 0;
    }
    if (left[0] == 0.25F && right[0] == -0.5F) {
      self.signal_blocks.fetch_add(1, std::memory_order_relaxed);
    } else if (left[0] == 0.0F && right[0] == 0.0F) {
      self.silent_blocks.fetch_add(1, std::memory_order_relaxed);
    } else {
      self.unexpected_blocks.fetch_add(1, std::memory_order_relaxed);
    }
    return 0;
  }

  void open() {
    jack_status_t status = JackFailure;
    client = jack_client_open("Oscillate smoke capture", JackNoStartServer, &status);
    if (client == nullptr) {
      throw std::runtime_error("cannot open capture client on selected JACK server");
    }
    left = jack_port_register(client, "left", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    right = jack_port_register(client, "right", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    if (left == nullptr || right == nullptr ||
        jack_set_process_callback(client, process, this) != 0 || jack_activate(client) != 0) {
      throw std::runtime_error("cannot activate JACK capture ports");
    }
    if (jack_connect(client, "Oscillate:left", jack_port_name(left)) != 0 ||
        jack_connect(client, "Oscillate:right", jack_port_name(right)) != 0) {
      throw std::runtime_error("cannot connect JACK output to capture probe");
    }
  }

  void close() noexcept {
    if (client != nullptr) {
      (void)jack_client_close(client);
      client = nullptr;
    }
  }
  ~CaptureProbe() { close(); }
};

template <typename Predicate>
bool until(Predicate predicate, std::chrono::steady_clock::duration timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(2ms);
  }
  return predicate();
}

using oscillate::test::require;

void run(bool recovery) {
  RenderProbe render;
  Gate gate;
  TransportCursor cursor;
  JackOutput output({&render, &RenderProbe::render}, cursor, {&gate, &Gate::allowed});
  REQUIRE_OK(output.start(JackStartConfig{48000, 128}), "JACK output failed to start");
  const auto started = output.status();
  REQUIRE_EQ(started.state, OutputState::streaming,
             "live JACK format differs from the requested 48 kHz / 128 frames");
  REQUIRE_EQ(started.server_rate, 48000,
             "live JACK format differs from the requested 48 kHz / 128 frames");
  REQUIRE_EQ(started.server_buffer_frames, 128,
             "live JACK format differs from the requested 48 kHz / 128 frames");
  CaptureProbe capture;
  capture.open();
  require(until([&] { return capture.signal_blocks.load() >= 4; }, 3s),
          "live JACK capture did not receive stereo output");
  gate.enabled.store(false, std::memory_order_release);
  require(until([&] { return capture.silent_blocks.load() >= 8; }, 3s),
          "closed callback gate did not produce silent JACK output");
  const auto parked = cursor.completed_snapshot();
  const auto calls = render.callbacks.load();
  const auto silent = capture.silent_blocks.load();
  REQUIRE_TRUE(until([&] { return capture.silent_blocks.load() >= silent + 8; }, 3s),
               "gated callbacks advanced transport or called the renderer");
  REQUIRE_EQ(cursor.completed_snapshot().next_sample, parked.next_sample,
             "gated callbacks advanced transport or called the renderer");
  REQUIRE_EQ(render.callbacks.load(), calls,
             "gated callbacks advanced transport or called the renderer");
  gate.enabled.store(true, std::memory_order_release);
  require(until([&] { return cursor.completed_snapshot().next_sample > parked.next_sample; }, 3s),
          "opening callback gate did not resume transport");
  std::cout << "CONNECTED rate=" << started.server_rate.value_or(0)
            << " frames=" << started.server_buffer_frames.value_or(0)
            << " signal_blocks=" << capture.signal_blocks.load()
            << " silent_blocks=" << capture.silent_blocks.load()
            << " completed_sample=" << cursor.completed_snapshot().next_sample
            << " client_xrun_notifications=" << output.status().xrun_notifications << '\n';
  if (!recovery) {
    output.stop();
    return;
  }
  std::cout << "STOP_NAMED_SERVER_NOW\n" << std::flush;
  require(until([&] { return output.status().state == OutputState::disconnected; }, 20s),
          "JACK server loss did not report disconnected");
  gate.enabled.store(false, std::memory_order_release);
  output.stop();
  capture.close();
  const auto stopped = cursor.completed_snapshot();
  const auto restart = cursor.restart_from_completed_after_quiesce();
  REQUIRE_TRUE(restart, "quiesced JACK loss did not preserve completed transport boundary");
  REQUIRE_EQ(restart->next_sample, stopped.next_sample,
             "quiesced JACK loss did not preserve completed transport boundary");
  REQUIRE_EQ(restart->generation, stopped.generation + 1,
             "quiesced JACK loss did not preserve completed transport boundary");
  const auto recovered = restart.value_or(TransportCursor::CompletedPosition{0, 0});
  std::cout << "DISCONNECTED completed_sample=" << recovered.next_sample
            << " generation=" << recovered.generation << " RESTART_NAMED_SERVER_NOW\n"
            << std::flush;
  const auto reopen_deadline = std::chrono::steady_clock::now() + 20s;
  bool reopened = false;
  while (std::chrono::steady_clock::now() < reopen_deadline && !reopened) {
    reopened = output.start({48000, 128}).has_value();
    if (!reopened) {
      std::this_thread::sleep_for(250ms);
    }
  }
  require(reopened, "explicit JACK reopen did not succeed after named server restart");
  gate.enabled.store(true, std::memory_order_release);
  require(
      until([&] { return cursor.completed_snapshot().next_sample > recovered.next_sample; }, 3s),
      "reopened JACK callback did not resume from completed position");
  const auto resumed = cursor.completed_snapshot();
  std::cout << "RECOVERED first_sample=" << recovered.next_sample
            << " generation=" << recovered.generation << " completed_sample=" << resumed.next_sample
            << " render_callbacks=" << render.callbacks.load() << '\n';
  output.stop();
}

void mismatch() {
  RenderProbe render;
  TransportCursor cursor;
  JackOutput output({&render, &RenderProbe::render}, cursor);
  const auto started = output.start({44100, 128});
  const auto status = output.status();
  REQUIRE_TRUE(!started,
               "live JACK server-rate mismatch did not reject playback before activation");
  REQUIRE_EQ(status.state, OutputState::error,
             "live JACK server-rate mismatch did not reject playback before activation");
  REQUIRE_TRUE(status.rate_mismatch,
               "live JACK server-rate mismatch did not reject playback before activation");
  REQUIRE_EQ(status.server_rate, 48000,
             "live JACK server-rate mismatch did not reject playback before activation");
  REQUIRE_EQ(render.callbacks.load(), 0,
             "live JACK server-rate mismatch did not reject playback before activation");
  REQUIRE_EQ(cursor.completed_snapshot().next_sample, 0,
             "live JACK server-rate mismatch did not reject playback before activation");
  std::cout << "RATE_MISMATCH project_rate=oscillate::test::sample_rate_44k server_rate="
            << status.server_rate.value_or(0) << " render_callbacks=" << render.callbacks.load()
            << '\n';
  output.stop();
}
} // namespace

int main(int argc, char** argv) {
  try {
    const char* server = std::getenv("JACK_DEFAULT_SERVER");
    REQUIRE_NE(server, nullptr,
               "set JACK_DEFAULT_SERVER to an isolated oscillate_smoke* dummy server");
    REQUIRE_TRUE(std::string_view(server).starts_with("oscillate_smoke"),
                 "set JACK_DEFAULT_SERVER to an isolated oscillate_smoke* dummy server");
    REQUIRE_EQ(argc, 2, "usage: oscillate_jack_live_smoke connect|recover|mismatch");
    REQUIRE_TRUE((std::string_view(argv[1]) == "connect" ||
                  std::string_view(argv[1]) == "recover" ||
                  std::string_view(argv[1]) == "mismatch"),
                 "usage: oscillate_jack_live_smoke connect|recover|mismatch");
    if (std::string_view(argv[1]) == "mismatch") {
      mismatch();
    } else {
      run(std::string_view(argv[1]) == "recover");
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
