// Render-ahead (ADR-0128): the device plays exactly what rendering just in
// time plays, a seek drops the window, an edit is heard within the window
// and one block without a gap, a load spike is absorbed, live input renders
// just in time, Stop fades, and the callback never allocates.
#include "oscillate/audio_io/render_ahead.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace oscillate;
using namespace oscillate::audio_io;
using oscillate::test::require;

constexpr std::uint32_t sample_rate = 48'000;
constexpr std::size_t block = 256;
constexpr std::size_t max_block = 1024;
constexpr std::size_t stereo = 2;
constexpr std::uint32_t generous_limit_ms = 100;
// About one device period, so the producer has time to work between blocks.
constexpr auto period = std::chrono::microseconds{block * 1'000'000 / sample_rate};
constexpr std::uint64_t loop_start = 1'000;
constexpr std::uint64_t loop_end = 3'000;
constexpr std::uint64_t seek_target = 50'000;
constexpr std::size_t played_blocks = 64;
constexpr float edited_gain = 0.5F;
// A load spike: this many periods long, every spike_every-th render.
constexpr int spike_periods = 3;
constexpr std::size_t spike_every = 16;

// Writes each frame's own project position, scaled by a gain that stands
// for any edit (read when the block renders), and can stall like a heavy
// block.
struct Source {
  std::atomic<float> gain{1.0F};
  std::atomic<bool> spikes{false};
  std::size_t renders = 0;

  static bool render(void* context, std::span<engine::StereoFrame> output,
                     engine::BlockContext block_context) noexcept {
    auto& self = *static_cast<Source*>(context);
    const auto gain = self.gain.load(std::memory_order_acquire);
    for (std::size_t frame = 0; frame < output.size(); ++frame) {
      const auto position = static_cast<float>(block_context.project_sample_position + frame);
      output[frame] = {position, position * gain};
    }
    if (self.spikes.load(std::memory_order_acquire) && ++self.renders % spike_every == 0) {
      const auto until = std::chrono::steady_clock::now() + period * spike_periods;
      while (std::chrono::steady_clock::now() < until) {
      }
    }
    return true;
  }
  [[nodiscard]] StereoRenderSource source() noexcept { return {this, &Source::render}; }
};

struct Device {
  std::vector<engine::StereoFrame> scratch = std::vector<engine::StereoFrame>(max_block);
  std::vector<float> output = std::vector<float>(block * stereo);
  std::vector<float> left;  // every frame played, in order
  std::vector<float> right;
  std::chrono::steady_clock::duration longest{};
  std::size_t late = 0;

  void play(RenderAhead& ahead, std::size_t blocks, bool paced = true) {
    auto deadline = std::chrono::steady_clock::now();
    for (std::size_t index = 0; index < blocks; ++index) {
      const auto started = std::chrono::steady_clock::now();
      require(ahead.render(true, scratch, output, {}) == BlockResult::rendered,
              "a block failed to render");
      const auto spent = std::chrono::steady_clock::now() - started;
      longest = std::max(longest, spent);
      late += spent > period ? 1U : 0U;
      for (std::size_t frame = 0; frame < block; ++frame) {
        left.push_back(output[frame * stereo]);
        right.push_back(output[frame * stereo + 1]);
      }
      if (paced) {
        deadline += period;
        std::this_thread::sleep_until(deadline);
      }
    }
  }
};

// The positions played run on from `from` without a gap or repeat.
void require_continuous(std::span<const float> played, std::size_t first, std::size_t count,
                        std::uint64_t from, const std::string& what) {
  for (std::size_t frame = 0; frame < count; ++frame)
    require(played[first + frame] == static_cast<float>(from + frame),
            what + ": frame " + std::to_string(first + frame) + " is out of place: " +
                std::to_string(played[first + frame]) + " for " + std::to_string(from + frame));
}

void plays_exactly_what_just_in_time_plays() {
  Source source;
  TransportCursor cursor;
  require(cursor.request_loop_wrap_at(loop_end, loop_start), "the loop wrap was not queued");
  RenderAhead ahead(source.source(), cursor, max_block);
  ahead.set_limit(generous_limit_ms, sample_rate);
  ahead.start_owned_thread();
  Device device;
  device.play(ahead, played_blocks);
  ahead.stop_owned_thread();
  // 0 .. loop end, then the loop from its start, wrapped once.
  require_continuous(device.left, 0, loop_end, 0, "before the wrap");
  require_continuous(device.left, loop_end, device.left.size() - loop_end, loop_start,
                     "after the wrap");
  REQUIRE_GE(ahead.status().ahead_blocks, played_blocks / 2,
             "the device did not play from the window");
}

void a_seek_drops_the_window() {
  Source source;
  TransportCursor cursor;
  RenderAhead ahead(source.source(), cursor, max_block);
  ahead.set_limit(generous_limit_ms, sample_rate);
  ahead.start_owned_thread();
  Device device;
  device.play(ahead, played_blocks / 2);
  const auto requested_at = device.left.size();
  require(cursor.request_seek(seek_target), "the seek was not queued");
  device.play(ahead, played_blocks / 2);
  ahead.stop_owned_thread();
  const auto jump = std::ranges::find(device.left, static_cast<float>(seek_target));
  require(jump != device.left.end(), "the seek target never played");
  const auto at = static_cast<std::size_t>(jump - device.left.begin());
  require_continuous(device.left, 0, at, 0, "before the seek");
  require_continuous(device.left, at, device.left.size() - at, seek_target, "after the seek");
  REQUIRE_LE(at - requested_at, ahead.worst_edit_delay_frames(),
             "the seek waited longer than the window and a block");
}

void an_edit_is_heard_within_the_window_without_a_gap() {
  Source source;
  TransportCursor cursor;
  RenderAhead ahead(source.source(), cursor, max_block);
  ahead.set_limit(generous_limit_ms, sample_rate);
  ahead.start_owned_thread();
  Device device;
  device.play(ahead, played_blocks / 2);
  const auto edited_at = device.left.size();
  source.gain.store(edited_gain, std::memory_order_release);
  device.play(ahead, played_blocks / 2);
  ahead.stop_owned_thread();
  require_continuous(device.left, 0, device.left.size(), 0, "around the edit");
  std::size_t heard = device.right.size();
  for (std::size_t frame = edited_at; frame < device.right.size(); ++frame)
    if (device.right[frame] == device.left[frame] * edited_gain && device.left[frame] != 0.0F) {
      heard = frame;
      break;
    }
  require(heard < device.right.size(), "the edit was never heard");
  REQUIRE_LE(heard - edited_at, ahead.worst_edit_delay_frames(),
             "the edit took longer than the window and a block");
  for (std::size_t frame = heard; frame < device.right.size(); ++frame)
    require(device.right[frame] == device.left[frame] * edited_gain,
            "the edit did not hold once heard");
}

void a_load_spike_is_absorbed() {
  // Just in time, every spike makes its block late.
  Source late_source;
  late_source.spikes = true;
  TransportCursor late_cursor;
  RenderAhead just_in_time(late_source.source(), late_cursor, max_block);
  Device late_device;
  late_device.play(just_in_time, played_blocks);
  REQUIRE_GE(late_device.late, played_blocks / spike_every - 1, "spikes did not make blocks late");
  // Rendered ahead, the window covers them.
  Source source;
  source.spikes = true;
  TransportCursor cursor;
  RenderAhead ahead(source.source(), cursor, max_block);
  ahead.set_limit(generous_limit_ms, sample_rate);
  ahead.start_owned_thread();
  Device device;
  device.play(ahead, played_blocks);
  ahead.stop_owned_thread();
  require_continuous(device.left, 0, device.left.size(), 0, "under spikes");
  REQUIRE_LE(device.late, 1, "the window did not absorb the spikes");
}

void live_input_renders_just_in_time() {
  Source source;
  TransportCursor cursor;
  RenderAhead ahead(source.source(), cursor, max_block);
  ahead.set_limit(generous_limit_ms, sample_rate);
  ahead.start_owned_thread();
  Device device;
  device.play(ahead, played_blocks / 2);
  ahead.set_live_input(true);
  // The window drains, then every block renders just in time and an edit is
  // heard in the next block.
  device.play(ahead, played_blocks / 2);
  REQUIRE_EQ(ahead.status().buffered_frames, 0, "the window did not drain for live input");
  const auto before = ahead.status().just_in_time_blocks;
  const auto edited_at = device.left.size();
  source.gain.store(edited_gain, std::memory_order_release);
  device.play(ahead, 1);
  ahead.stop_owned_thread();
  REQUIRE_EQ(ahead.status().just_in_time_blocks, before + 1, "live input rendered ahead");
  require(device.right[edited_at] == device.left[edited_at] * edited_gain,
          "an edit during live input was not heard at once");
  require_continuous(device.left, 0, device.left.size(), 0, "into live input");
}

void stop_fades_and_renders_nothing_more() {
  Source source;
  TransportCursor cursor;
  RenderAhead ahead(source.source(), cursor, max_block);
  ahead.set_limit(generous_limit_ms, sample_rate);
  ahead.start_owned_thread();
  Device device;
  device.play(ahead, played_blocks / 2);
  ahead.fade_out();
  const auto faded_from = device.left.size();
  device.play(ahead, 2);
  require(ahead.faded(), "the fade did not finish");
  const auto rendered = cursor.completed_snapshot().next_sample;
  device.play(ahead, 2);
  ahead.stop_owned_thread();
  REQUIRE_EQ(cursor.completed_snapshot().next_sample, rendered, "rendering went on after the fade");
  // Each frame no louder than the unfaded position, then silence.
  for (std::size_t frame = faded_from + RenderAhead::stop_fade_frames; frame < device.left.size();
       ++frame)
    REQUIRE_EQ(device.left[frame], 0.0F, "sound after the fade");
}

void a_quiesced_seek_clears_the_window() {
  Source source;
  TransportCursor cursor;
  RenderAhead ahead(source.source(), cursor, max_block);
  ahead.set_limit(generous_limit_ms, sample_rate);
  ahead.start_owned_thread();
  Device device;
  device.play(ahead, played_blocks / 2);
  ahead.stop_owned_thread();
  REQUIRE_GE(ahead.status().buffered_frames, block, "nothing was rendered ahead");
  require(cursor.seek_after_quiesce(seek_target).has_value(), "the paused seek failed");
  ahead.clear_after_quiesce();
  REQUIRE_EQ(ahead.status().buffered_frames, 0, "the window outlived the seek");
  REQUIRE_EQ(ahead.audible_position().next_sample, seek_target, "the playhead did not move");
  Device after;
  after.play(ahead, 1, false);
  require_continuous(after.left, 0, block, seek_target, "after the paused seek");
}

void the_callback_never_allocates() {
  Source source;
  TransportCursor cursor;
  RenderAhead ahead(source.source(), cursor, max_block);
  ahead.set_limit(generous_limit_ms, sample_rate);
  ahead.start_owned_thread();
  Device device;
  device.left.reserve(played_blocks * block);
  device.right.reserve(played_blocks * block);
  oscillate::test::counted_allocations = 0;
  oscillate::test::counting_allocations = true;
  device.play(ahead, played_blocks);
  oscillate::test::counting_allocations = false;
  ahead.stop_owned_thread();
  REQUIRE_EQ(oscillate::test::counted_allocations, 0, "the device callback allocated");
}

} // namespace

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "render ahead: ok",
      {{"plays_exactly_what_just_in_time_plays", plays_exactly_what_just_in_time_plays},
       {"a_seek_drops_the_window", a_seek_drops_the_window},
       {"an_edit_is_heard_within_the_window_without_a_gap",
        an_edit_is_heard_within_the_window_without_a_gap},
       {"a_load_spike_is_absorbed", a_load_spike_is_absorbed},
       {"live_input_renders_just_in_time", live_input_renders_just_in_time},
       {"stop_fades_and_renders_nothing_more", stop_fades_and_renders_nothing_more},
       {"a_quiesced_seek_clears_the_window", a_quiesced_seek_clears_the_window},
       {"the_callback_never_allocates", the_callback_never_allocates}});
}
