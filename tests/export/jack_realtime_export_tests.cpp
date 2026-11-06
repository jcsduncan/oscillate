#include "oscillate/export/jack_realtime_export.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"
#include "oscillate/test/wait.hpp"

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <new>
#include <span>
#include <thread>
#include <vector>

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

namespace {
// Summed across every fake JACK callback, whichever thread ran it.
std::atomic<std::size_t> callback_allocations{0};
} // namespace

namespace {
namespace fs = std::filesystem;
using oscillate::audio_io::JackApi;
using oscillate::audio_io::TransportCursor;
using oscillate::engine::BlockContext;
using oscillate::engine::StereoFrame;
using oscillate::exporting::JackRealtimeWavExport;
using oscillate::exporting::RealtimeNodeTail;
using oscillate::exporting::WavFormat;

using Directory = oscillate::test::TempDir;

std::vector<char> bytes(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

std::uint32_t word(const std::vector<char>& data, std::size_t at) {
  return std::uint32_t{static_cast<unsigned char>(data[at])} |
         std::uint32_t{static_cast<unsigned char>(data[at + 1])} << 8U |
         std::uint32_t{static_cast<unsigned char>(data[at + 2])} << 16U |
         std::uint32_t{static_cast<unsigned char>(data[at + 3])} << 24U;
}

struct FakeJack {
  JackApi api_table{};
  std::jthread driver;
  std::uint32_t period_frames = 64;
  std::array<float, 128> left{}, right{};
  JackApi::Process process = nullptr;
  JackApi::Change size_change = nullptr;
  JackApi::Event xrun = nullptr;
  JackApi::Shutdown shutdown = nullptr;
  void* callback_context = nullptr;
  unsigned deactivations = 0;
  unsigned opens = 0;
  unsigned closes = 0;
  std::atomic<bool> active{false};
  bool xrun_at_deactivation = false;

  static FakeJack& self(void* context) noexcept { return *static_cast<FakeJack*>(context); }
  static void* open(void* context, const char*) noexcept {
    ++self(context).opens;
    return context;
  }
  static int close(void* context, void*) noexcept {
    ++self(context).closes;
    return 0;
  }
  static int activate(void* context, void*) noexcept {
    self(context).active.store(true, std::memory_order_release);
    return 0;
  }
  static int deactivate(void* context, void*) noexcept {
    auto& fake = self(context);
    if (fake.xrun_at_deactivation)
      (void)fake.xrun(fake.callback_context);
    fake.active.store(false, std::memory_order_release);
    if (fake.driver.joinable()) {
      fake.driver.request_stop();
      fake.driver.join();
    }
    ++fake.deactivations;
    return 0;
  }
  static std::uint32_t rate(void*, void*) noexcept { return 48'000; }
  static std::uint32_t buffer(void* context, void*) noexcept { return self(context).period_frames; }
  static void* register_port(void* context, void*, const char* name) noexcept {
    auto& fake = self(context);
    return name[0] == 'l' ? static_cast<void*>(fake.left.data())
                          : static_cast<void*>(fake.right.data());
  }
  static float* port_buffer(void*, void* port, std::uint32_t) noexcept {
    return static_cast<float*>(port);
  }
  static int set_process(void* context, void*, JackApi::Process fn, void* data) noexcept {
    self(context).process = fn;
    self(context).callback_context = data;
    return 0;
  }
  static int set_change(void*, void*, JackApi::Change, void*) noexcept { return 0; }
  static int set_size(void* context, void*, JackApi::Change fn, void*) noexcept {
    self(context).size_change = fn;
    return 0;
  }
  static int set_xrun(void* context, void*, JackApi::Event fn, void*) noexcept {
    self(context).xrun = fn;
    return 0;
  }
  static void set_shutdown(void* context, void*, JackApi::Shutdown fn, void*) noexcept {
    self(context).shutdown = fn;
  }
  const JackApi* api() noexcept {
    api_table = {this,       open,     close,         activate,    deactivate,
                 rate,       buffer,   register_port, port_buffer, set_process,
                 set_change, set_size, set_xrun,      set_shutdown};
    return &api_table;
  }
  void run(std::uint32_t frames) noexcept {
    if (!active.load(std::memory_order_acquire))
      return;
    counted_allocations = 0;
    counting_allocations = true;
    (void)process(frames, callback_context);
    counting_allocations = false;
    callback_allocations.fetch_add(counted_allocations, std::memory_order_relaxed);
  }
  void emit_xrun() noexcept { (void)xrun(callback_context); }
  void change_size(std::uint32_t frames) noexcept {
    period_frames = frames;
    (void)size_change(frames, callback_context);
  }
};

struct Master {
  bool fail = false;
  bool slow = false;
  static bool render(void* context, std::span<StereoFrame> output, BlockContext block) noexcept {
    auto& master = *static_cast<Master*>(context);
    if (master.slow) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5);
      while (std::chrono::steady_clock::now() < deadline) {
      }
    }
    for (std::size_t index = 0; index < output.size(); ++index) {
      const auto value = static_cast<float>(block.project_sample_position + index) / 16.0F;
      output[index] = {value, -value};
    }
    return !master.fail;
  }
};

struct Health {
  std::optional<std::uint64_t> underruns{0};
  static JackRealtimeWavExport::HealthCounters snapshot(void* context) noexcept {
    const auto& health = *static_cast<Health*>(context);
    return {health.underruns};
  }
};

auto begin(const fs::path& target, FakeJack& jack, Master& master, TransportCursor& transport,
           Health& health, std::uint64_t first, std::uint64_t frames, std::size_t ring = 128,
           std::uint32_t latency = 0) {
  return JackRealtimeWavExport::begin(target, {48'000, WavFormat::float32, frames}, first, latency,
                                      ring, {&master, &Master::render}, transport,
                                      {48'000, jack.period_frames}, {&health, &Health::snapshot},
                                      jack.api());
}

void exact_master_interval_and_quiescent_stop() {
  const Directory directory("oscillate-jack-export");
  FakeJack jack;
  Master master;
  Health health;
  TransportCursor transport(16);
  auto opened = begin(directory.path() / "master.wav", jack, master, transport, health, 16, 65);
  REQUIRE_TRUE(opened);
  auto session = std::move(opened.value());
  jack.run(64);
  REQUIRE_EQ(session->frames_captured(), 64);
  jack.run(64);
  REQUIRE_TRUE(session->interval_captured());
  REQUIRE_TRUE(jack.left[0] == 80.0F / 16.0F && jack.right[0] == -80.0F / 16.0F);
  REQUIRE_TRUE(jack.left[1] == 0.0F && jack.right[3] == 0.0F);
  jack.run(64); // completed interval gates further output and transport
  REQUIRE_TRUE(jack.left[0] == 0.0F && transport.completed_snapshot().next_sample == 144);
  REQUIRE_EQ(callback_allocations.load(), 0);
  REQUIRE_TRUE(session->finish());
  REQUIRE_TRUE(jack.deactivations == 1 && jack.closes == 1 && !jack.active);
  const auto wave = bytes(directory.path() / "master.wav");
  REQUIRE_EQ(wave.size(), 56 + 65 * 8);
  REQUIRE_EQ(word(wave, 56), std::bit_cast<std::uint32_t>(1.0F));
  REQUIRE_EQ(word(wave, 60), std::bit_cast<std::uint32_t>(-1.0F));
  REQUIRE_EQ(word(wave, 56 + 64 * 8), std::bit_cast<std::uint32_t>(5.0F));
  REQUIRE_TRUE(!session->finish());
}

// The fake master emits stream frame N as N / 16. With 70 frames of output
// latency, the file starts at stream frame 86 and the device keeps sounding
// until the last captured frame, 86 + 65 - 1 = 150.
void output_latency_offsets_the_captured_window() {
  const Directory directory("oscillate-jack-export");
  FakeJack jack;
  Master master;
  Health health;
  TransportCursor transport(16);
  auto opened =
      begin(directory.path() / "latent.wav", jack, master, transport, health, 16, 65, 128, 70);
  REQUIRE_TRUE(opened);
  auto session = std::move(opened.value());
  jack.run(64); // stream 16..79: latency only
  REQUIRE_EQ(session->frames_captured(), 0);
  REQUIRE_EQ(jack.left[63], 79.0F / 16.0F);
  jack.run(64); // stream 80..143: capture starts mid-period at 86
  REQUIRE_EQ(session->frames_captured(), 58);
  jack.run(64); // stream 144..207: last captured frame is 150
  REQUIRE_TRUE(session->interval_captured());
  REQUIRE_TRUE(jack.left[6] == 150.0F / 16.0F && jack.left[7] == 0.0F && jack.right[63] == 0.0F);
  REQUIRE_EQ(callback_allocations.load(), 0);
  REQUIRE_TRUE(session->finish());
  const auto wave = bytes(directory.path() / "latent.wav");
  REQUIRE_EQ(wave.size(), 56 + 65 * 8);
  REQUIRE_EQ(word(wave, 56), std::bit_cast<std::uint32_t>(86.0F / 16.0F));
  REQUIRE_EQ(word(wave, 56 + 64 * 8), std::bit_cast<std::uint32_t>(150.0F / 16.0F));
}

void failures_preserve_old_render() {
  const Directory directory("oscillate-jack-export");
  const auto target = directory.path() / "master.wav";
  {
    std::ofstream old(target, std::ios::binary);
    old << "old master";
  }
  const auto golden = bytes(target);
  for (int scenario = 0; scenario < 8; ++scenario) {
    FakeJack jack;
    Master master;
    Health health;
    TransportCursor transport(0);
    auto opened = begin(target, jack, master, transport, health, 0, 64);
    REQUIRE_TRUE(opened);
    auto session = std::move(opened.value());
    if (scenario == 3)
      master.fail = true;
    if (scenario == 6)
      master.slow = true;
    jack.run(scenario == 4 || scenario == 7 ? 32 : 64);
    if (scenario == 0)
      jack.emit_xrun();
    if (scenario == 1)
      jack.xrun_at_deactivation = true;
    if (scenario == 2)
      health.underruns = 1;
    if (scenario == 5)
      health.underruns = std::nullopt;
    if (scenario == 6) {
      const auto timing = session->device_status().callback_timing_distribution;
      REQUIRE_TRUE(timing && timing->bins[5] + timing->bins[6] + timing->bins[7] != 0);
    }
    if (scenario == 7) {
      REQUIRE_TRUE(transport.request_seek(128));
      jack.run(32);
    }
    REQUIRE_TRUE(!session->finish());
    REQUIRE_EQ(bytes(target), golden);
    REQUIRE_TRUE(jack.deactivations == 1 && jack.closes == 1);
  }
}

void changed_callback_format_or_epoch_never_proves_zero_overruns() {
  const Directory directory("oscillate-jack-export");
  const auto target = directory.path() / "format.wav";
  {
    std::ofstream old(target, std::ios::binary);
    old << "existing";
  }
  const auto golden = bytes(target);
  {
    FakeJack jack;
    Master master;
    Health health;
    TransportCursor transport(0);
    auto opened = begin(target, jack, master, transport, health, 0, 65);
    REQUIRE_TRUE(opened);
    auto session = std::move(opened.value());
    jack.run(65); // one epoch, but JACK claimed a 64-frame period
    REQUIRE_TRUE(session->interval_captured());
    REQUIRE_TRUE(!session->finish());
    REQUIRE_EQ(bytes(target), golden);
  }
  {
    FakeJack jack;
    jack.period_frames = 65;
    Master master;
    Health health;
    TransportCursor transport(0);
    auto opened = begin(target, jack, master, transport, health, 0, 129, 256);
    REQUIRE_TRUE(opened);
    auto session = std::move(opened.value());
    jack.run(65);
    jack.change_size(64);
    jack.run(64); // final format matches server, but the epoch changed
    REQUIRE_TRUE(session->interval_captured());
    REQUIRE_TRUE(!session->finish());
    REQUIRE_EQ(bytes(target), golden);
  }
}

void refuses_ambiguous_baseline_and_start_position() {
  const Directory directory("oscillate-jack-export");
  FakeJack jack;
  Master master;
  Health health;
  TransportCursor transport(1);
  REQUIRE_TRUE(!begin(directory.path() / "wrong.wav", jack, master, transport, health, 0, 64));
  health.underruns = std::nullopt;
  REQUIRE_TRUE(!begin(directory.path() / "unknown.wav", jack, master, transport, health, 1, 64));
  health.underruns = 0;
  REQUIRE_TRUE(!begin(directory.path() / "range.wav", jack, master, transport, health, 1,
                      std::numeric_limits<std::uint64_t>::max() - 64, 128, 64));
  REQUIRE_EQ(jack.closes, 0);
}

void prepared_vst3_tail_preflight_guards_device_and_capture() {
  const Directory directory("oscillate-jack-export");
  FakeJack jack;
  Master master;
  Health health;
  TransportCursor transport(0);
  oscillate::engine::RoutingLatencyPlan route;
  route.topological_order = {1, 2};
  route.nodes = {{1, 0}, {2, 0}};
  route.edges = {{3, 1, 2, 0, 1.0F}};
  const auto target = directory.path() / "vst3-tail.wav";
  const auto attempt = [&](std::span<const RealtimeNodeTail> tails, std::uint64_t frames) {
    return JackRealtimeWavExport::begin_with_tail_plan(
        target, {48'000, WavFormat::float32, frames}, {route, 2, tails, 64, 480'000}, 0, 0, 128,
        {&master, &Master::render}, transport, {48'000, jack.period_frames},
        {&health, &Health::snapshot}, jack.api());
  };
  const std::array<RealtimeNodeTail, 2> unknown{{{1, std::nullopt}, {2, 0}}};
  REQUIRE_TRUE(!attempt(unknown, 72));
  REQUIRE_TRUE(jack.opens == 0 && !fs::exists(target));
  const std::array<RealtimeNodeTail, 2> finite{{{1, 8}, {2, 0}}};
  REQUIRE_TRUE(!attempt(finite, 64)); // The VST3 tail may not be omitted.
  REQUIRE_TRUE(jack.opens == 0 && !fs::exists(target));
  auto opened = attempt(finite, 72);
  REQUIRE_TRUE(opened && jack.opens == 1);
  auto session = std::move(opened.value());
  jack.run(64);
  jack.run(64);
  REQUIRE_TRUE(session->interval_captured());
  REQUIRE_TRUE(session->finish());
  REQUIRE_EQ(bytes(target).size(), 56 + 72 * 8);
}

void stop_joins_a_running_fake_device() {
  const Directory directory("oscillate-jack-export");
  FakeJack jack;
  Master master;
  Health health;
  TransportCursor transport(0);
  auto opened =
      begin(directory.path() / "joined.wav", jack, master, transport, health, 0, 10'000, 20'000);
  REQUIRE_TRUE(opened);
  auto session = std::move(opened.value());
  jack.driver = std::jthread([&jack](std::stop_token token) {
    while (!token.stop_requested() && jack.active.load(std::memory_order_acquire))
      jack.run(64);
  });
  oscillate::test::wait_until([&] { return session->interval_captured(); },
                              "the driver thread captures the interval");
  REQUIRE_TRUE(session->finish());
  REQUIRE_TRUE(!jack.driver.joinable() && jack.deactivations == 1 && jack.closes == 1);
  REQUIRE_EQ(bytes(directory.path() / "joined.wav").size(), 56 + 10'000 * 8);
}
} // namespace

int main() {
  exact_master_interval_and_quiescent_stop();
  output_latency_offsets_the_captured_window();
  failures_preserve_old_render();
  changed_callback_format_or_epoch_never_proves_zero_overruns();
  refuses_ambiguous_baseline_and_start_position();
  prepared_vst3_tail_preflight_guards_device_and_capture();
  stop_joins_a_running_fake_device();
}
