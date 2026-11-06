#include "oscillate/engine/delay_compensation.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/check.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

namespace {
using namespace oscillate;
using namespace oscillate::engine;
static_assert(std::is_base_of_v<ProcessorRuntime, DelayCompensationRuntime>);
static_assert(noexcept(std::declval<DelayCompensationRuntime&>().process(
    std::declval<std::span<StereoFrame>>(), BlockContext{})));

using oscillate::test::require;

using oscillate::test::take;

DelayCompensationConfig config(std::uint32_t delay, std::uint32_t budget = 64,
                               RoutingEdgeId route = 11) {
  return {route, delay, budget, (static_cast<std::uint64_t>(budget) + 1) * sizeof(StereoFrame)};
}

void planner_hook_and_parallel_impulses() {
  const std::array<RoutingNode, 3> nodes{{{1, 0}, {2, 16}, {3, 0}}};
  const std::array<RoutingEdge, 2> edges{{{11, 1, 3}, {12, 2, 3}}};
  auto plan = take(prepare_routing_latency(nodes, edges, {3, 64}));
  const auto fast = std::ranges::find_if(plan.edges, [](auto edge) { return edge.id == 11; });
  const auto slow = std::ranges::find_if(plan.edges, [](auto edge) { return edge.id == 12; });
  REQUIRE_NE(fast, plan.edges.end(), "planner did not identify the parallel delay");
  REQUIRE_NE(slow, plan.edges.end(), "planner did not identify the parallel delay");
  REQUIRE_EQ(fast->compensation_frames, 16, "planner did not identify the parallel delay");
  REQUIRE_EQ(slow->compensation_frames, 0, "planner did not identify the parallel delay");
  auto line = take(DelayCompensationRuntime::prepare(config(fast->compensation_frames)));
  std::array<StereoFrame, 96> input{};
  input[0] = {1.0F, -0.25F};
  input[32] = {-0.5F, 0.75F};
  line->process(input, {0, 1});
  REQUIRE_EQ(input[15].left, 0.0F, "compensated impulse did not align with 16-frame slow path");
  REQUIRE_EQ(input[16].left, 1.0F, "compensated impulse did not align with 16-frame slow path");
  REQUIRE_EQ(input[16].right, -0.25F, "compensated impulse did not align with 16-frame slow path");
  REQUIRE_EQ(input[48].left, -0.5F, "compensated impulse did not align with 16-frame slow path");
  REQUIRE_EQ(input[48].right, 0.75F, "compensated impulse did not align with 16-frame slow path");
  REQUIRE_EQ(line->tail_frames(), 16, "route identity or exact delay tail missing");
  REQUIRE_EQ(line->route_id(), 11, "route identity or exact delay tail missing");
}

void switch_boundary_growth_and_shrink() {
  auto line = take(DelayCompensationRuntime::prepare(config(16)));
  std::array<StereoFrame, 320> original{};
  original[168] = {0.4F, -0.2F};
  original[169] = {-0.6F, 0.3F};
  original[183] = {0.75F, -0.5F};
  original[201] = {0.25F, -0.125F};
  original[252] = {-0.375F, 0.625F};
  original[260] = {0.9F, -0.7F};
  std::array<StereoFrame, 320> output{};
  for (std::uint64_t time = 0; time < 320; ++time) {
    if (time == 200) {
      const std::array<RoutingNode, 3> slower{{{1, 0}, {2, 32}, {3, 0}}};
      const std::array<RoutingEdge, 2> edges{{{11, 1, 3}, {12, 2, 3}}};
      const auto plan = take(prepare_routing_latency(slower, edges, {3, 64}));
      REQUIRE_EQ(plan.edges[0].compensation_frames, 32,
                 "latency growth planner did not request 32 frames");
      line = take(
          DelayCompensationRuntime::prepare_transition_after_quiesce(*line, config(32), {time, 1}));
    } else if (time == 260) {
      line = take(
          DelayCompensationRuntime::prepare_transition_after_quiesce(*line, config(8), {time, 1}));
    }
    std::array<StereoFrame, 1> frame{{original[time]}};
    line->process(frame, {time, 1});
    output[time] = frame[0];
    const auto delay = time < 200 ? 16U : (time < 260 ? 32U : 8U);
    const auto expected = time >= delay ? original[time - delay] : StereoFrame{};
    REQUIRE_EQ(frame[0].left, expected.left,
               "delay changed sample position at or near graph switch");
    REQUIRE_EQ(frame[0].right, expected.right,
               "delay changed sample position at or near graph switch");
  }
  REQUIRE_EQ(output[199].left, 0.75F, "before/at/after transition impulse oracle failed");
  REQUIRE_EQ(output[200].left, 0.4F, "before/at/after transition impulse oracle failed");
  REQUIRE_EQ(output[201].left, -0.6F, "before/at/after transition impulse oracle failed");
  REQUIRE_EQ(output[260].left, -0.375F, "before/at/after transition impulse oracle failed");
  REQUIRE_EQ(output[268].left, 0.9F, "before/at/after transition impulse oracle failed");
}

void large_history_switch_boundary_impulses() {
  constexpr std::uint32_t budget = 8'192;
  constexpr std::uint64_t boundary = 20'480;
  const auto impulse = [](std::uint64_t frame) -> StereoFrame {
    return frame == boundary - budget || frame == boundary - budget + 127 || frame == boundary ||
                   frame == boundary + 64
               ? StereoFrame{1.0F, -0.5F}
               : StereoFrame{};
  };
  auto line = take(DelayCompensationRuntime::prepare(config(4'096, budget)));
  std::array<StereoFrame, 128> block{};
  for (std::uint64_t position = 0; position < boundary; position += block.size()) {
    for (std::size_t index = 0; index < block.size(); ++index) {
      block[index] = impulse(position + index);
    }
    line->process(block, {position, 1});
  }
  line = take(DelayCompensationRuntime::prepare_transition_after_quiesce(
      *line, config(budget, budget), {boundary, 1}));
  for (std::uint64_t position = boundary; position < boundary + budget + 128;
       position += block.size()) {
    for (std::size_t index = 0; index < block.size(); ++index) {
      block[index] = impulse(position + index);
    }
    line->process(block, {position, 1});
    for (std::size_t index = 0; index < block.size(); ++index) {
      const auto expected = impulse(position + index - budget);
      REQUIRE_EQ(block[index].left, expected.left,
                 "8,192-frame growth failed exact boundary alignment");
      REQUIRE_EQ(block[index].right, expected.right,
                 "8,192-frame growth failed exact boundary alignment");
    }
  }
  const auto short_budget = config(budget + 1, budget);
  auto rejected = DelayCompensationRuntime::prepare_transition_after_quiesce(
      *line, short_budget, {boundary + budget + 128, 1});
  REQUIRE_TRUE(!rejected, "over-budget large-history transition was accepted");
  REQUIRE_EQ(rejected.error().code, foundation::ErrorCode::resource_exhausted,
             "over-budget large-history transition was accepted");
}

void unprimeable_transition_holds_prior() {
  auto line = take(DelayCompensationRuntime::prepare(config(2, 8)));
  std::array<StereoFrame, 4> first{{{1.0F, 0.0F}, {}, {}, {}}};
  line->process(first, {0, 1});
  REQUIRE_EQ(first[2].left, 1.0F, "initial short delay failed");
  auto missing =
      DelayCompensationRuntime::prepare_transition_after_quiesce(*line, config(8, 8), {4, 1});
  REQUIRE_TRUE(!missing, "missing history did not explicitly hold old delay");
  REQUIRE_EQ(missing.error().code, foundation::ErrorCode::unavailable,
             "missing history did not explicitly hold old delay");
  REQUIRE_EQ(line->delay_frames(), 2, "missing history did not explicitly hold old delay");
  auto wrong_route =
      DelayCompensationRuntime::prepare_transition_after_quiesce(*line, config(1, 8, 12), {4, 1});
  REQUIRE_TRUE(!wrong_route, "different route reused delay history");
  REQUIRE_EQ(wrong_route.error().code, foundation::ErrorCode::conflict,
             "different route reused delay history");
  REQUIRE_TRUE(
      !DelayCompensationRuntime::prepare_transition_after_quiesce(*line, config(1, 8), {5, 1}),
      "inconsistent boundary was accepted for history transfer");
  REQUIRE_TRUE(
      !DelayCompensationRuntime::prepare_transition_after_quiesce(*line, config(1, 8), {4, 2}),
      "inconsistent boundary was accepted for history transfer");
  std::array<StereoFrame, 1> next{{{0.5F, -0.5F}}};
  line->process(next, {4, 1});
  REQUIRE_EQ(next[0].left, 0.0F, "failed transition damaged old line's continuation");
  REQUIRE_EQ(line->history_frames_after_quiesce(), 5,
             "failed transition damaged old line's continuation");
}

void budgets_partitions_and_transport() {
  auto over_budget = DelayCompensationRuntime::prepare(config(9, 8));
  REQUIRE_TRUE(!over_budget, "over-budget delay accepted");
  REQUIRE_EQ(over_budget.error().code, foundation::ErrorCode::resource_exhausted,
             "over-budget delay accepted");
  auto zero_route = DelayCompensationRuntime::prepare(config(0, 8, 0));
  REQUIRE_TRUE(!zero_route, "zero route accepted");
  REQUIRE_EQ(zero_route.error().code, foundation::ErrorCode::invalid_argument,
             "zero route accepted");
  auto too_small = config(1, 8);
  --too_small.max_history_bytes;
  auto short_memory = DelayCompensationRuntime::prepare(too_small);
  REQUIRE_TRUE(!short_memory, "history byte budget ignored");
  REQUIRE_EQ(short_memory.error().code, foundation::ErrorCode::resource_exhausted,
             "history byte budget ignored");
  auto zero = take(DelayCompensationRuntime::prepare(config(0, 0)));
  std::array<StereoFrame, 2> passthrough{{{0.2F, -0.4F}, {0.6F, -0.8F}}};
  zero->process(passthrough, {0, 1});
  REQUIRE_EQ(passthrough[0].left, 0.2F, "zero delay altered stereo samples");
  REQUIRE_EQ(passthrough[1].right, -0.8F, "zero delay altered stereo samples");

  std::array<StereoFrame, 257> source{};
  for (std::size_t index = 0; index < source.size(); ++index) {
    source[index] = {static_cast<float>(index) / 257.0F, -static_cast<float>(index) / 514.0F};
  }
  auto whole = source;
  auto split = source;
  auto a = take(DelayCompensationRuntime::prepare(config(23, 32)));
  auto b = take(DelayCompensationRuntime::prepare(config(23, 32)));
  a->process(whole, {0, 1});
  for (std::size_t offset = 0; offset < split.size();) {
    const auto count = std::min<std::size_t>(17, split.size() - offset);
    b->process(std::span(split).subspan(offset, count), {offset, 1});
    offset += count;
  }
  for (std::size_t index = 0; index < source.size(); ++index) {
    REQUIRE_EQ(whole[index].left, split[index].left, "delay output depends on callback partition");
    REQUIRE_EQ(whole[index].right, split[index].right,
               "delay output depends on callback partition");
  }
  std::array<StereoFrame, 4> guard{{{1.0F, 1.0F}, {1.0F, 1.0F}, {1.0F, 1.0F}, {1.0F, 1.0F}}};
  b->process(guard, {257, 0});
  require(std::ranges::all_of(guard,
                              [](auto frame) { return frame.left == 0.0F && frame.right == 0.0F; }),
          "zero generation was not rejected");
  guard.fill({1.0F, 1.0F});
  b->process(guard, {256, 1});
  REQUIRE_EQ(guard[0].left, 0.0F, "non-contiguous position was not rejected");
  REQUIRE_EQ(guard[3].right, 0.0F, "non-contiguous position was not rejected");
  guard.fill({1.0F, 1.0F});
  b->process(guard, {0, 2});
  REQUIRE_EQ(guard[0].left, 0.0F, "new generation exposed old delay history");
  REQUIRE_EQ(guard[3].right, 0.0F, "new generation exposed old delay history");
  REQUIRE_EQ(b->history_frames_after_quiesce(), 4, "new generation exposed old delay history");
  guard.fill({1.0F, 1.0F});
  b->process(guard, {4, 1});
  REQUIRE_EQ(guard[0].left, 0.0F, "stale generation was not guarded");
  REQUIRE_EQ(guard[3].right, 0.0F, "stale generation was not guarded");
  REQUIRE_EQ(b->guarded_frames_after_quiesce(), 12, "stale generation was not guarded");
}

void callback_allocation_and_invalid_input() {
  auto line = take(DelayCompensationRuntime::prepare(config(64, 64)));
  std::array<StereoFrame, 128> block{};
  block[0] = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()};
  line->process(block, {0, 1});
  REQUIRE_EQ(block[64].left, 0.0F, "non-finite frame entered delay history");
  REQUIRE_EQ(block[64].right, 0.0F, "non-finite frame entered delay history");
  REQUIRE_EQ(line->guarded_frames_after_quiesce(), 1, "non-finite frame entered delay history");
  counted_allocations = 0;
  counting_allocations = true;
  for (std::uint64_t iteration = 0; iteration < 1'000; ++iteration) {
    block.fill({0.125F, -0.25F});
    line->process(block, {128 + iteration * block.size(), 1});
  }
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "delay callback allocated");
  REQUIRE_EQ(block.back().left, 0.125F, "stereo delay failed after allocation probe");
  REQUIRE_EQ(block.back().right, -0.25F, "stereo delay failed after allocation probe");
}

void planned_loop_wrap_keeps_delay_history() {
  auto line = take(DelayCompensationRuntime::prepare(config(2, 8)));
  std::array<StereoFrame, 4> before{{{}, {}, {}, {1.0F, -1.0F}}};
  line->process(before, {100, 1});
  std::array<StereoFrame, 4> wrapped{};
  line->process(wrapped, {0, 2, 1, true});
  REQUIRE_EQ(wrapped[0].left, 0.0F,
             "planned loop wrap discarded or misaligned prior delay history");
  REQUIRE_EQ(wrapped[1].left, 1.0F,
             "planned loop wrap discarded or misaligned prior delay history");
  REQUIRE_EQ(wrapped[1].right, -1.0F,
             "planned loop wrap discarded or misaligned prior delay history");
  REQUIRE_EQ(wrapped[2].left, 0.0F,
             "planned loop wrap discarded or misaligned prior delay history");
  REQUIRE_EQ(line->guarded_frames_after_quiesce(), 0,
             "planned loop wrap discarded or misaligned prior delay history");
  std::array<StereoFrame, 4> after{};
  line->process(after, {4, 2, 1, false});
  REQUIRE_EQ(line->guarded_frames_after_quiesce(), 0,
             "planned wrap did not restore contiguous delay processing");
  line->process(after, {20, 3});
  REQUIRE_EQ(after[0].left, 0.0F, "ordinary seek reused delay history from an earlier traversal");
  REQUIRE_EQ(after[1].left, 0.0F, "ordinary seek reused delay history from an earlier traversal");
}
} // namespace

int main() {
  try {
    planner_hook_and_parallel_impulses();
    switch_boundary_growth_and_shrink();
    large_history_switch_boundary_impulses();
    unprimeable_transition_holds_prior();
    budgets_partitions_and_transport();
    callback_allocation_and_invalid_input();
    planned_loop_wrap_keeps_delay_history();
    std::cout << "delay compensation runtime contract passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
