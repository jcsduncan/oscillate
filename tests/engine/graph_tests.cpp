#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/check.hpp"

#include <oscillate/engine/graph.hpp>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

namespace {
using oscillate::engine::BlockContext;
using oscillate::engine::GraphPublisher;
using oscillate::engine::NodeRequest;
using oscillate::engine::prepare_graph;
using oscillate::engine::ProcessorRuntime;
using oscillate::engine::PublishStatus;
using oscillate::engine::RenderPlan;
using oscillate::engine::StereoFrame;
using oscillate::engine::TransitionKind;

using oscillate::test::require;

void near(float actual, float expected, std::string_view message) {
  REQUIRE_LT(std::fabs(actual - expected), 0.0001F, message);
}

class Ramp final : public ProcessorRuntime {
public:
  void process(std::span<StereoFrame> stereo, BlockContext /*context*/) noexcept override {
    for (auto& sample : stereo) {
      const auto value = static_cast<float>(++position_);
      sample.left += value;
      sample.right += value;
    }
  }

private:
  unsigned position_ = 0;
};

class Tail final : public ProcessorRuntime {
public:
  explicit Tail(int* destruction_count = nullptr) : destruction_count_(destruction_count) {}
  ~Tail() override {
    if (destruction_count_ != nullptr) {
      ++*destruction_count_;
    }
  }
  void process(std::span<StereoFrame> stereo, BlockContext /*context*/) noexcept override {
    for (auto& sample : stereo) {
      sample.left += history_ * 0.5F;
      sample.right += history_ * 0.5F;
      history_ = sample.left;
    }
  }

private:
  float history_ = 0.0F;
  int* destruction_count_;
};

struct CapacityFixtureState {
  int over_capacity = 0;
  int destroyed = 0;
};

class CapacitySensitive final : public ProcessorRuntime {
public:
  CapacitySensitive(std::size_t max_frames, CapacityFixtureState* state, bool count_destruction)
      : max_frames_(max_frames), state_(state), count_destruction_(count_destruction) {}
  ~CapacitySensitive() override {
    if (count_destruction_) {
      ++state_->destroyed;
    }
  }
  void process(std::span<StereoFrame> stereo, BlockContext /*context*/) noexcept override {
    if (stereo.size() > max_frames_) {
      ++state_->over_capacity;
    }
    for (auto& sample : stereo) {
      sample.left += 1.0F;
      sample.right += 1.0F;
    }
  }

private:
  std::size_t max_frames_;
  CapacityFixtureState* state_;
  bool count_destruction_;
};

NodeRequest ramp(std::uint64_t key = 1) {
  return {1, key, [] { return std::make_unique<Ramp>(); }};
}
NodeRequest tail(std::uint64_t key = 1, int* destruction_count = nullptr) {
  return {2, key, [destruction_count] { return std::make_unique<Tail>(destruction_count); }};
}

std::unique_ptr<RenderPlan> prepared(std::uint64_t revision, const std::vector<NodeRequest>& nodes,
                                     const RenderPlan* prior = nullptr,
                                     std::size_t max_frames = 4) {
  auto result = prepare_graph(revision, max_frames, nodes, prior);
  REQUIRE_OK(result, "fixture graph preparation failed");
  return std::move(result).value();
}

bool render(GraphPublisher& publisher, std::span<StereoFrame> stereo) {
  return publisher.render_block(stereo, {0, 1});
}

struct ContextFixture {
  BlockContext received{};
  std::size_t frames = 0;
  int calls = 0;
};

class StereoContextSource final : public ProcessorRuntime {
public:
  explicit StereoContextSource(ContextFixture& fixture) : fixture_(fixture) {}
  void process(std::span<StereoFrame> stereo, BlockContext context) noexcept override {
    fixture_.received = context;
    fixture_.frames = stereo.size();
    ++fixture_.calls;
    for (std::size_t index = 0; index < stereo.size(); ++index) {
      stereo[index].left = static_cast<float>(context.project_sample_position + index + 1);
      stereo[index].right = -static_cast<float>(context.project_sample_position + index + 1);
    }
  }

private:
  ContextFixture& fixture_;
};

class LeftOnlyEffect final : public ProcessorRuntime {
public:
  explicit LeftOnlyEffect(ContextFixture& fixture) : fixture_(fixture) {}
  void process(std::span<StereoFrame> stereo, BlockContext context) noexcept override {
    fixture_.received = context;
    fixture_.frames = stereo.size();
    ++fixture_.calls;
    for (auto& frame : stereo) {
      frame.left *= 2.0F;
    }
  }

private:
  ContextFixture& fixture_;
};

void stereo_channels_and_context_are_preserved() {
  GraphPublisher publisher;
  ContextFixture source_fixture;
  ContextFixture effect_fixture;
  const std::vector<NodeRequest> nodes{
      {1, 1, [&source_fixture] { return std::make_unique<StereoContextSource>(source_fixture); }},
      {2, 1, [&effect_fixture] { return std::make_unique<LeftOnlyEffect>(effect_fixture); }}};
  REQUIRE_EQ(publisher.publish(prepared(1, nodes, nullptr, 4)), PublishStatus::accepted,
             "stereo source publish failed");
  std::array<StereoFrame, 3> block{};
  require(publisher.render_block(block, {7, 3}), "stereo source render failed");
  REQUIRE_EQ(source_fixture.received.project_sample_position, 7,
             "processor context was not forwarded exactly");
  REQUIRE_EQ(source_fixture.received.transport_generation, 3,
             "processor context was not forwarded exactly");
  REQUIRE_EQ(source_fixture.frames, 3, "processor context was not forwarded exactly");
  REQUIRE_EQ(source_fixture.calls, 1, "processor context was not forwarded exactly");
  REQUIRE_EQ(effect_fixture.received.project_sample_position, 7,
             "processor context was not forwarded exactly");
  REQUIRE_EQ(effect_fixture.received.transport_generation, 3,
             "processor context was not forwarded exactly");
  REQUIRE_EQ(effect_fixture.frames, 3, "processor context was not forwarded exactly");
  REQUIRE_EQ(effect_fixture.calls, 1, "processor context was not forwarded exactly");
  near(block[0].left, 16.0F, "left channel lost source or effect");
  near(block[0].right, -8.0F, "right channel was mixed with left");
  near(block[2].left, 20.0F, "left frame count or position wrong");
  near(block[2].right, -10.0F, "right frame count or position wrong");

  require(publisher.render_block(block, {2, 4}), "seek generation render failed");
  REQUIRE_EQ(source_fixture.received.project_sample_position, 2,
             "engine inferred or altered a seek generation");
  REQUIRE_EQ(source_fixture.received.transport_generation, 4,
             "engine inferred or altered a seek generation");
  REQUIRE_EQ(source_fixture.calls, 2, "engine inferred or altered a seek generation");
  REQUIRE_EQ(effect_fixture.received.project_sample_position, 2,
             "engine inferred or altered a seek generation");
  REQUIRE_EQ(effect_fixture.received.transport_generation, 4,
             "engine inferred or altered a seek generation");
  REQUIRE_EQ(effect_fixture.calls, 2, "engine inferred or altered a seek generation");
  near(block[0].left, 6.0F, "seek did not use supplied position");
  near(block[0].right, -3.0F, "seek right channel wrong");
}

void invalid_context_preserves_pending_revision() {
  GraphPublisher publisher;
  ContextFixture fixture;
  const std::vector<NodeRequest> nodes{
      {1, 1, [&fixture] { return std::make_unique<StereoContextSource>(fixture); }}};
  REQUIRE_EQ(publisher.publish(prepared(1, nodes, nullptr, 4)), PublishStatus::accepted,
             "context fixture publish failed");
  std::array<StereoFrame, 2> block{{{9.0F, 9.0F}, {9.0F, 9.0F}}};
  require(!publisher.render_block(block, {0, 0}), "zero generation was accepted");
  REQUIRE_EQ(publisher.active_revision(), 0, "invalid generation activated graph");
  REQUIRE_TRUE(publisher.has_pending(), "invalid generation activated graph");
  REQUIRE_EQ(fixture.calls, 0, "invalid generation activated graph");
  near(block[0].left, 0.0F, "invalid generation left output not silent");
  near(block[0].right, 0.0F, "invalid generation right output not silent");
  require(!publisher.render_block(block, {std::numeric_limits<std::uint64_t>::max() - 1, 1}),
          "overflowing project-sample end was accepted");
  REQUIRE_EQ(publisher.active_revision(), 0, "overflowing context activated graph");
  REQUIRE_TRUE(publisher.has_pending(), "overflowing context activated graph");
  REQUIRE_EQ(fixture.calls, 0, "overflowing context activated graph");
  require(publisher.render_block(block, {std::numeric_limits<std::uint64_t>::max() - 2, 1}),
          "last representable two-frame interval failed");
  REQUIRE_EQ(publisher.active_revision(), 1, "valid boundary block failed to activate graph");
  REQUIRE_TRUE(!publisher.has_pending(), "valid boundary block failed to activate graph");
  REQUIRE_EQ(fixture.calls, 1, "valid boundary block failed to activate graph");
  REQUIRE_EQ(fixture.received.project_sample_position,
             std::numeric_limits<std::uint64_t>::max() - 2, "boundary position was altered");
}

void maximum_block_is_counted_in_frames() {
  GraphPublisher publisher;
  REQUIRE_EQ(publisher.publish(prepared(1, {ramp()}, nullptr, 8192)), PublishStatus::accepted,
             "maximum-size graph publish failed");
  std::vector<StereoFrame> too_many(8193, {7.0F, -7.0F});
  require(!publisher.render_block(too_many, {0, 1}), "8193-frame callback was accepted");
  REQUIRE_EQ(publisher.active_revision(), 0, "oversized callback activated pending revision");
  REQUIRE_TRUE(publisher.has_pending(), "oversized callback activated pending revision");
  near(too_many.front().left, 7.0F, "unbounded left callback buffer was touched");
  near(too_many.back().right, -7.0F, "unbounded right callback buffer was touched");
  std::vector<StereoFrame> maximum(8192);
  require(publisher.render_block(maximum, {0, 1}), "8192-frame callback was rejected");
  REQUIRE_EQ(publisher.active_revision(), 1, "maximum-size callback did not activate revision");
  REQUIRE_TRUE(!publisher.has_pending(), "maximum-size callback did not activate revision");
  near(maximum.front().left, 1.0F, "maximum-size left first frame wrong");
  near(maximum.back().right, 8192.0F, "maximum-size right last frame wrong");
}

void state_and_tail_survive_topology_edit() {
  GraphPublisher publisher;
  const std::vector first_nodes{ramp(), tail()};
  REQUIRE_EQ(publisher.publish(prepared(1, first_nodes)), PublishStatus::accepted,
             "first plan rejected");
  std::array<StereoFrame, 2> block{};
  require(render(publisher, block), "first render failed");
  near(block[0].left, 1.0F, "ramp first sample");
  near(block[1].left, 2.5F, "tail first block");

  int factory_calls = 0;
  std::vector<NodeRequest> second_nodes{ramp(), {2, 1, [&factory_calls] {
                                                   ++factory_calls;
                                                   return std::make_unique<Tail>();
                                                 }}};
  auto second = prepared(2, second_nodes, publisher.active_plan());
  REQUIRE_EQ(second->transitions().size(), 2, "compatible runtime not marked reused");
  REQUIRE_EQ(second->transitions()[0].kind, TransitionKind::reused,
             "compatible runtime not marked reused");
  REQUIRE_EQ(second->transitions()[1].kind, TransitionKind::reused,
             "compatible runtime not marked reused");
  REQUIRE_EQ(factory_calls, 0, "factory called for compatible processor");
  REQUIRE_EQ(publisher.publish(std::move(second)), PublishStatus::accepted, "second plan rejected");
  REQUIRE_EQ(publisher.active_revision(), 1, "plan switched before callback boundary");
  require(render(publisher, block), "second render failed");
  near(block[0].left, 4.25F, "tail history reset across graph switch");
  near(block[1].left, 6.125F, "ramp phase reset across graph switch");
  REQUIRE_EQ(publisher.active_revision(), 2, "revision not published");
  REQUIRE_EQ(publisher.collect_retired().reclaimed, 1, "old plan not reclaimed");
}

void failure_and_supersession_keep_valid_graph() {
  GraphPublisher publisher;
  std::array<StereoFrame, 1> block{};
  auto failed = prepare_graph(1, 4,
                              std::vector<NodeRequest>{{1, 1,
                                                        []() -> std::unique_ptr<ProcessorRuntime> {
                                                          throw std::runtime_error("fixture");
                                                        }}},
                              nullptr);
  REQUIRE_TRUE(!failed, "failed preparation published a graph");
  REQUIRE_TRUE(!publisher.has_pending(), "failed preparation published a graph");
  auto null_result = prepare_graph(
      1, 4, std::vector<NodeRequest>{{1, 1, [] { return std::unique_ptr<ProcessorRuntime>{}; }}},
      nullptr);
  require(!null_result, "null runtime accepted");
  REQUIRE_EQ(publisher.publish(prepared(1, {ramp()})), PublishStatus::accepted,
             "pending plan rejected");
  REQUIRE_EQ(publisher.publish(prepared(2, {ramp()})), PublishStatus::accepted,
             "new pending plan rejected");
  REQUIRE_EQ(publisher.publish(prepared(1, {ramp()})), PublishStatus::stale_revision,
             "older pending revision displaced newest plan");
  REQUIRE_EQ(publisher.publish(prepared(2, {tail()})), PublishStatus::stale_revision,
             "duplicate revision displaced pending plan");
  require(render(publisher, block), "superseding plan did not render");
  REQUIRE_EQ(publisher.active_revision(), 2, "superseded plan became active");
  near(block[0].left, 1.0F, "unexpected source output");
  REQUIRE_EQ(publisher.collect_retired().reclaimed, 0, "supersession entered retirement slot");
}

void backpressure_and_stale_base() {
  GraphPublisher publisher;
  std::array<StereoFrame, 1> block{};
  REQUIRE_EQ(publisher.publish(prepared(1, {ramp()})), PublishStatus::accepted,
             "first publish failed");
  require(render(publisher, block), "first render failed");
  auto second = prepared(2, {ramp()}, publisher.active_plan());
  REQUIRE_EQ(publisher.publish(std::move(second)), PublishStatus::accepted,
             "second publish failed");
  require(render(publisher, block), "second render failed");
  near(block[0].left, 2.0F, "second graph reset phase");
  auto third = prepared(3, {ramp()}, publisher.active_plan());
  REQUIRE_EQ(publisher.publish(std::move(third)), PublishStatus::accepted, "third publish failed");
  require(render(publisher, block), "backpressure render failed");
  REQUIRE_EQ(publisher.active_revision(), 2, "occupied retirement slot did not hold pending graph");
  REQUIRE_TRUE(publisher.has_pending(), "occupied retirement slot did not hold pending graph");
  near(block[0].left, 3.0F, "active graph did not render during backpressure");
  REQUIRE_EQ(publisher.collect_retired().reclaimed, 1, "retired graph not collected");
  require(render(publisher, block), "third render failed");
  REQUIRE_EQ(publisher.active_revision(), 3, "pending graph did not activate");
  near(block[0].left, 4.0F, "phase reset after backpressure");

  // Build against rev 1 and publish after rev 3 is active: callback rejects it.
  auto stale = prepared(4, {ramp()}, nullptr);
  REQUIRE_EQ(publisher.publish(std::move(stale)), PublishStatus::accepted,
             "stale fixture publish failed");
  REQUIRE_EQ(publisher.collect_retired().reclaimed, 1, "second old plan not collected");
  require(render(publisher, block), "active render failed on stale plan");
  REQUIRE_EQ(publisher.active_revision(), 3, "stale-base graph became active");
  REQUIRE_EQ(publisher.publish(prepared(4, {ramp()}, publisher.active_plan())),
             PublishStatus::stale_revision,
             "same-revision retry was accepted before rejection collection");
  const auto report = publisher.collect_retired();
  REQUIRE_EQ(report.reclaimed, 1, "stale-base rejection was not reported");
  REQUIRE_EQ(report.rejected_revision, 4, "stale-base rejection was not reported");
  REQUIRE_EQ(publisher.publish(prepared(4, {ramp()})), PublishStatus::stale_revision,
             "retry using stale base was accepted");
  auto retry = prepared(4, {ramp()}, publisher.active_plan());
  REQUIRE_EQ(retry->base_revision(), 3, "retry did not use active graph as base");
  REQUIRE_EQ(publisher.publish(std::move(retry)), PublishStatus::accepted,
             "collected stale-base revision could not be retried");
  require(render(publisher, block), "retry render failed");
  REQUIRE_EQ(publisher.active_revision(), 4, "retry did not activate same model revision");
  near(block[0].left, 6.0F, "retry reset running processor phase");
  REQUIRE_EQ(publisher.publish(prepared(4, {tail()})), PublishStatus::stale_revision,
             "active revision was replaced by duplicate");
  REQUIRE_EQ(publisher.publish(prepared(3, {tail()})), PublishStatus::stale_revision,
             "older revision was accepted after retry");
}

void replacement_has_visible_bounded_fallback() {
  GraphPublisher publisher;
  int destroyed = 0;
  REQUIRE_EQ(publisher.publish(prepared(1, {ramp(), tail(1, &destroyed)})), PublishStatus::accepted,
             "first publish failed");
  std::array<StereoFrame, 1> block{};
  require(render(publisher, block), "first render failed");
  auto replacement = prepared(2, {ramp(), tail(2)}, publisher.active_plan());
  REQUIRE_EQ(replacement->transitions()[1].kind, TransitionKind::replaced,
             "replacement limitation absent");
  REQUIRE_TRUE(replacement->transitions()[1].audible_continuity_limited,
               "replacement limitation absent");
  REQUIRE_EQ(publisher.publish(std::move(replacement)), PublishStatus::accepted,
             "replacement publish failed");
  require(render(publisher, block), "fallback block failed");
  near(block[0].left, 2.0F, "replacement did not render reused source phase");
  REQUIRE_EQ(destroyed, 0, "old runtime destroyed on callback");
  REQUIRE_EQ(publisher.collect_retired().reclaimed, 1,
             "old runtime not destroyed on control collection");
  REQUIRE_EQ(destroyed, 1, "old runtime not destroyed on control collection");
  require(render(publisher, block), "post-fallback render failed");
  near(block[0].left, 4.0F, "new processor history was not retained after fallback");
}

void growth_replaces_capacity_sensitive_runtime() {
  GraphPublisher publisher;
  CapacityFixtureState state;
  int factory_calls = 0;
  const auto request = [&](std::size_t capacity, bool count_destruction) {
    return NodeRequest{1, 1, [&, capacity, count_destruction] {
                         ++factory_calls;
                         return std::make_unique<CapacitySensitive>(capacity, &state,
                                                                    count_destruction);
                       }};
  };
  REQUIRE_EQ(publisher.publish(prepared(1, {request(4, true)}, nullptr, 4)),
             PublishStatus::accepted, "capacity fixture first publish failed");
  std::array<StereoFrame, 4> first{};
  require(render(publisher, first), "capacity fixture first render failed");
  REQUIRE_EQ(factory_calls, 1, "initial runtime factory count wrong");
  require(!prepare_graph(2, 8, std::vector<NodeRequest>{{1, 1, {}}}, publisher.active_plan()),
          "larger block reused runtime without replacement factory");

  auto enlarged = prepared(2, {request(8, false)}, publisher.active_plan(), 8);
  REQUIRE_EQ(factory_calls, 2, "larger block did not create a fresh runtime");
  REQUIRE_EQ(enlarged->transitions()[0].kind, TransitionKind::replaced,
             "capacity replacement limitation was not visible");
  REQUIRE_TRUE(enlarged->transitions()[0].audible_continuity_limited,
               "capacity replacement limitation was not visible");
  REQUIRE_EQ(publisher.publish(std::move(enlarged)), PublishStatus::accepted,
             "capacity replacement publish failed");
  std::array<StereoFrame, 8> second{};
  require(render(publisher, second), "larger block render failed");
  REQUIRE_EQ(state.over_capacity, 0, "undersized runtime processed a larger block");
  near(second.back().left, 1.0F, "replacement output fade did not complete");
  REQUIRE_EQ(state.destroyed, 0, "old runtime destroyed in callback");
  REQUIRE_EQ(publisher.collect_retired().reclaimed, 1,
             "old capacity runtime not reclaimed on control side");
  REQUIRE_EQ(state.destroyed, 1, "old capacity runtime not reclaimed on control side");
}

void undersized_pending_plan_waits_for_fitting_callback_block() {
  GraphPublisher publisher;
  REQUIRE_EQ(publisher.publish(prepared(1, {ramp()}, nullptr, 128)), PublishStatus::accepted,
             "wide graph publish failed");
  std::array<StereoFrame, 128> wide{};
  require(render(publisher, wide), "wide graph first render failed");
  near(wide.front().left, 1.0F, "wide graph first sample wrong");
  near(wide.back().left, 128.0F, "wide graph last sample wrong");

  auto narrow = prepared(2, {ramp()}, publisher.active_plan(), 64);
  REQUIRE_EQ(narrow->transitions()[0].kind, TransitionKind::reused,
             "narrow graph unexpectedly replaced compatible processor");
  REQUIRE_EQ(publisher.publish(std::move(narrow)), PublishStatus::accepted,
             "narrow graph publish failed");
  require(render(publisher, wide), "active wide graph did not cover oversized pending block");
  REQUIRE_EQ(publisher.active_revision(), 1, "undersized pending graph incorrectly became active");
  REQUIRE_TRUE(publisher.has_pending(), "undersized pending graph incorrectly became active");
  near(wide.front().left, 129.0F, "active graph stopped during pending capacity wait");
  near(wide.back().left, 256.0F, "active graph phase broke during pending capacity wait");
  REQUIRE_EQ(publisher.collect_retired().reclaimed, 0,
             "capacity-deferred plan was incorrectly retired");

  std::array<StereoFrame, 64> fitting{};
  require(render(publisher, fitting), "fitting block did not render pending graph");
  REQUIRE_EQ(publisher.active_revision(), 2, "pending graph did not activate on fitting block");
  REQUIRE_TRUE(!publisher.has_pending(), "pending graph did not activate on fitting block");
  near(fitting.front().left, 257.0F, "phase reset on deferred graph activation");
  near(fitting.back().left, 320.0F, "deferred graph ended at wrong sample");
  REQUIRE_EQ(publisher.collect_retired().reclaimed, 1, "old wide graph was not reclaimed");

  GraphPublisher initially_empty;
  REQUIRE_EQ(initially_empty.publish(prepared(1, {ramp()}, nullptr, 64)), PublishStatus::accepted,
             "empty publisher rejected narrow graph");
  require(!render(initially_empty, std::span<StereoFrame>{}), "zero-frame callback rendered");
  REQUIRE_EQ(initially_empty.active_revision(), 0, "zero-frame callback activated pending graph");
  REQUIRE_TRUE(initially_empty.has_pending(), "zero-frame callback activated pending graph");
  wide.fill({99.0F, 99.0F});
  require(!render(initially_empty, wide), "empty publisher rendered oversized pending plan");
  REQUIRE_EQ(initially_empty.active_revision(), 0,
             "oversized first block published a silent graph");
  REQUIRE_TRUE(initially_empty.has_pending(), "oversized first block published a silent graph");
  near(wide.front().left, 0.0F, "empty graph failed to silence oversized first block");
  near(wide.front().right, 0.0F, "empty graph failed to silence right channel");
  require(render(initially_empty, fitting), "fitting first block did not activate graph");
  REQUIRE_EQ(initially_empty.active_revision(), 1, "fitting first block revision incorrect");
}

void transition_ramp_is_bounded_and_reports_removed_nodes() {
  GraphPublisher publisher;
  REQUIRE_EQ(publisher.publish(prepared(1, {ramp()})), PublishStatus::accepted,
             "source graph rejected");
  std::array<StereoFrame, 4> block{};
  require(render(publisher, block), "source block failed");
  near(block.back().left, 4.0F, "source last sample incorrect");

  auto inserted = prepared(2, {ramp(), tail()}, publisher.active_plan());
  REQUIRE_EQ(inserted->transitions()[1].kind, TransitionKind::created,
             "inserted processor did not report transition");
  REQUIRE_TRUE(inserted->transitions()[1].audible_continuity_limited,
               "inserted processor did not report transition");
  REQUIRE_EQ(publisher.publish(std::move(inserted)), PublishStatus::accepted,
             "inserted graph rejected");
  require(render(publisher, block), "inserted graph failed");
  near(block[0].left, 4.25F, "transition fade first sample wrong");
  near(block[1].left, 6.25F, "transition fade second sample wrong");
  near(block[2].left, 9.4375F, "transition fade third sample wrong");
  near(block[3].left, 13.625F, "transition fade did not reach new graph");
  REQUIRE_EQ(publisher.collect_retired().reclaimed, 1, "old graph not reclaimed");

  auto removed = prepared(3, {ramp()}, publisher.active_plan());
  REQUIRE_EQ(removed->transitions().size(), 2, "removed processor did not report tail loss");
  REQUIRE_EQ(removed->transitions()[1].kind, TransitionKind::retired,
             "removed processor did not report tail loss");
  REQUIRE_TRUE(removed->transitions()[1].audible_continuity_limited,
               "removed processor did not report tail loss");
  REQUIRE_EQ(publisher.publish(std::move(removed)), PublishStatus::accepted,
             "removal graph rejected");
  require(render(publisher, block), "removal graph failed");
  near(block[0].left, 12.46875F, "removal fade first sample wrong");
  near(block[3].left, 12.0F, "removal fade did not reach new output");
}

void invalid_input_is_rejected() {
  require(!prepare_graph(0, 4, std::vector<NodeRequest>{}, nullptr), "zero revision accepted");
  require(!prepare_graph(1, 0, std::vector<NodeRequest>{}, nullptr), "zero frames accepted");
  require(!prepare_graph(1, 8193, std::vector<NodeRequest>{}, nullptr), "oversized block accepted");
  require(!prepare_graph(1, 4, std::vector<NodeRequest>{{0, 1, {}}}, nullptr),
          "zero instance ID accepted");
  require(!prepare_graph(1, 4, std::vector<NodeRequest>{{1, 0, {}}}, nullptr),
          "zero compatibility key accepted");
  require(!prepare_graph(1, 4, std::vector<NodeRequest>{{1, 1, {}}}, nullptr),
          "missing factory accepted");
  require(!prepare_graph(1, 4, std::vector<NodeRequest>{ramp(), ramp()}, nullptr),
          "duplicate ID accepted");
  std::vector<NodeRequest> too_many(1025, ramp());
  require(!prepare_graph(1, 4, too_many, nullptr), "node bound ignored");
  require(!prepare_graph(1, 4,
                         std::vector<NodeRequest>{
                             {1, 1, []() -> std::unique_ptr<ProcessorRuntime> { throw 7; }}},
                         nullptr),
          "unknown preparation failure accepted");
  auto first = prepared(1, {ramp()});
  require(!prepare_graph(1, 4, std::vector<NodeRequest>{ramp()}, first.get()),
          "non-advancing revision accepted");
  GraphPublisher publisher;
  REQUIRE_EQ(publisher.publish(nullptr), PublishStatus::invalid_plan, "null plan accepted");
  std::array<StereoFrame, 1> block{{{99.0F, 99.0F}}};
  require(!render(publisher, block), "render without plan succeeded");
  near(block[0].left, 0.0F, "no-plan output not silent");
  REQUIRE_EQ(publisher.publish(std::move(first)), PublishStatus::accepted, "valid plan rejected");
  std::array<StereoFrame, 5> oversized{};
  require(!render(publisher, oversized), "oversized block accepted");
  for (const auto sample : oversized) {
    near(sample.left, 0.0F, "oversized block not silenced");
  }
  std::vector<StereoFrame> beyond_cap(8193, {7.0F, 7.0F});
  require(!render(publisher, beyond_cap), "hard callback frame cap ignored");
  near(beyond_cap.front().left, 7.0F, "oversized caller buffer front was touched");
  near(beyond_cap.front().right, 7.0F, "oversized caller right buffer was touched");
  near(beyond_cap.back().left, 7.0F, "oversized caller buffer end was touched");
}

void callback_does_not_allocate_and_concurrent_handoff_reclaims() {
  GraphPublisher publisher;
  REQUIRE_EQ(publisher.publish(prepared(1, {ramp()})), PublishStatus::accepted,
             "initial stress graph rejected");
  std::atomic<std::size_t> callback_allocations{0};
  std::jthread callback([&](const std::stop_token& stop) {
    std::array<StereoFrame, 4> samples{};
    counting_allocations = true;
    while (!stop.stop_requested()) {
      (void)render(publisher, samples);
    }
    counting_allocations = false;
    callback_allocations.store(counted_allocations, std::memory_order_release);
  });
  for (std::uint64_t revision = 1; revision <= 100; ++revision) {
    if (revision != 1) {
      const auto* prior = publisher.active_plan();
      REQUIRE_NE(prior, nullptr, "stress graph lost active plan");
      auto next = prepared(revision, {ramp()}, prior);
      REQUIRE_EQ(publisher.publish(std::move(next)), PublishStatus::accepted,
                 "stress publish failed");
    }
    std::size_t attempts = 0;
    while (publisher.active_revision() != revision && attempts < 1'000'000) {
      const auto report = publisher.collect_retired();
      REQUIRE_EQ(report.rejected_revision, 0, "stress graph rejected unexpectedly");
      ++attempts;
      std::this_thread::yield();
    }
    REQUIRE_EQ(publisher.active_revision(), revision, "callback did not publish graph");
    (void)publisher.collect_retired();
  }
  callback.request_stop();
  callback.join();
  REQUIRE_EQ(callback_allocations.load(std::memory_order_acquire), 0,
             "callback allocated during graph handoff");
  REQUIRE_LE(publisher.collect_retired().reclaimed, 1, "retirement exceeded one-slot bound");
}

void concurrent_oversized_pending_supersession() {
  GraphPublisher publisher;
  REQUIRE_EQ(publisher.publish(prepared(1, {ramp()}, nullptr, 128)), PublishStatus::accepted,
             "concurrent capacity fixture setup failed");
  std::array<StereoFrame, 128> first{};
  require(render(publisher, first), "concurrent capacity first block failed");
  std::jthread callback([&](const std::stop_token& stop) {
    std::array<StereoFrame, 128> wide{};
    while (!stop.stop_requested()) {
      (void)render(publisher, wide);
    }
  });
  for (std::uint64_t revision = 2; revision <= 501; ++revision) {
    auto next = prepared(revision, {ramp()}, publisher.active_plan(), 64);
    REQUIRE_EQ(publisher.publish(std::move(next)), PublishStatus::accepted,
               "concurrent narrow publish failed");
    (void)publisher.collect_retired();
  }
  callback.request_stop();
  callback.join();
  (void)publisher.collect_retired();
  REQUIRE_EQ(publisher.active_revision(), 1,
             "oversized callback activated a narrow graph under supersession");
  REQUIRE_TRUE(publisher.has_pending(),
               "oversized callback activated a narrow graph under supersession");
  std::array<StereoFrame, 64> fitting{};
  require(render(publisher, fitting), "latest pending graph did not render fitting block");
  REQUIRE_EQ(publisher.active_revision(), 501, "latest pending revision was lost");
}
} // namespace

int main() {
  try {
    stereo_channels_and_context_are_preserved();
    invalid_context_preserves_pending_revision();
    maximum_block_is_counted_in_frames();
    state_and_tail_survive_topology_edit();
    failure_and_supersession_keep_valid_graph();
    backpressure_and_stale_base();
    replacement_has_visible_bounded_fallback();
    growth_replaces_capacity_sensitive_runtime();
    undersized_pending_plan_waits_for_fitting_callback_block();
    transition_ramp_is_bounded_and_reports_removed_nodes();
    invalid_input_is_rejected();
    callback_does_not_allocate_and_concurrent_handoff_reclaims();
    concurrent_oversized_pending_supersession();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
