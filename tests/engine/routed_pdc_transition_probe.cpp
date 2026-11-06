#include "oscillate/engine/routed_mixer.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/check.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

namespace {
using namespace oscillate;
using namespace oscillate::engine;

// Independent sample delay: a processor's reported latency changes only at a
// quiescent boundary, while its preallocated history and runtime owner survive.
class VariableLatency final : public ProcessorRuntime {
public:
  explicit VariableLatency(std::size_t capacity, std::size_t delay)
      : history_(capacity + 1), delay_(delay) {}

  void set_delay_after_quiesce(std::size_t delay) { delay_ = delay; }

  void process(std::span<StereoFrame> frames, BlockContext) noexcept override {
    for (auto& frame : frames) {
      history_[cursor_] = frame;
      frame = recorded_ >= delay_ ? history_[(cursor_ + history_.size() - delay_) % history_.size()]
                                  : StereoFrame{};
      cursor_ = (cursor_ + 1) % history_.size();
      if (recorded_ < history_.size() - 1)
        ++recorded_;
    }
  }

private:
  std::vector<StereoFrame> history_;
  std::size_t delay_;
  std::size_t cursor_ = 0;
  std::size_t recorded_ = 0;
};

using oscillate::test::require;

using oscillate::test::take;

StereoFrame source_at(std::uint64_t position, std::size_t block) {
  // Sparse, different-sign stereo impulses straddle each switch and expose
  // both premature arrivals and lost delayed history.
  if (position == 0 || position == 2 * block - 12 || position == 2 * block - 4 ||
      position == 2 * block || position == 2 * block + 3 || position == 4 * block - 16 ||
      position == 4 * block - 7 || position == 4 * block || position == 4 * block + 5)
    return {0.5F, -0.25F};
  return {};
}

RoutedMixerLimits limits(std::size_t block, std::uint32_t budget) {
  return {{4, budget},
          block,
          5 * block * sizeof(StereoFrame),
          4 * (static_cast<std::uint64_t>(budget) + 1) * sizeof(StereoFrame)};
}

void run(std::size_t block) {
  constexpr std::array<RoutingEdge, 4> edges{
      {{11, 1, 2}, {12, 2, 3}, {13, 3, 4}, {14, 1, 4, 0.5F}}};
  auto fixed = std::make_shared<VariableLatency>(16, 4);
  auto variable = std::make_shared<VariableLatency>(16, 4);
  std::array<RoutedNode, 4> nodes{
      {{{1, 0}, {}}, {{2, 4}, fixed}, {{3, 4}, variable}, {{4, 0}, {}}}};
  auto active = take(prepare_routed_mixer(nodes, edges, limits(block, 16)));
  std::vector<StereoFrame> source(block);
  std::vector<StereoFrame> output(block);
  const std::array<std::span<const StereoFrame>, 4> inputs{source, {}, {}, {}};
  std::size_t callback_allocations = 0;

  for (std::uint64_t position = 0; position < 6 * block; position += block) {
    if (position == 2 * block || position == 4 * block) {
      const auto next_variable_delay = position == 2 * block ? 12U : 4U;
      nodes[2].routing.processor_latency_frames = next_variable_delay;
      const auto rejected = prepare_routed_mixer(
          nodes, edges, limits(block, position == 2 * block ? 15 : 7), active.get(), {position, 1});
      REQUIRE_TRUE(!rejected, "over-budget latency revision did not explicitly fail");
      REQUIRE_EQ(rejected.error().code, foundation::ErrorCode::resource_exhausted,
                 "over-budget latency revision did not explicitly fail");
      REQUIRE_EQ(active->plan().edges.back().compensation_frames,
                 (position == 2 * block ? 8U : 16U), "failed revision modified active PDC plan");
      variable->set_delay_after_quiesce(next_variable_delay);
      active =
          take(prepare_routed_mixer(nodes, edges, limits(block, 16), active.get(), {position, 1}));
      REQUIRE_EQ(active->plan().edges.back().compensation_frames,
                 (position == 2 * block ? 16U : 8U),
                 "new PDC plan did not follow the changed bus latency");
    }
    for (std::size_t index = 0; index < block; ++index)
      source[index] = source_at(position + index, block);
    counted_allocations = 0;
    counting_allocations = true;
    const auto rendered = active->mix_block(inputs, output, {position, 1});
    counting_allocations = false;
    callback_allocations += counted_allocations;
    require(rendered, "routed callback rejected a prepared PDC transition");
    const auto delay = position < 2 * block ? 8U : (position < 4 * block ? 16U : 8U);
    for (std::size_t index = 0; index < block; ++index) {
      const auto time = position + index;
      const auto source_frame = time >= delay ? source_at(time - delay, block) : StereoFrame{};
      REQUIRE_EQ(output[index].left, 1.5F * source_frame.left,
                 "parallel send and two-bus path differ from independent impulse oracle");
      REQUIRE_EQ(output[index].right, 1.5F * source_frame.right,
                 "parallel send and two-bus path differ from independent impulse oracle");
    }
  }
  REQUIRE_EQ(callback_allocations, 0, "PDC transition callback allocated");
  std::cout << "block_frames=" << block
            << " switches=2 callback_allocations=" << callback_allocations
            << " alignment=exact budget_refusal=explicit\n";
}
} // namespace

int main() {
  try {
    run(64);
    run(128);
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
