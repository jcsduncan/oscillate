#include "oscillate/engine/routed_mixer.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/tolerances.hpp"

#include <algorithm>
#include <array>
#include <atomic>
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
#include <utility>
#include <vector>

namespace tolerance = oscillate::test::tolerance;

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

namespace {
using namespace oscillate;
using namespace oscillate::engine;

using oscillate::test::require;

using oscillate::test::take;

RoutedMixerLimits limits(std::size_t nodes, std::size_t edges, std::uint32_t delay = 32) {
  return {{3, delay},
          64,
          (nodes + 1) * 64 * sizeof(StereoFrame),
          edges * (static_cast<std::uint64_t>(delay) + 1) * sizeof(StereoFrame)};
}

struct HistoryLimitFixture {
  std::size_t nodes;
  std::size_t edges;
  std::size_t block_frames;
  std::uint32_t delay = 16;
};

RoutedMixerLimits source_history_limits(HistoryLimitFixture fixture) {
  auto result = limits(fixture.nodes, fixture.edges, fixture.delay);
  result.max_frames = fixture.block_frames;
  result.max_buffer_bytes = (fixture.nodes + 1) * fixture.block_frames * sizeof(StereoFrame);
  result.max_source_history_bytes =
      fixture.nodes * (static_cast<std::uint64_t>(fixture.delay) + 1) * sizeof(StereoFrame);
  return result;
}

void newly_added_edge_uses_exact_source_history() {
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
  for (const std::size_t block_size : {64U, 128U}) {
    const std::array<RoutedNode, 3> nodes{
        {{{1, 0}, {}}, {{2, 8}, std::make_shared<EightFrameLatency>()}, {{3, 0}, {}}}};
    const std::array<RoutingEdge, 2> original{{{11, 1, 2}, {12, 2, 3}}};
    const std::array<RoutingEdge, 3> added{{original[0], original[1], {13, 1, 3, 0.5F}}};
    auto original_limits = source_history_limits({3, 2, block_size});
    auto added_limits = source_history_limits({3, 3, block_size});
    auto mixer = take(prepare_routed_mixer(nodes, original, original_limits));
    std::vector<StereoFrame> source(block_size);
    std::vector<StereoFrame> output(block_size);
    source[block_size - 4] = {1.0F, -0.5F};
    const std::array<std::span<const StereoFrame>, 3> inputs{source, {}, {}};
    REQUIRE_TRUE(mixer->mix_block(inputs, output, {0, 1}),
                 "pre-switch delayed impulse escaped early");
    REQUIRE_EQ(output.back().left, 0.0F, "pre-switch delayed impulse escaped early");
    auto next =
        take(prepare_routed_mixer(nodes, added, added_limits, mixer.get(), {block_size, 1}));
    REQUIRE_EQ(next->plan().edges.back().compensation_frames, 8,
               "new send did not receive expected compensation");
    std::ranges::fill(source, StereoFrame{});
    source[0] = {0.25F, -0.125F};
    counted_allocations = 0;
    counting_allocations = true;
    const auto rendered = next->mix_block(inputs, output, {block_size, 1});
    counting_allocations = false;
    // At B+4 both the old eight-frame bus and freshly primed send must
    // reproduce the pre-switch impulse: 1 + 0.5, with no earlier arrival.
    require(rendered && counted_allocations == 0 && output[3].left == 0.0F &&
                output[4].left == 1.5F && output[4].right == -0.75F && output[8].left == 0.375F &&
                output[8].right == -0.1875F && output[9].left == 0.0F,
            "new edge was not sample-aligned and gained at the held boundary");

    auto changed = added;
    changed[2].gain = 0.25F;
    auto rapid =
        take(prepare_routed_mixer(nodes, changed, added_limits, next.get(), {block_size * 2, 1}));
    std::ranges::fill(source, StereoFrame{});
    source[block_size - 4] = {1.0F, -0.5F};
    require(rapid->mix_block(inputs, output, {block_size * 2, 1}),
            "rapid compensated send edit failed");
    auto removed = take(
        prepare_routed_mixer(nodes, original, original_limits, rapid.get(), {block_size * 3, 1}));
    std::ranges::fill(source, StereoFrame{});
    REQUIRE_TRUE(removed->mix_block(inputs, output, {block_size * 3, 1}),
                 "route removal altered the continuing bus or processor state");
    REQUIRE_EQ(output[4].left, 1.0F, "route removal altered the continuing bus or processor state");
    REQUIRE_EQ(output[4].right, -0.5F,
               "route removal altered the continuing bus or processor state");
    auto readded =
        take(prepare_routed_mixer(nodes, added, added_limits, removed.get(), {block_size * 4, 1}));
    source[0] = {1.0F, -0.5F};
    REQUIRE_TRUE(readded->mix_block(inputs, output, {block_size * 4, 1}),
                 "readded route lost history across a rapid removal");
    REQUIRE_EQ(output[8].left, 1.5F, "readded route lost history across a rapid removal");
    REQUIRE_EQ(output[8].right, -0.75F, "readded route lost history across a rapid removal");

    std::ranges::fill(source, StereoFrame{});
    require(readded->mix_block(inputs, output, {1000, 2}), "new transport generation was rejected");
    auto stale =
        prepare_routed_mixer(nodes, added, added_limits, readded.get(), {1000 + block_size, 1});
    REQUIRE_TRUE(!stale, "old transport generation reused source history after seek");
    REQUIRE_EQ(stale.error().code, foundation::ErrorCode::unavailable,
               "old transport generation reused source history after seek");
    auto after_seek = take(prepare_routed_mixer(nodes, original, original_limits, readded.get(),
                                                {1000 + block_size, 2}));
    require(after_seek->mix_block(inputs, output, {1000 + block_size, 2}),
            "route removal after seek failed");
    auto sought_add = take(prepare_routed_mixer(nodes, added, added_limits, after_seek.get(),
                                                {1000 + block_size * 2, 2}));
    require(sought_add->mix_block(inputs, output, {1000 + block_size * 2, 2}),
            "source history did not restart on the new transport generation");
  }
}

void new_edge_rejects_insufficient_history_and_budget() {
  const std::array<RoutedNode, 3> nodes{{{{1, 0}, {}}, {{2, 8}, {}}, {{3, 0}, {}}}};
  const std::array<RoutingEdge, 2> original{{{11, 1, 2}, {12, 2, 3}}};
  const std::array<RoutingEdge, 3> added{{original[0], original[1], {13, 1, 3}}};
  auto base = source_history_limits({3, 2, 64});
  auto next_limits = source_history_limits({3, 3, 64});
  --base.max_source_history_bytes;
  require(!prepare_routed_mixer(nodes, original, base),
          "one-byte-short source history budget was accepted");
  base = source_history_limits({3, 2, 64});
  auto mixer = take(prepare_routed_mixer(nodes, original, base));
  std::array<StereoFrame, 4> source{};
  std::array<StereoFrame, 4> output{};
  const std::array<std::span<const StereoFrame>, 3> inputs{source, {}, {}};
  require(mixer->mix_block(inputs, output, {0, 1}), "short warmup failed");
  auto early = prepare_routed_mixer(nodes, added, next_limits, mixer.get(), {4, 1});
  REQUIRE_TRUE(!early, "new eight-frame route accepted four source frames");
  REQUIRE_EQ(early.error().code, foundation::ErrorCode::unavailable,
             "new eight-frame route accepted four source frames");
  auto stale = prepare_routed_mixer(nodes, added, next_limits, mixer.get(), {4, 2});
  REQUIRE_TRUE(!stale, "different generation accepted source history");
  REQUIRE_EQ(stale.error().code, foundation::ErrorCode::unavailable,
             "different generation accepted source history");
  auto reused_id = added;
  reused_id[2].id = 11;
  reused_id[0].id = 14;
  auto conflict = prepare_routed_mixer(nodes, reused_id, next_limits, mixer.get(), {4, 1});
  REQUIRE_TRUE(!conflict, "edge ID was reused for different endpoints");
  REQUIRE_EQ(conflict.error().code, foundation::ErrorCode::conflict,
             "edge ID was reused for different endpoints");
  source[0] = {1.0F, -1.0F};
  REQUIRE_TRUE(mixer->mix_block(inputs, output, {4, 1}),
               "failed preparation damaged the previous graph");
  REQUIRE_EQ(output[0].left, 1.0F, "failed preparation damaged the previous graph");
  source.fill({});
  require(mixer->mix_block(inputs, output, {1000, 2}), "seek fixture failed");
  auto short_seek = prepare_routed_mixer(nodes, added, next_limits, mixer.get(), {1004, 2});
  REQUIRE_TRUE(!short_seek,
               "source history from the old generation leaked into a short post-seek warmup");
  REQUIRE_EQ(short_seek.error().code, foundation::ErrorCode::unavailable,
             "source history from the old generation leaked into a short post-seek warmup");

  auto history_off_compensated = take(prepare_routed_mixer(nodes, original, limits(3, 2, 16)));
  require(history_off_compensated->mix_block(inputs, output, {0, 1}),
          "history-off compensated graph failed");
  auto late_opt_in =
      prepare_routed_mixer(nodes, added, next_limits, history_off_compensated.get(), {4, 1});
  REQUIRE_TRUE(!late_opt_in, "enabling history on the successor fabricated prior source samples");
  REQUIRE_EQ(late_opt_in.error().code, foundation::ErrorCode::unavailable,
             "enabling history on the successor fabricated prior source samples");

  const std::array<RoutedNode, 2> flat{{{{1, 0}, {}}, {{3, 0}, {}}}};
  const std::array<RoutingEdge, 1> flat_original{{{11, 1, 3}}};
  const std::array<RoutingEdge, 2> flat_added{{flat_original[0], {12, 1, 3, 0.5F}}};
  source[0] = {1.0F, -1.0F};
  auto flat_mixer = take(prepare_routed_mixer(flat, flat_original, limits(2, 1, 0)));
  const std::array<std::span<const StereoFrame>, 2> flat_inputs{source, {}};
  require(flat_mixer->mix_block(flat_inputs, output, {0, 1}), "flat source failed");
  auto no_prime =
      take(prepare_routed_mixer(flat, flat_added, limits(2, 2, 0), flat_mixer.get(), {4, 1}));
  REQUIRE_TRUE(no_prime->mix_block(flat_inputs, output, {4, 1}),
               "zero-delay edge unnecessarily required historical prime");
  REQUIRE_EQ(output[0].left, 1.5F, "zero-delay edge unnecessarily required historical prime");
}

void parallel_routes_and_bus_processor() {
  struct Twice final : ProcessorRuntime {
    void process(std::span<StereoFrame> frames, BlockContext) noexcept override {
      for (auto& frame : frames) {
        frame.left *= 2.0F;
        frame.right *= 2.0F;
      }
    }
  };
  const std::array<RoutedNode, 3> nodes{
      {{{1, 0}, {}}, {{2, 8}, {}}, {{3, 0}, std::make_shared<Twice>()}}};
  const std::array<RoutingEdge, 2> edges{{{11, 1, 3}, {12, 2, 3}}};
  auto mixer = take(prepare_routed_mixer(nodes, edges, limits(3, 2)));
  REQUIRE_EQ(mixer->plan().edges[0].compensation_frames, 8,
             "routing planner was not connected to mixer");
  REQUIRE_EQ(mixer->max_frames(), 64, "routing planner was not connected to mixer");
  std::array<StereoFrame, 32> fast{};
  std::array<StereoFrame, 32> slow{};
  std::array<StereoFrame, 32> master{};
  fast[0] = {1.0F, -0.25F};
  slow[8] = {1.0F, -0.25F}; // Fixture represents the slow node's declared latency.
  const std::array<std::span<const StereoFrame>, 3> inputs{fast, slow, {}};
  require(mixer->mix_block(inputs, master, {0, 1}), "routed block rejected");
  REQUIRE_EQ(master[7].left, 0.0F, "parallel route arrivals or master processor were wrong");
  REQUIRE_EQ(master[8].left, 4.0F, "parallel route arrivals or master processor were wrong");
  REQUIRE_EQ(master[8].right, -1.0F, "parallel route arrivals or master processor were wrong");
  REQUIRE_EQ(master[9].left, 0.0F, "parallel route arrivals or master processor were wrong");
}

void dynamic_edge_gain_uses_project_sample_values() {
  const std::array<RoutedNode, 2> nodes{{{{1, 0}, {}}, {{3, 0}, {}}}};
  const std::array<RoutingEdge, 1> edges{{{11, 1, 3, 0.5F}}};
  auto mixer = take(prepare_routed_mixer(nodes, edges, limits(2, 1)));
  const std::array<StereoFrame, 4> source{
      {{1.0F, -1.0F}, {1.0F, -1.0F}, {1.0F, -1.0F}, {1.0F, -1.0F}}};
  const std::array<std::span<const StereoFrame>, 2> inputs{source, {}};
  std::array<StereoFrame, 4> output{};
  const std::array<float, 4> values{0.0F, 0.5F, 1.0F, 2.0F};
  const std::array<EdgeGainSpan, 1> gains{{{11, values}}};
  counting_allocations = true;
  const auto rendered = mixer->mix_block(inputs, output, {0, 1}, gains);
  counting_allocations = false;
  REQUIRE_TRUE(rendered, "dynamic send gain did not apply per project sample without allocation");
  REQUIRE_EQ(counted_allocations, 0,
             "dynamic send gain did not apply per project sample without allocation");
  REQUIRE_EQ(output[0].left, 0.0F,
             "dynamic send gain did not apply per project sample without allocation");
  REQUIRE_EQ(output[1].left, 0.5F,
             "dynamic send gain did not apply per project sample without allocation");
  REQUIRE_EQ(output[2].left, 1.0F,
             "dynamic send gain did not apply per project sample without allocation");
  REQUIRE_EQ(output[3].left, 2.0F,
             "dynamic send gain did not apply per project sample without allocation");
  REQUIRE_EQ(output[3].right, -2.0F,
             "dynamic send gain did not apply per project sample without allocation");
  const std::array<EdgeGainSpan, 1> wrong_id{{{12, values}}};
  REQUIRE_TRUE(!mixer->mix_block(inputs, output, {4, 1}, wrong_id),
               "wrong edge gain identity was accepted");
  REQUIRE_EQ(output[0].left, 0.0F, "wrong edge gain identity was accepted");
  const std::array<float, 4> invalid_values{0.0F, 0.5F, std::numeric_limits<float>::quiet_NaN(),
                                            2.0F};
  const std::array<EdgeGainSpan, 1> invalid{{{11, invalid_values}}};
  REQUIRE_TRUE(!mixer->mix_block(inputs, output, {4, 1}, invalid),
               "invalid gain changed routed history or cursor");
  REQUIRE_TRUE(mixer->mix_block(inputs, output, {4, 1}, gains),
               "invalid gain changed routed history or cursor");
  REQUIRE_EQ(output[2].left, 1.0F, "invalid gain changed routed history or cursor");
}

void multihop_bus_aligns_with_direct_send() {
  struct FourFrameLatency final : ProcessorRuntime {
    std::array<StereoFrame, 4> history{};
    std::size_t cursor = 0;
    void process(std::span<StereoFrame> frames, BlockContext) noexcept override {
      for (auto& frame : frames) {
        std::swap(frame, history[cursor]);
        cursor = (cursor + 1) % history.size();
      }
    }
  };
  const std::array<RoutedNode, 3> nodes{
      {{{1, 0}, {}}, {{2, 4}, std::make_shared<FourFrameLatency>()}, {{3, 0}, {}}}};
  const std::array<RoutingEdge, 3> edges{{{11, 1, 2}, {12, 2, 3}, {13, 1, 3}}};
  auto mixer = take(prepare_routed_mixer(nodes, edges, limits(3, 3)));
  std::array<StereoFrame, 16> source{};
  std::array<StereoFrame, 16> output{};
  source[0] = {0.25F, -0.5F};
  const std::array<std::span<const StereoFrame>, 3> inputs{source, std::span<const StereoFrame>{},
                                                           std::span<const StereoFrame>{}};
  REQUIRE_TRUE(mixer->mix_block(inputs, output, {0, 1}),
               "multihop bus and direct send did not align at master");
  REQUIRE_EQ(output[3].left, 0.0F, "multihop bus and direct send did not align at master");
  REQUIRE_EQ(output[4].left, 0.5F, "multihop bus and direct send did not align at master");
  REQUIRE_EQ(output[4].right, -1.0F, "multihop bus and direct send did not align at master");
  REQUIRE_EQ(output[5].left, 0.0F, "multihop bus and direct send did not align at master");
}

void gained_main_send_and_history_transition() {
  struct FourFrameLatency final : ProcessorRuntime {
    std::array<StereoFrame, 4> history{};
    std::size_t cursor = 0;
    void process(std::span<StereoFrame> frames, BlockContext) noexcept override {
      for (auto& frame : frames) {
        std::swap(frame, history[cursor]);
        cursor = (cursor + 1) % history.size();
      }
    }
  };
  const auto processor = std::make_shared<FourFrameLatency>();
  const std::array<RoutedNode, 3> nodes{{{{1, 0}, {}}, {{2, 4}, processor}, {{3, 0}, {}}}};
  std::array<RoutingEdge, 3> edges{{{11, 1, 2, 0.5F}, {12, 2, 3}, {13, 1, 3, 0.25F}}};
  auto mixer = take(prepare_routed_mixer(nodes, edges, limits(3, 3)));
  REQUIRE_EQ(mixer->plan().edges[2].compensation_frames, 4,
             "send gain or compensation absent from immutable route plan");
  REQUIRE_EQ(mixer->plan().edges[2].gain, 0.25F,
             "send gain or compensation absent from immutable route plan");
  std::array<StereoFrame, 64> source{};
  std::array<StereoFrame, 64> output{};
  source[0] = {1.0F, -0.5F};
  source[60] = {1.0F, -0.5F};
  const std::array<std::span<const StereoFrame>, 3> inputs{source, {}, {}};
  REQUIRE_TRUE(mixer->mix_block(inputs, output, {0, 1}),
               "independent main plus send gain/latency oracle failed");
  REQUIRE_EQ(output[3].left, 0.0F, "independent main plus send gain/latency oracle failed");
  REQUIRE_EQ(output[4].left, 0.75F, "independent main plus send gain/latency oracle failed");
  REQUIRE_EQ(output[4].right, -0.375F, "independent main plus send gain/latency oracle failed");

  edges[2].gain = 0.5F;
  auto next = take(prepare_routed_mixer(nodes, edges, limits(3, 3), mixer.get(), {64, 1}));
  source.fill({});
  REQUIRE_TRUE(next->mix_block(inputs, output, {64, 1}),
               "changed send gain lost primed route history or processor state");
  REQUIRE_EQ(output[0].left, 1.0F,
             "changed send gain lost primed route history or processor state");
  REQUIRE_EQ(output[0].right, -0.5F,
             "changed send gain lost primed route history or processor state");
  REQUIRE_EQ(output[1].left, 0.0F,
             "changed send gain lost primed route history or processor state");

  edges[2].gain = 0.0F;
  auto muted = take(prepare_routed_mixer(nodes, edges, limits(3, 3), next.get(), {128, 1}));
  source[0] = {1.0F, -0.5F};
  REQUIRE_TRUE(muted->mix_block(inputs, output, {128, 1}),
               "zero-gain send did not leave the main route audible");
  REQUIRE_EQ(output[4].left, 0.5F, "zero-gain send did not leave the main route audible");
  REQUIRE_EQ(output[4].right, -0.25F, "zero-gain send did not leave the main route audible");
}

void non_finite_processor_output_is_guarded() {
  struct Poison final : ProcessorRuntime {
    void process(std::span<StereoFrame> frames, BlockContext) noexcept override {
      for (auto& frame : frames) {
        frame.left = std::numeric_limits<float>::quiet_NaN();
        frame.right = std::numeric_limits<float>::infinity();
      }
    }
  };
  const std::array<RoutedNode, 1> nodes{{{{3, 0}, std::make_shared<Poison>()}}};
  auto mixer = take(prepare_routed_mixer(nodes, {}, limits(1, 0, 0)));
  std::array<StereoFrame, 4> output{};
  const std::array<std::span<const StereoFrame>, 1> input{{}};
  REQUIRE_TRUE(mixer->mix_block(input, output, {0, 1}),
               "non-finite master processor output escaped routed mixer");
  REQUIRE_EQ(output[0].left, 0.0F, "non-finite master processor output escaped routed mixer");
  REQUIRE_EQ(output[3].right, 0.0F, "non-finite master processor output escaped routed mixer");
}

void transition_growth_shrink_and_failed_hold() {
  std::array<RoutedNode, 3> nodes{{{{1, 0}, {}}, {{2, 8}, {}}, {{3, 0}, {}}}};
  const std::array<RoutingEdge, 2> edges{{{11, 1, 3}, {12, 2, 3}}};
  auto mixer = take(prepare_routed_mixer(nodes, edges, limits(3, 2)));
  std::array<StereoFrame, 64> fast{};
  std::array<StereoFrame, 64> slow{};
  std::array<StereoFrame, 64> output{};
  fast[48] = {0.5F, -0.25F};
  fast[56] = {1.0F, -0.5F};
  slow[56] = {0.5F, -0.25F};
  const std::array<std::span<const StereoFrame>, 3> first{fast, slow, {}};
  REQUIRE_TRUE(mixer->mix_block(first, output, {0, 1}), "prior routed output did not align");
  REQUIRE_EQ(output[56].left, 1.0F, "prior routed output did not align");

  nodes[1].routing.processor_latency_frames = 16;
  auto unready = prepare_routed_mixer(nodes, edges, limits(3, 2), mixer.get(), {63, 1});
  REQUIRE_TRUE(!unready, "unquiesced transition did not hold prior mixer");
  REQUIRE_EQ(unready.error().code, foundation::ErrorCode::unavailable,
             "unquiesced transition did not hold prior mixer");
  auto next = take(prepare_routed_mixer(nodes, edges, limits(3, 2), mixer.get(), {64, 1}));
  fast.fill({});
  slow.fill({});
  fast[0] = {0.25F, -0.125F};
  slow[16] = {0.25F, -0.125F};
  const std::array<std::span<const StereoFrame>, 3> second{fast, slow, {}};
  REQUIRE_TRUE(next->mix_block(second, output, {64, 1}),
               "latency growth failed history prime or aligned impulse");
  REQUIRE_EQ(output[0].left, 0.5F, "latency growth failed history prime or aligned impulse");
  REQUIRE_EQ(output[16].left, 0.5F, "latency growth failed history prime or aligned impulse");

  nodes[1].routing.processor_latency_frames = 4;
  auto shorter = take(prepare_routed_mixer(nodes, edges, limits(3, 2), next.get(), {128, 1}));
  fast.fill({});
  slow.fill({});
  fast[0] = {0.75F, -0.25F};
  slow[4] = {0.75F, -0.25F};
  const std::array<std::span<const StereoFrame>, 3> third{fast, slow, {}};
  REQUIRE_TRUE(shorter->mix_block(third, output, {128, 1}), "latency shrink did not align impulse");
  REQUIRE_EQ(output[4].left, 1.5F, "latency shrink did not align impulse");

  auto over = limits(3, 2, 2);
  auto rejected = prepare_routed_mixer(nodes, edges, over, shorter.get(), {192, 1});
  REQUIRE_TRUE(!rejected, "over-budget update damaged active route");
  REQUIRE_EQ(rejected.error().code, foundation::ErrorCode::resource_exhausted,
             "over-budget update damaged active route");
  REQUIRE_EQ(shorter->plan().edges[0].compensation_frames, 4,
             "over-budget update damaged active route");

  const std::array<RoutingEdge, 2> alternate{{{12, 2, 3}, {13, 1, 2}}};
  auto alternate_mixer = take(prepare_routed_mixer(nodes, alternate, limits(3, 2)));
  const std::array<std::span<const StereoFrame>, 3> silent{std::span<const StereoFrame>{},
                                                           std::span<const StereoFrame>{},
                                                           std::span<const StereoFrame>{}};
  require(alternate_mixer->mix_block(silent, output, {0, 1}),
          "prior alternate route did not render");
  const std::array<RoutingEdge, 3> added{{alternate[0], alternate[1], {11, 1, 3}}};
  auto no_history =
      prepare_routed_mixer(nodes, added, limits(3, 3), alternate_mixer.get(), {64, 1});
  REQUIRE_TRUE(!no_history, "new delayed route activated without primed history");
  REQUIRE_EQ(no_history.error().code, foundation::ErrorCode::unavailable,
             "new delayed route activated without primed history");
}

void state_reuse_and_rejection() {
  struct Counter final : ProcessorRuntime {
    float phase = 0;
    void process(std::span<StereoFrame> frames, BlockContext) noexcept override {
      for (auto& frame : frames) {
        frame.left += ++phase;
      }
    }
  };
  auto running = std::make_shared<Counter>();
  std::array<RoutedNode, 2> nodes{{{{1, 0}, running}, {{3, 0}, {}}}};
  const std::array<RoutingEdge, 1> edges{{{11, 1, 3}}};
  auto first_limits = limits(2, 1, 0);
  auto mixer = take(prepare_routed_mixer(nodes, edges, first_limits));
  std::array<StereoFrame, 4> output{};
  const std::array<std::span<const StereoFrame>, 2> inputs{std::span<const StereoFrame>{},
                                                           std::span<const StereoFrame>{}};
  REQUIRE_TRUE(mixer->mix_block(inputs, output, {0, 1}), "stateful route fixture failed");
  REQUIRE_EQ(output[3].left, 4.0F, "stateful route fixture failed");
  auto same = take(prepare_routed_mixer(nodes, edges, first_limits, mixer.get(), {4, 1}));
  REQUIRE_TRUE(same->mix_block(inputs, output, {4, 1}),
               "topology transition reset processor runtime");
  REQUIRE_EQ(output[0].left, 5.0F, "topology transition reset processor runtime");
  struct RetainedStage final : ProcessorRuntime {
    explicit RetainedStage(std::shared_ptr<Counter> stage) : stage(std::move(stage)) {}
    void process(std::span<StereoFrame> frames, BlockContext context) noexcept override {
      stage->process(frames, context);
    }
    [[nodiscard]] bool contains(const ProcessorRuntime* owned) const noexcept override {
      return owned == this || owned == stage.get();
    }
    std::shared_ptr<Counter> stage;
  };
  std::array<RoutedNode, 2> retained{
      {{{1, 0}, std::make_shared<RetainedStage>(running)}, {{3, 0}, {}}}};
  retained[0].state_handoff_prior = std::make_shared<Counter>();
  auto wrong_prior = prepare_routed_mixer(retained, edges, first_limits, same.get(), {8, 1});
  REQUIRE_TRUE(!wrong_prior, "state handoff accepted an unrelated predecessor");
  REQUIRE_EQ(wrong_prior.error().code, foundation::ErrorCode::conflict,
             "state handoff accepted an unrelated predecessor");
  retained[0].state_handoff_prior = running;
  auto attached = take(prepare_routed_mixer(retained, edges, first_limits, same.get(), {8, 1}));
  counted_allocations = 0;
  counting_allocations = true;
  const bool attached_rendered = attached->mix_block(inputs, output, {8, 1});
  counting_allocations = false;
  REQUIRE_TRUE(attached_rendered, "retained stage state was reset or processed twice at handoff");
  REQUIRE_EQ(counted_allocations, 0,
             "retained stage state was reset or processed twice at handoff");
  REQUIRE_EQ(output[0].left, 9.0F, "retained stage state was reset or processed twice at handoff");
  REQUIRE_EQ(output[3].left, 12.0F, "retained stage state was reset or processed twice at handoff");
  nodes[1].processor = running;
  auto duplicate = prepare_routed_mixer(nodes, edges, first_limits);
  REQUIRE_TRUE(!duplicate, "one processor owner was accepted on two live routes");
  REQUIRE_EQ(duplicate.error().code, foundation::ErrorCode::conflict,
             "one processor owner was accepted on two live routes");
  nodes[1].processor.reset();
  nodes[0].processor = std::make_shared<Counter>();
  auto changed = prepare_routed_mixer(nodes, edges, first_limits, same.get(), {8, 1});
  REQUIRE_TRUE(!changed, "stateful processor replacement was silently accepted");
  REQUIRE_EQ(changed.error().code, foundation::ErrorCode::conflict,
             "stateful processor replacement was silently accepted");
  nodes[0].replacement = RoutedTailReplacement{running, 8};
  nodes[1].processor = running;
  auto stolen = prepare_routed_mixer(nodes, edges, first_limits, same.get(), {8, 1});
  REQUIRE_TRUE(!stolen, "draining predecessor was reused on another live route");
  REQUIRE_EQ(stolen.error().code, foundation::ErrorCode::conflict,
             "draining predecessor was reused on another live route");
  nodes[1].processor.reset();
  auto draining = take(prepare_routed_mixer(nodes, edges, first_limits, same.get(), {8, 1}));
  require(draining->mix_block(inputs, output, {8, 1}), "bounded drain did not start");
  const std::array<RoutedNode, 1> only_master{{{{3, 0}, {}}}};
  auto removed = prepare_routed_mixer(only_master, {}, limits(1, 0, 0), draining.get(), {12, 1});
  REQUIRE_TRUE(!removed, "route removal truncated an active predecessor tail");
  REQUIRE_EQ(removed.error().code, foundation::ErrorCode::conflict,
             "route removal truncated an active predecessor tail");

  auto retiring = std::make_shared<Counter>();
  std::array<RoutedNode, 2> handoff_nodes{{{{1, 0}, retiring}, {{3, 0}, {}}}};
  auto live = take(prepare_routed_mixer(handoff_nodes, edges, first_limits));
  REQUIRE_TRUE(live->mix_block(inputs, output, {0, 1}),
               "predecessor processor did not fill its initial history");
  REQUIRE_EQ(output[3].left, 4.0F, "predecessor processor did not fill its initial history");
  handoff_nodes[0].processor.reset();
  handoff_nodes[0].replacement = RoutedTailReplacement{retiring, 12};
  auto dry = take(prepare_routed_mixer(handoff_nodes, edges, first_limits, live.get(), {4, 1}));
  REQUIRE_TRUE(dry->mix_block(inputs, output, {4, 1}),
               "dry successor did not retain its predecessor tail");
  REQUIRE_EQ(output[0].left, 5.0F, "dry successor did not retain its predecessor tail");
  REQUIRE_EQ(output[3].left, 8.0F, "dry successor did not retain its predecessor tail");
  handoff_nodes[0].replacement.reset();
  handoff_nodes[0].processor = std::make_shared<Counter>();
  auto resumed = take(prepare_routed_mixer(handoff_nodes, edges, first_limits, dry.get(), {8, 1}));
  counted_allocations = 0;
  counting_allocations = true;
  const bool mixed = resumed->mix_block(inputs, output, {8, 1});
  counting_allocations = false;
  REQUIRE_TRUE(mixed, "new processor did not sum with inherited tail without callback allocation");
  REQUIRE_EQ(counted_allocations, 0,
             "new processor did not sum with inherited tail without callback allocation");
  REQUIRE_EQ(output[0].left, 10.0F,
             "new processor did not sum with inherited tail without callback allocation");
  REQUIRE_EQ(output[3].left, 16.0F,
             "new processor did not sum with inherited tail without callback allocation");
  const auto resumed_owner = std::dynamic_pointer_cast<Counter>(handoff_nodes[0].processor);
  handoff_nodes[0].processor = std::make_shared<RetainedStage>(resumed_owner);
  handoff_nodes[0].state_handoff_prior = resumed_owner;
  // An earlier predecessor still draining carries on beside a handoff that
  // retains a different stage.
  auto carried = prepare_routed_mixer(handoff_nodes, edges, first_limits, resumed.get(), {12, 1});
  REQUIRE_OK(carried, "state handoff refused beside an unrelated running drain");
  // A successor may never run a processor that is still draining.
  handoff_nodes[0].processor = std::make_shared<RetainedStage>(retiring);
  auto doubled = prepare_routed_mixer(handoff_nodes, edges, first_limits, resumed.get(), {12, 1});
  REQUIRE_TRUE(!doubled, "a successor was allowed to run a processor that is still draining");
  REQUIRE_EQ(doubled.error().code, foundation::ErrorCode::conflict,
             "a successor was allowed to run a processor that is still draining");
}

void invalid_inputs_budgets_and_callback_allocations() {
  const std::array<RoutedNode, 3> nodes{{{{1, 0}, {}}, {{2, 4}, {}}, {{3, 0}, {}}}};
  const std::array<RoutingEdge, 2> edges{{{11, 1, 3}, {12, 2, 3}}};
  auto small = limits(3, 2);
  --small.max_buffer_bytes;
  require(!prepare_routed_mixer(nodes, edges, small), "buffer budget ignored");
  small = limits(3, 2);
  --small.max_history_bytes;
  require(!prepare_routed_mixer(nodes, edges, small), "history budget ignored");
  small = limits(3, 2);
  small.max_frames = 8193;
  require(!prepare_routed_mixer(nodes, edges, small), "invalid block bound accepted");
  const std::array<RoutingEdge, 3> cycle{{edges[0], edges[1], {13, 3, 1}}};
  require(!prepare_routed_mixer(nodes, cycle, limits(3, 3)), "cycle accepted");

  auto mixer = take(prepare_routed_mixer(nodes, edges, limits(3, 2)));
  std::array<StereoFrame, 64> input{};
  std::array<StereoFrame, 64> output{};
  input[0] = {1.0F, -1.0F};
  const std::array<std::span<const StereoFrame>, 3> sources{input, {}, {}};
  counted_allocations = 0;
  counting_allocations = true;
  for (std::uint64_t block = 0; block < 1000; ++block) {
    require(mixer->mix_block(sources, output, {block * 64, 1}), "valid block rejected");
  }
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "routed callback allocated");
  REQUIRE_EQ(output[4].left, 1.0F, "routed callback lost an impulse");
  REQUIRE_EQ(output[4].right, -1.0F, "routed callback lost an impulse");
  output.fill({1.0F, 1.0F});
  const std::array<std::span<const StereoFrame>, 2> wrong_sources{std::span<const StereoFrame>{},
                                                                  std::span<const StereoFrame>{}};
  REQUIRE_TRUE(!mixer->mix_block(wrong_sources, output, {64000, 1}),
               "invalid source count was not silenced");
  REQUIRE_EQ(output[0].left, 0.0F, "invalid source count was not silenced");
  REQUIRE_TRUE(!mixer->mix_block(sources, output, {64000, 0}),
               "invalid generation was not silenced");
  REQUIRE_EQ(output[0].left, 0.0F, "invalid generation was not silenced");
  const std::array<std::span<const StereoFrame>, 3> short_sources{
      std::span(input).first(2), std::span<const StereoFrame>{}, std::span<const StereoFrame>{}};
  output.fill({1.0F, 1.0F});
  REQUIRE_TRUE(!mixer->mix_block(short_sources, output, {64000, 1}),
               "short source span was not rejected before route history moved");
  REQUIRE_EQ(output[0].left, 0.0F, "short source span was not rejected before route history moved");
  const std::array<std::span<const StereoFrame>, 3> empty_sources{std::span<const StereoFrame>{},
                                                                  std::span<const StereoFrame>{},
                                                                  std::span<const StereoFrame>{}};
  require(mixer->mix_block(empty_sources, std::span<StereoFrame>{}, {64000, 1}),
          "zero-frame block should leave route state untouched");
  std::array<StereoFrame, 8193> excessive{};
  excessive.fill({1.0F, 1.0F});
  REQUIRE_TRUE(!mixer->mix_block(sources, excessive, {64000, 1}),
               "over-hard-cap adapter span caused unbounded callback work");
  REQUIRE_EQ(excessive.back().left, 1.0F,
             "over-hard-cap adapter span caused unbounded callback work");
  REQUIRE_TRUE(!mixer->mix_block(sources, output, {63936, 1}),
               "stale transport position was not rejected");
  REQUIRE_EQ(output[0].left, 0.0F, "stale transport position was not rejected");
}

void muted_nodes_silence_output_even_with_dynamic_send_gain() {
  const std::array<RoutingEdge, 2> edges{{{11, 1, 2}, {12, 2, 3}}};
  const std::array<RoutedNode, 3> source_muted{
      {{{1, 0}, {}, {}, true}, {{2, 0}, {}}, {{3, 0}, {}}}};
  auto mixer = take(prepare_routed_mixer(source_muted, edges, limits(3, 2)));
  std::array<StereoFrame, 64> input{};
  input.fill({0.5F, -0.5F});
  std::array<StereoFrame, 64> output{};
  const std::array<std::span<const StereoFrame>, 3> sources{input, {}, {}};
  std::array<float, 64> boosted{};
  boosted.fill(4.0F);
  const std::array<EdgeGainSpan, 2> dynamic{{{11, boosted}, {12, boosted}}};
  require(mixer->mix_block(sources, output, {0, 1}, dynamic), "muted source block rejected");
  require(std::ranges::all_of(output, [](StereoFrame frame) { return frame.left == 0.0F; }),
          "dynamic send gain bypassed source mute");

  const std::array<RoutedNode, 3> bus_muted{{{{1, 0}, {}}, {{2, 0}, {}, {}, true}, {{3, 0}, {}}}};
  auto bus = take(prepare_routed_mixer(bus_muted, edges, limits(3, 2)));
  counted_allocations = 0;
  counting_allocations = true;
  require(bus->mix_block(sources, output, {0, 1}), "muted bus block rejected");
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "muted routed callback allocated");
  require(std::ranges::all_of(output, [](StereoFrame frame) { return frame.left == 0.0F; }),
          "muted bus leaked its summed input");
}

void skipped_muted_nodes_do_no_work_and_change_nothing() {
  struct Counted final : SidechainProcessorRuntime {
    int calls = 0;
    void process(std::span<StereoFrame> frames, BlockContext) noexcept override {
      ++calls;
      for (auto& frame : frames)
        frame = {frame.left * 0.5F + 0.25F, frame.right * 0.5F - 0.25F};
    }
    void process_with_sidechain(std::span<StereoFrame> frames, std::span<const StereoFrame> key,
                                BlockContext context) noexcept override {
      process(frames, context);
      for (std::size_t index = 0; index < frames.size(); ++index)
        frames[index].left += key[index].left;
    }
  };
  // 1 (muted) and 2 (heard) feed master 3; 4 (muted) keys 2.
  const auto render = [](bool skip) {
    auto muted = std::make_shared<Counted>();
    auto heard = std::make_shared<Counted>();
    auto key = std::make_shared<Counted>();
    const std::array<RoutedNode, 4> nodes{
        {{{1, 0}, muted, {}, true}, {{2, 0}, heard}, {{3, 0}, {}}, {{4, 0}, key, {}, true}}};
    const std::array<RoutingEdge, 3> edges{
        {{11, 1, 3}, {12, 2, 3}, {13, 4, 2, 1.0F, RoutingEdgeKind::sidechain}}};
    auto bounded = limits(4, 3);
    bounded.max_buffer_bytes += 2 * 64 * sizeof(StereoFrame); // detector and key tap
    bounded.max_history_bytes += 64 * sizeof(StereoFrame);    // the key's input delay
    bounded.skip_muted_nodes = skip;
    auto mixer = take(prepare_routed_mixer(nodes, edges, bounded));
    std::array<StereoFrame, 64> input{};
    for (std::size_t index = 0; index < input.size(); ++index)
      input[index] = {0.01F * static_cast<float>(index), -0.02F * static_cast<float>(index)};
    const std::array<std::span<const StereoFrame>, 4> sources{input, input, {}, input};
    std::array<StereoFrame, 64> output{};
    std::vector<StereoFrame> all;
    for (std::uint64_t block = 0; block < 3; ++block) {
      require(mixer->mix_block(sources, output, {block * 64, 1}), "skip fixture block rejected");
      all.insert(all.end(), output.begin(), output.end());
    }
    return std::tuple{all, muted->calls, heard->calls, key->calls};
  };
  const auto [plain, plain_muted, plain_heard, plain_key] = render(false);
  const auto [skipped, skip_muted, skip_heard, skip_key] = render(true);
  REQUIRE_EQ(plain_muted, 3, "a muted node still ran its processor");
  REQUIRE_EQ(skip_muted, 0, "a muted node still ran its processor");
  REQUIRE_EQ(skip_heard, 3, "skipping stopped a heard node or a muted key");
  REQUIRE_EQ(skip_key, 3, "skipping stopped a heard node or a muted key");
  require(std::ranges::equal(plain, skipped,
                             [](StereoFrame left, StereoFrame right) {
                               return left.left == right.left && left.right == right.right;
                             }),
          "skipping muted nodes changed the mix");
  (void)plain_heard;
  (void)plain_key;
}

void pipelined_steps_match_block_by_block_mixing() {
  // Stateful, so any reordering of a node's blocks would show.
  struct Smooth final : SidechainProcessorRuntime {
    StereoFrame state{};
    void process(std::span<StereoFrame> frames, BlockContext) noexcept override {
      for (auto& frame : frames) {
        state = {state.left * 0.7F + frame.left * 0.3F, state.right * 0.6F + frame.right * 0.4F};
        frame = {frame.left + state.left, frame.right - state.right};
      }
    }
    void process_with_sidechain(std::span<StereoFrame> frames, std::span<const StereoFrame> key,
                                BlockContext context) noexcept override {
      process(frames, context);
      for (std::size_t index = 0; index < frames.size(); ++index)
        frames[index].right *= 1.0F - 0.5F * std::min(1.0F, std::abs(key[index].left));
    }
  };
  struct FourFrameLatency final : ProcessorRuntime {
    std::array<StereoFrame, 4> history{};
    std::size_t cursor = 0;
    void process(std::span<StereoFrame> frames, BlockContext) noexcept override {
      for (auto& frame : frames) {
        std::swap(frame, history[cursor]);
        cursor = (cursor + 1) % history.size();
      }
    }
  };
  // Sources 1 (A), 2 (latent B) and 3 (a muted key) at level 0; a keyed bus
  // 4 and a return 5 at level 1; master 6 at level 2.
  const auto nodes = [] {
    return std::array<RoutedNode, 6>{{{{1, 0}, std::make_shared<Smooth>()},
                                      {{2, 4}, std::make_shared<FourFrameLatency>()},
                                      {{3, 0}, std::make_shared<Smooth>(), {}, true},
                                      {{4, 0}, std::make_shared<Smooth>()},
                                      {{5, 0}, std::make_shared<Smooth>()},
                                      {{6, 0}, std::make_shared<Smooth>()}}};
  };
  const std::array<RoutingEdge, 6> edges{{{11, 1, 4},
                                          {12, 2, 4},
                                          {13, 4, 6},
                                          {14, 1, 5, 0.5F},
                                          {15, 5, 6},
                                          {16, 3, 4, 1.0F, RoutingEdgeKind::sidechain}}};
  auto bounded = limits(6, 6, 8);
  bounded.latency.master = 6;
  bounded.max_buffer_bytes += 2 * 64 * sizeof(StereoFrame); // detector and key tap
  bounded.max_history_bytes += 64 * sizeof(StereoFrame);    // the key's input delay
  constexpr std::array<std::size_t, 5> block_sizes{64, 64, 17, 64, 37};
  const auto source_at = [](std::size_t node, std::uint64_t frame) {
    const auto phase = static_cast<float>(frame) * (0.031F + 0.017F * static_cast<float>(node));
    return StereoFrame{std::sin(phase), std::cos(phase * 1.3F)};
  };
  std::array<std::vector<StereoFrame>, 6> sources;
  std::vector<float> send_gain;
  const auto fill = [&](std::uint64_t first, std::size_t frames) {
    for (std::size_t node = 0; node < 3; ++node) {
      sources[node].resize(frames);
      for (std::size_t index = 0; index < frames; ++index)
        sources[node][index] = source_at(node, first + index);
    }
    send_gain.resize(frames);
    for (std::size_t index = 0; index < frames; ++index)
      send_gain[index] = 0.25F + 0.5F * static_cast<float>((first + index) % 50) / 50.0F;
  };
  const auto views = [&] {
    return std::array<std::span<const StereoFrame>, 6>{sources[0], sources[1], sources[2],
                                                       {},         {},         {}};
  };
  const auto gains = [&](std::span<const RoutingEdge> plan) {
    std::array<EdgeGainSpan, 6> spans{};
    for (std::size_t edge = 0; edge < spans.size(); ++edge)
      spans[edge] = {plan[edge].id, plan[edge].id == 14 ? std::span<const float>(send_gain)
                                                        : std::span<const float>{}};
    return spans;
  };

  std::vector<StereoFrame> plain;
  {
    auto mixer = take(prepare_routed_mixer(nodes(), edges, bounded));
    std::vector<RoutingEdge> plan_edges;
    for (const auto& planned : mixer->plan().edges)
      plan_edges.push_back(*std::ranges::find(edges, planned.id, &RoutingEdge::id));
    std::uint64_t first = 0;
    for (const auto frames : block_sizes) {
      fill(first, frames);
      std::vector<StereoFrame> output(frames);
      const auto edge_gains = gains(plan_edges);
      require(mixer->mix_block(views(), output, {first, 1}, edge_gains), "plain block rejected");
      plain.insert(plain.end(), output.begin(), output.end());
      first += frames;
    }
  }
  for (const std::size_t workers : {std::size_t{0}, std::size_t{3}}) {
    AudioWorkerPool pool(workers);
    if (workers != 0)
      pool.start_owned_threads(workers);
    auto mixer = take(prepare_routed_mixer(nodes(), edges, bounded));
    std::vector<RoutingEdge> plan_edges;
    for (const auto& planned : mixer->plan().edges)
      plan_edges.push_back(*std::ranges::find(edges, planned.id, &RoutingEdge::id));
    const auto depth = mixer->prepare_pipeline();
    REQUIRE_EQ(depth, 3u, "pipeline depth is not the graph's level count");
    struct Extra {
      std::atomic<int> runs{0};
    } extra;
    const auto count_extra = [](void* context, std::size_t, std::size_t) noexcept {
      static_cast<Extra*>(context)->runs.fetch_add(1);
    };
    std::vector<StereoFrame> piped;
    std::vector<StereoFrame> output(mixer->max_frames());
    std::uint64_t first = 0;
    std::size_t step = 0;
    while (piped.size() < plain.size()) {
      const bool real = step < block_sizes.size();
      const auto frames = real ? block_sizes[step] : std::size_t{64};
      if (real) {
        fill(first, frames);
      } else {
        for (auto& source : sources)
          source.clear();
      }
      const auto edge_gains = gains(plan_edges);
      const auto emitted = mixer->mix_step(
          views(), frames, {first, 1},
          real ? std::span<const EdgeGainSpan>(edge_gains) : std::span<const EdgeGainSpan>{}, {},
          output, workers != 0 ? &pool : nullptr, {2, count_extra, &extra});
      REQUIRE_OK(emitted, "pipeline step rejected");
      REQUIRE_EQ((step < *depth - 1), (*emitted == 0), "pipeline emitted before it filled");
      if (step >= *depth - 1)
        REQUIRE_EQ(*emitted, block_sizes[step - (*depth - 1)],
                   "pipeline emitted a block of the wrong length");
      piped.insert(piped.end(), output.begin(),
                   output.begin() + static_cast<std::ptrdiff_t>(*emitted));
      first += frames;
      ++step;
    }
    if (workers != 0)
      pool.stop_owned_threads();
    REQUIRE_EQ(extra.runs.load(), static_cast<int>(2 * step), "a step skipped the caller's tasks");
    require(std::ranges::equal(plain, piped,
                               [](StereoFrame left, StereoFrame right) {
                                 return left.left == right.left && left.right == right.right;
                               }),
            "pipelined mixing changed the output");
  }
}

void post_insert_channel_gain_is_bounded_and_stateful() {
  std::array<RoutedNode, 2> nodes{{{{1, 0}, {}, {}, false, {0.5F, 0.25F}}, {{3, 0}, {}}}};
  const std::array<RoutingEdge, 1> edges{{{11, 1, 3}}};
  auto mixer = take(prepare_routed_mixer(nodes, edges, limits(2, 1)));
  std::array<StereoFrame, 64> input{};
  input.fill({0.8F, -0.4F});
  std::array<StereoFrame, 64> output{};
  const std::array<std::span<const StereoFrame>, 2> sources{input, {}};
  counted_allocations = 0;
  counting_allocations = true;
  require(mixer->mix_block(sources, output, {0, 1}), "channel gain block rejected");
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0,
             "post-insert channel gain or callback allocation contract failed");
  REQUIRE_EQ(output[0].left, 0.4F,
             "post-insert channel gain or callback allocation contract failed");
  REQUIRE_EQ(output[0].right, -0.1F,
             "post-insert channel gain or callback allocation contract failed");
  nodes[0].output_gain = {1.0F, 0.0F};
  auto next = take(prepare_routed_mixer(nodes, edges, limits(2, 1), mixer.get(), {64, 1}));
  REQUIRE_TRUE(next->mix_block(sources, output, {64, 1}),
               "channel gain did not change at the held graph boundary");
  REQUIRE_EQ(output[0].left, 0.8F, "channel gain did not change at the held graph boundary");
  REQUIRE_EQ(output[0].right, 0.0F, "channel gain did not change at the held graph boundary");
  nodes[0].output_gain.left = std::numeric_limits<float>::quiet_NaN();
  require(!prepare_routed_mixer(nodes, edges, limits(2, 1)),
          "non-finite channel gain entered a graph");
  nodes[0].output_gain.left = 4.1F;
  require(!prepare_routed_mixer(nodes, edges, limits(2, 1)),
          "out-of-range channel gain entered a graph");
}

void held_gain_changes_ramp_and_republish_without_a_step() {
  std::array<RoutedNode, 2> nodes{{{{1, 0}, {}, {}, false, {0.5F, 0.5F}}, {{3, 0}, {}}}};
  const std::array<RoutingEdge, 1> edges{{{11, 1, 3}}};
  auto bounded = limits(2, 1);
  bounded.gain_ramp_frames = 128;
  auto initial = take(prepare_routed_mixer(nodes, edges, bounded));
  std::array<StereoFrame, 64> input{};
  input.fill({1.0F, 1.0F});
  std::array<StereoFrame, 64> output{};
  const std::array<std::span<const StereoFrame>, 2> sources{input, {}};
  REQUIRE_TRUE(initial->mix_block(sources, output, {0, 1}),
               "initial graph did not use its exact saved gain");
  REQUIRE_EQ(output[63].left, 0.5F, "initial graph did not use its exact saved gain");

  nodes[0].output_gain = {1.0F, 1.0F};
  auto raised = take(prepare_routed_mixer(nodes, edges, bounded, initial.get(), {64, 1}));
  counted_allocations = 0;
  counting_allocations = true;
  const bool raised_ok = raised->mix_block(sources, output, {64, 1});
  counting_allocations = false;
  REQUIRE_TRUE(raised_ok, "raised gain ramp or callback allocation contract failed");
  REQUIRE_EQ(counted_allocations, 0, "raised gain ramp or callback allocation contract failed");
  REQUIRE_LT(std::abs(output[0].left - 0.50390625F), tolerance::float_round_trip_f,
             "raised gain ramp or callback allocation contract failed");
  REQUIRE_LT(std::abs(output[63].left - 0.75F), tolerance::float_round_trip_f,
             "raised gain ramp or callback allocation contract failed");

  nodes[0].output_gain = {0.0F, 0.0F};
  auto lowered = take(prepare_routed_mixer(nodes, edges, bounded, raised.get(), {128, 1}));
  REQUIRE_TRUE(lowered->mix_block(sources, output, {128, 1}),
               "new gain edit did not start at the audible in-progress gain");
  REQUIRE_LT(std::abs(output[0].left - (0.75F * 127.0F / 128.0F)), tolerance::float_round_trip_f,
             "new gain edit did not start at the audible in-progress gain");
  REQUIRE_LT(std::abs(output[63].left - 0.375F), tolerance::float_round_trip_f,
             "new gain edit did not start at the audible in-progress gain");

  auto unchanged = take(prepare_routed_mixer(nodes, edges, bounded, lowered.get(), {192, 1}));
  REQUIRE_TRUE(unchanged->mix_block(sources, output, {192, 1}),
               "unrelated graph publication restarted or dropped the gain ramp");
  REQUIRE_LT(std::abs(output[0].left - (0.75F * 63.0F / 128.0F)), tolerance::float_round_trip_f,
             "unrelated graph publication restarted or dropped the gain ramp");
  REQUIRE_EQ(output[63].left, 0.0F,
             "unrelated graph publication restarted or dropped the gain ramp");
}

void per_sample_node_gain_is_bounded_and_preallocated() {
  const std::array<RoutedNode, 2> nodes{{{{1, 0}, {}, {}, false, {0.5F, 0.5F}}, {{3, 0}, {}}}};
  const std::array<RoutingEdge, 1> edges{{{11, 1, 3}}};
  auto mixer = take(prepare_routed_mixer(nodes, edges, limits(2, 1)));
  std::array<StereoFrame, 64> input{};
  input.fill({1.0F, 1.0F});
  std::array<StereoFrame, 64> output{};
  std::array<StereoFrame, 64> dynamic{};
  for (std::size_t index = 0; index < dynamic.size(); ++index) {
    dynamic[index] = {static_cast<float>(index) / 64.0F, 0.75F};
  }
  const std::array<NodeGainSpan, 2> node_gains{{{1, dynamic}, {3, {}}}};
  const std::array<std::span<const StereoFrame>, 2> sources{input, {}};
  counted_allocations = 0;
  counting_allocations = true;
  const bool rendered = mixer->mix_block(sources, output, {0, 1}, {}, node_gains);
  counting_allocations = false;
  REQUIRE_TRUE(rendered,
               "per-sample node gain did not override static gain without callback allocation");
  REQUIRE_EQ(counted_allocations, 0,
             "per-sample node gain did not override static gain without callback allocation");
  REQUIRE_EQ(output[0].left, 0.0F,
             "per-sample node gain did not override static gain without callback allocation");
  REQUIRE_EQ(output[32].left, 0.5F,
             "per-sample node gain did not override static gain without callback allocation");
  REQUIRE_EQ(output[63].right, 0.75F,
             "per-sample node gain did not override static gain without callback allocation");

  const std::array<NodeGainSpan, 2> wrong_id{{{3, dynamic}, {1, {}}}};
  output.fill({1.0F, 1.0F});
  REQUIRE_TRUE(!mixer->mix_block(sources, output, {64, 1}, {}, wrong_id),
               "misbound dynamic node gain was accepted");
  REQUIRE_EQ(output[0].left, 0.0F, "misbound dynamic node gain was accepted");
  dynamic[0].left = 4.1F;
  output.fill({1.0F, 1.0F});
  REQUIRE_TRUE(!mixer->mix_block(sources, output, {64, 1}, {}, node_gains),
               "out-of-range dynamic node gain was accepted");
  REQUIRE_EQ(output[0].left, 0.0F, "out-of-range dynamic node gain was accepted");
  dynamic[0].left = 0.0F;
  REQUIRE_TRUE(mixer->mix_block(sources, output, {64, 1}, {}, node_gains),
               "invalid dynamic gain advanced mixer state");
  REQUIRE_EQ(output[63].right, 0.75F, "invalid dynamic gain advanced mixer state");

  // Muting under dynamic gain fades out over the ramp and then stays silent.
  constexpr float dynamic_level = 0.75F;
  constexpr std::size_t mute_ramp_frames = 64;
  dynamic.fill({dynamic_level, dynamic_level});
  const std::array<RoutedNodeControl, 2> mute{{{1, true, {0.5F, 0.5F}}, {3, false, {1.0F, 1.0F}}}};
  mixer->apply_node_controls(mute);
  REQUIRE_TRUE(mixer->mix_block(sources, output, {128, 1}, {}, node_gains),
               "a mute under dynamic gain did not fade out");
  REQUIRE_LT(std::abs(output[0].left - dynamic_level * (1.0F - 1.0F / mute_ramp_frames)),
             tolerance::float_round_trip_f, "a mute under dynamic gain did not fade out");
  REQUIRE_EQ(output[63].left, 0.0F, "a mute under dynamic gain did not fade out");
  REQUIRE_TRUE(mixer->mix_block(sources, output, {192, 1}, {}, node_gains),
               "a mute under dynamic gain did not stay silent");
  require(std::ranges::all_of(output, [](StereoFrame frame) { return frame.left == 0.0F; }),
          "a mute under dynamic gain did not stay silent");

  // A fade that ends inside a block leaves the rest of the block silent.
  constexpr std::size_t half_block = 32;
  const std::array<RoutedNodeControl, 2> unmute{
      {{1, false, {0.5F, 0.5F}}, {3, false, {1.0F, 1.0F}}}};
  mixer->apply_node_controls(unmute);
  REQUIRE_TRUE(mixer->mix_block(sources, output, {256, 1}, {}, node_gains),
               "an unmute under dynamic gain did not render");
  mixer->apply_node_controls(mute);
  const std::array<std::span<const StereoFrame>, 2> half_sources{
      std::span<const StereoFrame>(input).first(half_block), {}};
  const std::array<NodeGainSpan, 2> half_gains{
      {{1, std::span<const StereoFrame>(dynamic).first(half_block)}, {3, {}}}};
  REQUIRE_TRUE(
      mixer->mix_block(half_sources, std::span(output).first(half_block), {320, 1}, {}, half_gains),
      "a mute fade that ends mid-block did not render");
  REQUIRE_TRUE(mixer->mix_block(sources, output, {352, 1}, {}, node_gains),
               "a mute fade that ends mid-block did not render");
  REQUIRE_GT(output[0].left, 0.0F, "a mute fade that ends mid-block did not keep fading");
  require(std::ranges::all_of(std::span(output).subspan(half_block),
                              [](StereoFrame frame) { return frame.left == 0.0F; }),
          "a mute fade that ends mid-block left the rest of the block audible");
}

void sidechain_is_separate_aligned_and_allocation_free() {
  struct OneFrameDelay final : ProcessorRuntime {
    StereoFrame prior{};
    void process(std::span<StereoFrame> frames, BlockContext) noexcept override {
      for (auto& frame : frames)
        std::swap(frame, prior);
    }
  };
  struct DetectorMultiply final : SidechainProcessorRuntime {
    void process(std::span<StereoFrame> frames, BlockContext) noexcept override {
      std::ranges::fill(frames, StereoFrame{});
    }
    void process_with_sidechain(std::span<StereoFrame> frames,
                                std::span<const StereoFrame> detector,
                                BlockContext) noexcept override {
      for (std::size_t index = 0; index < frames.size(); ++index) {
        frames[index].left *= detector[index].left;
        frames[index].right *= detector[index].right;
      }
    }
  };
  const std::array<RoutedNode, 3> nodes{{{{1, 0}, {}},
                                         {{2, 1}, std::make_shared<OneFrameDelay>()},
                                         {{3, 0}, std::make_shared<DetectorMultiply>()}}};
  const std::array<RoutingEdge, 2> edges{
      {{11, 1, 3}, {12, 2, 3, 1.0F, RoutingEdgeKind::sidechain}}};
  auto bounded = limits(3, 2, 1);
  bounded.max_buffer_bytes += 2 * 64 * sizeof(StereoFrame); // detector and source tap
  bounded.max_history_bytes += 2 * sizeof(StereoFrame);
  auto mixer = take(prepare_routed_mixer(nodes, edges, bounded));
  auto short_budget = bounded;
  short_budget.max_buffer_bytes -= 64 * sizeof(StereoFrame);
  require(!prepare_routed_mixer(nodes, edges, short_budget),
          "sidechain input allocation escaped the graph buffer budget");
  REQUIRE_EQ(mixer->plan().edges[0].compensation_frames, 1,
             "sidechain and main audio were not aligned to the same processor input");
  REQUIRE_EQ(mixer->plan().edges[1].compensation_frames, 0,
             "sidechain and main audio were not aligned to the same processor input");
  std::array<StereoFrame, 4> main{{{1.0F, 1.0F}}};
  std::array<StereoFrame, 4> detector{{{0.5F, 0.25F}}};
  std::array<StereoFrame, 4> output{};
  const std::array<std::span<const StereoFrame>, 3> sources{main, detector, {}};
  counted_allocations = 0;
  counting_allocations = true;
  const bool rendered = mixer->mix_block(sources, output, {0, 1});
  counting_allocations = false;
  REQUIRE_TRUE(rendered, "sidechain route leaked into audible input or missed aligned impulse");
  REQUIRE_EQ(counted_allocations, 0,
             "sidechain route leaked into audible input or missed aligned impulse");
  REQUIRE_EQ(output[0].left, 0.0F,
             "sidechain route leaked into audible input or missed aligned impulse");
  REQUIRE_EQ(output[1].left, 0.5F,
             "sidechain route leaked into audible input or missed aligned impulse");
  REQUIRE_EQ(output[1].right, 0.25F,
             "sidechain route leaked into audible input or missed aligned impulse");
  REQUIRE_EQ(output[2].left, 0.0F,
             "sidechain route leaked into audible input or missed aligned impulse");
  constexpr std::array detector_gain{1.0F, 0.5F, 1.0F, 1.0F};
  const std::array<EdgeGainSpan, 2> gains{{{11, {}}, {12, detector_gain}}};
  REQUIRE_TRUE(mixer->mix_block(sources, output, {0, 2}, gains),
               "sidechain edge gain did not control only the detector input");
  REQUIRE_EQ(output[1].left, 0.25F, "sidechain edge gain did not control only the detector input");
  REQUIRE_EQ(output[1].right, 0.125F,
             "sidechain edge gain did not control only the detector input");
  const std::array<RoutingEdge, 2> reused_kind{{edges[0], {12, 2, 3}}};
  require(!prepare_routed_mixer(nodes, reused_kind, bounded, mixer.get(), {4, 2}),
          "route ID changed from sidechain to main without a new identity");
  const std::array<RoutedNode, 3> incapable{
      {{{1, 0}, {}}, {{2, 1}, std::make_shared<OneFrameDelay>()}, {{3, 0}, {}}}};
  require(!prepare_routed_mixer(incapable, edges, bounded),
          "sidechain destination without a capable processor was accepted");

  // The target's own source has no incoming main edge. Its input must still
  // wait for the detector's declared processor latency before compression.
  const std::array<RoutedNode, 3> direct_target{{{{1, 0}, {}},
                                                 {{2, 1}, std::make_shared<OneFrameDelay>()},
                                                 {{3, 0}, std::make_shared<DetectorMultiply>()}}};
  const std::array<RoutingEdge, 3> direct_edges{
      {{21, 2, 3, 1.0F, RoutingEdgeKind::sidechain}, {22, 3, 1}, {23, 2, 1, 0.0F}}};
  auto direct_limits = limits(3, 4, 1);
  direct_limits.latency.master = 1;
  direct_limits.max_buffer_bytes += 2 * 64 * sizeof(StereoFrame); // detector and source tap
  auto direct = take(prepare_routed_mixer(direct_target, direct_edges, direct_limits));
  const auto planned_target = std::ranges::find(direct->plan().nodes, 3, &PlannedNodeLatency::id);
  REQUIRE_NE(planned_target, direct->plan().nodes.end(),
             "direct sidechain target did not account for detector latency");
  REQUIRE_EQ(planned_target->output_latency_frames, 1,
             "direct sidechain target did not account for detector latency");
  const std::array<std::span<const StereoFrame>, 3> direct_sources{detector, main,
                                                                   std::span<const StereoFrame>{}};
  counted_allocations = 0;
  counting_allocations = true;
  const auto direct_rendered = direct->mix_block(direct_sources, output, {0, 1});
  counting_allocations = false;
  REQUIRE_TRUE(direct_rendered, "direct target source was not aligned to its latent detector");
  REQUIRE_EQ(counted_allocations, 0, "direct target source was not aligned to its latent detector");
  REQUIRE_EQ(output[0].left, 0.0F, "direct target source was not aligned to its latent detector");
  REQUIRE_EQ(output[1].left, 0.5F, "direct target source was not aligned to its latent detector");
  REQUIRE_EQ(output[1].right, 0.25F, "direct target source was not aligned to its latent detector");
  REQUIRE_EQ(output[2].left, 0.0F, "direct target source was not aligned to its latent detector");
  auto changed_edges = direct_edges;
  changed_edges[0].gain = 0.5F;
  auto changed =
      take(prepare_routed_mixer(direct_target, changed_edges, direct_limits, direct.get(), {4, 1}));
  counted_allocations = 0;
  counting_allocations = true;
  const auto changed_rendered = changed->mix_block(direct_sources, output, {4, 1});
  counting_allocations = false;
  REQUIRE_TRUE(changed_rendered, "sidechain gain handoff lost direct input delay history");
  REQUIRE_EQ(counted_allocations, 0, "sidechain gain handoff lost direct input delay history");
  REQUIRE_EQ(output[0].left, 0.0F, "sidechain gain handoff lost direct input delay history");
  REQUIRE_EQ(output[1].left, 0.25F, "sidechain gain handoff lost direct input delay history");
  REQUIRE_EQ(output[1].right, 0.125F, "sidechain gain handoff lost direct input delay history");
  REQUIRE_EQ(output[2].left, 0.0F, "sidechain gain handoff lost direct input delay history");
  auto under_history = direct_limits;
  under_history.max_history_bytes = direct_edges.size() * 2 * sizeof(StereoFrame);
  require(!prepare_routed_mixer(direct_target, direct_edges, under_history),
          "direct sidechain source history escaped the graph memory budget");
  const std::array<RoutingEdge, 2> without_detector{{direct_edges[1], direct_edges[2]}};
  auto old_route = take(prepare_routed_mixer(direct_target, without_detector, direct_limits));
  require(old_route->mix_block(direct_sources, output, {0, 1}),
          "pre-sidechain route did not render");
  const auto unprimed =
      prepare_routed_mixer(direct_target, direct_edges, direct_limits, old_route.get(), {4, 1});
  REQUIRE_TRUE(!unprimed,
               "new delayed sidechain route replaced an active graph without input history");
  REQUIRE_EQ(unprimed.error().code, foundation::ErrorCode::unavailable,
             "new delayed sidechain route replaced an active graph without input history");
  REQUIRE_TRUE(old_route->mix_block(direct_sources, output, {4, 1}),
               "new delayed sidechain route replaced an active graph without input history");
}

void planned_loop_wrap_keeps_parallel_path_alignment() {
  struct EightFrameLatency final : ProcessorRuntime {
    std::array<StereoFrame, 8> line{};
    std::size_t cursor = 0;
    std::uint64_t generation = 0;
    void process(std::span<StereoFrame> frames, BlockContext context) noexcept override {
      if (generation != context.state_generation()) {
        line.fill({});
        cursor = 0;
        generation = context.state_generation();
      }
      for (auto& frame : frames) {
        std::swap(frame, line[cursor]);
        cursor = (cursor + 1) % line.size();
      }
    }
  };
  const std::array<RoutedNode, 3> nodes{
      {{{1, 0}, {}}, {{2, 8}, std::make_shared<EightFrameLatency>()}, {{3, 0}, {}}}};
  const std::array<RoutingEdge, 3> edges{{{11, 1, 2}, {12, 2, 3}, {13, 1, 3}}};
  auto mixer = take(prepare_routed_mixer(nodes, edges, source_history_limits({3, 3, 64})));
  std::array<StereoFrame, 64> source{}, output{};
  source[60] = {1.0F, -1.0F};
  const std::array<std::span<const StereoFrame>, 3> inputs{source, {}, {}};
  REQUIRE_TRUE(mixer->mix_block(inputs, output, {0, 1}),
               "loop fixture did not hold its latent impulse");
  REQUIRE_EQ(output.back().left, 0.0F, "loop fixture did not hold its latent impulse");
  source.fill({});
  counted_allocations = 0;
  counting_allocations = true;
  const bool rendered = mixer->mix_block(inputs, output, {0, 2, 1, true});
  counting_allocations = false;
  REQUIRE_TRUE(rendered, "planned wrap reset PDC or misaligned the parallel route");
  REQUIRE_EQ(counted_allocations, 0, "planned wrap reset PDC or misaligned the parallel route");
  REQUIRE_EQ(output[3].left, 0.0F, "planned wrap reset PDC or misaligned the parallel route");
  REQUIRE_EQ(output[4].left, 2.0F, "planned wrap reset PDC or misaligned the parallel route");
  REQUIRE_EQ(output[4].right, -2.0F, "planned wrap reset PDC or misaligned the parallel route");
  REQUIRE_EQ(output[5].left, 0.0F, "planned wrap reset PDC or misaligned the parallel route");
}
} // namespace

// Predecessors replaced in quick succession each drain to the end of their
// own bounded tail, summed, while the newest owner plays.
void successive_replacements_drain_together() {
  struct Constant final : ProcessorRuntime {
    explicit Constant(float level) : level(level) {}
    void process(std::span<StereoFrame> frames, BlockContext) noexcept override {
      std::ranges::fill(frames, StereoFrame{level, level});
    }
    float level;
  };
  const std::array<RoutingEdge, 1> edges{{{11, 1, 3}}};
  auto bounded = limits(2, 1);
  bounded.max_frames = 4;
  const auto first = std::make_shared<Constant>(1.0F);
  const auto second = std::make_shared<Constant>(2.0F);
  std::array<RoutedNode, 2> nodes{{{{1, 0}, first}, {{3, 0}, {}}}};
  std::array<StereoFrame, 4> silence{}, output{};
  const std::array<std::span<const StereoFrame>, 2> inputs{silence, {}};
  auto mixer = take(prepare_routed_mixer(nodes, edges, bounded));
  REQUIRE_TRUE(mixer->mix_block(inputs, output, {0, 1}), "first owner did not play");
  REQUIRE_EQ(output[0].left, 1.0F, "first owner did not play");
  nodes[0].processor = second;
  nodes[0].replacement = RoutedTailReplacement{first, 8};
  mixer = take(prepare_routed_mixer(nodes, edges, bounded, mixer.get(), {4, 1}));
  REQUIRE_TRUE(mixer->mix_block(inputs, output, {4, 1}),
               "the first drain did not sum with the second owner");
  REQUIRE_EQ(output[0].left, 3.0F, "the first drain did not sum with the second owner");
  nodes[0].processor = std::make_shared<Constant>(0.0F);
  nodes[0].replacement = RoutedTailReplacement{second, 8};
  auto third = prepare_routed_mixer(nodes, edges, bounded, mixer.get(), {8, 1});
  REQUIRE_OK(third, "a second replacement was refused while the first still drained");
  mixer = std::move(third).value();
  const std::array<float, 3> expected{3.0F, 2.0F, 0.0F};
  for (std::size_t block = 0; block < expected.size(); ++block) {
    counted_allocations = 0;
    counting_allocations = true;
    const bool mixed = mixer->mix_block(inputs, output, {8 + 4 * block, 1});
    counting_allocations = false;
    REQUIRE_TRUE(mixed, "successive drains did not each run their own length, summed");
    REQUIRE_EQ(counted_allocations, 0,
               "successive drains did not each run their own length, summed");
    REQUIRE_EQ(output[0].left, expected[block],
               "successive drains did not each run their own length, summed");
    REQUIRE_EQ(output[3].left, expected[block],
               "successive drains did not each run their own length, summed");
  }
}

int main() {
  try {
    parallel_routes_and_bus_processor();
    successive_replacements_drain_together();
    dynamic_edge_gain_uses_project_sample_values();
    multihop_bus_aligns_with_direct_send();
    gained_main_send_and_history_transition();
    newly_added_edge_uses_exact_source_history();
    new_edge_rejects_insufficient_history_and_budget();
    non_finite_processor_output_is_guarded();
    transition_growth_shrink_and_failed_hold();
    state_reuse_and_rejection();
    invalid_inputs_budgets_and_callback_allocations();
    muted_nodes_silence_output_even_with_dynamic_send_gain();
    skipped_muted_nodes_do_no_work_and_change_nothing();
    pipelined_steps_match_block_by_block_mixing();
    post_insert_channel_gain_is_bounded_and_stateful();
    held_gain_changes_ramp_and_republish_without_a_step();
    per_sample_node_gain_is_bounded_and_preallocated();
    sidechain_is_separate_aligned_and_allocation_free();
    planned_loop_wrap_keeps_parallel_path_alignment();
    std::cout << "routed mixer contract passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
