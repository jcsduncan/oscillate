#include "oscillate/engine/chunked_processor.hpp"
#include "oscillate/engine/routed_publisher.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/tolerances.hpp"
#include "oscillate/test/wait.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace tolerance = oscillate::test::tolerance;

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;
using oscillate::test::fail_next_allocation;

namespace {
using namespace oscillate;
using namespace oscillate::engine;

using oscillate::test::require;

RoutedMixerLimits limits(std::size_t nodes, std::size_t edges, std::uint32_t delay = 32) {
  return {{3, delay},
          64,
          (nodes + 1) * 64 * sizeof(StereoFrame),
          edges * (static_cast<std::uint64_t>(delay) + 1) * sizeof(StereoFrame)};
}

// The fixtures' block, as limits() sizes it.
constexpr std::uint64_t fixture_block_frames = 64;
// Blocks a live latency change takes to settle: each changed delay
// crossfades from its old reading to its new one (ADR-0049).
constexpr std::uint64_t crossfade_blocks =
    (DelayCompensationRuntime::crossfade_frames + fixture_block_frames - 1) / fixture_block_frames;

void pdc_transition_and_failures() {
  std::array<RoutedNode, 3> nodes{{{{1, 0}, {}}, {{2, 8}, {}}, {{3, 0}, {}}}};
  const std::array<RoutingEdge, 2> edges{{{11, 1, 3}, {12, 2, 3}}};
  RoutedMixerPublisher publisher;
  REQUIRE_TRUE(!publisher.prepare_and_publish_live(1, nodes, edges, limits(3, 2)),
               "a live switch was admitted with no active graph");
  REQUIRE_OK(publisher.prepare_and_publish(1, nodes, edges, limits(3, 2)),
             "initial plan publication contract failed");
  REQUIRE_TRUE(publisher.has_pending(), "initial plan publication contract failed");
  std::array<StereoFrame, 64> fast{}, slow{}, output{};
  fast[48] = {0.5F, -0.25F};
  fast[56] = {1.0F, -0.5F};
  slow[56] = {0.5F, -0.25F};
  std::array<std::span<const StereoFrame>, 3> input{fast, slow, {}};
  REQUIRE_EQ(publisher.render_block(input, output, {0, 1}), RoutedRenderStatus::rendered,
             "initial routed render or impulse alignment failed");
  REQUIRE_EQ(publisher.active_revision(), 1, "initial routed render or impulse alignment failed");
  REQUIRE_EQ(output[56].left, 1.0F, "initial routed render or impulse alignment failed");
  REQUIRE_TRUE(!publisher.prepare_and_publish(2, nodes, edges, limits(3, 2)),
               "an active graph was replaced other than by a live switch");
  REQUIRE_TRUE(!publisher.has_pending(),
               "an active graph was replaced other than by a live switch");

  nodes[1].routing.processor_latency_frames = 16;
  auto over_budget = publisher.prepare_and_publish_live(2, nodes, edges, limits(3, 2, 4));
  REQUIRE_TRUE(!over_budget, "over-budget preparation displaced valid graph");
  REQUIRE_EQ(publisher.active_revision(), 1, "over-budget preparation displaced valid graph");
  REQUIRE_TRUE(!publisher.has_pending(), "over-budget preparation displaced valid graph");
  auto grown = publisher.prepare_and_publish_live(2, nodes, edges, limits(3, 2));
  REQUIRE_OK(grown, "primed update was not pending");
  REQUIRE_TRUE(publisher.has_pending(), "primed update was not pending");
  fast.fill({});
  slow.fill({});
  // The switch takes the next block: no silent block, and the transport
  // moves on.
  counting_allocations = true;
  const auto switched = publisher.render_block(input, output, {64, 1});
  counting_allocations = false;
  REQUIRE_EQ(switched, RoutedRenderStatus::rendered,
             "latency transition held a block or allocated on callback");
  REQUIRE_EQ(counted_allocations, 0, "latency transition held a block or allocated on callback");
  REQUIRE_EQ(publisher.active_revision(), 2,
             "latency transition held a block or allocated on callback");
  REQUIRE_EQ(publisher.collect_retired(), 1, "old mixer was not reclaimed on control");
  REQUIRE_EQ(publisher.collect_retired(), 0, "old mixer was not reclaimed on control");
  // Once the changed delay has crossfaded, both paths align at 16 frames.
  std::uint64_t position = 2 * fixture_block_frames;
  for (std::uint64_t block = 0; block < crossfade_blocks; ++block, position += fixture_block_frames)
    REQUIRE_EQ(publisher.render_block(input, output, {position, 1}), RoutedRenderStatus::rendered,
               "latency transition did not settle");
  fast[0] = {0.25F, -0.125F};
  slow[16] = {0.25F, -0.125F};
  REQUIRE_EQ(publisher.render_block(input, output, {position, 1}), RoutedRenderStatus::rendered,
             "latency transition lost impulse alignment");
  REQUIRE_EQ(output[0].left, 0.0F, "latency transition lost impulse alignment");
  REQUIRE_EQ(output[16].left, 0.5F, "latency transition lost impulse alignment");
  position += 64;

  nodes[1].routing.processor_latency_frames = 32;
  REQUIRE_TRUE(!publisher.prepare_and_publish_live(3, nodes, edges, limits(3, 2, 4)),
               "failed second edit displaced valid graph");
  REQUIRE_EQ(publisher.active_revision(), 2, "failed second edit displaced valid graph");
  REQUIRE_TRUE(!publisher.has_pending(), "failed second edit displaced valid graph");
  REQUIRE_EQ(publisher.render_block(input, output, {position, 1}), RoutedRenderStatus::rendered,
             "failed edit did not keep the old graph playing");
  REQUIRE_EQ(publisher.active_revision(), 2, "failed edit did not keep the old graph playing");
}

class Tail final : public ProcessorRuntime {
public:
  void process(std::span<StereoFrame> frames, BlockContext) noexcept override {
    for (auto& frame : frames) {
      std::swap(frame, memory_[cursor_]);
      cursor_ = (cursor_ + 1) % memory_.size();
    }
  }

private:
  std::array<StereoFrame, 4> memory_{};
  std::size_t cursor_{0};
};

class EightFrameTail final : public ProcessorRuntime {
public:
  void process(std::span<StereoFrame> frames, BlockContext) noexcept override {
    for (auto& frame : frames) {
      std::swap(frame, history_[cursor_]);
      cursor_ = (cursor_ + 1) % history_.size();
    }
  }

private:
  std::array<StereoFrame, 8> history_{};
  std::size_t cursor_ = 0;
};

// A route added by a live switch starts its compensating delay from its
// source's recent output, so a sample sent before the switch arrives on time.
void live_new_route_uses_source_history() {
  const std::array<RoutedNode, 3> nodes{
      {{{1, 0}, {}}, {{2, 8}, std::make_shared<EightFrameTail>()}, {{3, 0}, {}}}};
  const std::array<RoutingEdge, 2> original{{{11, 1, 2}, {12, 2, 3}}};
  const std::array<RoutingEdge, 3> added{{original[0], original[1], {13, 1, 3, 0.5F}}};
  auto original_limits = limits(3, 2, 16);
  auto added_limits = limits(3, 3, 16);
  original_limits.max_source_history_bytes = 3ULL * 17 * sizeof(StereoFrame);
  added_limits.max_source_history_bytes = original_limits.max_source_history_bytes;
  RoutedMixerPublisher publisher;
  REQUIRE_OK(publisher.prepare_and_publish(1, nodes, original, original_limits),
             "opt-in initial graph failed to prepare");
  std::array<StereoFrame, 64> source{}, output{};
  source[60] = {1.0F, -0.5F};
  const std::array<std::span<const StereoFrame>, 3> inputs{source, {}, {}};
  REQUIRE_EQ(publisher.render_block(inputs, output, {0, 1}), RoutedRenderStatus::rendered,
             "opt-in initial graph did not retain the pre-boundary impulse");
  REQUIRE_EQ(output[63].left, 0.0F, "opt-in initial graph did not retain the pre-boundary impulse");
  auto under_budget = added_limits;
  --under_budget.max_source_history_bytes;
  REQUIRE_TRUE(!publisher.prepare_and_publish_live(2, nodes, added, under_budget),
               "over-budget new route displaced the active graph");
  REQUIRE_EQ(publisher.active_revision(), 1, "over-budget new route displaced the active graph");
  REQUIRE_TRUE(!publisher.has_pending(), "over-budget new route displaced the active graph");
  REQUIRE_OK(publisher.prepare_and_publish_live(2, nodes, added, added_limits),
             "new compensated edge could not prime at the switch");
  REQUIRE_TRUE(publisher.has_pending(), "new compensated edge could not prime at the switch");
  source.fill({});
  counted_allocations = 0;
  counting_allocations = true;
  const auto status = publisher.render_block(inputs, output, {64, 1});
  counting_allocations = false;
  REQUIRE_EQ(status, RoutedRenderStatus::rendered,
             "publication lost the primed impulse, gain, or callback allocation contract");
  REQUIRE_EQ(counted_allocations, 0,
             "publication lost the primed impulse, gain, or callback allocation contract");
  REQUIRE_EQ(publisher.active_revision(), 2,
             "publication lost the primed impulse, gain, or callback allocation contract");
  REQUIRE_EQ(output[3].left, 0.0F,
             "publication lost the primed impulse, gain, or callback allocation contract");
  REQUIRE_EQ(output[4].left, 1.5F,
             "publication lost the primed impulse, gain, or callback allocation contract");
  REQUIRE_EQ(output[4].right, -0.75F,
             "publication lost the primed impulse, gain, or callback allocation contract");
  REQUIRE_EQ(publisher.collect_retired(), 1,
             "publication lost the primed impulse, gain, or callback allocation contract");
}

class Double final : public ProcessorRuntime {
public:
  void process(std::span<StereoFrame> frames, BlockContext) noexcept override {
    for (auto& frame : frames) {
      frame.left *= 2.0F;
      frame.right *= 2.0F;
    }
  }
};

class HookedDouble final : public ProcessorRuntime {
public:
  void process(std::span<StereoFrame> frames, BlockContext) noexcept override {
    ++blocks;
    for (auto& frame : frames) {
      frame.left *= 2.0F;
      frame.right *= 2.0F;
    }
  }
  void graph_activated(std::uint64_t) noexcept override { ++activations; }
  void graph_deactivated(std::uint64_t) noexcept override { ++deactivations; }
  std::uint32_t blocks = 0;
  std::uint32_t activations = 0;
  std::uint32_t deactivations = 0;
};

class AbsoluteEventProbe final : public ProcessorRuntime {
public:
  void process(std::span<StereoFrame> frames, BlockContext context) noexcept override {
    if (calls < positions.size()) {
      positions[calls] = context.project_sample_position;
      wraps[calls] = context.planned_loop_wrap;
      generations[calls] = context.state_generation();
    }
    ++calls;
    for (std::size_t index = 0; index < frames.size(); ++index) {
      const auto gain = context.project_sample_position + index >= 129 ? 2.0F : 1.0F;
      frames[index].left *= gain;
      frames[index].right *= gain;
    }
  }
  void graph_activated(std::uint64_t) noexcept override { ++activations; }
  void graph_deactivated(std::uint64_t) noexcept override { ++deactivations; }
  std::array<std::uint64_t, 2> positions{};
  std::array<std::uint64_t, 2> generations{};
  std::array<bool, 2> wraps{};
  std::size_t calls = 0;
  std::size_t activations = 0;
  std::size_t deactivations = 0;
};

void chunked_processor_preserves_absolute_events() {
  auto probe = std::make_shared<AbsoluteEventProbe>();
  require(!ChunkedProcessorRuntime::prepare(probe, 0), "zero-sized processor chunks were admitted");
  auto chunked = ChunkedProcessorRuntime::prepare(probe, 128);
  REQUIRE_OK(chunked, "bounded processor did not prepare");
  std::array<StereoFrame, 256> frames{};
  frames.fill({0.25F, -0.25F});
  chunked.value()->graph_activated(2);
  counted_allocations = 0;
  counting_allocations = true;
  chunked.value()->process(frames, {0, 3, 7, true});
  counting_allocations = false;
  chunked.value()->graph_deactivated(2);
  REQUIRE_EQ(counted_allocations, 0,
             "chunked processor lost an absolute event, state generation or lifecycle");
  REQUIRE_EQ(probe->calls, 2,
             "chunked processor lost an absolute event, state generation or lifecycle");
  REQUIRE_EQ(probe->positions[0], 0,
             "chunked processor lost an absolute event, state generation or lifecycle");
  REQUIRE_EQ(probe->positions[1], 128,
             "chunked processor lost an absolute event, state generation or lifecycle");
  REQUIRE_EQ(probe->generations[0], 7,
             "chunked processor lost an absolute event, state generation or lifecycle");
  REQUIRE_EQ(probe->generations[1], 7,
             "chunked processor lost an absolute event, state generation or lifecycle");
  REQUIRE_TRUE(probe->wraps[0],
               "chunked processor lost an absolute event, state generation or lifecycle");
  REQUIRE_TRUE(!probe->wraps[1],
               "chunked processor lost an absolute event, state generation or lifecycle");
  REQUIRE_EQ(frames[128].left, 0.25F,
             "chunked processor lost an absolute event, state generation or lifecycle");
  REQUIRE_EQ(frames[129].left, 0.5F,
             "chunked processor lost an absolute event, state generation or lifecycle");
  REQUIRE_EQ(probe->activations, 1,
             "chunked processor lost an absolute event, state generation or lifecycle");
  REQUIRE_EQ(probe->deactivations, 1,
             "chunked processor lost an absolute event, state generation or lifecycle");
}

void source_overlay_keeps_old_audio_until_candidate_ready(std::size_t frames) {
  auto master = std::make_shared<HookedDouble>();
  std::array<RoutedNode, 2> nodes{{{{1, 0}, {}}, {{3, 0}, master}}};
  const std::array<RoutingEdge, 1> edges{{{11, 1, 3}}};
  auto budget = limits(2, 1, 0);
  budget.max_frames = frames;
  budget.max_buffer_bytes = 3 * frames * sizeof(StereoFrame);
  RoutedMixerPublisher publisher;
  REQUIRE_OK(publisher.prepare_and_publish(1, nodes, edges, budget),
             "source overlay fixture failed to prepare");
  std::array<StereoFrame, 128> old_source{}, new_source{}, output{};
  std::array<std::span<const StereoFrame>, 2> selected{};
  const std::array<std::span<const StereoFrame>, 0> empty{};
  struct Probe {
    std::array<StereoFrame, 128>* old_source;
    std::array<StereoFrame, 128>* new_source;
    std::array<std::span<const StereoFrame>, 2>* selected;
    std::size_t old_frames = 0;
    std::size_t new_frames = 0;
    bool candidate_ready = false;
  } probe{&old_source, &new_source, &selected};
  const RoutedSourceFill fill{
      &probe, nullptr,
      [](void* opaque, std::uint64_t revision, BlockContext, std::size_t count,
         std::span<const std::span<const StereoFrame>>& spans) noexcept {
        auto& state = *static_cast<Probe*>(opaque);
        if (revision >= 2 && !state.candidate_ready)
          return false;
        auto& source = revision == 1 ? *state.old_source : *state.new_source;
        std::ranges::fill(std::span(source).first(count),
                          revision == 1 ? StereoFrame{0.25F, 0.0F} : StereoFrame{0.4F, 0.0F});
        (revision == 1 ? state.old_frames : state.new_frames) += count;
        (*state.selected)[0] = std::span(source).first(count);
        (*state.selected)[1] = {};
        spans = *state.selected;
        return true;
      }};
  auto block = std::span(output).first(frames);
  REQUIRE_EQ(publisher.render_block(empty, block, {0, 1}, fill), RoutedRenderStatus::rendered,
             "source overlay initial audio was not audible");
  REQUIRE_EQ(output[0].left, 0.5F, "source overlay initial audio was not audible");
  REQUIRE_EQ(probe.old_frames, frames, "source overlay initial audio was not audible");
  REQUIRE_EQ(master->activations, 1, "source overlay initial audio was not audible");
  REQUIRE_OK(publisher.prepare_source_overlay(2),
             "source overlay did not reserve one publication slot");
  REQUIRE_TRUE(!publisher.prepare_source_overlay(3),
               "source overlay did not reserve one publication slot");
  counted_allocations = 0;
  counting_allocations = true;
  const auto fallback = publisher.render_block(empty, block, {frames, 1}, fill);
  counting_allocations = false;
  REQUIRE_EQ(fallback, RoutedRenderStatus::rendered,
             "unready source silenced output, consumed candidate or displaced old graph");
  REQUIRE_EQ(counted_allocations, 0,
             "unready source silenced output, consumed candidate or displaced old graph");
  REQUIRE_EQ(publisher.active_revision(), 1,
             "unready source silenced output, consumed candidate or displaced old graph");
  REQUIRE_TRUE(publisher.has_pending(),
               "unready source silenced output, consumed candidate or displaced old graph");
  REQUIRE_EQ(output[0].left, 0.5F,
             "unready source silenced output, consumed candidate or displaced old graph");
  REQUIRE_EQ(probe.old_frames, 2 * frames,
             "unready source silenced output, consumed candidate or displaced old graph");
  REQUIRE_EQ(probe.new_frames, 0,
             "unready source silenced output, consumed candidate or displaced old graph");
  REQUIRE_EQ(master->blocks, 2,
             "unready source silenced output, consumed candidate or displaced old graph");
  probe.candidate_ready = true;
  counted_allocations = 0;
  counting_allocations = true;
  const auto changed = publisher.render_block(empty, block, {2 * frames, 1}, fill);
  counting_allocations = false;
  REQUIRE_EQ(changed, RoutedRenderStatus::rendered,
             "ready source did not switch without resetting mixer state or losing audio");
  REQUIRE_EQ(counted_allocations, 0,
             "ready source did not switch without resetting mixer state or losing audio");
  REQUIRE_EQ(publisher.active_revision(), 2,
             "ready source did not switch without resetting mixer state or losing audio");
  REQUIRE_TRUE(!publisher.has_pending(),
               "ready source did not switch without resetting mixer state or losing audio");
  REQUIRE_EQ(output[0].left, 0.8F,
             "ready source did not switch without resetting mixer state or losing audio");
  REQUIRE_EQ(probe.old_frames, 2 * frames,
             "ready source did not switch without resetting mixer state or losing audio");
  REQUIRE_EQ(probe.new_frames, frames,
             "ready source did not switch without resetting mixer state or losing audio");
  REQUIRE_EQ(master->blocks, 3,
             "ready source did not switch without resetting mixer state or losing audio");
  REQUIRE_EQ(master->activations, 1,
             "ready source did not switch without resetting mixer state or losing audio");
  REQUIRE_EQ(master->deactivations, 0,
             "ready source did not switch without resetting mixer state or losing audio");
  REQUIRE_EQ(publisher.collect_retired(), 1,
             "ready source did not switch without resetting mixer state or losing audio");
}

void control_overlay_reuses_route_without_silent_blocks(std::size_t frames) {
  const std::array<RoutedNode, 3> nodes{{{{1, 0}, {}}, {{2, 0}, {}}, {{3, 0}, {}}}};
  const std::array<RoutingEdge, 2> edges{{{11, 1, 3}, {12, 2, 3}}};
  auto budget = limits(3, 2, 0);
  budget.max_frames = frames;
  budget.max_buffer_bytes = 4 * frames * sizeof(StereoFrame);
  RoutedMixerPublisher publisher;
  REQUIRE_OK(publisher.prepare_and_publish(1, nodes, edges, budget),
             "control overlay fixture did not prepare");
  std::array<StereoFrame, 128> first{}, second{}, output{};
  std::ranges::fill(first, StereoFrame{1.0F, 1.0F});
  std::ranges::fill(second, StereoFrame{0.5F, 0.5F});
  std::array<std::span<const StereoFrame>, 3> sources{
      std::span(first).first(frames), std::span(second).first(frames), {}};
  const std::array<std::span<const StereoFrame>, 0> empty{};
  std::uint64_t filled_revision = 0;
  struct SourceFixture {
    std::array<std::span<const StereoFrame>, 3>* sources;
    std::uint64_t* revision;
  } source_fixture{&sources, &filled_revision};
  const RoutedSourceFill fill{&source_fixture, nullptr,
                              [](void* opaque, std::uint64_t revision, BlockContext, std::size_t,
                                 std::span<const std::span<const StereoFrame>>& selected) noexcept {
                                auto& fixture = *static_cast<SourceFixture*>(opaque);
                                *fixture.revision = revision;
                                selected = *fixture.sources;
                                return revision == 1;
                              }};
  auto block = std::span(output).first(frames);
  REQUIRE_EQ(publisher.render_block(empty, block, {0, 1}, fill), RoutedRenderStatus::rendered,
             "control overlay baseline did not render the two routes");
  REQUIRE_EQ(output[0].left, 1.5F, "control overlay baseline did not render the two routes");
  REQUIRE_EQ(filled_revision, 1, "control overlay baseline did not render the two routes");
  std::array<RoutedNodeControl, 3> controls{
      {{1, true, {1.0F, 1.0F}}, {2, false, {1.0F, 1.0F}}, {3, false, {1.0F, 1.0F}}}};
  REQUIRE_TRUE(!publisher.prepare_control_overlay(2, std::span(controls).first(2)),
               "control overlay admitted an incomplete mask");
  REQUIRE_OK(publisher.prepare_control_overlay(2, controls),
             "control overlay admitted an incomplete mask");
  REQUIRE_TRUE(publisher.has_pending(), "control overlay admitted an incomplete mask");
  counted_allocations = 0;
  counting_allocations = true;
  const auto muted = publisher.render_block(empty, block, {frames, 1}, fill);
  counting_allocations = false;
  REQUIRE_EQ(muted, RoutedRenderStatus::rendered,
             "mute overlay silenced a callback, lost the source cursor or failed its ramp");
  REQUIRE_EQ(counted_allocations, 0,
             "mute overlay silenced a callback, lost the source cursor or failed its ramp");
  REQUIRE_EQ(publisher.active_revision(), 2,
             "mute overlay silenced a callback, lost the source cursor or failed its ramp");
  REQUIRE_EQ(filled_revision, 1,
             "mute overlay silenced a callback, lost the source cursor or failed its ramp");
  REQUIRE_GT(output[0].left, 1.4F,
             "mute overlay silenced a callback, lost the source cursor or failed its ramp");
  REQUIRE_EQ(output[frames - 1].left, 0.5F,
             "mute overlay silenced a callback, lost the source cursor or failed its ramp");
  REQUIRE_EQ(publisher.collect_retired(), 1,
             "mute overlay did not retain the other source and route histories");
  REQUIRE_EQ(publisher.render_block(empty, block, {2 * frames, 1}, fill),
             RoutedRenderStatus::rendered,
             "mute overlay did not retain the other source and route histories");
  REQUIRE_EQ(output[0].left, 0.5F,
             "mute overlay did not retain the other source and route histories");
  controls[0] = {1, false, {0.25F, 0.5F}};
  REQUIRE_OK(publisher.prepare_control_overlay(3, controls),
             "live unmute/level/pan overlay was refused");
  counted_allocations = 0;
  counting_allocations = true;
  const auto unmuted = publisher.render_block(empty, block, {3 * frames, 1}, fill);
  counting_allocations = false;
  REQUIRE_EQ(unmuted, RoutedRenderStatus::rendered,
             "unmute/level/pan overlay did not ramp to the exact stereo gain");
  REQUIRE_EQ(counted_allocations, 0,
             "unmute/level/pan overlay did not ramp to the exact stereo gain");
  REQUIRE_EQ(publisher.active_revision(), 3,
             "unmute/level/pan overlay did not ramp to the exact stereo gain");
  REQUIRE_EQ(filled_revision, 1, "unmute/level/pan overlay did not ramp to the exact stereo gain");
  REQUIRE_GT(output[0].left, 0.5F,
             "unmute/level/pan overlay did not ramp to the exact stereo gain");
  REQUIRE_LT(output[0].left, 0.75F,
             "unmute/level/pan overlay did not ramp to the exact stereo gain");
  REQUIRE_EQ(output[frames - 1].left, 0.75F,
             "unmute/level/pan overlay did not ramp to the exact stereo gain");
  REQUIRE_EQ(output[frames - 1].right, 1.0F,
             "unmute/level/pan overlay did not ramp to the exact stereo gain");
}

void control_overlay_can_publish_an_unchanged_projection(std::size_t frames) {
  // Soloing the sole source changes the saved revision without changing its
  // effective node gain. It still needs an audible revision acknowledgement.
  const std::array<RoutedNode, 2> nodes{{{{1, 0}, {}}, {{3, 0}, {}}}};
  const std::array<RoutingEdge, 1> edges{{{11, 1, 3}}};
  auto budget = limits(2, 1, 0);
  budget.max_frames = frames;
  budget.max_buffer_bytes = 3 * frames * sizeof(StereoFrame);
  RoutedMixerPublisher publisher;
  REQUIRE_OK(publisher.prepare_and_publish(1, nodes, edges, budget),
             "unchanged control projection failed to prepare");
  std::array<StereoFrame, 128> source{}, output{};
  std::ranges::fill(source, StereoFrame{0.25F, -0.5F});
  const std::array<std::span<const StereoFrame>, 2> inputs{std::span(source).first(frames), {}};
  auto block = std::span(output).first(frames);
  REQUIRE_EQ(publisher.render_block(inputs, block, {0, 1}), RoutedRenderStatus::rendered,
             "unchanged control projection baseline did not render");
  const std::array<RoutedNodeControl, 2> controls{
      {{1, false, {1.0F, 1.0F}}, {3, false, {1.0F, 1.0F}}}};
  REQUIRE_OK(publisher.prepare_control_overlay(2, controls),
             "unchanged control projection did not publish as an overlay");
  counted_allocations = 0;
  counting_allocations = true;
  const auto changed = publisher.render_block(inputs, block, {frames, 1});
  counting_allocations = false;
  REQUIRE_EQ(changed, RoutedRenderStatus::rendered,
             "revision-only control overlay inserted silence or changed audio");
  REQUIRE_EQ(counted_allocations, 0,
             "revision-only control overlay inserted silence or changed audio");
  REQUIRE_EQ(publisher.active_revision(), 2,
             "revision-only control overlay inserted silence or changed audio");
  REQUIRE_EQ(output.front().left, 0.25F,
             "revision-only control overlay inserted silence or changed audio");
  REQUIRE_EQ(output.front().right, -0.5F,
             "revision-only control overlay inserted silence or changed audio");
  REQUIRE_EQ(publisher.collect_retired(), 1,
             "revision-only control overlay inserted silence or changed audio");
}

void control_overlay_rejects_processor_and_unknown_automation() {
  const std::array<RoutedNode, 2> processed{{{{1, 0}, std::make_shared<Tail>()}, {{3, 0}, {}}}};
  const std::array<RoutingEdge, 1> edges{{{11, 1, 3}}};
  const std::array<RoutedNodeControl, 2> changed{
      {{1, true, {1.0F, 1.0F}}, {3, false, {1.0F, 1.0F}}}};
  std::array<StereoFrame, 64> source{}, output{};
  const std::array<std::span<const StereoFrame>, 2> sources{source, {}};
  RoutedMixerPublisher with_processor;
  const auto prepared = with_processor.prepare_and_publish(1, processed, edges, limits(2, 1, 0));
  const auto rendered = with_processor.render_block(sources, output, {0, 1});
  const auto attempted = with_processor.prepare_control_overlay(2, changed);
  REQUIRE_TRUE(prepared, "processor route accepted a graph-revision-sensitive control overlay");
  REQUIRE_EQ(rendered, RoutedRenderStatus::rendered,
             "processor route accepted a graph-revision-sensitive control overlay");
  REQUIRE_TRUE(!attempted, "processor route accepted a graph-revision-sensitive control overlay");
  REQUIRE_TRUE(!with_processor.has_pending(),
               "processor route accepted a graph-revision-sensitive control overlay");
  REQUIRE_EQ(with_processor.active_revision(), 1,
             "processor route accepted a graph-revision-sensitive control overlay");

  class UnknownGain final : public RoutedGainSource {
  public:
    bool fill_block(BlockContext, std::size_t, RoutedGainSpans& gains) noexcept override {
      gains = {};
      return true;
    }
  };
  const std::array<RoutedNode, 2> dry{{{{1, 0}, {}}, {{3, 0}, {}}}};
  RoutedMixerPublisher with_unknown_gain;
  REQUIRE_TRUE(with_unknown_gain.prepare_and_publish(1, dry, edges, limits(2, 1, 0),
                                                     std::make_shared<UnknownGain>()),
               "unknown automation source accepted a conflicting control overlay");
  REQUIRE_EQ(with_unknown_gain.render_block(sources, output, {0, 1}), RoutedRenderStatus::rendered,
             "unknown automation source accepted a conflicting control overlay");
  REQUIRE_TRUE(!with_unknown_gain.prepare_control_overlay(2, changed),
               "unknown automation source accepted a conflicting control overlay");
  REQUIRE_TRUE(!with_unknown_gain.has_pending(),
               "unknown automation source accepted a conflicting control overlay");
  REQUIRE_EQ(with_unknown_gain.active_revision(), 1,
             "unknown automation source accepted a conflicting control overlay");
}

void control_overlay_supersedes_an_inflight_gain_ramp() {
  const std::array<RoutedNode, 2> nodes{{{{1, 0}, {}}, {{3, 0}, {}}}};
  const std::array<RoutingEdge, 1> edges{{{11, 1, 3}}};
  auto budget = limits(2, 1, 0);
  budget.gain_ramp_frames = 128;
  RoutedMixerPublisher publisher;
  REQUIRE_OK(publisher.prepare_and_publish(1, nodes, edges, budget),
             "ramp supersession fixture failed to prepare");
  std::array<StereoFrame, 64> source{}, output{};
  std::ranges::fill(source, StereoFrame{1.0F, 1.0F});
  const std::array<std::span<const StereoFrame>, 2> inputs{source, {}};
  REQUIRE_EQ(publisher.render_block(inputs, output, {0, 1}), RoutedRenderStatus::rendered,
             "ramp supersession baseline failed");
  std::array<RoutedNodeControl, 2> controls{{{1, false, {0.0F, 0.0F}}, {3, false, {1.0F, 1.0F}}}};
  REQUIRE_TRUE(publisher.prepare_control_overlay(2, controls),
               "first control ramp did not reach its exact midpoint");
  REQUIRE_EQ(publisher.render_block(inputs, output, {64, 1}), RoutedRenderStatus::rendered,
             "first control ramp did not reach its exact midpoint");
  REQUIRE_EQ(output.back().left, 0.5F, "first control ramp did not reach its exact midpoint");
  REQUIRE_EQ(publisher.collect_retired(), 1, "first control ramp did not reach its exact midpoint");
  controls[0].output_gain = {1.0F, 1.0F};
  REQUIRE_OK(publisher.prepare_control_overlay(3, controls),
             "a settled publisher refused a newer control target");
  counted_allocations = 0;
  counting_allocations = true;
  const auto resumed = publisher.render_block(inputs, output, {128, 1});
  counting_allocations = false;
  REQUIRE_EQ(resumed, RoutedRenderStatus::rendered,
             "new control target restarted from a stale gain or lost a callback block");
  REQUIRE_EQ(counted_allocations, 0,
             "new control target restarted from a stale gain or lost a callback block");
  REQUIRE_EQ(publisher.active_revision(), 3,
             "new control target restarted from a stale gain or lost a callback block");
  REQUIRE_GT(output.front().left, 0.5F,
             "new control target restarted from a stale gain or lost a callback block");
  REQUIRE_LT(output.front().left, 0.51F,
             "new control target restarted from a stale gain or lost a callback block");
  REQUIRE_EQ(output.back().left, 0.75F,
             "new control target restarted from a stale gain or lost a callback block");
}

void control_overlay_keeps_qualified_processor_history(std::size_t frames) {
  class RevisionIndependentDelay final : public ProcessorRuntime {
  public:
    void process(std::span<StereoFrame> block, BlockContext) noexcept override {
      ++blocks;
      for (auto& frame : block)
        std::swap(frame, previous);
    }
    void graph_activated(std::uint64_t) noexcept override { ++activations; }
    void graph_deactivated(std::uint64_t) noexcept override { ++deactivations; }
    [[nodiscard]] bool control_overlay_revision_independent() const noexcept override {
      return true;
    }
    StereoFrame previous{};
    std::size_t blocks = 0;
    std::size_t activations = 0;
    std::size_t deactivations = 0;
  };
  auto processor = std::make_shared<RevisionIndependentDelay>();
  const std::array<RoutedNode, 2> nodes{{{{1, 0}, processor}, {{3, 0}, {}}}};
  const std::array<RoutingEdge, 1> edges{{{11, 1, 3}}};
  auto budget = limits(2, 1, 0);
  budget.max_frames = frames;
  budget.max_buffer_bytes = 3 * frames * sizeof(StereoFrame);
  RoutedMixerPublisher publisher;
  REQUIRE_OK(publisher.prepare_and_publish(1, nodes, edges, budget),
             "qualified processor overlay fixture did not prepare");
  std::array<StereoFrame, 128> source{}, output{};
  std::ranges::fill(source, StereoFrame{1.0F, 1.0F});
  const std::array<std::span<const StereoFrame>, 2> inputs{std::span(source).first(frames), {}};
  auto block = std::span(output).first(frames);
  REQUIRE_EQ(publisher.render_block(inputs, block, {0, 1}), RoutedRenderStatus::rendered,
             "qualified processor baseline did not hold its one-frame history");
  REQUIRE_EQ(output[0].left, 0.0F,
             "qualified processor baseline did not hold its one-frame history");
  REQUIRE_EQ(output[frames - 1].left, 1.0F,
             "qualified processor baseline did not hold its one-frame history");
  const std::array<RoutedNodeControl, 2> controls{
      {{1, false, {0.5F, 0.5F}}, {3, false, {1.0F, 1.0F}}}};
  REQUIRE_OK(publisher.prepare_control_overlay(2, controls),
             "revision-independent stateful processor was refused");
  counted_allocations = 0;
  counting_allocations = true;
  const auto changed = publisher.render_block(inputs, block, {frames, 1});
  counting_allocations = false;
  REQUIRE_EQ(changed, RoutedRenderStatus::rendered,
             "control overlay reset qualified processor history or inserted silence");
  REQUIRE_EQ(counted_allocations, 0,
             "control overlay reset qualified processor history or inserted silence");
  REQUIRE_EQ(publisher.active_revision(), 2,
             "control overlay reset qualified processor history or inserted silence");
  REQUIRE_GT(output[0].left, 0.9F,
             "control overlay reset qualified processor history or inserted silence");
  REQUIRE_EQ(output[frames - 1].left, 0.5F,
             "control overlay reset qualified processor history or inserted silence");
  REQUIRE_EQ(processor->blocks, 2,
             "control overlay reset qualified processor history or inserted silence");
  REQUIRE_EQ(processor->activations, 1,
             "control overlay reset qualified processor history or inserted silence");
  REQUIRE_EQ(processor->deactivations, 0,
             "control overlay reset qualified processor history or inserted silence");
}

void edge_overlay_ramps_send_without_losing_delay_history(std::size_t frames) {
  // The direct path is delayed by the eight-frame return processor latency.
  // A changing send gain may alter amplitude, but must retain that alignment.
  const std::array<RoutedNode, 3> nodes{{{{1, 0}, {}}, {{2, 8}, {}}, {{3, 0}, {}}}};
  const std::array<RoutingEdge, 3> edges{
      {{11, 1, 3}, {12, 1, 2, 1.0F, RoutingEdgeKind::main}, {13, 2, 3}}};
  auto budget = limits(3, 3);
  budget.max_frames = frames;
  budget.max_buffer_bytes = 4 * frames * sizeof(StereoFrame);
  RoutedMixerPublisher publisher;
  REQUIRE_OK(publisher.prepare_and_publish(1, nodes, edges, budget),
             "send overlay baseline did not prepare");
  std::array<StereoFrame, 128> source{}, output{};
  std::ranges::fill(source, StereoFrame{1.0F, 1.0F});
  const std::array<std::span<const StereoFrame>, 3> inputs{std::span(source).first(frames), {}, {}};
  auto block = std::span(output).first(frames);
  REQUIRE_EQ(publisher.render_block(inputs, block, {0, 1}), RoutedRenderStatus::rendered,
             "send overlay baseline did not align direct and return paths");
  REQUIRE_EQ(block.back().left, 2.0F,
             "send overlay baseline did not align direct and return paths");
  const std::array<RoutedEdgeControl, 3> changed{{{11, 1.0F}, {12, 0.5F}, {13, 1.0F}}};
  REQUIRE_TRUE(!publisher.prepare_edge_overlay(2, std::span(changed).first(2)),
               "send overlay accepted an incomplete mask");
  REQUIRE_TRUE(publisher.prepare_edge_overlay(2, changed),
               "send overlay accepted an incomplete mask");
  counted_allocations = 0;
  counting_allocations = true;
  const auto status = publisher.render_block(inputs, block, {frames, 1});
  counting_allocations = false;
  REQUIRE_EQ(status, RoutedRenderStatus::rendered,
             "send overlay inserted silence, reset delay history or missed gain ramp");
  REQUIRE_EQ(counted_allocations, 0,
             "send overlay inserted silence, reset delay history or missed gain ramp");
  REQUIRE_EQ(publisher.active_revision(), 2,
             "send overlay inserted silence, reset delay history or missed gain ramp");
  REQUIRE_GT(output.front().left, 1.9F,
             "send overlay inserted silence, reset delay history or missed gain ramp");
  REQUIRE_EQ(block.back().left, 1.5F,
             "send overlay inserted silence, reset delay history or missed gain ramp");
  REQUIRE_EQ(publisher.collect_retired(), 1,
             "send overlay inserted silence, reset delay history or missed gain ramp");
  REQUIRE_EQ(publisher.render_block(inputs, block, {2 * frames, 1}), RoutedRenderStatus::rendered,
             "send overlay lost steady aligned output after the ramp");
  REQUIRE_EQ(output.front().left, 1.5F, "send overlay lost steady aligned output after the ramp");
}

void edge_overlay_rejects_changed_identity_and_unknown_automation() {
  class UnknownGain final : public RoutedGainSource {
  public:
    bool fill_block(BlockContext, std::size_t, RoutedGainSpans& gains) noexcept override {
      gains = {};
      return true;
    }
  };
  const std::array<RoutedNode, 2> nodes{{{{1, 0}, {}}, {{3, 0}, {}}}};
  const std::array<RoutingEdge, 1> edges{{{11, 1, 3}}};
  RoutedMixerPublisher publisher;
  REQUIRE_TRUE(
      publisher.prepare_and_publish(1, nodes, edges, limits(2, 1), std::make_shared<UnknownGain>()),
      "automated edge fixture did not prepare");
  REQUIRE_EQ(publisher.active_revision(), 0, "automated edge fixture did not prepare");
  std::array<StereoFrame, 64> input{}, output{};
  const std::array<std::span<const StereoFrame>, 2> sources{input, {}};
  REQUIRE_EQ(publisher.render_block(sources, output, {0, 1}), RoutedRenderStatus::rendered,
             "automated edge fixture did not activate");
  const std::array<RoutedEdgeControl, 1> wrong_id{{{12, 0.5F}}};
  const std::array<RoutedEdgeControl, 1> automated{{{11, 0.5F}}};
  REQUIRE_TRUE(!publisher.prepare_edge_overlay(2, wrong_id),
               "edge overlay admitted a changed edge identity or unknown automation binding");
  REQUIRE_TRUE(!publisher.prepare_edge_overlay(2, automated),
               "edge overlay admitted a changed edge identity or unknown automation binding");
  REQUIRE_TRUE(!publisher.has_pending(),
               "edge overlay admitted a changed edge identity or unknown automation binding");
  REQUIRE_EQ(publisher.active_revision(), 1,
             "edge overlay admitted a changed edge identity or unknown automation binding");
}

class TailWrapper final : public ProcessorRuntime {
public:
  TailWrapper(std::shared_ptr<EightFrameTail> tail, std::size_t& calls)
      : tail_(std::move(tail)), calls_(calls) {}

  void process(std::span<StereoFrame> frames, BlockContext context) noexcept override {
    ++calls_;
    tail_->process(frames, context);
  }

private:
  std::shared_ptr<EightFrameTail> tail_;
  std::size_t& calls_;
};

void replacement_drains_only_selected_predecessor_stage() {
  auto tail = std::make_shared<EightFrameTail>();
  std::size_t wrapper_calls = 0;
  auto predecessor = std::make_shared<TailWrapper>(tail, wrapper_calls);
  std::array<RoutedNode, 1> nodes{{{{3, 0}, predecessor}}};
  RoutedMixerPublisher publisher;
  REQUIRE_OK(publisher.prepare_and_publish(1, nodes, {}, limits(1, 0, 0)),
             "selected-stage tail fixture did not prepare");
  std::array<StereoFrame, 4> source{}, output{};
  source[3] = {0.5F, -0.25F};
  const std::array<std::span<const StereoFrame>, 1> inputs{source};
  REQUIRE_EQ(publisher.render_block(inputs, output, {0, 1}), RoutedRenderStatus::rendered,
             "selected-stage predecessor did not retain the impulse");
  REQUIRE_EQ(wrapper_calls, 1, "selected-stage predecessor did not retain the impulse");
  REQUIRE_EQ(output[3].left, 0.0F, "selected-stage predecessor did not retain the impulse");
  nodes[0].processor = std::make_shared<Double>();
  nodes[0].replacement = RoutedTailReplacement{predecessor, 8, tail};
  REQUIRE_OK(publisher.prepare_and_publish_live(2, nodes, {}, limits(1, 0, 0)),
             "selected-stage replacement did not prepare");
  source.fill({});
  counted_allocations = 0;
  counting_allocations = true;
  const auto first = publisher.render_block(inputs, output, {4, 1});
  const auto second = publisher.render_block(inputs, output, {8, 1});
  counting_allocations = false;
  REQUIRE_EQ(first, RoutedRenderStatus::rendered,
             "selected tail stage did not drain or reran the predecessor wrapper");
  REQUIRE_EQ(second, RoutedRenderStatus::rendered,
             "selected tail stage did not drain or reran the predecessor wrapper");
  REQUIRE_EQ(counted_allocations, 0,
             "selected tail stage did not drain or reran the predecessor wrapper");
  REQUIRE_EQ(wrapper_calls, 1,
             "selected tail stage did not drain or reran the predecessor wrapper");
  REQUIRE_EQ(output[3].left, 0.5F,
             "selected tail stage did not drain or reran the predecessor wrapper");
  REQUIRE_EQ(output[3].right, -0.25F,
             "selected tail stage did not drain or reran the predecessor wrapper");
}

void bounded_replacement_drains_across_routing_revisions() {
  auto predecessor = std::make_shared<EightFrameTail>();
  std::weak_ptr<ProcessorRuntime> predecessor_lifetime = predecessor;
  std::array<RoutedNode, 1> nodes{{{{3, 0}, predecessor}}};
  RoutedMixerPublisher publisher;
  nodes[0].replacement = RoutedTailReplacement{predecessor, 8};
  require(!publisher.prepare_and_publish(1, nodes, {}, limits(1, 0, 0)),
          "initial graph accepted a predecessor that was never published");
  nodes[0].replacement.reset();
  REQUIRE_OK(publisher.prepare_and_publish(1, nodes, {}, limits(1, 0, 0)),
             "tail replacement fixture did not prepare");
  std::array<StereoFrame, 4> source{};
  std::array<StereoFrame, 4> output{};
  const std::array<std::span<const StereoFrame>, 1> input{source};
  source[3] = {1.0F, -0.5F};
  REQUIRE_EQ(publisher.render_block(input, output, {0, 1}), RoutedRenderStatus::rendered,
             "predecessor did not retain the independent impulse");
  REQUIRE_EQ(output[3].left, 0.0F, "predecessor did not retain the independent impulse");

  auto successor = std::make_shared<Double>();
  nodes[0].replacement = RoutedTailReplacement{predecessor, 8};
  require(!publisher.prepare_and_publish_live(2, nodes, {}, limits(1, 0, 0)),
          "unchanged owner accepted a replacement declaration");
  nodes[0].processor = successor;
  nodes[0].replacement = RoutedTailReplacement{predecessor, 8, successor};
  require(!publisher.prepare_and_publish_live(2, nodes, {}, limits(1, 0, 0)),
          "active successor was accepted as its own draining stage");
  nodes[0].replacement = RoutedTailReplacement{std::make_shared<Double>(), 8};
  REQUIRE_TRUE(!publisher.prepare_and_publish_live(2, nodes, {}, limits(1, 0, 0)),
               "unrelated predecessor owner was accepted");
  REQUIRE_EQ(publisher.active_revision(), 1, "unrelated predecessor owner was accepted");
  nodes[0].replacement = RoutedTailReplacement{predecessor, 0};
  require(!publisher.prepare_and_publish_live(2, nodes, {}, limits(1, 0, 0)),
          "zero-length drain was accepted");
  nodes[0].replacement = RoutedTailReplacement{predecessor, routed_tail_frame_cap + 1};
  require(!publisher.prepare_and_publish_live(2, nodes, {}, limits(1, 0, 0)),
          "unbounded drain was accepted");
  nodes[0].replacement = RoutedTailReplacement{predecessor, 8};
  REQUIRE_OK(publisher.prepare_and_publish_live(2, nodes, {}, limits(1, 0, 0)),
             "bounded replacement did not prepare after failed attempts");
  nodes[0].replacement.reset();
  predecessor.reset();
  source.fill({0.25F, 0.25F});
  counting_allocations = true;
  const auto switched = publisher.render_block(input, output, {4, 1});
  counting_allocations = false;
  REQUIRE_EQ(switched, RoutedRenderStatus::rendered,
             "new live input or pre-tail output changed at publication");
  REQUIRE_EQ(counted_allocations, 0, "new live input or pre-tail output changed at publication");
  REQUIRE_EQ(publisher.active_revision(), 2,
             "new live input or pre-tail output changed at publication");
  REQUIRE_EQ(output[0].left, 0.5F, "new live input or pre-tail output changed at publication");
  REQUIRE_EQ(output[3].left, 0.5F, "new live input or pre-tail output changed at publication");
  REQUIRE_EQ(publisher.collect_retired(), 1,
             "predecessor tail owner was reclaimed before its declared drain");
  REQUIRE_TRUE(!predecessor_lifetime.expired(),
               "predecessor tail owner was reclaimed before its declared drain");

  nodes[0].processor = successor;
  nodes[0].replacement.reset();
  REQUIRE_OK(publisher.prepare_and_publish_live(3, nodes, {}, limits(1, 0, 0)),
             "routing-only edit lost active tail handoff");
  counting_allocations = true;
  const auto continued = publisher.render_block(input, output, {8, 1});
  counting_allocations = false;
  REQUIRE_EQ(continued, RoutedRenderStatus::rendered,
             "old zero-input impulse tail did not continue at its exact frame after routing edit");
  REQUIRE_EQ(counted_allocations, 0,
             "old zero-input impulse tail did not continue at its exact frame after routing edit");
  REQUIRE_EQ(output[0].left, 0.5F,
             "old zero-input impulse tail did not continue at its exact frame after routing edit");
  REQUIRE_EQ(output[2].left, 0.5F,
             "old zero-input impulse tail did not continue at its exact frame after routing edit");
  REQUIRE_EQ(output[3].left, 1.5F,
             "old zero-input impulse tail did not continue at its exact frame after routing edit");
  REQUIRE_EQ(output[3].right, 0.0F,
             "old zero-input impulse tail did not continue at its exact frame after routing edit");
  REQUIRE_EQ(
      publisher.collect_retired(), 1,
      "drained owner was destroyed by the prior plan while current plan still referred to it");
  REQUIRE_TRUE(
      !predecessor_lifetime.expired(),
      "drained owner was destroyed by the prior plan while current plan still referred to it");

  REQUIRE_OK(publisher.prepare_and_publish_live(4, nodes, {}, limits(1, 0, 0)),
             "completed predecessor tail was not retired off the callback");
  REQUIRE_EQ(publisher.render_block(input, output, {12, 1}), RoutedRenderStatus::rendered,
             "completed predecessor tail was not retired off the callback");
  REQUIRE_EQ(publisher.collect_retired(), 1,
             "completed predecessor tail was not retired off the callback");
  REQUIRE_TRUE(predecessor_lifetime.expired(),
               "completed predecessor tail was not retired off the callback");

  nodes[0].processor = std::make_shared<Double>();
  nodes[0].replacement = RoutedTailReplacement{successor, 8};
  REQUIRE_OK(publisher.prepare_and_publish_live(5, nodes, {}, limits(1, 0, 0)),
             "completed drain blocked the next replacement");
  REQUIRE_EQ(publisher.render_block(input, output, {16, 1}), RoutedRenderStatus::rendered,
             "completed drain blocked the next replacement");
  source.fill({});
  REQUIRE_EQ(publisher.render_block(input, output, {0, 2}), RoutedRenderStatus::rendered,
             "seek carried a predecessor tail into a new transport generation");
  REQUIRE_EQ(output[0].left, 0.0F,
             "seek carried a predecessor tail into a new transport generation");
  REQUIRE_EQ(output[3].right, 0.0F,
             "seek carried a predecessor tail into a new transport generation");
}

void tail_reuse_and_changed_owner_rejection() {
  auto tail = std::make_shared<Tail>();
  std::array<RoutedNode, 1> nodes{{{{3, 0}, tail}}};
  RoutedMixerPublisher publisher;
  REQUIRE_OK(publisher.prepare_and_publish(1, nodes, {}, limits(1, 0, 0)),
             "tail graph was not prepared");
  std::array<StereoFrame, 4> impulse{};
  impulse[3] = {1.0F, -0.5F};
  std::array<StereoFrame, 4> output{};
  const std::array<std::span<const StereoFrame>, 1> first{impulse};
  REQUIRE_EQ(publisher.render_block(first, output, {0, 1}), RoutedRenderStatus::rendered,
             "tail fixture did not hold its impulse");
  REQUIRE_EQ(output[3].left, 0.0F, "tail fixture did not hold its impulse");
  nodes[0].processor = std::make_shared<Tail>();
  REQUIRE_TRUE(!publisher.prepare_and_publish_live(2, nodes, {}, limits(1, 0, 0)),
               "changed processor owner was silently activated");
  REQUIRE_EQ(publisher.active_revision(), 1, "changed processor owner was silently activated");
  nodes[0].processor = tail;
  REQUIRE_OK(publisher.prepare_and_publish_live(2, nodes, {}, limits(1, 0, 0)),
             "compatible tail graph did not prepare");
  const std::array<std::span<const StereoFrame>, 1> empty{{}};
  REQUIRE_EQ(publisher.render_block(empty, output, {4, 1}), RoutedRenderStatus::rendered,
             "tail history reset across graph publication");
  REQUIRE_EQ(output[3].left, 1.0F, "tail history reset across graph publication");
  REQUIRE_EQ(output[3].right, -0.5F, "tail history reset across graph publication");
  REQUIRE_EQ(publisher.collect_retired(), 1, "retired tail graph not collected");
}

void source_order_and_cursor_rejection() {
  const std::array<RoutedNode, 3> nodes{{{{1, 0}, {}}, {{2, 0}, {}}, {{3, 0}, {}}}};
  const std::array<RoutingEdge, 2> original{{{11, 1, 3}, {12, 2, 3}}};
  const std::array<RoutingEdge, 2> reordered{{{11, 2, 1}, {12, 1, 3}}};
  RoutedMixerPublisher publisher;
  REQUIRE_OK(publisher.prepare_and_publish(1, nodes, original, limits(3, 2, 0)),
             "order fixture did not prepare");
  std::array<StereoFrame, 4> output{};
  const std::array<std::span<const StereoFrame>, 3> silence{{{}, {}, {}}};
  REQUIRE_EQ(publisher.render_block(silence, output, {0, 1}), RoutedRenderStatus::rendered,
             "order fixture did not render");
  REQUIRE_TRUE(!publisher.prepare_and_publish_live(2, nodes, reordered, limits(3, 2, 0)),
               "new source order was published without named inputs");
  REQUIRE_EQ(publisher.active_revision(), 1, "new source order was published without named inputs");
  REQUIRE_TRUE(!publisher.has_pending(), "new source order was published without named inputs");
  REQUIRE_EQ(publisher.render_block(silence, output, {5, 1}), RoutedRenderStatus::invalid_block,
             "a cursor that skipped samples was accepted");
  REQUIRE_EQ(publisher.render_block(silence, output, {4, 1}), RoutedRenderStatus::rendered,
             "old graph could not resume after rejected order change");
}

void source_fill_linearizes_with_live_publication() {
  const std::array<RoutedNode, 1> nodes{{{{3, 0}, {}}}};
  RoutedMixerPublisher publisher;
  REQUIRE_OK(publisher.prepare_and_publish(1, nodes, {}, limits(1, 0, 0)),
             "source-fill initial graph did not prepare");
  std::array<StereoFrame, 4> input{};
  std::array<StereoFrame, 4> output{};
  const std::array<std::span<const StereoFrame>, 1> sources{input};
  struct Probe {
    std::array<StereoFrame, 4>* input;
    unsigned calls = 0;
    unsigned consumed = 0;
    std::uint64_t last_revision = 0;
    bool fail = false;
  } probe{&input};
  const RoutedSourceFill fill{
      &probe, [](void* context, std::uint64_t revision, BlockContext, std::size_t frames) noexcept {
        auto& state = *static_cast<Probe*>(context);
        ++state.calls;
        state.last_revision = revision;
        if (state.fail)
          return false;
        for (std::size_t index = 0; index < frames; ++index)
          (*state.input)[index] = {
              (revision == 1 ? 0.25F : 0.5F) * static_cast<float>(++state.consumed), 0.0F};
        return true;
      }};
  REQUIRE_EQ(publisher.render_block(sources, output, {0, 1}, fill), RoutedRenderStatus::rendered,
             "initial graph did not fill one source block");
  REQUIRE_EQ(output[0].left, 0.25F, "initial graph did not fill one source block");
  REQUIRE_EQ(probe.calls, 1, "initial graph did not fill one source block");
  REQUIRE_EQ(probe.consumed, 4, "initial graph did not fill one source block");
  REQUIRE_EQ(probe.last_revision, 1, "initial graph did not fill one source block");
  REQUIRE_OK(publisher.prepare_and_publish_live(2, nodes, {}, limits(1, 0, 0)),
             "source-fill successor did not prepare");
  probe.fail = true;
  counting_allocations = true;
  const auto failed = publisher.render_block(sources, output, {4, 1}, fill);
  counting_allocations = false;
  REQUIRE_EQ(failed, RoutedRenderStatus::invalid_block,
             "failed fill advanced source or activated pending graph");
  REQUIRE_EQ(counted_allocations, 0, "failed fill advanced source or activated pending graph");
  REQUIRE_EQ(publisher.active_revision(), 1,
             "failed fill advanced source or activated pending graph");
  REQUIRE_TRUE(publisher.has_pending(), "failed fill advanced source or activated pending graph");
  REQUIRE_EQ(probe.consumed, 4, "failed fill advanced source or activated pending graph");
  // The candidate was tried first, then the old graph's own source.
  REQUIRE_EQ(probe.calls, 3, "failed fill advanced source or activated pending graph");
  REQUIRE_EQ(probe.last_revision, 1, "failed fill advanced source or activated pending graph");
  REQUIRE_EQ(output[0].left, 0.0F, "failed fill advanced source or activated pending graph");
  probe.fail = false;
  counting_allocations = true;
  const auto switched = publisher.render_block(sources, output, {4, 1}, fill);
  counting_allocations = false;
  REQUIRE_EQ(switched, RoutedRenderStatus::rendered,
             "pending publication did not select candidate source content at the switch");
  REQUIRE_EQ(counted_allocations, 0,
             "pending publication did not select candidate source content at the switch");
  REQUIRE_EQ(publisher.active_revision(), 2,
             "pending publication did not select candidate source content at the switch");
  REQUIRE_EQ(probe.calls, 4,
             "pending publication did not select candidate source content at the switch");
  REQUIRE_EQ(probe.consumed, 8,
             "pending publication did not select candidate source content at the switch");
  REQUIRE_EQ(probe.last_revision, 2,
             "pending publication did not select candidate source content at the switch");
  REQUIRE_EQ(output[0].left, 2.5F,
             "pending publication did not select candidate source content at the switch");
  REQUIRE_EQ(publisher.collect_retired(), 1,
             "the next block did not continue the same source position");
  REQUIRE_EQ(publisher.render_block(sources, output, {8, 1}, fill), RoutedRenderStatus::rendered,
             "the next block did not continue the same source position");
  REQUIRE_EQ(probe.consumed, 12, "the next block did not continue the same source position");
  REQUIRE_EQ(probe.last_revision, 2, "the next block did not continue the same source position");
  REQUIRE_EQ(output[0].left, 4.5F, "the next block did not continue the same source position");

  const auto calls_before_invalid = probe.calls;
  REQUIRE_EQ(publisher.render_block(sources, output, {13, 1}, fill),
             RoutedRenderStatus::invalid_block,
             "out-of-order mixer cursor consumed prefetched source frames");
  REQUIRE_EQ(probe.calls, calls_before_invalid,
             "out-of-order mixer cursor consumed prefetched source frames");
  REQUIRE_EQ(probe.consumed, 12, "out-of-order mixer cursor consumed prefetched source frames");
  REQUIRE_EQ(output[0].left, 0.0F, "out-of-order mixer cursor consumed prefetched source frames");
  const std::array<std::span<const StereoFrame>, 0> absent{};
  output.fill({7.0F, 7.0F});
  REQUIRE_EQ(publisher.render_block(absent, output, {12, 1}, fill),
             RoutedRenderStatus::invalid_block,
             "invalid active source layout consumed prefetched frames");
  REQUIRE_EQ(probe.calls, calls_before_invalid,
             "invalid active source layout consumed prefetched frames");
  REQUIRE_EQ(probe.consumed, 12, "invalid active source layout consumed prefetched frames");
  REQUIRE_EQ(output[0].left, 0.0F, "invalid active source layout consumed prefetched frames");
  std::array<StereoFrame, 5> too_wide{};
  REQUIRE_EQ(publisher.render_block(sources, too_wide, {12, 1}, fill),
             RoutedRenderStatus::invalid_block,
             "oversized active block consumed prefetched frames");
  REQUIRE_EQ(probe.calls, calls_before_invalid,
             "oversized active block consumed prefetched frames");
  REQUIRE_EQ(probe.consumed, 12, "oversized active block consumed prefetched frames");
}

void revision_selects_matching_source_bank() {
  const std::array<RoutedNode, 1> nodes{{{{3, 0}, {}}}};
  RoutedMixerPublisher publisher;
  REQUIRE_OK(publisher.prepare_and_publish(1, nodes, {}, limits(1, 0, 0)),
             "source-bank initial graph did not prepare");
  std::array<StereoFrame, 4> old_frames{}, candidate_frames{}, output{};
  old_frames.fill({0.25F, 0.25F});
  candidate_frames.fill({0.75F, 0.75F});
  const std::array<std::span<const StereoFrame>, 1> old_bank{old_frames};
  const std::array<std::span<const StereoFrame>, 1> candidate_bank{candidate_frames};
  struct Probe {
    std::span<const std::span<const StereoFrame>> old_bank;
    std::span<const std::span<const StereoFrame>> candidate_bank;
    bool candidate_ready = false;
    std::uint64_t selected_revision = 0;
  } probe{old_bank, candidate_bank};
  const RoutedSourceFill fill{&probe, nullptr,
                              [](void* context, std::uint64_t revision, BlockContext, std::size_t,
                                 std::span<const std::span<const StereoFrame>>& selected) noexcept {
                                auto& state = *static_cast<Probe*>(context);
                                state.selected_revision = revision;
                                if (revision == 2 && !state.candidate_ready)
                                  return false;
                                selected = revision == 2 ? state.candidate_bank : state.old_bank;
                                return true;
                              }};
  REQUIRE_EQ(publisher.render_block(old_bank, output, {0, 1}, fill), RoutedRenderStatus::rendered,
             "initial revision did not choose its source bank");
  REQUIRE_EQ(output[0].left, 0.25F, "initial revision did not choose its source bank");
  REQUIRE_EQ(probe.selected_revision, 1, "initial revision did not choose its source bank");
  REQUIRE_OK(publisher.prepare_and_publish_live(2, nodes, {}, limits(1, 0, 0)),
             "candidate source-bank graph did not prepare");
  // An unready candidate waits: the old graph plays its own bank meanwhile.
  REQUIRE_EQ(publisher.render_block(old_bank, output, {4, 1}, fill), RoutedRenderStatus::rendered,
             "unready candidate bank activated or silenced the old source");
  REQUIRE_EQ(publisher.active_revision(), 1,
             "unready candidate bank activated or silenced the old source");
  REQUIRE_TRUE(publisher.has_pending(),
               "unready candidate bank activated or silenced the old source");
  REQUIRE_EQ(output[0].left, 0.25F, "unready candidate bank activated or silenced the old source");
  REQUIRE_EQ(probe.selected_revision, 1,
             "unready candidate bank activated or silenced the old source");
  probe.candidate_ready = true;
  counting_allocations = true;
  const auto switched = publisher.render_block(old_bank, output, {8, 1}, fill);
  counting_allocations = false;
  REQUIRE_EQ(switched, RoutedRenderStatus::rendered,
             "candidate graph mixed stale inputs or allocated in the callback");
  REQUIRE_EQ(counted_allocations, 0,
             "candidate graph mixed stale inputs or allocated in the callback");
  REQUIRE_EQ(publisher.active_revision(), 2,
             "candidate graph mixed stale inputs or allocated in the callback");
  REQUIRE_EQ(output[0].left, 0.75F,
             "candidate graph mixed stale inputs or allocated in the callback");
  REQUIRE_EQ(probe.selected_revision, 2,
             "candidate graph mixed stale inputs or allocated in the callback");
}

// A block that switches nothing fills inside the mixer's schedule (ADR-0127):
// the source decides, then each node renders its own part. A source that
// asks for a whole fill gets fill_selected; one that refuses silences the
// block; a node that fails part way fails the block.
void staged_fill_renders_inside_the_schedule() {
  constexpr float staged_level = 0.5F;
  constexpr float whole_level = 0.25F;
  constexpr std::size_t frames = 4;
  const std::array<RoutedNode, 1> nodes{{{{3, 0}, {}}}};
  RoutedMixerPublisher publisher;
  REQUIRE_OK(publisher.prepare_and_publish(1, nodes, {}, limits(1, 0, 0)),
             "the staged-fill graph did not prepare");
  std::array<StereoFrame, frames> bank_frames{}, output{};
  const std::array<std::span<const StereoFrame>, 1> bank{bank_frames};
  struct Probe {
    std::span<StereoFrame> frames;
    std::span<const std::span<const StereoFrame>> bank;
    RoutedPrepareStatus answer = RoutedPrepareStatus::prepared;
    bool fail_node = false;
    std::size_t node_fills = 0;
    std::size_t whole_fills = 0;
  } probe{bank_frames, bank};
  const RoutedSourceFill fill{&probe, nullptr,
                              [](void* context, std::uint64_t, BlockContext, std::size_t,
                                 std::span<const std::span<const StereoFrame>>& selected) noexcept {
                                auto& state = *static_cast<Probe*>(context);
                                ++state.whole_fills;
                                std::ranges::fill(state.frames,
                                                  StereoFrame{whole_level, whole_level});
                                selected = state.bank;
                                return true;
                              },
                              [](void* context, std::uint64_t, BlockContext, std::size_t,
                                 std::span<const std::span<const StereoFrame>>& selected) noexcept {
                                auto& state = *static_cast<Probe*>(context);
                                if (state.answer == RoutedPrepareStatus::prepared)
                                  selected = state.bank;
                                return state.answer;
                              },
                              [](void* context, std::size_t, BlockContext, std::size_t offset,
                                 std::size_t count, AudioWorkerPool*) noexcept {
                                auto& state = *static_cast<Probe*>(context);
                                ++state.node_fills;
                                std::ranges::fill(state.frames.subspan(offset, count),
                                                  StereoFrame{staged_level, staged_level});
                                return !state.fail_node;
                              }};
  std::uint64_t position = 0;
  const auto render = [&] {
    const auto status = publisher.render_block(bank, output, {position, 1}, fill);
    // A refused block leaves the graph where it was.
    if (status == RoutedRenderStatus::rendered)
      position += frames;
    return status;
  };
  // The first block activates the graph, which fills whole.
  REQUIRE_EQ(render(), RoutedRenderStatus::rendered, "the first block did not render");
  probe.whole_fills = 0;
  REQUIRE_EQ(render(), RoutedRenderStatus::rendered, "a staged fill did not render");
  REQUIRE_EQ(output[0].left, staged_level, "the node did not render its own source");
  REQUIRE_EQ(probe.node_fills, 1, "the node did not fill inside the schedule");
  REQUIRE_EQ(probe.whole_fills, 0, "a staged block filled whole as well");
  probe.answer = RoutedPrepareStatus::whole_block;
  REQUIRE_EQ(render(), RoutedRenderStatus::rendered, "a whole fill did not render");
  REQUIRE_EQ(output[0].left, whole_level, "the source's whole fill was not used");
  REQUIRE_EQ(probe.whole_fills, 1, "the source's whole fill was not used");
  probe.answer = RoutedPrepareStatus::failed;
  REQUIRE_EQ(render(), RoutedRenderStatus::invalid_block, "a refused block rendered");
  REQUIRE_EQ(output[0].left, 0.0F, "a refused block was not silenced");
  probe.answer = RoutedPrepareStatus::prepared;
  probe.fail_node = true;
  const auto fills_before = probe.node_fills;
  REQUIRE_EQ(render(), RoutedRenderStatus::invalid_block, "a failed node fill rendered");
  REQUIRE_EQ(probe.node_fills, fills_before + 1, "the failing node was not asked to fill");
  REQUIRE_EQ(output[0].left, 0.0F, "a failed node fill was not silenced");
}

void invalid_blocks_never_activate() {
  const std::array<RoutedNode, 1> nodes{{{{3, 0}, {}}}};
  RoutedMixerPublisher publisher;
  std::array<StereoFrame, 4> output{};
  output.fill({1.0F, 1.0F});
  const std::array<std::span<const StereoFrame>, 1> input{{}};
  REQUIRE_EQ(publisher.render_block(input, output, {0, 1}), RoutedRenderStatus::no_graph,
             "no-graph path did not silence");
  REQUIRE_EQ(output[0].left, 0.0F, "no-graph path did not silence");
  REQUIRE_OK(publisher.prepare_and_publish(1, nodes, {}, limits(1, 0, 0)),
             "invalid-block fixture did not prepare");
  REQUIRE_TRUE(!publisher.prepare_and_publish(2, nodes, {}, limits(1, 0, 0)),
               "a second pending plan replaced the first");
  REQUIRE_TRUE(publisher.has_pending(), "a second pending plan replaced the first");
  const std::array<std::span<const StereoFrame>, 0> absent{};
  REQUIRE_EQ(publisher.render_block(absent, output, {0, 1}), RoutedRenderStatus::invalid_block,
             "invalid source layout activated pending mixer");
  REQUIRE_TRUE(publisher.has_pending(), "invalid source layout activated pending mixer");
  REQUIRE_EQ(publisher.active_revision(), 0, "invalid source layout activated pending mixer");
  REQUIRE_EQ(publisher.render_block(input, output, {0, 0}), RoutedRenderStatus::invalid_block,
             "invalid generation activated pending mixer");
  REQUIRE_TRUE(publisher.has_pending(), "invalid generation activated pending mixer");
  REQUIRE_EQ(publisher.render_block(input, {}, {0, 1}), RoutedRenderStatus::invalid_block,
             "empty adapter block activated pending mixer");
  REQUIRE_TRUE(publisher.has_pending(), "empty adapter block activated pending mixer");
  REQUIRE_EQ(publisher.render_block(input, output, {0, 1}), RoutedRenderStatus::rendered,
             "valid block could not activate pending mixer");
  REQUIRE_EQ(publisher.render_block(absent, output, {4, 1}), RoutedRenderStatus::invalid_block,
             "active mixer accepted missing source spans");
  REQUIRE_EQ(publisher.active_revision(), 1, "active mixer accepted missing source spans");

  REQUIRE_OK(publisher.prepare_and_publish_live(2, nodes, {}, limits(1, 0, 0)),
             "successor did not prepare");
  REQUIRE_EQ(publisher.render_block(absent, output, {4, 1}), RoutedRenderStatus::invalid_block,
             "invalid switch inputs activated successor");
  REQUIRE_TRUE(publisher.has_pending(), "invalid switch inputs activated successor");
  REQUIRE_EQ(publisher.active_revision(), 1, "invalid switch inputs activated successor");
  REQUIRE_EQ(publisher.render_block(input, output, {4, 1}), RoutedRenderStatus::rendered,
             "valid switch did not recover after invalid inputs");
  REQUIRE_EQ(publisher.active_revision(), 2, "valid switch did not recover after invalid inputs");
  REQUIRE_EQ(publisher.collect_retired(), 1, "valid switch did not recover after invalid inputs");

  std::array<StereoFrame, 8193> too_large{};
  too_large[0] = {7.0F, 7.0F};
  REQUIRE_EQ(publisher.render_block(input, too_large, {8, 1}), RoutedRenderStatus::invalid_block,
             "oversized adapter block was mutated");
  REQUIRE_EQ(too_large[0].left, 7.0F, "oversized adapter block was mutated");
}

void preparation_allocation_failure() {
  const std::array<RoutedNode, 1> nodes{{{{3, 0}, {}}}};
  RoutedMixerPublisher publisher;
  fail_next_allocation = true;
  const auto failed = publisher.prepare_and_publish(1, nodes, {}, limits(1, 0, 0));
  REQUIRE_TRUE(!failed, "allocation failure published a partial routed plan");
  REQUIRE_EQ(failed.error().code, foundation::ErrorCode::resource_exhausted,
             "allocation failure published a partial routed plan");
  REQUIRE_EQ(publisher.active_revision(), 0, "allocation failure published a partial routed plan");
  REQUIRE_TRUE(!publisher.has_pending(), "allocation failure published a partial routed plan");
  REQUIRE_OK(publisher.prepare_and_publish(1, nodes, {}, limits(1, 0, 0)),
             "allocation failure prevented a clean retry");
}

void concurrent_handoff() {
  const std::array<RoutedNode, 1> nodes{{{{3, 0}, {}}}};
  RoutedMixerPublisher publisher;
  REQUIRE_OK(publisher.prepare_and_publish(1, nodes, {}, limits(1, 0, 0)),
             "concurrency fixture did not prepare");
  std::atomic<std::uint64_t> renders{0};
  std::jthread callback([&](const std::stop_token& stop) {
    std::array<StereoFrame, 4> output{};
    const std::array<std::span<const StereoFrame>, 1> input{{}};
    std::uint64_t position = 0;
    while (!stop.stop_requested()) {
      const auto status = publisher.render_block(input, output, {position, 1});
      if (status == RoutedRenderStatus::rendered) {
        position += output.size();
        renders.fetch_add(1, std::memory_order_release);
      } else {
        std::this_thread::yield();
      }
    }
  });
  // Each milestone waits on the callback's progress, up to the test deadline,
  // rather than a fixed number of yields.
  constexpr std::uint64_t warm_up_renders = 8;
  oscillate::test::wait_until([&] { return renders.load() >= warm_up_renders; },
                              "callback fixture made no progress");
  REQUIRE_OK(publisher.prepare_and_publish_live(2, nodes, {}, limits(1, 0, 0)),
             "concurrent transition did not prepare");
  oscillate::test::wait_until([&] { return publisher.active_revision() == 2; },
                              "concurrent transition did not activate");
  // The retired plan reaches the control side once the callback has finished
  // the switch, which can be just after the new revision shows as active.
  std::size_t retired = 0;
  oscillate::test::wait_until(
      [&] {
        retired += publisher.collect_retired();
        return retired != 0;
      },
      "concurrent transition did not reclaim the old plan");
  REQUIRE_EQ(retired, 1, "concurrent transition reclaimed more than the old plan");
  callback.request_stop();
  callback.join();
}

// A live candidate whose max_frames is below every device block can never be
// adopted. The control side withdraws it while the callback keeps playing:
// the callback retires it before touching it again, the old graph plays on
// unbroken, and later publications succeed.
void unadoptable_live_candidate_is_withdrawn() {
  constexpr std::size_t device_block = 64;
  constexpr std::uint64_t first_revision = 1;
  constexpr std::uint64_t short_revision = 2;
  constexpr std::uint64_t later_revision = 3;
  constexpr std::uint64_t renders_while_pending = 16;
  const std::array<RoutedNode, 1> nodes{{{{3, 0}, {}}}};
  const auto fitting = limits(1, 0, 0);
  auto too_short = fitting;
  too_short.max_frames = device_block / 2;
  RoutedMixerPublisher publisher;
  REQUIRE_OK(publisher.prepare_and_publish(first_revision, nodes, {}, fitting),
             "withdrawal fixture did not prepare");
  std::atomic<std::uint64_t> renders{0};
  std::atomic<bool> broken{false};
  std::jthread callback([&](const std::stop_token& stop) {
    std::array<StereoFrame, device_block> output{};
    const std::array<std::span<const StereoFrame>, 1> input{{}};
    std::uint64_t position = 0;
    while (!stop.stop_requested()) {
      if (publisher.render_block(input, output, {position, 1}) != RoutedRenderStatus::rendered)
        broken.store(true, std::memory_order_relaxed);
      position += output.size();
      renders.fetch_add(1, std::memory_order_release);
      std::this_thread::yield();
    }
  });
  oscillate::test::wait_until([&] { return publisher.active_revision() == first_revision; },
                              "the callback did not adopt the first graph");
  REQUIRE_OK(publisher.prepare_and_publish_live(short_revision, nodes, {}, too_short),
             "the too-short live candidate did not prepare");
  const auto published_at = renders.load(std::memory_order_acquire);
  oscillate::test::wait_until(
      [&] { return renders.load() >= published_at + renders_while_pending; },
      "callback fixture made no progress");
  REQUIRE_EQ(publisher.active_revision(), first_revision,
             "a candidate shorter than the device block was adopted");
  REQUIRE_TRUE(publisher.has_pending(), "a candidate shorter than the device block was adopted");
  require(!publisher.prepare_and_publish_live(later_revision, nodes, {}, fitting),
          "a publication passed a pending candidate");
  REQUIRE_TRUE(publisher.withdraw_pending(), "the pending candidate could not be withdrawn");
  REQUIRE_TRUE(!publisher.withdraw_pending(), "a withdrawal was requested twice");
  oscillate::test::wait_until([&] { return !publisher.has_pending(); },
                              "the callback did not answer the withdrawal");
  REQUIRE_EQ(publisher.collect_retired(), 1, "the withdrawn candidate was not reclaimed");
  REQUIRE_EQ(publisher.active_revision(), first_revision,
             "withdrawing the candidate changed the audible graph");
  REQUIRE_OK(publisher.prepare_and_publish_live(later_revision, nodes, {}, fitting),
             "a publication after the withdrawal was refused");
  oscillate::test::wait_until([&] { return publisher.active_revision() == later_revision; },
                              "the publication after the withdrawal did not activate");
  callback.request_stop();
  callback.join();
  REQUIRE_TRUE(!broken.load(), "the old graph stopped rendering around the withdrawal");
  REQUIRE_EQ(publisher.collect_retired(), 1, "the replaced graph was not reclaimed");
}

// A live candidate whose source bank never fills renders nothing: the old
// graph keeps playing its own bank. Withdrawn, it is retired at the next
// block without allocating, and the next candidate switches normally.
void unfillable_live_candidate_is_withdrawn() {
  constexpr std::size_t block = 4;
  constexpr std::uint64_t first_revision = 1;
  constexpr std::uint64_t unfilled_revision = 2;
  constexpr std::uint64_t later_revision = 3;
  constexpr int attempts = 8;
  constexpr float old_level = 0.25F;
  const std::array<RoutedNode, 1> nodes{{{{3, 0}, {}}}};
  RoutedMixerPublisher publisher;
  REQUIRE_OK(publisher.prepare_and_publish(first_revision, nodes, {}, limits(1, 0, 0)),
             "unfilled-bank fixture did not prepare");
  std::array<StereoFrame, block> frames{}, output{};
  frames.fill({old_level, old_level});
  const std::array<std::span<const StereoFrame>, 1> bank{frames};
  struct Banks {
    std::span<const std::span<const StereoFrame>> old;
  } banks{bank};
  const RoutedSourceFill fill{&banks, nullptr,
                              [](void* context, std::uint64_t revision, BlockContext, std::size_t,
                                 std::span<const std::span<const StereoFrame>>& selected) noexcept {
                                if (revision == unfilled_revision)
                                  return false;
                                selected = static_cast<Banks*>(context)->old;
                                return true;
                              }};
  std::uint64_t position = 0;
  const auto render = [&] {
    const auto status = publisher.render_block(bank, output, {position, 1}, fill);
    position += block;
    return status;
  };
  REQUIRE_EQ(render(), RoutedRenderStatus::rendered, "the first graph did not render");
  REQUIRE_OK(publisher.prepare_and_publish_live(unfilled_revision, nodes, {}, limits(1, 0, 0)),
             "the unfillable live candidate did not prepare");
  for (int attempt = 0; attempt < attempts; ++attempt) {
    REQUIRE_EQ(render(), RoutedRenderStatus::rendered,
               "the old graph stopped while the candidate could not fill");
    REQUIRE_EQ(output[0].left, old_level,
               "the old graph stopped while the candidate could not fill");
  }
  REQUIRE_EQ(publisher.active_revision(), first_revision, "an unfilled candidate was adopted");
  REQUIRE_EQ(publisher.live_candidate_passed_blocks(), static_cast<std::uint64_t>(attempts),
             "the blocks that passed the candidate over were not counted");
  REQUIRE_TRUE(publisher.withdraw_pending(), "the unfilled candidate could not be withdrawn");
  REQUIRE_TRUE(publisher.has_pending(), "a withdrawal showed as done before the callback answered");
  counting_allocations = true;
  counted_allocations = 0;
  const auto answered = render();
  counting_allocations = false;
  REQUIRE_EQ(answered, RoutedRenderStatus::rendered, "the withdrawal interrupted the old graph");
  REQUIRE_EQ(counted_allocations, 0, "the callback allocated answering a withdrawal");
  REQUIRE_TRUE(!publisher.has_pending(), "the callback did not answer the withdrawal");
  REQUIRE_EQ(publisher.collect_retired(), 1, "the withdrawn candidate was not reclaimed");
  REQUIRE_TRUE(!publisher.withdraw_pending(), "a withdrawal was accepted with nothing pending");
  REQUIRE_OK(publisher.prepare_and_publish_live(later_revision, nodes, {}, limits(1, 0, 0)),
             "a publication after the withdrawal was refused");
  REQUIRE_EQ(render(), RoutedRenderStatus::rendered, "the later candidate did not switch");
  REQUIRE_EQ(publisher.active_revision(), later_revision, "the later candidate did not switch");
  REQUIRE_EQ(publisher.live_candidate_passed_blocks(), static_cast<std::uint64_t>(attempts),
             "a block with no candidate waiting, or one adopting it, was counted");
}

} // namespace

// A live switch (no hold) to an identical graph must be inaudible: the
// callback hands over delay-compensation lines, histories and position, and
// the shared processor keeps running, so the output is bit-identical to never
// switching, with no silent block and no callback allocation.
void live_switch_continues_running_state() {
  struct EightFrameLatency final : ProcessorRuntime {
    std::array<StereoFrame, 8> line{};
    std::size_t cursor = 0;
    void process(std::span<StereoFrame> frames, BlockContext) noexcept override {
      for (auto& frame : frames) {
        std::swap(frame, line[cursor]);
        cursor = (cursor + 1) % line.size();
      }
    }
  };
  const std::array<RoutingEdge, 3> edges{{{11, 1, 3}, {12, 2, 3}, {13, 1, 2, 0.5F}}};
  const auto run = [&](bool switch_live) {
    auto latency = std::make_shared<EightFrameLatency>();
    const std::array<RoutedNode, 3> nodes{{{{1, 0}, {}}, {{2, 8}, latency}, {{3, 0}, {}}}};
    auto bounded = limits(3, 3);
    bounded.max_source_history_bytes = 3ULL * 33 * sizeof(StereoFrame);
    RoutedMixerPublisher publisher;
    REQUIRE_OK(publisher.prepare_and_publish(1, nodes, edges, bounded),
               "live fixture did not publish");
    std::vector<StereoFrame> rendered;
    std::array<StereoFrame, 64> a{}, b{}, output{};
    for (std::uint64_t block = 0; block < 20; ++block) {
      for (std::size_t frame = 0; frame < 64; ++frame) {
        const auto time = static_cast<float>(block * 64 + frame);
        a[frame] = {std::sin(time * 0.05F), std::cos(time * 0.03F)};
        b[frame] = {std::sin(time * 0.11F) * 0.5F, 0.25F};
      }
      if (switch_live && block == 10) {
        (void)publisher.collect_retired();
        REQUIRE_OK(publisher.prepare_and_publish_live(2, nodes, edges, bounded),
                   "live switch was not prepared while the graph played");
        REQUIRE_TRUE(publisher.has_pending(),
                     "live switch was not prepared while the graph played");
      }
      const std::array<std::span<const StereoFrame>, 3> input{a, b, {}};
      counting_allocations = block == 10;
      counted_allocations = 0;
      const auto status = publisher.render_block(input, output, {block * 64, 1});
      counting_allocations = false;
      REQUIRE_EQ(status, RoutedRenderStatus::rendered,
                 "a live switch held, silenced or allocated on the callback");
      REQUIRE_EQ(counted_allocations, 0,
                 "a live switch held, silenced or allocated on the callback");
      rendered.insert(rendered.end(), output.begin(), output.end());
    }
    REQUIRE_EQ(publisher.active_revision(), (switch_live ? 2U : 1U),
               "the live switch did not activate at its block");
    return rendered;
  };
  const auto steady = run(false);
  const auto switched = run(true);
  for (std::size_t frame = 0; frame < steady.size(); ++frame)
    require(steady[frame].left == switched[frame].left &&
                steady[frame].right == switched[frame].right,
            "a live switch to the same graph changed the output at frame " + std::to_string(frame));
}

// A pure delay that can continue another one live (ADR-0049): primed from
// the node's input history, or flushing what a longer predecessor held.
class ContinuingDelay final : public ProcessorRuntime {
public:
  explicit ContinuingDelay(std::size_t frames) : line_(frames) {}
  void process(std::span<StereoFrame> stereo, BlockContext) noexcept override {
    for (auto& frame : stereo) {
      std::swap(frame, line_[cursor_]);
      cursor_ = (cursor_ + 1) % line_.size();
    }
  }
  [[nodiscard]] bool continues_latency_of(const ProcessorRuntime* prior) const noexcept override {
    return prior == nullptr || dynamic_cast<const ContinuingDelay*>(prior) != nullptr;
  }
  std::size_t continue_latency_of(ProcessorRuntime* prior, std::span<const StereoFrame> older,
                                  std::span<const StereoFrame> newer, BlockContext,
                                  std::span<StereoFrame> flushed) noexcept override {
    // Hold the newest inputs, as if this line had always run.
    const auto total = older.size() + newer.size();
    for (std::size_t index = 0; index < line_.size() && index < total; ++index) {
      const auto at = total - line_.size() + index;
      line_[index] = at < older.size() ? older[at] : newer[at - older.size()];
    }
    cursor_ = 0;
    const auto* old = dynamic_cast<const ContinuingDelay*>(prior);
    if (old == nullptr || old->line_.size() <= line_.size())
      return 0;
    // The longer predecessor still held these, oldest first, before ours.
    const auto extra = old->line_.size() - line_.size();
    for (std::size_t index = 0; index < extra; ++index)
      flushed[index] = old->line_[(old->cursor_ + index) % old->line_.size()];
    return extra;
  }

private:
  std::vector<StereoFrame> line_;
  std::size_t cursor_ = 0;
};

// Live latency changes keep time exactly when the output latency holds
// (ADR-0049): three paths of the same input sum to exactly 3 x(t - 16)
// while one path gains, then shortens, a lookahead; with no silent block
// and no callback allocation. A changed output latency fades instead.
void live_latency_changes_keep_time() {
  const std::array<RoutingEdge, 3> edges{{{21, 1, 4}, {22, 2, 4}, {23, 3, 4}}};
  auto bounded = limits(4, 3, 32);
  bounded.latency.master = 4;
  bounded.max_processor_input_history_frames = 32;
  bounded.max_history_bytes += 4ULL * 33 * sizeof(StereoFrame);
  const auto fixed = std::make_shared<ContinuingDelay>(16);
  auto graph = [&](std::shared_ptr<ProcessorRuntime> middle, std::uint32_t middle_latency,
                   std::shared_ptr<ProcessorRuntime> long_path, std::uint32_t long_latency) {
    return std::array<RoutedNode, 4>{{{{1, 0}, {}},
                                      {{2, middle_latency}, std::move(middle)},
                                      {{3, long_latency}, std::move(long_path)},
                                      {{4, 0}, {}}}};
  };
  RoutedMixerPublisher publisher;
  REQUIRE_OK(publisher.prepare_and_publish(1, graph({}, 0, fixed, 16), edges, bounded),
             "latency fixture did not publish");
  const auto input = [](std::uint64_t time) {
    return StereoFrame{std::sin(static_cast<float>(time) * 0.07F),
                       std::cos(static_cast<float>(time) * 0.05F)};
  };
  std::array<StereoFrame, 64> x{}, output{};
  const auto eight = std::make_shared<ContinuingDelay>(8);
  const auto four = std::make_shared<ContinuingDelay>(4);
  for (std::uint64_t block = 0; block < 30; ++block) {
    for (std::size_t frame = 0; frame < 64; ++frame)
      x[frame] = input(block * 64 + frame);
    if (block == 10 || block == 20) {
      (void)publisher.collect_retired();
      const auto nodes = block == 10 ? graph(eight, 8, fixed, 16) : graph(four, 4, fixed, 16);
      const auto prepared =
          publisher.prepare_and_publish_live(block == 10 ? 2 : 3, nodes, edges, bounded);
      REQUIRE_OK(prepared, "latency change was not prepared live: " +
                               (prepared ? std::string() : prepared.error().message));
    }
    const std::array<std::span<const StereoFrame>, 4> sources{x, x, x, {}};
    counting_allocations = block == 10 || block == 20;
    counted_allocations = 0;
    const auto status = publisher.render_block(sources, output, {block * 64, 1});
    counting_allocations = false;
    REQUIRE_EQ(status, RoutedRenderStatus::rendered, "a live latency change held or allocated");
    REQUIRE_EQ(counted_allocations, 0, "a live latency change held or allocated");
    for (std::size_t frame = 0; frame < 64; ++frame) {
      const auto time = block * 64 + frame;
      StereoFrame expected{};
      if (time >= 16) {
        const auto value = input(time - 16);
        expected = {((0.0F + value.left) + value.left) + value.left,
                    ((0.0F + value.right) + value.right) + value.right};
      }
      require(output[frame].left == expected.left && output[frame].right == expected.right,
              "live latency change lost time at block " + std::to_string(block) + " frame " +
                  std::to_string(frame));
    }
  }
  REQUIRE_EQ(publisher.active_revision(), 3, "both latency changes did not activate");

  // Lengthening the longest path changes the output latency. Time cannot be
  // kept, so each changed delay crossfades on its own: a steady input stays
  // steady through the switch, with no fade or dip.
  const auto render_steady = [&](std::uint64_t first, std::uint64_t last, std::uint64_t checked) {
    for (std::uint64_t block = first; block < last; ++block) {
      for (std::size_t frame = 0; frame < 64; ++frame)
        x[frame] = {1.0F, 1.0F};
      const std::array<std::span<const StereoFrame>, 4> sources{x, x, x, {}};
      counting_allocations = true;
      counted_allocations = 0;
      const auto status = publisher.render_block(sources, output, {block * 64, 1});
      counting_allocations = false;
      REQUIRE_EQ(status, RoutedRenderStatus::rendered,
                 "an output latency change held or allocated");
      REQUIRE_EQ(counted_allocations, 0, "an output latency change held or allocated");
      if (block < checked)
        continue;
      for (const auto& frame : output)
        require(std::abs(frame.left - 3.0F) < tolerance::float_processing_f &&
                    std::abs(frame.right - 3.0F) < tolerance::float_processing_f,
                "a steady input changed level across an output latency change at block " +
                    std::to_string(block));
    }
  };
  render_steady(30, 32, 31);
  (void)publisher.collect_retired();
  const auto longer = std::make_shared<ContinuingDelay>(24);
  REQUIRE_OK(publisher.prepare_and_publish_live(4, graph(four, 4, longer, 24), edges, bounded),
             "output latency change was not prepared live");
  render_steady(32, 40, 32);
  REQUIRE_EQ(publisher.active_revision(), 4, "the output latency change did not activate");
}

void live_controls_glide_without_a_revision() {
  const std::array<RoutedNode, 2> nodes{{{{1, 0}, {}}, {{3, 0}, {}}}};
  const std::array<RoutingEdge, 1> edges{{{11, 1, 3}}};
  RoutedMixerPublisher publisher;
  REQUIRE_OK(publisher.prepare_and_publish(1, nodes, edges, limits(2, 1, 0)),
             "live control fixture did not prepare");
  std::array<StereoFrame, 64> source{}, output{};
  std::ranges::fill(source, StereoFrame{1.0F, 1.0F});
  const std::array<std::span<const StereoFrame>, 2> inputs{source, {}};
  REQUIRE_EQ(publisher.render_block(inputs, output, {0, 1}), RoutedRenderStatus::rendered,
             "live control baseline did not render");
  REQUIRE_EQ(output.back().left, 1.0F, "live control baseline did not render");
  // A fader mid-drag: heard at the next block, gliding from the old gain,
  // with no new revision and no allocation on the callback.
  require(publisher.push_live_node_gain(1, {0.5F, 0.25F}), "live channel gain was refused");
  counted_allocations = 0;
  counting_allocations = true;
  const auto faded = publisher.render_block(inputs, output, {64, 1});
  counting_allocations = false;
  REQUIRE_EQ(faded, RoutedRenderStatus::rendered,
             "live channel gain did not glide to its value without a revision");
  REQUIRE_EQ(counted_allocations, 0,
             "live channel gain did not glide to its value without a revision");
  REQUIRE_EQ(publisher.active_revision(), 1,
             "live channel gain did not glide to its value without a revision");
  REQUIRE_GT(output.front().left, 0.99F,
             "live channel gain did not glide to its value without a revision");
  REQUIRE_EQ(output.back().left, 0.5F,
             "live channel gain did not glide to its value without a revision");
  REQUIRE_EQ(output.back().right, 0.25F,
             "live channel gain did not glide to its value without a revision");
  REQUIRE_TRUE(publisher.push_live_edge_gain(11, 0.5F), "live send gain was not heard");
  REQUIRE_EQ(publisher.render_block(inputs, output, {128, 1}), RoutedRenderStatus::rendered,
             "live send gain was not heard");
  REQUIRE_EQ(output.back().left, 0.25F, "live send gain was not heard");
  REQUIRE_EQ(output.back().right, 0.125F, "live send gain was not heard");
  // Invalid values never reach the callback; unknown IDs change nothing.
  require(!publisher.push_live_node_gain(1, {std::numeric_limits<float>::quiet_NaN(), 1.0F}) &&
              !publisher.push_live_node_gain(1, {5.0F, 1.0F}) &&
              !publisher.push_live_edge_gain(11, -1.0F) && publisher.push_live_node_gain(99, {}) &&
              publisher.render_block(inputs, output, {192, 1}) == RoutedRenderStatus::rendered &&
              output.back().left == 0.25F,
          "an invalid or unknown live control changed the mix");
  // The send's drag ends back at its saved gain; the fader's release saves
  // the fader's live value, so nothing audible changes.
  require(publisher.push_live_edge_gain(11, 1.0F) &&
              publisher.render_block(inputs, output, {256, 1}) == RoutedRenderStatus::rendered &&
              output.back().left == 0.5F,
          "live send gain did not return to its saved value");
  const std::array<RoutedNodeControl, 2> saved{
      {{1, false, {0.5F, 0.25F}}, {3, false, {1.0F, 1.0F}}}};
  REQUIRE_TRUE(publisher.prepare_control_overlay(2, saved), "saving the live value moved the mix");
  REQUIRE_EQ(publisher.render_block(inputs, output, {320, 1}), RoutedRenderStatus::rendered,
             "saving the live value moved the mix");
  REQUIRE_EQ(publisher.active_revision(), 2, "saving the live value moved the mix");
  REQUIRE_EQ(output.front().left, 0.5F, "saving the live value moved the mix");
  REQUIRE_EQ(output.back().left, 0.5F, "saving the live value moved the mix");
  REQUIRE_EQ(output.back().right, 0.25F, "saving the live value moved the mix");
  REQUIRE_EQ(publisher.collect_retired(), 1, "saving the live value moved the mix");
  // The mailbox is bounded; draining it makes room again.
  bool full = false;
  for (std::size_t index = 0; index <= 256 && !full; ++index)
    full = !publisher.push_live_node_gain(1, {0.5F, 0.25F});
  REQUIRE_TRUE(full, "the live control mailbox was unbounded or did not drain");
  REQUIRE_EQ(publisher.render_block(inputs, output, {384, 1}), RoutedRenderStatus::rendered,
             "the live control mailbox was unbounded or did not drain");
  REQUIRE_TRUE(publisher.push_live_node_gain(1, {0.5F, 0.25F}),
               "the live control mailbox was unbounded or did not drain");
  // A channel under automation keeps its automation.
  class BoundGain final : public RoutedGainSource {
  public:
    bool fill_block(BlockContext, std::size_t, RoutedGainSpans& gains) noexcept override {
      gains = {};
      return true;
    }
  };
  RoutedMixerPublisher automated;
  REQUIRE_TRUE(automated.prepare_and_publish(1, nodes, edges, limits(2, 1, 0),
                                             std::make_shared<BoundGain>()),
               "a live control overrode channel automation");
  REQUIRE_EQ(automated.render_block(inputs, output, {0, 1}), RoutedRenderStatus::rendered,
             "a live control overrode channel automation");
  REQUIRE_TRUE(automated.push_live_node_gain(1, {0.0F, 0.0F}),
               "a live control overrode channel automation");
  REQUIRE_EQ(automated.render_block(inputs, output, {64, 1}), RoutedRenderStatus::rendered,
             "a live control overrode channel automation");
  REQUIRE_EQ(output.back().left, 1.0F, "a live control overrode channel automation");
}

int main() {
  try {
    pdc_transition_and_failures();
    live_switch_continues_running_state();
    live_latency_changes_keep_time();
    live_new_route_uses_source_history();
    tail_reuse_and_changed_owner_rejection();
    bounded_replacement_drains_across_routing_revisions();
    replacement_drains_only_selected_predecessor_stage();
    chunked_processor_preserves_absolute_events();
    source_overlay_keeps_old_audio_until_candidate_ready(64);
    source_overlay_keeps_old_audio_until_candidate_ready(128);
    control_overlay_reuses_route_without_silent_blocks(64);
    control_overlay_reuses_route_without_silent_blocks(128);
    control_overlay_can_publish_an_unchanged_projection(64);
    control_overlay_can_publish_an_unchanged_projection(128);
    control_overlay_rejects_processor_and_unknown_automation();
    control_overlay_supersedes_an_inflight_gain_ramp();
    control_overlay_keeps_qualified_processor_history(64);
    control_overlay_keeps_qualified_processor_history(128);
    edge_overlay_ramps_send_without_losing_delay_history(64);
    edge_overlay_ramps_send_without_losing_delay_history(128);
    edge_overlay_rejects_changed_identity_and_unknown_automation();
    live_controls_glide_without_a_revision();
    source_order_and_cursor_rejection();
    source_fill_linearizes_with_live_publication();
    revision_selects_matching_source_bank();
    staged_fill_renders_inside_the_schedule();
    invalid_blocks_never_activate();
    preparation_allocation_failure();
    concurrent_handoff();
    unadoptable_live_candidate_is_withdrawn();
    unfillable_live_candidate_is_withdrawn();
  } catch (const std::exception& error) {
    std::fprintf(stderr, "routed publisher test: %s\n", error.what());
    return 1;
  }
}
