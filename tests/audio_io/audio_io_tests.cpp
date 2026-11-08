#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"

#include <oscillate/audio_io/graph_source.hpp>
#include <oscillate/audio_io/metronome.hpp>
#include <oscillate/audio_io/pipewire_output.hpp>
#include <oscillate/audio_io/pipewire_sinks.hpp>
#include <oscillate/audio_io/stereo_block.hpp>
#include <oscillate/engine/delay_compensation.hpp>
#include <oscillate/engine/graph.hpp>
#include <oscillate/foundation/diagnostic.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using namespace oscillate::audio_io;

using oscillate::test::require;

// A count one thread publishes and another sleeps on. The waiter blocks on a
// condition variable, never in a spin or yield loop that would hand a shared
// CPU back to the publisher one timeslice at a time.
class PublishedCount {
public:
  explicit PublishedCount(std::uint64_t initial = 0) : value_(initial) {}
  void publish(std::uint64_t value) {
    {
      const std::lock_guard lock(mutex_);
      value_ = value;
    }
    changed_.notify_all();
  }
  void wait_for(std::uint64_t at_least) {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [&] { return value_ >= at_least; });
  }
  [[nodiscard]] std::uint64_t value() {
    const std::lock_guard lock(mutex_);
    return value_;
  }

private:
  std::mutex mutex_;
  std::condition_variable changed_;
  std::uint64_t value_;
};

struct FakeRenderer {
  bool fail = false;
  std::size_t calls = 0;
  oscillate::engine::BlockContext received{};
  static bool render(void* context, std::span<oscillate::engine::StereoFrame> stereo,
                     oscillate::engine::BlockContext block) noexcept {
    auto& self = *static_cast<FakeRenderer*>(context);
    ++self.calls;
    self.received = block;
    for (std::size_t index = 0; index < stereo.size(); ++index) {
      stereo[index] = {static_cast<float>(index + 1), -static_cast<float>(index + 1)};
    }
    return !self.fail;
  }
};

void block_contract() {
  FakeRenderer fake;
  const StereoRenderSource source{&fake, &FakeRenderer::render};
  std::array<oscillate::engine::StereoFrame, 4> scratch{};
  std::array<float, 8> output{};
  const oscillate::engine::BlockContext block{32, 7};
  REQUIRE_EQ(render_stereo_block(source, block, scratch, output), BlockResult::rendered,
             "fake render must succeed");
  REQUIRE_EQ(output, (std::array<float, 8>{1, -1, 2, -2, 3, -3, 4, -4}),
             "distinct stereo channels must remain in device order");
  REQUIRE_EQ(fake.received.project_sample_position, 32,
             "graph receives transport context unchanged");
  REQUIRE_EQ(fake.received.transport_generation, 7, "graph receives transport context unchanged");
  fake.fail = true;
  output.fill(99);
  REQUIRE_EQ(render_stereo_block(source, block, scratch, output),
             BlockResult::silent_render_failure, "renderer failure must report silence");
  REQUIRE_EQ(output, (std::array<float, 8>{}), "partial renderer output must never leak");
  REQUIRE_EQ(fake.calls, 2, "render called once per valid block");

  std::array<float, 7> malformed{};
  REQUIRE_EQ(render_stereo_block(source, block, scratch, malformed), BlockResult::invalid_buffer,
             "odd sample count is invalid");
  std::array<float, 10> oversized{};
  REQUIRE_EQ(render_stereo_block(source, block, scratch, oversized), BlockResult::invalid_buffer,
             "scratch capacity is enforced");
  REQUIRE_EQ(fake.calls, 2, "invalid blocks must not reach renderer");
  REQUIRE_EQ(render_stereo_block({}, block, scratch, output), BlockResult::silent_render_failure,
             "no graph source produces bounded silence");
  REQUIRE_EQ(render_stereo_block(source, {32, 0}, scratch, output),
             BlockResult::silent_render_failure, "zero transport generation cannot reach renderer");
  REQUIRE_EQ(fake.calls, 2, "invalid transport context must not invoke renderer");

  TransportCursor transport(40);
  output.fill(99);
  REQUIRE_EQ(render_transport_block(source, transport, false, scratch, output),
             BlockResult::silent_render_failure, "unavailable device format produces silence");
  REQUIRE_EQ(output, (std::array<float, 8>{}), "unavailable format clears the whole block");
  REQUIRE_EQ(render_transport_block(source, transport, true, scratch, malformed),
             BlockResult::invalid_buffer,
             "invalid device buffer is rejected before cursor advances");
  fake.fail = false;
  REQUIRE_EQ(render_transport_block(source, transport, true, scratch, output),
             BlockResult::rendered, "valid format resumes rendering");
  REQUIRE_EQ(fake.received.project_sample_position, 40,
             "device format loss and invalid buffers preserve transport position");
  REQUIRE_EQ(fake.received.transport_generation, 1,
             "device format loss and invalid buffers preserve transport position");
}

void transport_contract() {
  TransportCursor transport(100);
  auto first = transport.next_block(4);
  auto second = transport.next_block(4);
  REQUIRE_TRUE(first, "initial context starts at requested sample with generation one");
  REQUIRE_EQ(first->project_sample_position, 100,
             "initial context starts at requested sample with generation one");
  REQUIRE_EQ(first->transport_generation, 1,
             "initial context starts at requested sample with generation one");
  REQUIRE_TRUE(second, "continuous blocks advance project position without changing generation");
  REQUIRE_EQ(second->project_sample_position, 104,
             "continuous blocks advance project position without changing generation");
  REQUIRE_EQ(second->transport_generation, 1,
             "continuous blocks advance project position without changing generation");
  require(transport.request_seek(25), "control thread may queue one seek");
  require(!transport.request_seek(50), "pending seek cannot be silently overwritten");
  auto seek = transport.next_block(2);
  REQUIRE_TRUE(seek, "seek changes position and generation together at a block boundary");
  REQUIRE_EQ(seek->project_sample_position, 25,
             "seek changes position and generation together at a block boundary");
  REQUIRE_EQ(seek->transport_generation, 2,
             "seek changes position and generation together at a block boundary");
  require(transport.request_seek(0), "consumed seek opens command slot");
  auto restart = transport.next_block(1);
  REQUIRE_TRUE(restart, "second discontinuity advances generation");
  REQUIRE_EQ(restart->project_sample_position, 0, "second discontinuity advances generation");
  REQUIRE_EQ(restart->transport_generation, 3, "second discontinuity advances generation");
  require(transport.request_loop_wrap(10), "primed loop wrap may queue at a block boundary");
  auto wrapped = transport.next_block(2);
  REQUIRE_TRUE(wrapped, "planned wrap changes source generation but preserves processor history");
  REQUIRE_EQ(wrapped->project_sample_position, 10,
             "planned wrap changes source generation but preserves processor history");
  REQUIRE_EQ(wrapped->transport_generation, 4,
             "planned wrap changes source generation but preserves processor history");
  REQUIRE_EQ(wrapped->state_generation(), 3,
             "planned wrap changes source generation but preserves processor history");
  REQUIRE_TRUE(wrapped->planned_loop_wrap,
               "planned wrap changes source generation but preserves processor history");
  auto after_wrap = transport.next_block(2);
  REQUIRE_TRUE(after_wrap, "only the first planned loop block carries the wrap boundary");
  REQUIRE_EQ(after_wrap->project_sample_position, 12,
             "only the first planned loop block carries the wrap boundary");
  REQUIRE_EQ(after_wrap->transport_generation, 4,
             "only the first planned loop block carries the wrap boundary");
  REQUIRE_EQ(after_wrap->state_generation(), 3,
             "only the first planned loop block carries the wrap boundary");
  REQUIRE_TRUE(!after_wrap->planned_loop_wrap,
               "only the first planned loop block carries the wrap boundary");
  require(transport.request_seek(20), "ordinary seek remains available after a planned wrap");
  auto after_seek = transport.next_block(2);
  REQUIRE_TRUE(after_seek, "ordinary seek starts a new source and processor generation");
  REQUIRE_EQ(after_seek->project_sample_position, 20,
             "ordinary seek starts a new source and processor generation");
  REQUIRE_EQ(after_seek->transport_generation, 5,
             "ordinary seek starts a new source and processor generation");
  REQUIRE_EQ(after_seek->state_generation(), 5,
             "ordinary seek starts a new source and processor generation");
  REQUIRE_TRUE(!after_seek->planned_loop_wrap,
               "ordinary seek starts a new source and processor generation");
  require(!transport.next_block(0), "zero frame block has no valid interval");
  TransportCursor near_limit(std::numeric_limits<std::uint64_t>::max() - 1);
  require(!near_limit.next_block(2), "overflowing interval is rejected");
  REQUIRE_OK(near_limit.next_block(1), "a valid interval remains possible after rejection");
  require(!near_limit.next_block(1), "cursor stops before interval overflow");

  TransportCursor quiesced(42);
  const auto resumed = quiesced.restart_from_completed_after_quiesce();
  REQUIRE_TRUE(resumed, "quiesced restart keeps the exact last completed sample");
  REQUIRE_EQ(resumed->next_sample, 42, "quiesced restart keeps the exact last completed sample");
  REQUIRE_EQ(resumed->generation, 2, "quiesced restart keeps the exact last completed sample");
  require(quiesced.request_seek(999), "a queued seek may exist before device quiescence");
  const auto positioned = quiesced.seek_after_quiesce(1234);
  REQUIRE_TRUE(positioned, "quiesced seek publishes the explicit sample with a fresh generation");
  REQUIRE_EQ(positioned->next_sample, 1234,
             "quiesced seek publishes the explicit sample with a fresh generation");
  REQUIRE_EQ(positioned->generation, 3,
             "quiesced seek publishes the explicit sample with a fresh generation");
  require(!quiesced.seek_after_quiesce(std::numeric_limits<std::uint64_t>::max()),
          "quiesced seek rejects an unrenderable sample without mutation");
  const auto after_rejection = quiesced.next_block(4);
  REQUIRE_TRUE(after_rejection,
               "failed seek preserves the prior completed position and clears no pending command");
  REQUIRE_EQ(after_rejection->project_sample_position, 1234,
             "failed seek preserves the prior completed position and clears no pending command");
  REQUIRE_EQ(after_rejection->transport_generation, 3,
             "failed seek preserves the prior completed position and clears no pending command");

  // The control thread queues each seek against a running callback, then
  // sleeps until the callback publishes the generation it started. Neither
  // side spins or yields to the other, so the handshake also completes when
  // both threads share one loaded CPU.
  constexpr std::uint64_t concurrent_seeks = 1000;
  constexpr std::uint64_t first_generation = 1;
  constexpr std::uint64_t last_generation = first_generation + concurrent_seeks;
  TransportCursor concurrent;
  PublishedCount observed_generation(first_generation);
  std::atomic<bool> stop{false};
  std::thread callback([&] {
    for (auto seen = first_generation;
         seen < last_generation && !stop.load(std::memory_order_acquire);) {
      const auto next = concurrent.next_block(1);
      if (next && next->transport_generation != seen) {
        seen = next->transport_generation;
        observed_generation.publish(seen);
      }
    }
  });
  bool every_seek_queued = true;
  for (std::uint64_t index = 1; index <= concurrent_seeks; ++index) {
    // A rejected seek starts no generation to wait for: stop the callback.
    if (!concurrent.request_seek(index)) {
      every_seek_queued = false;
      break;
    }
    observed_generation.wait_for(first_generation + index);
  }
  stop.store(true, std::memory_order_release);
  callback.join();
  require(every_seek_queued, "each seek is queued once the callback consumed the previous one");
  REQUIRE_EQ(observed_generation.value(), last_generation,
             "concurrent seeks are consumed once and advance generation exactly once");
}

void completed_position_concurrency_contract() {
  TransportCursor cursor;
  constexpr std::uint64_t last_generation = 20'001;
  std::thread publisher([&] {
    for (std::uint64_t generation = 2; generation <= last_generation; ++generation)
      (void)cursor.seek_after_quiesce(generation * 2 + 1);
  });
  bool torn = false;
  for (;;) {
    const auto completed = cursor.completed_snapshot();
    if (completed.generation > 1 && completed.next_sample != completed.generation * 2 + 1)
      torn = true;
    if (completed.generation == last_generation)
      break;
  }
  publisher.join();
  require(!torn, "completed sample and generation must come from the same callback publication");
}

void telemetry_contract() {
  DeviceTelemetry telemetry;
  REQUIRE_EQ(telemetry.snapshot().state, OutputState::stopped, "initially stopped");
  telemetry.start(48000);
  auto status = telemetry.snapshot();
  REQUIRE_EQ(status.state, OutputState::connecting,
             "requested rate is separate from observed rate");
  REQUIRE_EQ(status.requested_client_rate, 48000, "requested rate is separate from observed rate");
  REQUIRE_TRUE(!status.negotiated_client_rate, "unobserved values remain unknown");
  REQUIRE_TRUE(!status.observed_graph_rate, "unobserved values remain unknown");
  REQUIRE_TRUE(!status.xruns, "unobserved values remain unknown");
  REQUIRE_TRUE(!status.client_xrun_notifications, "unobserved values remain unknown");
  REQUIRE_TRUE(!status.graph_xrun_recovery_events, "unobserved values remain unknown");
  REQUIRE_TRUE(!status.measured_callback_blocks, "callback timing is unknown before a valid block");
  REQUIRE_TRUE(!status.callback_over_period_blocks,
               "callback timing is unknown before a valid block");
  REQUIRE_TRUE(!status.max_callback_duration_ns, "callback timing is unknown before a valid block");
  REQUIRE_TRUE(!status.callback_timing_distribution,
               "callback timing is unknown before a valid block");
  REQUIRE_TRUE(!status.selected_target_serial,
               "unselected target and unconnected stream have no device identity");
  REQUIRE_TRUE(!status.stream_node_id,
               "unselected target and unconnected stream have no device identity");
  telemetry.stream_node(0);
  REQUIRE_EQ(telemetry.snapshot().stream_node_id, 0,
             "PipeWire node ID zero is a valid observed identity");
  telemetry.client_rate(48000);
  telemetry.graph_rate(44100);
  telemetry.block_frames(128);
  telemetry.state(OutputState::streaming);
  telemetry.render_failure();
  telemetry.oversized_block();
  telemetry.invalid_buffer();
  telemetry.callback_duration({9, 0, 48'000});
  telemetry.callback_duration({9, 64, 0});
  telemetry.callback_duration({1'000'000, 64, 48'000});
  telemetry.callback_duration({1'333'333, 64, 48'000});
  telemetry.callback_duration({1'333'334, 64, 48'000});
  telemetry.callback_duration({2'000'000, 128, 48'000});
  status = telemetry.snapshot();
  REQUIRE_EQ(status.negotiated_client_rate, 48000,
             "independent stream and graph rates are retained");
  REQUIRE_EQ(status.observed_graph_rate, 44100, "independent stream and graph rates are retained");
  REQUIRE_EQ(status.observed_block_frames, 128, "independent stream and graph rates are retained");
  REQUIRE_EQ(status.render_failures, 1, "RT event counters are visible off-thread");
  REQUIRE_EQ(status.oversized_blocks, 1, "RT event counters are visible off-thread");
  REQUIRE_EQ(status.invalid_buffers, 1, "RT event counters are visible off-thread");
  REQUIRE_EQ(status.measured_callback_blocks, 4,
             "only valid blocks count, with a strict per-block processing-period threshold");
  REQUIRE_EQ(status.callback_over_period_blocks, 1,
             "only valid blocks count, with a strict per-block processing-period threshold");
  REQUIRE_EQ(status.max_callback_duration_ns, 2'000'000,
             "only valid blocks count, with a strict per-block processing-period threshold");
  REQUIRE_TRUE(status.callback_timing_distribution,
               "histogram reflects the current callback format rather than mixing block sizes");
  REQUIRE_EQ(status.callback_timing_distribution->rate, 48'000,
             "histogram reflects the current callback format rather than mixing block sizes");
  REQUIRE_EQ(status.callback_timing_distribution->frames, 128,
             "histogram reflects the current callback format rather than mixing block sizes");
  REQUIRE_EQ(status.callback_timing_distribution->bins[2], 1,
             "histogram reflects the current callback format rather than mixing block sizes");
  require(!status.xruns, "callback timing must not be relabelled as a backend xrun");
  telemetry.graph_xrun_recovery(false);
  telemetry.graph_xrun_recovery(true);
  telemetry.graph_xrun_recovery(false);
  status = telemetry.snapshot();
  REQUIRE_EQ(status.graph_xrun_recovery_events, 1,
             "node recovery notices are counted separately from backend xruns");
  REQUIRE_TRUE(!status.xruns, "node recovery notices are counted separately from backend xruns");
  telemetry.graph_xrun_recovery(std::nullopt);
  require(!telemetry.snapshot().graph_xrun_recovery_events,
          "a missed graph-position observation invalidates a zero-xrun inference");
  telemetry.state(OutputState::disconnected);
  telemetry.invalidate_connection();
  REQUIRE_EQ(telemetry.snapshot().state, OutputState::disconnected,
             "disconnect remains observable without touching project state");
  require(!telemetry.snapshot().negotiated_client_rate,
          "disconnect invalidates the previous negotiated format");
  REQUIRE_TRUE(!telemetry.snapshot().observed_graph_rate,
               "disconnect invalidates prior graph observations");
  REQUIRE_TRUE(!telemetry.snapshot().observed_block_frames,
               "disconnect invalidates prior graph observations");
  require(!telemetry.snapshot().stream_node_id,
          "disconnect invalidates the old stream node identity");
  telemetry.start(96000, 872);
  status = telemetry.snapshot();
  REQUIRE_EQ(status.selected_target_serial, 872,
             "new selection keeps its target serial separate from a connected stream node");
  REQUIRE_TRUE(!status.stream_node_id,
               "new selection keeps its target serial separate from a connected stream node");
  REQUIRE_TRUE(!status.negotiated_client_rate, "reselection clears stale observations");
  REQUIRE_TRUE(!status.observed_graph_rate, "reselection clears stale observations");
  REQUIRE_EQ(status.render_failures, 0, "reselection clears stale observations");
  REQUIRE_EQ(status.oversized_blocks, 0, "reselection clears stale observations");
  require(!status.graph_xrun_recovery_events, "reselection clears node recovery observations");
  telemetry.graph_xrun_recovery(false);
  REQUIRE_EQ(telemetry.snapshot().graph_xrun_recovery_events, 0,
             "an observed clean graph cycle reports zero recovery notices");
  telemetry.client_rate(48'000);
  telemetry.graph_rate(96'000);
  status = telemetry.snapshot();
  REQUIRE_TRUE(
      status.client_rate_mismatch,
      "client-rate mismatch must be distinguishable from server conversion or render failure");
  REQUIRE_EQ(
      status.negotiated_client_rate, 48'000,
      "client-rate mismatch must be distinguishable from server conversion or render failure");
  REQUIRE_EQ(
      status.observed_graph_rate, 96'000,
      "client-rate mismatch must be distinguishable from server conversion or render failure");
  REQUIRE_EQ(
      status.render_failures, 0,
      "client-rate mismatch must be distinguishable from server conversion or render failure");
  telemetry.invalidate_format();
  require(!telemetry.snapshot().client_rate_mismatch,
          "format invalidation retained a stale client-rate mismatch");
  status = telemetry.snapshot();
  REQUIRE_TRUE(!status.measured_callback_blocks,
               "reselection clears callback timing and retains unknown backend xruns");
  REQUIRE_TRUE(!status.callback_over_period_blocks,
               "reselection clears callback timing and retains unknown backend xruns");
  REQUIRE_TRUE(!status.max_callback_duration_ns,
               "reselection clears callback timing and retains unknown backend xruns");
  REQUIRE_TRUE(!status.callback_timing_distribution,
               "reselection clears callback timing and retains unknown backend xruns");
  REQUIRE_TRUE(!status.xruns,
               "reselection clears callback timing and retains unknown backend xruns");
  REQUIRE_EQ(status.graph_xrun_recovery_events, 0,
             "reselection clears callback timing and retains unknown backend xruns");
}

// DeviceTelemetry is the one source of callback load (UI meter, trace) and
// publishes failures and xruns as fixed records for the control thread's log.
void telemetry_load_and_diagnostics() {
  namespace foundation = oscillate::foundation;
  constexpr std::uint32_t rate = oscillate::test::sample_rate_48k;
  constexpr std::uint32_t frames = 48; // A 1 ms period.
  constexpr double period_seconds = static_cast<double>(frames) / rate;
  constexpr std::uint64_t half_period_ns = 500'000;
  constexpr std::uint64_t double_period_ns = 2'000'000;
  constexpr double half_load = 0.5;
  constexpr double double_load = 2.0;
  constexpr double tolerance = 1e-12;
  constexpr std::int64_t sample = 4'800;
  auto& ring = foundation::realtime_diagnostics();
  foundation::DiagnosticEvent record{};
  while (ring.try_take(record)) {
  }
  (void)ring.take_dropped();

  DeviceTelemetry telemetry;
  telemetry.start(rate);
  REQUIRE_FALSE(telemetry.snapshot().callback_load.has_value(), "no load before a block");
  telemetry.callback_duration({half_period_ns, frames, rate});
  auto load = telemetry.snapshot().callback_load;
  REQUIRE_OK(load);
  REQUIRE_NEAR(load->average, half_load, tolerance, "the first block primes the average");
  REQUIRE_NEAR(load->peak, half_load, tolerance);
  telemetry.callback_duration({double_period_ns, frames, rate});
  load = telemetry.snapshot().callback_load;
  const double expected_average =
      half_load + ((1.0 - std::exp(-period_seconds / CallbackLoad::average_seconds)) *
                   (double_load - half_load));
  REQUIRE_NEAR(load->average, expected_average, tolerance, "one exponential-average step");
  REQUIRE_NEAR(load->peak, double_load, tolerance, "the peak holds the slowest block");

  telemetry.render_failure(frames, sample);
  telemetry.graph_xrun_recovery(true);
  telemetry.graph_xrun_recovery(true);
  telemetry.graph_xrun_recovery(false);
  telemetry.graph_xrun_recovery(true);
  REQUIRE_FALSE(telemetry.snapshot().xruns.has_value(), "PipeWire has no server xrun count");
  REQUIRE_FALSE(telemetry.snapshot().client_xrun_notifications.has_value(),
                "PipeWire has no client xrun notifications");
  REQUIRE_TRUE(ring.try_take(record));
  REQUIRE_EQ(record.kind, foundation::DiagnosticKind::block_silenced);
  REQUIRE_EQ(record.detail, frames);
  REQUIRE_EQ(record.sample_position, sample);
  REQUIRE_EQ(record.category, foundation::DiagnosticCategory::device);
  for (int episode = 0; episode < 2; ++episode) {
    REQUIRE_TRUE(ring.try_take(record));
    REQUIRE_EQ(record.kind, foundation::DiagnosticKind::xrun, "one record per recovery episode");
  }
  REQUIRE_FALSE(ring.try_take(record));

  telemetry.reports_server_xruns();
  REQUIRE_EQ(telemetry.snapshot().xruns, std::optional<std::uint64_t>{0},
             "a backend that reports xruns starts from a known zero");
  REQUIRE_EQ(telemetry.snapshot().client_xrun_notifications, std::optional<std::uint64_t>{0},
             "client xrun notifications start from a known zero");
  telemetry.server_xrun();
  REQUIRE_EQ(telemetry.snapshot().xruns, std::optional<std::uint64_t>{1});
  REQUIRE_EQ(telemetry.snapshot().client_xrun_notifications, std::optional<std::uint64_t>{1},
             "a server xrun callback counts as a client notification");
  REQUIRE_TRUE(ring.try_take(record));
  REQUIRE_EQ(record.kind, foundation::DiagnosticKind::xrun);

  telemetry.start(rate);
  REQUIRE_FALSE(telemetry.snapshot().xruns.has_value(), "a restart forgets the xrun count");
  REQUIRE_FALSE(telemetry.snapshot().client_xrun_notifications.has_value(),
                "a restart forgets the client notification count");
  REQUIRE_FALSE(telemetry.snapshot().callback_load.has_value(), "and the load");
}

void callback_headroom_contract() {
  CallbackHeadroomTelemetry timing;
  timing.record({1, 0, 48'000});
  timing.record({1, 64, 0});
  require(!timing.snapshot(), "invalid timing samples do not establish a histogram");
  constexpr std::uint32_t rate = oscillate::test::sample_rate_48k;
  constexpr std::uint32_t frames = 48;
  for (const auto elapsed : {250'000ULL, 500'000ULL, 750'000ULL, 900'000ULL, 1'000'000ULL,
                             1'250'000ULL, 1'500'000ULL, 1'500'001ULL}) {
    timing.record({elapsed, frames, rate});
  }
  const auto first = timing.snapshot();
  REQUIRE_TRUE(first, "a valid callback tags its rate and block size");
  REQUIRE_EQ(first->rate, rate, "a valid callback tags its rate and block size");
  REQUIRE_EQ(first->frames, frames, "a valid callback tags its rate and block size");
  for (const auto bin : first->bins) {
    REQUIRE_EQ(bin, 1, "fraction-of-period bins have inclusive boundaries");
  }
  timing.record({750'000, frames, rate});
  const auto later = timing.snapshot();
  REQUIRE_TRUE(later, "same-format snapshots can be differenced for a recent window");
  REQUIRE_EQ(later->epoch, first->epoch,
             "same-format snapshots can be differenced for a recent window");
  REQUIRE_EQ(later->bins[2] - first->bins[2], 1,
             "same-format snapshots can be differenced for a recent window");
  require(!summarize_callback_window(*first, *later),
          "one callback must not qualify the default observation window");
  const auto window = summarize_callback_window(*first, *later, 1);
  REQUIRE_TRUE(window, "window free-time bound must use the slowest bin upper edge");
  REQUIRE_EQ(window->blocks, 1, "window free-time bound must use the slowest bin upper edge");
  REQUIRE_EQ(window->slowest_bin_upper_percent, 75,
             "window free-time bound must use the slowest bin upper edge");
  REQUIRE_EQ(window->observed_free_us_lower_bound, 250,
             "window free-time bound must use the slowest bin upper edge");
  require(!summarize_callback_window(*later, *first, 1),
          "reversed counters must not create false available callback time");
  timing.record({1'250'000, frames, rate});
  const auto over_period = timing.snapshot();
  const auto overloaded = summarize_callback_window(*later, *over_period, 1);
  REQUIRE_TRUE(overloaded, "over-period callback must not imply available time");
  REQUIRE_EQ(overloaded->slowest_bin_upper_percent, 125,
             "over-period callback must not imply available time");
  REQUIRE_EQ(overloaded->observed_free_us_lower_bound, 0,
             "over-period callback must not imply available time");
  timing.record({750'000, 96, rate});
  const auto resized = timing.snapshot();
  REQUIRE_TRUE(resized, "buffer-size change starts a new histogram epoch");
  REQUIRE_NE(resized->epoch, later->epoch, "buffer-size change starts a new histogram epoch");
  REQUIRE_EQ(resized->frames, 96, "buffer-size change starts a new histogram epoch");
  REQUIRE_EQ(resized->bins[1], 1, "buffer-size change starts a new histogram epoch");
  REQUIRE_EQ(resized->bins[2], 0, "buffer-size change starts a new histogram epoch");
  require(!summarize_callback_window(*later, *resized, 1),
          "a changed format cannot reuse the prior headroom observation");
  timing.record({750'000, 96, 96'000});
  const auto rerated = timing.snapshot();
  REQUIRE_TRUE(rerated, "rate change starts a new histogram epoch");
  REQUIRE_NE(rerated->epoch, resized->epoch, "rate change starts a new histogram epoch");
  REQUIRE_EQ(rerated->rate, 96'000, "rate change starts a new histogram epoch");
  REQUIRE_EQ(rerated->bins[2], 1, "rate change starts a new histogram epoch");
  timing.reset();
  require(!timing.snapshot(), "a restarted output has no stale timing distribution");
}

void selected_target_registry_contract() {
  SelectedTargetRegistry target;
  target.select(356);
  REQUIRE_TRUE(!target.present(), "selection begins unseen but not lost");
  REQUIRE_TRUE(!target.lost(), "selection begins unseen but not lost");
  require(!target.observe_node({12, 355}), "other target serial cannot satisfy selection");
  require(!target.remove_node(12), "unrelated removal cannot lose an unseen target");
  REQUIRE_TRUE(target.observe_node({93, 356}), "matching serial remembers its PipeWire global ID");
  REQUIRE_TRUE(target.present(), "matching serial remembers its PipeWire global ID");
  REQUIRE_TRUE(!target.remove_node(94), "other global removal preserves the selected target");
  REQUIRE_TRUE(target.present(), "other global removal preserves the selected target");
  REQUIRE_TRUE(target.remove_node(93), "matching global removal makes the loss sticky");
  REQUIRE_TRUE(target.lost(), "matching global removal makes the loss sticky");
  REQUIRE_TRUE(!target.present(), "matching global removal makes the loss sticky");
  REQUIRE_TRUE(!target.observe_node({93, 356}),
               "late or repeated events cannot resurrect a lost selection");
  REQUIRE_TRUE(!target.remove_node(93),
               "late or repeated events cannot resurrect a lost selection");
  target.select(356);
  REQUIRE_TRUE(!target.lost(), "explicit reselection permits the same serial on a fresh stream");
  REQUIRE_TRUE(target.observe_node({7, 356}),
               "explicit reselection permits the same serial on a fresh stream");
  target.select(std::nullopt);
  REQUIRE_TRUE(!target.present(), "unselected streams ignore target events");
  REQUIRE_TRUE(!target.lost(), "unselected streams ignore target events");
  REQUIRE_TRUE(!target.observe_node({7, 356}), "unselected streams ignore target events");
}

void sink_catalog_contract() {
  const auto named = parse_pipewire_sink(
      {"Audio/Sink", "18446744073709551615", "Studio monitors", "alsa_output.monitor"});
  REQUIRE_TRUE(named, "sink descriptions and full-width object serials are preserved");
  REQUIRE_EQ(named->serial, std::numeric_limits<std::uint64_t>::max(),
             "sink descriptions and full-width object serials are preserved");
  REQUIRE_EQ(named->name, "Studio monitors",
             "sink descriptions and full-width object serials are preserved");
  const auto fallback = parse_pipewire_sink({"Audio/Sink", "42", "", "alsa_output.headphones"});
  REQUIRE_TRUE(fallback, "node name is used when description is empty");
  REQUIRE_EQ(fallback->name, "alsa_output.headphones",
             "node name is used when description is empty");
  require(!parse_pipewire_sink({"Audio/Source", "42", "Microphone", "mic"}),
          "capture sources are not output choices");
  REQUIRE_TRUE(!parse_pipewire_sink({"Audio/Sink", "0", "Output", "output"}),
               "malformed or unusable registry nodes are omitted");
  REQUIRE_TRUE(!parse_pipewire_sink({"Audio/Sink", "4junk", "Output", "output"}),
               "malformed or unusable registry nodes are omitted");
  REQUIRE_TRUE(!parse_pipewire_sink({"Audio/Sink", "18446744073709551616", "Output", "output"}),
               "malformed or unusable registry nodes are omitted");
  REQUIRE_TRUE(!parse_pipewire_sink({"Audio/Sink", nullptr, "Output", "output"}),
               "malformed or unusable registry nodes are omitted");
  REQUIRE_TRUE(!parse_pipewire_sink({"Audio/Sink", "7", "", ""}),
               "malformed or unusable registry nodes are omitted");
  require(!list_pipewire_sinks(std::chrono::milliseconds::zero()),
          "zero discovery deadline is rejected before connecting");
}

void graph_bridge_and_lifecycle() {
  oscillate::engine::GraphPublisher publisher;
  const auto source = graph_render_source(publisher);
  std::array<oscillate::engine::StereoFrame, 4> scratch{};
  std::array<float, 8> output{};
  REQUIRE_EQ(render_stereo_block(source, {0, 1}, scratch, output),
             BlockResult::silent_render_failure,
             "inactive graph safely reports unavailable playback");
  REQUIRE_EQ(output, (std::array<float, 8>{}), "inactive graph output is silence");

  TransportCursor transport;
  PipeWireOutput device(source, transport);
  REQUIRE_EQ(device.status().state, OutputState::stopped, "no stream before start");
  require(!device.start(12345, 128), "unsupported rate rejected before creating a loop");
  require(!device.start(48000, 0), "zero quantum rejected before creating a loop");
  require(!device.start(48000, 128, 0), "zero target serial rejected before creating a loop");
  device.stop();
  REQUIRE_EQ(device.status().state, OutputState::stopped, "stop is idempotent");
}

// A held planned wrap is replayed as a seek for processors: the replayed
// block has a new state generation, so no processor sees a position jump
// that claims continuous history. An unheld wrap keeps processor history.
void held_loop_wrap_replays_as_seek() {
  struct Holding {
    int holds = 0;
    oscillate::engine::BlockContext last{};
    static bool render(void* context, std::span<oscillate::engine::StereoFrame>,
                       oscillate::engine::BlockContext block) noexcept {
      static_cast<Holding*>(context)->last = block;
      return true;
    }
    static bool freeze(void* context) noexcept {
      auto& self = *static_cast<Holding*>(context);
      return self.holds-- > 0;
    }
  } holding;
  const StereoRenderSource source{&holding, &Holding::render, &Holding::freeze};
  TransportCursor cursor(100);
  std::array<oscillate::engine::StereoFrame, 4> scratch{};
  std::array<float, 8> output{};
  const auto render = [&] {
    REQUIRE_EQ(render_transport_block(source, cursor, true, scratch, output), BlockResult::rendered,
               "fixture block failed");
    return holding.last;
  };
  (void)render();
  holding.holds = 1;
  require(cursor.request_loop_wrap(10), "loop wrap was rejected");
  const auto held = render();
  REQUIRE_TRUE(held.planned_loop_wrap,
               "planned wrap block lost its wrap flag or processor history");
  REQUIRE_EQ(held.state_generation(), 1,
             "planned wrap block lost its wrap flag or processor history");
  REQUIRE_EQ(held.transport_generation, 2,
             "planned wrap block lost its wrap flag or processor history");
  REQUIRE_EQ(held.project_sample_position, 10,
             "planned wrap block lost its wrap flag or processor history");
  const auto replayed = render();
  REQUIRE_EQ(replayed.project_sample_position, 10,
             "held planned wrap did not replay as a processor seek");
  REQUIRE_EQ(replayed.transport_generation, 2,
             "held planned wrap did not replay as a processor seek");
  REQUIRE_EQ(replayed.state_generation(), 2,
             "held planned wrap did not replay as a processor seek");
  REQUIRE_TRUE(!replayed.planned_loop_wrap, "held planned wrap did not replay as a processor seek");
  require(cursor.request_loop_wrap(20), "second loop wrap was rejected");
  const auto continuous = render();
  REQUIRE_TRUE(continuous.planned_loop_wrap, "unheld planned wrap reset processor history");
  REQUIRE_EQ(continuous.state_generation(), 2, "unheld planned wrap reset processor history");
  REQUIRE_EQ(continuous.transport_generation, 3, "unheld planned wrap reset processor history");
}

// A scheduled wrap waits for the block that starts at its boundary, then
// wraps exactly like an immediate planned wrap.
void scheduled_loop_wrap_waits_for_its_boundary() {
  TransportCursor cursor(0);
  require(cursor.request_loop_wrap_at(256, 64), "scheduled wrap was rejected");
  require(!cursor.request_seek(0), "a second request was accepted while a wrap is queued");
  for (std::uint64_t expected = 0; expected < 256; expected += 64) {
    const auto block = cursor.next_block(64);
    REQUIRE_TRUE(block, "scheduled wrap fired before its boundary");
    REQUIRE_EQ(block->project_sample_position, expected,
               "scheduled wrap fired before its boundary");
    REQUIRE_EQ(block->transport_generation, 1, "scheduled wrap fired before its boundary");
    REQUIRE_TRUE(!block->planned_loop_wrap, "scheduled wrap fired before its boundary");
  }
  const auto wrapped = cursor.next_block(64);
  REQUIRE_TRUE(wrapped, "scheduled wrap did not fire as a planned wrap at its boundary");
  REQUIRE_EQ(wrapped->project_sample_position, 64,
             "scheduled wrap did not fire as a planned wrap at its boundary");
  REQUIRE_TRUE(wrapped->planned_loop_wrap,
               "scheduled wrap did not fire as a planned wrap at its boundary");
  REQUIRE_EQ(wrapped->transport_generation, 2,
             "scheduled wrap did not fire as a planned wrap at its boundary");
  REQUIRE_EQ(wrapped->state_generation(), 1,
             "scheduled wrap did not fire as a planned wrap at its boundary");
  const auto after = cursor.next_block(64);
  REQUIRE_TRUE(after, "scheduled wrap did not release the request slot");
  REQUIRE_EQ(after->project_sample_position, 128,
             "scheduled wrap did not release the request slot");
  REQUIRE_TRUE(!after->planned_loop_wrap, "scheduled wrap did not release the request slot");
  REQUIRE_TRUE(cursor.request_seek(0), "scheduled wrap did not release the request slot");
}

// Writes each frame's project sample into the left channel and records every
// render call's context, so tests can read the exact output timeline.
struct TimelineRenderer {
  static constexpr std::size_t max_calls = 64;
  std::array<oscillate::engine::BlockContext, max_calls> contexts{};
  std::array<std::size_t, max_calls> lengths{};
  std::size_t calls = 0;
  std::optional<std::uint64_t> freeze_at; // freeze the part starting here, once
  bool froze = false;
  static bool render(void* context, std::span<oscillate::engine::StereoFrame> stereo,
                     oscillate::engine::BlockContext block) noexcept {
    auto& self = *static_cast<TimelineRenderer*>(context);
    if (self.calls < max_calls) {
      self.contexts[self.calls] = block;
      self.lengths[self.calls] = stereo.size();
    }
    ++self.calls;
    self.froze = self.freeze_at == block.project_sample_position;
    if (self.froze)
      self.freeze_at.reset();
    for (std::size_t index = 0; index < stereo.size(); ++index) {
      const auto sample =
          self.froze ? -1.0F : static_cast<float>(block.project_sample_position + index);
      stereo[index] = {sample, sample};
    }
    return true;
  }
  static bool freeze(void* context) noexcept {
    return static_cast<TimelineRenderer*>(context)->froze;
  }
};

// Renders `blocks` device blocks of `frames` and returns the left channel.
std::vector<float> render_timeline(TimelineRenderer& renderer, TransportCursor& cursor,
                                   std::size_t frames, std::size_t blocks) {
  const StereoRenderSource source{&renderer, &TimelineRenderer::render, &TimelineRenderer::freeze};
  std::vector<oscillate::engine::StereoFrame> scratch(frames);
  std::vector<float> interleaved(frames * 2);
  std::vector<float> left;
  for (std::size_t block = 0; block < blocks; ++block) {
    REQUIRE_EQ(render_transport_block(source, cursor, true, scratch, interleaved),
               BlockResult::rendered, "timeline block did not render");
    for (std::size_t frame = 0; frame < frames; ++frame)
      left.push_back(interleaved[frame * 2]);
  }
  return left;
}

void scheduled_wrap_is_sample_exact_at_any_boundary() {
  constexpr std::array<std::size_t, 6> periods{1, 7, 64, 128, 333, 1024};
  constexpr std::array<std::uint64_t, 5> offsets{0, 1, 63, 500, 4097};
  for (const auto period : periods) {
    for (const auto offset : offsets) {
      const std::uint64_t start = 1000;
      const std::uint64_t end = 3000 + offset; // usually not on the block grid
      TransportCursor cursor(start);
      require(cursor.request_loop_wrap_at(end, start), "scheduled wrap was rejected");
      TimelineRenderer renderer;
      const auto pass = end - start;
      const auto blocks = static_cast<std::size_t>((pass + 50) / period + 2);
      const auto left = render_timeline(renderer, cursor, period, blocks);
      for (std::size_t index = 0; index < left.size(); ++index) {
        const auto expected = index < pass ? start + index : start + (index - pass);
        REQUIRE_EQ(left[index], static_cast<float>(expected),
                   "loop output is not sample-exact across the boundary (period " +
                       std::to_string(period) + ", end " + std::to_string(end) + ", frame " +
                       std::to_string(index) + ")");
      }
      const auto counts = cursor.loop_wrap_counts();
      REQUIRE_EQ(counts.applied, 1, "wrap was not counted as exact");
      REQUIRE_EQ(counts.late, 0, "wrap was not counted as exact");
      // The wrap part is a planned wrap that keeps processor history.
      bool seen = false;
      for (std::size_t call = 0; call < std::min(renderer.calls, TimelineRenderer::max_calls);
           ++call) {
        const auto& context = renderer.contexts[call];
        if (!context.planned_loop_wrap)
          continue;
        seen = true;
        REQUIRE_EQ(context.project_sample_position, start, "wrap part has the wrong context");
        REQUIRE_EQ(context.transport_generation, 2, "wrap part has the wrong context");
        REQUIRE_EQ(context.state_generation(), 1, "wrap part has the wrong context");
      }
      require(seen || renderer.calls > TimelineRenderer::max_calls, "no wrap part was rendered");
    }
  }
}

void scheduled_wrap_preserves_delayed_audio() {
  // The impulse is the last source frame before a non-block-aligned loop end.
  // A two-frame PDC path must emit it in the second frame of the next pass.
  // This exercises the transport split and the actual delay runtime together.
  struct DelayedImpulse {
    std::unique_ptr<oscillate::engine::DelayCompensationRuntime> line;
    std::size_t calls = 0;
    static bool render(void* context, std::span<oscillate::engine::StereoFrame> stereo,
                       oscillate::engine::BlockContext block) noexcept {
      auto& self = *static_cast<DelayedImpulse*>(context);
      ++self.calls;
      for (std::size_t frame = 0; frame < stereo.size(); ++frame) {
        const bool impulse = block.project_sample_position + frame == 99;
        stereo[frame] = impulse ? oscillate::engine::StereoFrame{1.0F, -1.0F}
                                : oscillate::engine::StereoFrame{};
      }
      self.line->process(stereo, block);
      return true;
    }
  };
  for (const std::size_t period : {64U, 128U}) {
    auto prepared = oscillate::engine::DelayCompensationRuntime::prepare({1, 2, 8, 72});
    REQUIRE_OK(prepared, "delay-line fixture could not be prepared");
    DelayedImpulse renderer{std::move(prepared.value())};
    TransportCursor cursor(90);
    require(cursor.request_loop_wrap_at(100, 0), "delayed-audio wrap could not be scheduled");
    std::vector<oscillate::engine::StereoFrame> scratch(period);
    std::vector<float> output(period * 2);
    const StereoRenderSource source{&renderer, &DelayedImpulse::render};
    REQUIRE_EQ(render_transport_block(source, cursor, true, scratch, output), BlockResult::rendered,
               "delayed-audio loop block failed to render");
    REQUIRE_EQ(renderer.calls, 2, "delayed-audio loop did not split exactly at the boundary");
    REQUIRE_EQ(cursor.loop_wrap_counts().applied, 1,
               "delayed-audio loop did not split exactly at the boundary");
    REQUIRE_EQ(cursor.loop_wrap_counts().late, 0,
               "delayed-audio loop did not split exactly at the boundary");
    for (std::size_t frame = 0; frame < period; ++frame) {
      const float expected = frame == 11 || frame == 111 ? 1.0F : 0.0F;
      require(output[frame * 2] == expected && output[frame * 2 + 1] == -expected,
              "PDC history was lost or shifted at the scheduled wrap, frame " +
                  std::to_string(frame) + ": " + std::to_string(output[frame * 2]));
    }
    REQUIRE_EQ(renderer.line->guarded_frames_after_quiesce(), 0,
               "the planned wrap tripped the delay-line continuity guard");
  }
}

void late_wrap_applies_next_block_and_is_counted() {
  TransportCursor cursor(0);
  TimelineRenderer renderer;
  (void)render_timeline(renderer, cursor, 64, 3); // now at 192
  require(cursor.request_loop_wrap_at(100, 0), "late wrap was rejected");
  const auto left = render_timeline(renderer, cursor, 64, 1);
  REQUIRE_EQ(left.front(), 0.0F, "late wrap did not apply at block start");
  REQUIRE_EQ(left.back(), 63.0F, "late wrap did not apply at block start");
  const auto counts = cursor.loop_wrap_counts();
  REQUIRE_EQ(counts.applied, 1, "late wrap was not counted late");
  REQUIRE_EQ(counts.late, 1, "late wrap was not counted late");
}

void withdrawn_and_superseded_wraps() {
  TransportCursor cursor(0);
  require(!cursor.cancel_loop_wrap(), "cancel succeeded with nothing queued");
  require(cursor.request_loop_wrap_at(200, 0), "first wrap rejected");
  require(cursor.cancel_loop_wrap(), "queued wrap could not be withdrawn");
  require(!cursor.cancel_loop_wrap(), "a wrap was withdrawn twice");
  TimelineRenderer renderer;
  auto left = render_timeline(renderer, cursor, 64, 5); // crosses 200
  for (std::size_t index = 0; index < left.size(); ++index)
    REQUIRE_EQ(left[index], static_cast<float>(index), "withdrawn wrap still applied");
  REQUIRE_EQ(cursor.loop_wrap_counts().applied, 0, "withdrawn wrap was counted");
  // Supersede: queue, withdraw, queue another range; only the last applies.
  require(cursor.request_loop_wrap_at(400, 100), "second wrap rejected");
  require(cursor.cancel_loop_wrap(), "second wrap could not be withdrawn");
  require(cursor.request_loop_wrap_at(350, 300), "superseding wrap rejected");
  left = render_timeline(renderer, cursor, 64, 3); // 320..512 device frames
  for (std::size_t index = 0; index < left.size(); ++index) {
    const auto position = 320 + index;
    const auto expected = position < 350 ? position : 300 + (position - 350);
    REQUIRE_EQ(left[index], static_cast<float>(expected), "superseded wrap used the old range");
  }
  // An applied wrap cannot be withdrawn; a seek is not withdrawn by it.
  require(!cursor.cancel_loop_wrap(), "an applied wrap was withdrawn");
  REQUIRE_TRUE(cursor.request_seek(0), "cancel withdrew a seek");
  REQUIRE_TRUE(!cursor.cancel_loop_wrap(), "cancel withdrew a seek");
  // A seek after quiesce drops a queued request.
  (void)render_timeline(renderer, cursor, 64, 1);
  require(cursor.request_loop_wrap_at(10'000, 0), "wrap rejected after seek");
  REQUIRE_OK(cursor.seek_after_quiesce(5), "seek after quiesce did not free the request slot");
  REQUIRE_TRUE(cursor.request_loop_wrap_at(20, 0),
               "seek after quiesce did not free the request slot");
}

void frozen_first_part_replays_before_the_wrap() {
  TransportCursor cursor(0);
  require(cursor.request_loop_wrap_at(100, 0), "wrap rejected");
  TimelineRenderer renderer;
  renderer.freeze_at = 64; // the part [64, 100) holds once
  auto left = render_timeline(renderer, cursor, 64, 2);
  for (std::size_t index = 64; index < 128; ++index)
    REQUIRE_EQ(left[index], (index < 100 ? -1.0F : 0.0F),
               "a held part must be followed by silence, not the wrap");
  REQUIRE_EQ(cursor.completed_snapshot().next_sample, 64, "held part must replay before the wrap");
  REQUIRE_EQ(cursor.loop_wrap_counts().applied, 0, "held part must replay before the wrap");
  left = render_timeline(renderer, cursor, 64, 1);
  for (std::size_t index = 0; index < 64; ++index) {
    const auto expected = index < 36 ? 64 + index : index - 36;
    REQUIRE_EQ(left[index], static_cast<float>(expected), "replayed part did not wrap exactly");
  }
}

void restart_in_place_keeps_position() {
  TransportCursor cursor(500);
  TimelineRenderer renderer;
  (void)render_timeline(renderer, cursor, 32, 1);
  require(cursor.request_restart_in_place(), "in-place restart rejected");
  require(!cursor.request_loop_wrap_at(600, 0), "a wrap was queued over a pending restart");
  const auto left = render_timeline(renderer, cursor, 32, 1);
  const auto& context = renderer.contexts[1];
  REQUIRE_EQ(left.front(), 532.0F, "in-place restart moved the playhead or kept processor history");
  REQUIRE_EQ(context.transport_generation, 2,
             "in-place restart moved the playhead or kept processor history");
  REQUIRE_EQ(context.state_generation(), 2,
             "in-place restart moved the playhead or kept processor history");
  REQUIRE_TRUE(!context.planned_loop_wrap,
               "in-place restart moved the playhead or kept processor history");
}

// The control thread queues and withdraws wraps against a running callback.
// Every request is either withdrawn or applied exactly once, and an applied
// wrap always lands on the boundary of the request that won. The control
// thread reads the callback's position from the renderer and sleeps on the
// renderer's wrap count, so neither thread spins or yields to the other and
// the race also completes when both share one loaded CPU.
void wrap_withdrawal_races_the_callback() {
  constexpr int rounds = 20'000;
  // Every third round waits for its wrap to apply; the others withdraw it.
  constexpr int applied_round_period = 3;
  constexpr std::uint64_t loop_length = 1'000;
  constexpr int boundary_spread = 97;
  constexpr std::uint64_t minimum_queued = 1'000;
  constexpr std::size_t block_frames = 37;
  TransportCursor cursor(0);
  std::atomic<bool> stop{false};
  PublishedCount observed;
  std::atomic<std::uint64_t> wrong{0};
  std::atomic<std::uint64_t> exact{0};
  // The first sample after the last part the callback rendered.
  std::atomic<std::uint64_t> rendered_end{0};
  std::jthread callback([&] {
    std::array<oscillate::engine::StereoFrame, block_frames> scratch{};
    std::array<float, block_frames * 2> output{};
    struct Checker {
      PublishedCount* observed;
      std::atomic<std::uint64_t>* wrong;
      std::atomic<std::uint64_t>* exact;
      std::atomic<std::uint64_t>* rendered_end;
      std::uint64_t expected = 0;
      std::uint64_t wraps = 0;
      static bool render(void* context, std::span<oscillate::engine::StereoFrame> stereo,
                         oscillate::engine::BlockContext block) noexcept {
        auto& self = *static_cast<Checker*>(context);
        if (block.planned_loop_wrap) {
          // Every wrap jumps back one loop length from its boundary: exactly
          // at it, or (late) at the first block start after it.
          if (block.project_sample_position + loop_length == self.expected)
            self.exact->fetch_add(1, std::memory_order_relaxed);
          else if (block.project_sample_position + loop_length > self.expected)
            self.wrong->fetch_add(1, std::memory_order_relaxed);
          // Test-only synchronisation: a real callback would not lock.
          self.observed->publish(++self.wraps);
        } else if (block.project_sample_position != self.expected) {
          self.wrong->fetch_add(1, std::memory_order_relaxed);
        }
        self.expected = block.project_sample_position + stereo.size();
        self.rendered_end->store(self.expected, std::memory_order_release);
        return true;
      }
    } checker{&observed, &wrong, &exact, &rendered_end};
    const StereoRenderSource source{&checker, &Checker::render};
    while (!stop.load(std::memory_order_acquire))
      (void)render_transport_block(source, cursor, true, scratch, output);
  });
  std::uint64_t queued = 0;
  std::uint64_t withdrawn = 0;
  for (int round = 0; round < rounds; ++round) {
    const auto at = rendered_end.load(std::memory_order_acquire);
    const auto boundary = at + loop_length + static_cast<std::uint64_t>(round % boundary_spread);
    if (!cursor.request_loop_wrap_at(boundary, boundary - loop_length))
      continue;
    ++queued;
    if (round % applied_round_period != 0) {
      if (cursor.cancel_loop_wrap())
        ++withdrawn;
    } else {
      observed.wait_for(queued - withdrawn);
    }
  }
  observed.wait_for(queued - withdrawn);
  stop.store(true, std::memory_order_release);
  callback.join();
  const auto counts = cursor.loop_wrap_counts();
  REQUIRE_GT(queued, minimum_queued, "race test did not exercise both outcomes");
  REQUIRE_GT(withdrawn, 0, "race test did not exercise both outcomes");
  REQUIRE_EQ(counts.applied + withdrawn, queued,
             "a wrap was applied twice, lost or both withdrawn and applied");
  REQUIRE_EQ(observed.value(), counts.applied,
             "a wrap was applied twice, lost or both withdrawn and applied");
  REQUIRE_EQ(wrong.load(), 0,
             "an applied wrap did not land on its boundary, or a late wrap was not counted");
  REQUIRE_EQ(exact.load(), counts.applied - counts.late,
             "an applied wrap did not land on its boundary, or a late wrap was not counted");
}

void stop_gate_holds_transport() {
  CallbackStopGate gate;
  FakeRenderer fake;
  TransportCursor cursor(123);
  std::array<oscillate::engine::StereoFrame, 4> scratch{};
  std::array<float, 8> output{};
  const StereoRenderSource source{&fake, &FakeRenderer::render};
  REQUIRE_EQ(render_transport_block(source, cursor, !gate.requested(), scratch, output),
             BlockResult::rendered, "running callback reaches renderer");
  gate.request();
  output.fill(77.0F);
  REQUIRE_EQ(render_transport_block(source, cursor, !gate.requested(), scratch, output),
             BlockResult::silent_render_failure,
             "requested stop suppresses a subsequent callback render");
  REQUIRE_EQ(fake.calls, 1, "stop gate emits silence without entering renderer");
  REQUIRE_EQ(output, (std::array<float, 8>{}), "stop gate emits silence without entering renderer");
  REQUIRE_EQ(cursor.completed_snapshot().next_sample, 127,
             "suppressed callback does not advance transport");
  gate.reset_after_quiesce();
  REQUIRE_EQ(render_transport_block(source, cursor, !gate.requested(), scratch, output),
             BlockResult::rendered, "a quiesced output can reset its gate for a new stream");
  REQUIRE_EQ(fake.received.project_sample_position, 127,
             "restart begins at the held transport boundary");
}
} // namespace

// A silent song, so only the metronome is heard.
struct SilentRenderer {
  static bool render(void*, std::span<oscillate::engine::StereoFrame> stereo,
                     oscillate::engine::BlockContext) noexcept {
    for (auto& frame : stereo)
      frame = {0.0F, 0.0F};
    return true;
  }
};

// Sign changes of a click: twice its frequency per second of sound.
std::size_t zero_crossings(std::uint64_t frames, bool accent, std::uint32_t rate) {
  std::size_t count = 0;
  float last = 0.0F;
  for (std::uint64_t frame = 1; frame < frames; ++frame) {
    const auto value = click_sample(frame, accent, rate);
    if ((value > 0.0F && last < 0.0F) || (value < 0.0F && last > 0.0F))
      ++count;
    if (value != 0.0F)
      last = value;
  }
  return count;
}

// The click: accented bars ring higher and louder, every click ends, and
// clicks land on their beats whatever the block size; off is silent.
void metronome_clicks_on_the_beats() {
  constexpr std::uint32_t rate = oscillate::test::sample_rate_48k;
  const auto length = click_frames(rate);
  REQUIRE_EQ(length, 1'440, "a click lasts 30 ms");
  REQUIRE_EQ(click_sample(length, false, rate), 0.0F,
             "a click starts at zero and ends after its length");
  REQUIRE_EQ(click_sample(0, true, rate), 0.0F, "a click starts at zero and ends after its length");
  const auto plain = zero_crossings(length, false, rate);
  const auto accented = zero_crossings(length, true, rate);
  REQUIRE_GE(plain, 58, "1 kHz clicks, 1.5 kHz accents");
  REQUIRE_LE(plain, 60, "1 kHz clicks, 1.5 kHz accents");
  REQUIRE_GE(accented, 88, "1 kHz clicks, 1.5 kHz accents");
  REQUIRE_LE(accented, 90, "1 kHz clicks, 1.5 kHz accents");
  // 120 BPM: a beat every 24000 frames, the bar's first accented.
  constexpr std::uint64_t beat_count = 8;
  std::vector<ClickBeat> beats;
  beats.reserve(beat_count);
  constexpr std::uint64_t beat = 24'000;
  for (std::uint64_t index = 0; index < beat_count; ++index)
    beats.push_back({index * beat, index % 4 == 0});
  Metronome metronome;
  metronome.publish(beats, rate);
  constexpr std::size_t total = 4 * beat;
  const auto render = [&](std::size_t block) {
    std::vector<float> out(total * 2, 0.0F);
    for (std::size_t at = 0; at < total; at += block) {
      const auto frames = std::min(block, total - at);
      metronome.add_clicks(at, std::span(out).subspan(at * 2, frames * 2));
    }
    return out;
  };
  const auto disabled = render(total);
  require(std::ranges::all_of(disabled, [](float v) { return v == 0.0F; }),
          "a disabled click is silent");
  metronome.set_enabled(true);
  const auto whole = render(total);
  REQUIRE_EQ(whole, render(37), "the clicks are the same however the blocks split");
  REQUIRE_EQ(whole, render(256), "the clicks are the same however the blocks split");
  for (std::uint64_t index = 0; index < 4; ++index) {
    const auto at = index * beat;
    float peak = 0.0F;
    for (std::uint64_t frame = at; frame < at + length; ++frame)
      peak = std::max(peak, std::abs(whole[frame * 2]));
    REQUIRE_GT(peak, (index == 0 ? 0.4F : 0.25F), "each beat clicks, the bar's first louder");
    REQUIRE_EQ(whole[(at + length + 10) * 2], 0.0F, "silence between clicks");
    REQUIRE_EQ(whole[at * 2], whole[at * 2 + 1], "both channels click alike");
  }
  // From a later position (a seek), the click is the same function of time.
  std::vector<float> late(beat * 2, 0.0F);
  metronome.add_clicks(beat, late);
  require(
      std::equal(late.begin(), late.end(), whole.begin() + static_cast<std::ptrdiff_t>(beat * 2)),
      "a seek lands on the same clicks");
}

// A count-in holds the transport and plays only its clicks; the song then
// starts where it was, with its own clicks, and blocks publish when they
// are heard.
void count_in_holds_the_transport() {
  constexpr std::uint32_t rate = oscillate::test::sample_rate_48k;
  Metronome metronome;
  constexpr std::uint64_t start = 96'000;
  metronome.publish(std::vector<ClickBeat>{{start, true}}, rate);
  metronome.set_enabled(true);
  TransportCursor transport(start);
  transport.set_metronome(&metronome);
  constexpr std::uint64_t beat = 1'000;
  metronome.request_count_in(4, beat);
  require(metronome.counting_in(), "a count-in is pending");
  const StereoRenderSource source{nullptr, &SilentRenderer::render};
  std::array<oscillate::engine::StereoFrame, 1'500> scratch{};
  std::vector<float> output(std::size_t{1'500} * 2);
  require(!transport.audible_point(), "nothing is audible before a block renders");
  constexpr std::int64_t heard = 5'000'000'000;
  std::vector<float> heardout;
  std::size_t blocks = 0;
  while (metronome.counting_in()) {
    REQUIRE_EQ(render_transport_block(source, transport, true, scratch, output, {heard, rate}),
               BlockResult::rendered, "a count-in block renders");
    heardout.insert(heardout.end(), output.begin(), output.end());
    ++blocks;
    REQUIRE_LT(blocks, 10, "the count-in ends");
  }
  // 4000 count-in frames: two whole blocks and 1000 frames of the third,
  // whose last 500 frames are the song from `start`.
  REQUIRE_EQ(blocks, 3, "the count-in lasts four beats");
  REQUIRE_EQ(transport.completed_snapshot().next_sample, start + 500,
             "the transport waited for the count-in, then moved on");
  for (std::uint64_t index = 0; index < 4; ++index)
    REQUIRE_NE(heardout[(index * beat + 100) * 2], 0.0F, "each count-in beat clicks");
  REQUIRE_NE(heardout[4'000 * 2 + 100 * 2], 0.0F, "the song's first beat clicks after it");
  const auto point = transport.audible_point();
  REQUIRE_TRUE(point, "the song's first sample is heard after the count-in frames of its block");
  REQUIRE_EQ(point->sample, start,
             "the song's first sample is heard after the count-in frames of its block");
  REQUIRE_EQ(point->heard_ns, heard + static_cast<std::int64_t>(1'000 * 1e9 / rate),
             "the song's first sample is heard after the count-in frames of its block");
}

int main() {
  try {
    block_contract();
    transport_contract();
    completed_position_concurrency_contract();
    telemetry_contract();
    telemetry_load_and_diagnostics();
    callback_headroom_contract();
    selected_target_registry_contract();
    sink_catalog_contract();
    graph_bridge_and_lifecycle();
    held_loop_wrap_replays_as_seek();
    scheduled_loop_wrap_waits_for_its_boundary();
    scheduled_wrap_is_sample_exact_at_any_boundary();
    scheduled_wrap_preserves_delayed_audio();
    late_wrap_applies_next_block_and_is_counted();
    withdrawn_and_superseded_wraps();
    frozen_first_part_replays_before_the_wrap();
    restart_in_place_keeps_position();
    wrap_withdrawal_races_the_callback();
    stop_gate_holds_transport();
    metronome_clicks_on_the_beats();
    count_in_holds_the_transport();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
