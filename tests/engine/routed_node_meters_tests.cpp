#include "oscillate/engine/routed_node_meters.hpp"
#include "oscillate/engine/routed_publisher.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/tolerances.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace tolerance = oscillate::test::tolerance;

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

namespace {
using namespace oscillate::engine;

using oscillate::test::require;

bool near(float actual, float expected) {
  return std::abs(actual - expected) <= tolerance::float_round_trip_f;
}

void feed(RoutedNodeMeters& meters, std::size_t node, std::span<const StereoFrame> block,
          std::uint64_t first, std::uint64_t generation, std::size_t blocks) {
  for (std::size_t index = 0; index < blocks; ++index)
    meters.observe(node, block, {first + index * block.size(), generation});
}

void window_peak_and_rms_match_hand_values() {
  RoutedNodeMeters meters(2);
  REQUIRE_EQ(meters.size(), 2, "meter slot existed before any publication");
  REQUIRE_TRUE(!meters.latest(0), "meter slot existed before any publication");
  REQUIRE_TRUE(!meters.latest(1), "meter slot existed before any publication");
  REQUIRE_TRUE(!meters.latest(2), "meter slot existed before any publication");
  // Node 0: alternating 0.6 / -0.8 on the left gives RMS sqrt(0.5); the
  // right channel is a constant -0.25.
  std::array<StereoFrame, 128> mixed{};
  for (std::size_t frame = 0; frame < mixed.size(); ++frame)
    mixed[frame] = {frame % 2 == 0 ? 0.6F : -0.8F, -0.25F};
  // Node 1: full-scale square wave, which must latch clipping.
  std::array<StereoFrame, 128> full{};
  for (std::size_t frame = 0; frame < full.size(); ++frame)
    full[frame] = {frame % 2 == 0 ? 1.0F : -1.0F, 0.0F};

  counting_allocations = true;
  feed(meters, 0, mixed, 0, 1, 7);
  counting_allocations = false;
  const auto partial = meters.latest(0);
  REQUIRE_TRUE(partial, "an incomplete window was published as a level");
  REQUIRE_EQ(partial->transport_generation, 1, "an incomplete window was published as a level");
  REQUIRE_EQ(partial->end_sample, 0, "an incomplete window was published as a level");
  REQUIRE_EQ(partial->left_peak, 0.0F, "an incomplete window was published as a level");
  REQUIRE_EQ(partial->left_rms, 0.0F, "an incomplete window was published as a level");
  counting_allocations = true;
  feed(meters, 0, mixed, 7 * 128, 1, 1);
  feed(meters, 1, full, 0, 1, 8);
  counting_allocations = false;
  const auto first = meters.latest(0);
  REQUIRE_EQ(counted_allocations, 0, "node meter observation allocated");
  REQUIRE_TRUE(first, "window peak/RMS differ from the hand-computed values");
  REQUIRE_EQ(first->end_sample, RoutedNodeMeters::window_frames,
             "window peak/RMS differ from the hand-computed values");
  REQUIRE_EQ(first->transport_generation, 1,
             "window peak/RMS differ from the hand-computed values");
  REQUIRE_TRUE(near(first->left_peak, 0.8F),
               "window peak/RMS differ from the hand-computed values");
  REQUIRE_TRUE(near(first->right_peak, 0.25F),
               "window peak/RMS differ from the hand-computed values");
  REQUIRE_TRUE(near(first->left_rms, static_cast<float>(std::sqrt(0.5))),
               "window peak/RMS differ from the hand-computed values");
  REQUIRE_TRUE(near(first->right_rms, 0.25F),
               "window peak/RMS differ from the hand-computed values");
  REQUIRE_TRUE(!first->clipped, "window peak/RMS differ from the hand-computed values");
  const auto clipped = meters.latest(1);
  REQUIRE_TRUE(clipped, "full-scale node did not report clipping");
  REQUIRE_TRUE(clipped->clipped, "full-scale node did not report clipping");
  REQUIRE_TRUE(near(clipped->left_rms, 1.0F), "full-scale node did not report clipping");
  REQUIRE_EQ(clipped->right_peak, 0.0F, "full-scale node did not report clipping");
}

void blocks_larger_than_a_window_publish_each_block() {
  RoutedNodeMeters meters(1);
  std::array<StereoFrame, 2048> block{};
  block.fill({0.5F, 0.5F});
  block[2047] = {0.0F, -0.75F};
  meters.observe(0, block, {0, 1});
  const auto reading = meters.latest(0);
  REQUIRE_TRUE(reading, "an oversized block was not measured as one window");
  REQUIRE_EQ(reading->end_sample, 2048, "an oversized block was not measured as one window");
  REQUIRE_TRUE(near(reading->left_peak, 0.5F), "an oversized block was not measured as one window");
  REQUIRE_TRUE(near(reading->right_peak, 0.75F),
               "an oversized block was not measured as one window");
}

void seek_discards_the_window_and_loop_wrap_continues_it() {
  RoutedNodeMeters meters(1);
  std::array<StereoFrame, 512> loud{};
  loud.fill({0.9F, 0.9F});
  std::array<StereoFrame, 512> quiet{};
  quiet.fill({0.1F, 0.1F});
  feed(meters, 0, loud, 0, 1, 1);
  // A seek starts a new generation away from the expected position.
  meters.observe(0, quiet, {5000, 2});
  const auto sought = meters.latest(0);
  REQUIRE_TRUE(sought, "seek did not publish silence at once");
  REQUIRE_EQ(sought->transport_generation, 2, "seek did not publish silence at once");
  REQUIRE_EQ(sought->end_sample, 5000, "seek did not publish silence at once");
  REQUIRE_EQ(sought->left_peak, 0.0F, "seek did not publish silence at once");
  meters.observe(0, quiet, {5512, 2});
  const auto after_seek = meters.latest(0);
  REQUIRE_TRUE(after_seek, "pre-seek audio leaked into the next window");
  REQUIRE_TRUE(near(after_seek->left_peak, 0.1F), "pre-seek audio leaked into the next window");
  REQUIRE_EQ(after_seek->end_sample, 6024, "pre-seek audio leaked into the next window");

  RoutedNodeMeters looped(1);
  feed(looped, 0, loud, 0, 1, 1);
  looped.observe(0, quiet, {0, 2, 0, true});
  const auto wrapped = looped.latest(0);
  REQUIRE_TRUE(wrapped, "planned loop wrap discarded the continuous window");
  REQUIRE_EQ(wrapped->transport_generation, 2, "planned loop wrap discarded the continuous window");
  REQUIRE_EQ(wrapped->end_sample, 512, "planned loop wrap discarded the continuous window");
  REQUIRE_TRUE(near(wrapped->left_peak, 0.9F), "planned loop wrap discarded the continuous window");
}

RoutedMixerLimits metered_limits(bool node_meters) {
  RoutedMixerLimits limits{
      {3, 32}, 128, 4 * 128 * sizeof(StereoFrame), 2 * 33 * sizeof(StereoFrame)};
  limits.node_meters = node_meters;
  return limits;
}

void publisher_reports_post_fader_node_levels() {
  // Node 1 plays at (0.5, 0.25) gain; node 2 is muted; both reach master 3.
  std::array<RoutedNode, 3> nodes{{{{1, 0}, {}, std::nullopt, false, {0.5F, 0.25F}},
                                   {{2, 0}, {}, std::nullopt, true, {1.0F, 1.0F}},
                                   {{3, 0}, {}}}};
  const std::array<RoutingEdge, 2> edges{{{11, 1, 3}, {12, 2, 3}}};
  RoutedMixerPublisher publisher;
  REQUIRE_OK(publisher.prepare_and_publish(1, nodes, edges, metered_limits(true)),
             "metered graph did not publish");
  require(!publisher.active_node_meter(1), "meter read before the graph became active");
  std::array<StereoFrame, 128> source{};
  source.fill({0.8F, -0.8F});
  std::array<StereoFrame, 128> output{};
  const std::array<std::span<const StereoFrame>, 3> inputs{source, source, {}};
  counted_allocations = 0;
  counting_allocations = true;
  for (std::uint64_t block = 0; block < 8; ++block)
    REQUIRE_EQ(publisher.render_block(inputs, output, {block * 128, 1}),
               RoutedRenderStatus::rendered, "metered graph did not render");
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "metered routed render allocated");
  const auto audible = publisher.active_node_meter(1);
  REQUIRE_TRUE(audible, "track meter was not measured after level and pan");
  REQUIRE_EQ(audible->transport_generation, 1, "track meter was not measured after level and pan");
  REQUIRE_TRUE(near(audible->left_peak, 0.4F), "track meter was not measured after level and pan");
  REQUIRE_TRUE(near(audible->right_peak, 0.2F), "track meter was not measured after level and pan");
  REQUIRE_TRUE(near(audible->left_rms, 0.4F), "track meter was not measured after level and pan");
  REQUIRE_TRUE(near(audible->right_rms, 0.2F), "track meter was not measured after level and pan");
  const auto muted = publisher.active_node_meter(2);
  REQUIRE_TRUE(muted, "muted track meter showed its pre-mute level");
  REQUIRE_EQ(muted->left_peak, 0.0F, "muted track meter showed its pre-mute level");
  REQUIRE_EQ(muted->right_rms, 0.0F, "muted track meter showed its pre-mute level");
  const auto master = publisher.active_node_meter(3);
  REQUIRE_TRUE(master, "master node meter did not sum its inputs");
  REQUIRE_TRUE(near(master->left_peak, 0.4F), "master node meter did not sum its inputs");
  REQUIRE_TRUE(near(master->right_peak, 0.2F), "master node meter did not sum its inputs");
  require(!publisher.active_node_meter(99), "unknown node returned a reading");

  RoutedMixerPublisher unmetered;
  REQUIRE_OK(unmetered.prepare_and_publish(1, nodes, edges, metered_limits(false)),
             "a mixer without node meters reported one");
  REQUIRE_EQ(unmetered.render_block(inputs, output, {0, 1}), RoutedRenderStatus::rendered,
             "a mixer without node meters reported one");
  REQUIRE_TRUE(!unmetered.active_node_meter(1), "a mixer without node meters reported one");
}
} // namespace

int main() {
  try {
    window_peak_and_rms_match_hand_values();
    blocks_larger_than_a_window_publish_each_block();
    seek_discards_the_window_and_loop_wrap_continues_it();
    publisher_reports_post_fader_node_levels();
  } catch (const std::exception& error) {
    std::fprintf(stderr, "routed node meters test: %s\n", error.what());
    return 1;
  }
  return 0;
}
