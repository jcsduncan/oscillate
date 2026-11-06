#include "oscillate/test/check.hpp"
#include "oscillate/track_chain/track_chain.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace oscillate;
using engine::BlockContext;
using engine::StereoFrame;

using oscillate::test::require;

template <typename Id> [[nodiscard]] Id id(std::uint64_t serial) {
  auto made = Id::from_bits({0x7472616368ULL, serial});
  REQUIRE_OK(made, "test identity is invalid");
  return made.value();
}

[[nodiscard]] StereoFrame input(std::uint64_t time) {
  return {static_cast<float>(time % 97) * 0.01F, -static_cast<float>(time % 89) * 0.01F};
}

// Delays its input by a fixed number of frames, like a lookahead stage.
class Delay final : public engine::ProcessorRuntime {
public:
  explicit Delay(std::size_t frames) : line_(frames, StereoFrame{}) {}
  void process(std::span<StereoFrame> stereo, BlockContext) noexcept override {
    for (auto& frame : stereo) {
      if (line_.empty())
        continue;
      line_.push_back(frame);
      frame = line_.front();
      line_.pop_front();
    }
  }

private:
  std::deque<StereoFrame> line_;
};

// Scales its input; counts the frames it processed.
class Gain final : public engine::SidechainProcessorRuntime {
public:
  explicit Gain(float gain) : gain_(gain) {}
  void process(std::span<StereoFrame> stereo, BlockContext) noexcept override {
    for (auto& frame : stereo)
      frame = {frame.left * gain_, frame.right * gain_};
    processed += stereo.size();
  }
  void process_with_sidechain(std::span<StereoFrame> stereo, std::span<const StereoFrame> key,
                              BlockContext context) noexcept override {
    keyed += key.size();
    process(stereo, context);
  }
  std::size_t processed = 0;
  std::size_t keyed = 0;

private:
  float gain_;
};

[[nodiscard]] project::Vst3PluginInstance plugin(std::uint64_t serial, std::uint8_t state = 0) {
  return {.id = id<project::PluginInstanceId>(serial),
          .class_id = "class",
          .vendor = "vendor",
          .name = "plugin",
          .version = "1",
          .bundle_path = "/plugin.vst3",
          .binary_sha256 = "sha",
          .component_state = {state},
          .parameters = {}};
}

[[nodiscard]] track_chain::SavedStage effect(std::uint64_t slot, std::uint64_t serial,
                                             std::uint8_t state = 0) {
  return {id<project::InsertSlotId>(slot), plugin(serial, state)};
}

// A VST3 host fake: each preparation is a new gain stage.
struct Host {
  std::size_t prepared = 0;
  std::uint32_t latency = 0;
  std::optional<std::uint64_t> ring_out = 100;
  track_chain::ChainContext context() {
    track_chain::ChainContext result{native_inserts::StageContext{48'000}};
    result.vst3 =
        [this](const track_chain::SavedStage& saved) -> foundation::Result<track_chain::Stage> {
      ++prepared;
      return foundation::Result<track_chain::Stage>::success(
          {saved, std::make_shared<Gain>(1.0F), latency, ring_out, ring_out, true});
    };
    return result;
  }
};

void unchanged_stages_carry_over() {
  Host host;
  const std::vector<track_chain::SavedStage> before{effect(1, 1), effect(2, 2)};
  auto first = track_chain::prepare_chain(before, host.context());
  REQUIRE_TRUE(first, "first chain did not prepare");
  REQUIRE_TRUE(first.value().runtime, "first chain did not prepare");
  REQUIRE_EQ(host.prepared, 2, "first chain did not prepare");
  // Insert a stage between them and change nothing else.
  const std::vector<track_chain::SavedStage> after{effect(1, 1), effect(3, 3), effect(2, 2)};
  auto second = track_chain::prepare_chain(after, host.context(), &first.value());
  REQUIRE_TRUE(second, "only the new stage should be prepared");
  REQUIRE_EQ(host.prepared, 3, "only the new stage should be prepared");
  REQUIRE_EQ(second.value().stages[0].runtime, first.value().stages[0].runtime,
             "unchanged stages did not carry over into a new chain");
  REQUIRE_EQ(second.value().stages[2].runtime, first.value().stages[1].runtime,
             "unchanged stages did not carry over into a new chain");
  REQUIRE_NE(second.value().runtime, first.value().runtime,
             "unchanged stages did not carry over into a new chain");
  // A changed state is a different stage: it is prepared afresh.
  const std::vector<track_chain::SavedStage> changed{effect(1, 1), effect(3, 3), effect(2, 2, 7)};
  auto third = track_chain::prepare_chain(changed, host.context(), &second.value());
  REQUIRE_TRUE(third, "a changed stage was not prepared again");
  REQUIRE_EQ(host.prepared, 4, "a changed stage was not prepared again");
  REQUIRE_NE(third.value().stages[2].runtime, second.value().stages[2].runtime,
             "a changed stage was not prepared again");
  REQUIRE_EQ(third.value().stages[1].runtime, second.value().stages[1].runtime,
             "a changed stage was not prepared again");
  // An empty chain has no runtime.
  auto empty = track_chain::prepare_chain({}, host.context(), &third.value());
  REQUIRE_TRUE(empty, "an empty chain prepared a runtime");
  REQUIRE_TRUE(!empty.value().runtime, "an empty chain prepared a runtime");
  REQUIRE_TRUE(empty.value().stages.empty(), "an empty chain prepared a runtime");
}

void switch_retires_by_ring_out() {
  Host host;
  const std::vector<track_chain::SavedStage> both{effect(1, 1), effect(2, 2)};
  auto prior = track_chain::prepare_chain(both, host.context());
  REQUIRE_OK(prior, "switch fixture did not prepare");
  const std::vector<track_chain::SavedStage> one{effect(1, 1)};
  auto next = track_chain::prepare_chain(one, host.context(), &prior.value());
  REQUIRE_OK(next, "switch successor did not prepare");
  // A removed stage with a bounded tail drains that tail, unfaded.
  auto bounded = track_chain::switch_chain(prior.value(), next.value(), 4'000);
  REQUIRE_TRUE(bounded.drain, "a bounded removed stage did not drain its tail");
  REQUIRE_TRUE(!bounded.handoff, "a bounded removed stage did not drain its tail");
  REQUIRE_EQ(bounded.drain->drain_frames, 100, "a bounded removed stage did not drain its tail");
  REQUIRE_EQ(bounded.drain->drain_processor, prior.value().stages[1].runtime,
             "a bounded removed stage did not drain its tail");
  REQUIRE_EQ(bounded.drain->prior_owner, prior.value().runtime,
             "a bounded removed stage did not drain its tail");
  REQUIRE_TRUE(!bounded.drain->fade_out, "a bounded removed stage did not drain its tail");
  // One that cannot bound its tail rings for the given time and fades.
  prior.value().stages[1].ring_out_frames.reset();
  auto unbounded = track_chain::switch_chain(prior.value(), next.value(), 4'000);
  REQUIRE_TRUE(unbounded.drain, "an unbounded removed stage did not ring out and fade");
  REQUIRE_EQ(unbounded.drain->drain_frames, 4'000,
             "an unbounded removed stage did not ring out and fade");
  REQUIRE_TRUE(unbounded.drain->fade_out, "an unbounded removed stage did not ring out and fade");
  // A tail longer than the mixer's cap is capped and fades.
  prior.value().stages[1].ring_out_frames = engine::routed_tail_frame_cap + 1;
  auto capped = track_chain::switch_chain(prior.value(), next.value(), 4'000);
  REQUIRE_TRUE(capped.drain, "an over-long tail was not capped and faded");
  REQUIRE_EQ(capped.drain->drain_frames, engine::routed_tail_frame_cap,
             "an over-long tail was not capped and faded");
  REQUIRE_TRUE(capped.drain->fade_out, "an over-long tail was not capped and faded");
  // Nothing rings: the kept stages pass over by handoff.
  prior.value().stages[1].ring_out_frames = 0;
  auto handoff = track_chain::switch_chain(prior.value(), next.value(), 4'000);
  REQUIRE_TRUE(!handoff.drain, "a silent removal did not hand the kept stages over");
  REQUIRE_EQ(handoff.handoff, prior.value().runtime,
             "a silent removal did not hand the kept stages over");
  // A removed lookahead stage hands its samples on through the exact latency
  // handover instead of draining them.
  Host latent;
  latent.latency = 16;
  auto latent_prior = track_chain::prepare_chain(both, latent.context());
  auto latent_next = track_chain::prepare_chain(one, latent.context(), &latent_prior.value());
  REQUIRE_TRUE(latent_prior, "lookahead switch fixture did not prepare");
  REQUIRE_TRUE(latent_next, "lookahead switch fixture did not prepare");
  auto lookahead = track_chain::switch_chain(latent_prior.value(), latent_next.value(), 4'000);
  REQUIRE_TRUE(!lookahead.drain, "a removed lookahead stage drained instead of handing over");
  REQUIRE_EQ(lookahead.handoff, latent_prior.value().runtime,
             "a removed lookahead stage drained instead of handing over");
  // Nothing at all from no prior chain.
  auto fresh = track_chain::switch_chain({}, next.value(), 4'000);
  REQUIRE_TRUE(!fresh.drain, "a fresh chain retired a predecessor");
  REQUIRE_TRUE(!fresh.handoff, "a fresh chain retired a predecessor");
}

void detector_keys_its_stage() {
  Host host;
  auto context = host.context();
  context.detector_slot = id<project::InsertSlotId>(2);
  const std::vector<track_chain::SavedStage> saved{effect(1, 1), effect(2, 2)};
  auto chain = track_chain::prepare_chain(saved, context);
  REQUIRE_TRUE(chain, "the keyed stage was not found");
  REQUIRE_TRUE(chain.value().runtime, "the keyed stage was not found");
  REQUIRE_EQ(chain.value().runtime->detector(), 1, "the keyed stage was not found");
  std::array<StereoFrame, 32> block{};
  std::array<StereoFrame, 32> key{};
  chain.value().runtime->process_with_sidechain(block, key, {0, 1});
  const auto& keyed = static_cast<const Gain&>(*chain.value().stages[1].runtime);
  const auto& plain = static_cast<const Gain&>(*chain.value().stages[0].runtime);
  REQUIRE_EQ(keyed.keyed, 32, "the key did not reach only its stage");
  REQUIRE_EQ(plain.keyed, 0, "the key did not reach only its stage");
  REQUIRE_EQ(plain.processed, 32, "the key did not reach only its stage");
  context.detector_slot = id<project::InsertSlotId>(9);
  require(!track_chain::prepare_chain(saved, context),
          "a key on a slot outside the chain was accepted");
  host.ring_out = 0;
  auto receipts = track_chain::receipts(id<project::TrackId>(1), chain.value());
  REQUIRE_EQ(receipts.size(), 2, "receipts do not describe the chain");
  REQUIRE_TRUE(receipts[1].detector, "receipts do not describe the chain");
  REQUIRE_EQ(receipts[0].processor, routing::StageProcessor::vst3,
             "receipts do not describe the chain");
  REQUIRE_EQ(receipts[0].plugin, id<project::PluginInstanceId>(1),
             "receipts do not describe the chain");
  REQUIRE_EQ(receipts[1].slot, id<project::InsertSlotId>(2), "receipts do not describe the chain");
}

// Renders `blocks` of 32 frames of input() through `chain` from `start`.
void render(engine::ProcessorRuntime& chain, std::uint64_t start, std::uint64_t blocks,
            std::vector<StereoFrame>& output) {
  std::array<StereoFrame, 32> block{};
  for (std::uint64_t index = 0; index < blocks; ++index) {
    const auto first = start + index * block.size();
    for (std::size_t frame = 0; frame < block.size(); ++frame)
      block[frame] = input(first + frame);
    chain.process(block, {first, 1});
    output.insert(output.end(), block.begin(), block.end());
  }
}

void added_lookahead_continues_its_predecessor() {
  // A chain [gain] gains a 24-frame lookahead stage. Primed from the chain's
  // recorded boundary input, the new stage emits exactly what it would have
  // had it always run.
  auto gain = std::make_shared<Gain>(0.5F);
  track_chain::ChainRuntime before({gain}, {0}, {true}, std::nullopt);
  std::vector<StereoFrame> ignored;
  render(before, 0, 20, ignored);
  auto delay = std::make_shared<Delay>(24);
  track_chain::ChainRuntime after({gain, delay}, {0, 24}, {true, true}, std::nullopt);
  require(after.continues_latency_of(&before), "an added lookahead stage cannot continue");
  std::array<StereoFrame, 1> flushed{};
  REQUIRE_EQ(after.continue_latency_of(&before, {}, {}, {640, 1}, flushed), 0,
             "an added stage flushed frames");
  std::vector<StereoFrame> output;
  render(after, 640, 4, output);
  for (std::size_t frame = 0; frame < output.size(); ++frame) {
    const auto source = input(640 + frame - 24);
    require(output[frame].left == source.left * 0.5F && output[frame].right == source.right * 0.5F,
            "the primed lookahead lost time at frame " + std::to_string(frame));
  }
  // A VST3 stage cannot be replayed, so it never continues its predecessor.
  track_chain::ChainRuntime opaque({gain, delay}, {0, 24}, {true, false}, std::nullopt);
  require(!opaque.continues_latency_of(&before), "a non-replayable stage claimed to continue");
}

void removed_lookahead_hands_on_its_samples() {
  // A chain [delay 16, gain] loses its delay: the samples the delay still
  // held continue through the gain, and the path then plays undelayed.
  auto delay = std::make_shared<Delay>(16);
  auto gain = std::make_shared<Gain>(2.0F);
  track_chain::ChainRuntime before({delay, gain}, {16, 0}, {true, true}, std::nullopt);
  std::vector<StereoFrame> ignored;
  render(before, 0, 10, ignored);
  track_chain::ChainRuntime after({gain}, {0}, {true}, std::nullopt);
  require(after.continues_latency_of(&before), "a removed lookahead stage cannot continue");
  std::array<StereoFrame, 64> flushed{};
  const auto count = after.continue_latency_of(&before, {}, {}, {320, 1}, flushed);
  REQUIRE_EQ(count, 16, "the removed stage did not hand on its held samples");
  for (std::size_t frame = 0; frame < count; ++frame) {
    const auto source = input(320 - 16 + frame);
    REQUIRE_EQ(flushed[frame].left, source.left * 2.0F,
               "a handed-on sample skipped the stages after it");
    REQUIRE_EQ(flushed[frame].right, source.right * 2.0F,
               "a handed-on sample skipped the stages after it");
  }
  REQUIRE_EQ(after.releasable_latency(), 0, "releasable latency is not the chain's total");
  REQUIRE_EQ(before.releasable_latency(), 16, "releasable latency is not the chain's total");
}

} // namespace

int main() {
  try {
    unchanged_stages_carry_over();
    switch_retires_by_ring_out();
    detector_keys_its_stage();
    added_lookahead_continues_its_predecessor();
    removed_lookahead_hands_on_its_samples();
  } catch (const std::exception& error) {
    std::fprintf(stderr, "track chain test: %s\n", error.what());
    return 1;
  }
  return 0;
}
