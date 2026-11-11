// The sub-block wavefront (ADR-0127): a live block cut into parts, each
// level one part behind the level before it, renders exactly what whole
// blocks rendered level by level, for every graph shape, block size, part
// size and helper count, with sources filled inside the schedule or before
// it.
#include "oscillate/engine/audio_worker_pool.hpp"
#include "oscillate/engine/routed_mixer.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {
using namespace oscillate;
using namespace oscillate::engine;
using oscillate::test::require;
using oscillate::test::take;

constexpr std::size_t largest_block = 1024;
constexpr std::uint64_t transport_generation = 1;
constexpr std::size_t rendered_blocks = 12;
// Block sizes a device may ask for, the odd one included.
constexpr std::array block_sizes{std::size_t{64},  std::size_t{128}, std::size_t{256},
                                 std::size_t{333}, std::size_t{512}, std::size_t{1024}};
// Fixed part lengths, the odd one and one longer than some blocks included.
constexpr std::array part_sizes{std::size_t{32}, std::size_t{64}, std::size_t{100},
                                std::size_t{256}};
constexpr std::array helper_counts{std::size_t{1}, std::size_t{3}, std::size_t{7}};
// A latent stage's delay, which the plan compensates on parallel paths.
constexpr std::uint32_t latent_frames = 8;
constexpr std::uint32_t compensation_limit = 64;
constexpr std::uint32_t gain_ramp_frames = 300;
// The block at which a mute and a live send change arrive mid-render.
constexpr std::size_t edit_block = 7;
constexpr float send_gain = 0.4F;
constexpr float live_send_gain = 0.15F;
constexpr float bus_gain = 0.8F;
constexpr double tone_rate = 0.0031;
constexpr double tone_spread = 0.37;
constexpr double tone_right_ratio = 0.7;
constexpr double automation_rate = 0.0007;
constexpr float half = 0.5F;
// The fake processors' shapes: Colour's drive and fold-back, Ducker's
// follower and depth.
constexpr float colour_drive = 1.3F;
constexpr float colour_fold = 0.2F;
constexpr float follower_rate = 0.05F;
constexpr float duck_depth = 2.0F;
// Smoothing coefficients: tracks step up from the first, buses, returns,
// masters and the stages after the master's fader have their own.
constexpr float track_smoothing = 0.1F;
constexpr float nested_track_smoothing = 0.07F;
constexpr float session_track_smoothing = 0.05F;
constexpr float group_smoothing = 0.6F;
constexpr float second_group_smoothing = 0.7F;
constexpr float nested_second_group_smoothing = 0.45F;
constexpr float return_smoothing = 0.3F;
constexpr float master_smoothing = 0.5F;
constexpr float nested_master_smoothing = 0.55F;
constexpr float output_stage_smoothing = 0.9F;
constexpr float chain_first_smoothing = 0.4F;
constexpr float chain_last_smoothing = 0.8F;
// Node ids: tracks count up from 1; groups, a return and the master.
constexpr RoutingNodeId first_group = 20;
constexpr RoutingNodeId second_group = 21;
constexpr RoutingNodeId return_bus = 30;
constexpr RoutingNodeId master_id = 100;
constexpr RoutingNodeId session_tracks = 8;
constexpr RoutingNodeId nested_tracks = 6;
constexpr RoutingNodeId small_tracks = 4;
constexpr RoutingNodeId latent_track = 3;
constexpr RoutingNodeId ducked_track = 5;
constexpr RoutingNodeId key_track = 2;
constexpr RoutingNodeId send_track = 6;
// Generous buffer and history budgets for the test graphs.
constexpr std::size_t budget_factor = 4;
// The contexts a recording master keeps.
constexpr std::size_t recorded_contexts = 64;
// Helpers for the single-scenario cases, the parts they cut a block into,
// a wrapped block's generation, and the fewest parts a split has.
constexpr std::size_t few_helpers = 2;
constexpr std::size_t some_helpers = 3;
constexpr std::size_t quarter_parts = 4;
constexpr std::size_t eighth_parts = 8;
constexpr std::uint64_t wrapped_generation = 2;
// A Busy processor that costs nothing.
constexpr std::chrono::microseconds no_cost{0};
constexpr std::size_t fewest_split_parts = 2;

// Order-sensitive state: any lost, reordered or repeated frame changes the
// bits of everything after it.
class Colour final : public ProcessorRuntime {
public:
  explicit Colour(float coefficient) : coefficient_(coefficient) {}
  void process(std::span<StereoFrame> frames, BlockContext) noexcept override {
    for (auto& frame : frames) {
      left_ += coefficient_ * (std::tanh(frame.left * colour_drive) - left_);
      right_ += coefficient_ * (frame.right - right_ * right_ * colour_fold - right_);
      frame = {left_, right_};
    }
  }

private:
  float coefficient_;
  float left_ = 0.0F;
  float right_ = 0.0F;
};

// A latent stage: delays its input by latent_frames.
class Latent final : public ProcessorRuntime {
public:
  void process(std::span<StereoFrame> frames, BlockContext) noexcept override {
    for (auto& frame : frames) {
      std::swap(frame, line_[cursor_]);
      cursor_ = (cursor_ + 1) % line_.size();
    }
  }

private:
  std::array<StereoFrame, latent_frames> line_{};
  std::size_t cursor_ = 0;
};

// Ducks its input by a follower on the key.
class Ducker final : public SidechainProcessorRuntime {
public:
  void process(std::span<StereoFrame> frames, BlockContext context) noexcept override {
    process_with_sidechain(frames, {}, context);
  }
  void process_with_sidechain(std::span<StereoFrame> frames, std::span<const StereoFrame> key,
                              BlockContext) noexcept override {
    for (std::size_t frame = 0; frame < frames.size(); ++frame) {
      const auto level =
          key.empty() ? 0.0F : std::abs(key[frame].left) + std::abs(key[frame].right);
      follower_ += follower_rate * (level - follower_);
      const auto gain = 1.0F / (1.0F + duck_depth * follower_);
      frames[frame] = {frames[frame].left * gain, frames[frame].right * gain};
    }
  }

private:
  float follower_ = 0.0F;
};

// A chain of stages, as track_chain::ChainRuntime is: the mixer may run its
// stages one part apart on different helpers. Its middle stage reads the
// detector input.
class Chain final : public SidechainProcessorRuntime {
public:
  static constexpr std::size_t detector_stage = 1;
  Chain() {
    stages_.push_back(std::make_shared<Colour>(chain_first_smoothing));
    stages_.push_back(std::make_shared<Ducker>());
    stages_.push_back(std::make_shared<Colour>(chain_last_smoothing));
    stages_.push_back(std::make_shared<Latent>());
  }
  void process(std::span<StereoFrame> frames, BlockContext context) noexcept override {
    process_with_sidechain(frames, {}, context);
  }
  void process_with_sidechain(std::span<StereoFrame> frames, std::span<const StereoFrame> key,
                              BlockContext context) noexcept override {
    for (std::size_t stage = 0; stage < stages_.size(); ++stage)
      run_stage(stage, frames, key, context);
  }
  [[nodiscard]] std::size_t stage_count() const noexcept override { return stages_.size(); }
  // Counts the stages the mixer ran one at a time.
  static inline std::atomic<std::size_t> staged_runs{0};
  void process_stage(std::size_t stage, std::span<StereoFrame> frames,
                     std::span<const StereoFrame> key, BlockContext context) noexcept override {
    staged_runs.fetch_add(1, std::memory_order_relaxed);
    run_stage(stage, frames, key, context);
  }

private:
  void run_stage(std::size_t stage, std::span<StereoFrame> frames, std::span<const StereoFrame> key,
                 BlockContext context) noexcept {
    if (stage == detector_stage)
      static_cast<Ducker&>(*stages_[stage]).process_with_sidechain(frames, key, context);
    else
      stages_[stage]->process(frames, context);
  }

  std::vector<std::shared_ptr<ProcessorRuntime>> stages_;
};

// A plugin host's stand-in: its output depends on how the block is cut (a
// gain from the call's length), so it says it does not render any split
// alike, and the mixer must give it whole blocks.
class Blockwise final : public ProcessorRuntime {
public:
  void process(std::span<StereoFrame> frames, BlockContext) noexcept override {
    const auto gain = 1.0F / (1.0F + static_cast<float>(frames.size()) * blockwise_slope);
    for (auto& frame : frames)
      frame = {frame.left * gain, frame.right * gain};
  }
  [[nodiscard]] bool renders_any_split() const noexcept override { return false; }

private:
  static constexpr float blockwise_slope = 0.001F;
};

// Records the contexts it is given, to check the parts' positions and the
// loop flag.
class Recorder final : public ProcessorRuntime {
public:
  void process(std::span<StereoFrame> frames, BlockContext context) noexcept override {
    if (count_ < contexts_.size())
      contexts_[count_++] = {context, frames.size()};
  }
  struct Seen {
    BlockContext context;
    std::size_t frames;
  };
  [[nodiscard]] std::span<const Seen> seen() const noexcept { return {contexts_.data(), count_}; }

private:
  std::array<Seen, recorded_contexts> contexts_{};
  std::size_t count_ = 0;
};

// Burns a fixed time a block, to make a level expensive.
class Busy final : public ProcessorRuntime {
public:
  explicit Busy(std::chrono::microseconds per_frame_block) : cost_(per_frame_block) {}
  void process(std::span<StereoFrame> frames, BlockContext) noexcept override {
    const auto until = std::chrono::steady_clock::now() + cost_ * frames.size() / largest_block;
    while (std::chrono::steady_clock::now() < until) {
    }
  }

private:
  std::chrono::microseconds cost_;
};

enum class Shape : std::uint8_t { session, nested, flat, staged, plugin };

struct Graph {
  std::vector<RoutedNode> nodes;
  std::vector<RoutingEdge> edges;
};

// session: eight tracks (one latent, one ducked by another's key), two
// groups, a return fed by two sends, and a master with a stage after its
// fader (the rebalance stage's place). nested: tracks into a subgroup into a
// group, a group send to a return. flat: tracks straight into the master.
Graph make_graph(Shape shape) {
  Graph graph;
  RoutingEdgeId edge = 1;
  const auto colour = [](float coefficient) { return std::make_shared<Colour>(coefficient); };
  const auto step = [](float base, RoutingNodeId track) {
    return base * static_cast<float>(track);
  };
  if (shape == Shape::staged) {
    // A master chain keyed by a track, behind a group: the master pipelines
    // its stages over the parts.
    for (RoutingNodeId track = 1; track <= small_tracks; ++track) {
      graph.nodes.emplace_back(RoutingNode{track, 0}, colour(step(track_smoothing, track)));
      graph.edges.push_back({edge++, track, first_group});
    }
    graph.nodes.emplace_back(RoutingNode{first_group, 0}, colour(group_smoothing));
    graph.edges.push_back({edge++, first_group, master_id});
    graph.edges.push_back({edge++, 1, master_id, 1.0F, RoutingEdgeKind::sidechain});
    graph.nodes.emplace_back(RoutingNode{master_id, latent_frames}, std::make_shared<Chain>());
    graph.nodes.back().output_stage = colour(output_stage_smoothing);
    return graph;
  }
  if (shape == Shape::plugin) {
    // A plugin on a track and one in the master chain: both stay whole while
    // the rest of the graph runs in parts.
    auto graph_with_plugins = make_graph(Shape::staged);
    graph_with_plugins.nodes[1].processor = std::make_shared<Blockwise>();
    graph_with_plugins.nodes.back().output_stage = std::make_shared<Blockwise>();
    return graph_with_plugins;
  }
  if (shape == Shape::flat) {
    for (RoutingNodeId track = 1; track <= small_tracks; ++track) {
      graph.nodes.emplace_back(RoutingNode{track, 0}, colour(step(track_smoothing, track)));
      graph.edges.push_back({edge++, track, master_id});
    }
    graph.nodes.emplace_back(RoutingNode{master_id, 0}, colour(master_smoothing));
    return graph;
  }
  if (shape == Shape::nested) {
    for (RoutingNodeId track = 1; track <= nested_tracks; ++track) {
      graph.nodes.emplace_back(RoutingNode{track, 0}, colour(step(nested_track_smoothing, track)));
      graph.edges.push_back(
          {edge++, track,
           track <= nested_tracks / fewest_split_parts ? first_group : second_group});
    }
    graph.nodes.emplace_back(RoutingNode{first_group, 0}, colour(group_smoothing));
    graph.nodes.emplace_back(RoutingNode{second_group, 0}, colour(nested_second_group_smoothing));
    graph.edges.push_back({edge++, first_group, second_group, bus_gain});
    graph.nodes.emplace_back(RoutingNode{return_bus, 0}, colour(return_smoothing));
    graph.edges.push_back({edge++, second_group, return_bus, send_gain});
    graph.edges.push_back({edge++, second_group, master_id});
    graph.edges.push_back({edge++, return_bus, master_id});
    graph.nodes.emplace_back(RoutingNode{master_id, 0}, colour(nested_master_smoothing));
    return graph;
  }
  for (RoutingNodeId track = 1; track <= session_tracks; ++track) {
    if (track == latent_track)
      graph.nodes.emplace_back(RoutingNode{track, latent_frames}, std::make_shared<Latent>());
    else if (track == ducked_track)
      graph.nodes.emplace_back(RoutingNode{track, 0}, std::make_shared<Ducker>());
    else
      graph.nodes.emplace_back(RoutingNode{track, 0}, colour(step(session_track_smoothing, track)));
    graph.edges.push_back(
        {edge++, track, track <= session_tracks / fewest_split_parts ? first_group : second_group});
  }
  graph.edges.push_back({edge++, key_track, ducked_track, 1.0F, RoutingEdgeKind::sidechain});
  graph.nodes.emplace_back(RoutingNode{first_group, 0}, colour(group_smoothing));
  graph.nodes.emplace_back(RoutingNode{second_group, 0}, colour(second_group_smoothing));
  graph.nodes.emplace_back(RoutingNode{return_bus, 0}, colour(return_smoothing));
  graph.edges.push_back({edge++, 1, return_bus, send_gain});
  graph.edges.push_back({edge++, send_track, return_bus, send_gain});
  graph.edges.push_back({edge++, first_group, master_id});
  graph.edges.push_back({edge++, second_group, master_id, bus_gain});
  graph.edges.push_back({edge++, return_bus, master_id});
  graph.nodes.emplace_back(RoutingNode{master_id, 0}, colour(master_smoothing));
  graph.nodes.back().output_stage = colour(output_stage_smoothing);
  return graph;
}

// A track's tone at an absolute frame; zero for nodes without a source.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a node and a frame.
StereoFrame tone(RoutingNodeId node, std::uint64_t frame) {
  if (node >= first_group)
    return {};
  const auto time = static_cast<double>(frame);
  const auto pitch = 1.0 + tone_spread * static_cast<double>(node);
  return {static_cast<float>(std::sin(time * tone_rate * pitch)),
          static_cast<float>(std::cos(time * tone_rate * tone_right_ratio * pitch))};
}

// Automation: a slow per-frame gain for every edge and node.
float automated(std::uint64_t frame, std::size_t which) {
  return half + half * static_cast<float>(std::sin(static_cast<double>(frame) * automation_rate +
                                                   static_cast<double>(which)));
}

struct Setup {
  Shape shape = Shape::session;
  std::size_t block = largest_block;
  RoutedSubblocks subblocks = RoutedSubblocks::off;
  std::size_t part = 0;
  std::size_t helpers = 0;
  bool fill_in_schedule = false;
  bool automation = false;
};

// Fills the node spans part by part from the schedule, checking each node's
// parts arrive in order and cover the block.
struct ScheduledFill {
  std::span<const RoutingNodeId> order;
  std::vector<std::vector<StereoFrame>>* sources;
  std::vector<std::uint64_t> next;
  std::atomic<bool> out_of_order{false};

  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): RoutedNodeFill::fill.
  static bool fill(void* context, std::size_t node, BlockContext part, std::size_t offset,
                   std::size_t frames, AudioWorkerPool*) noexcept {
    auto& self = *static_cast<ScheduledFill*>(context);
    if (self.next[node] != part.project_sample_position)
      self.out_of_order.store(true, std::memory_order_relaxed);
    self.next[node] = part.project_sample_position + frames;
    auto& buffer = (*self.sources)[node];
    for (std::size_t frame = 0; frame < frames; ++frame)
      buffer[offset + frame] = tone(self.order[node], part.project_sample_position + frame);
    return true;
  }
};

// The parts the last render() cut its last block into.
std::size_t last_render_parts = 0;

std::vector<StereoFrame> render(const Setup& setup) {
  auto graph = make_graph(setup.shape);
  RoutedMixerLimits limits{
      {master_id, compensation_limit},
      largest_block,
      (graph.nodes.size() + 1) * largest_block * sizeof(StereoFrame) * budget_factor,
      graph.edges.size() * (compensation_limit + 1) * sizeof(StereoFrame) * budget_factor};
  limits.gain_ramp_frames = gain_ramp_frames;
  limits.subblocks = setup.subblocks;
  limits.fixed_subblock_frames = setup.part;
  auto mixer = take(prepare_routed_mixer(graph.nodes, graph.edges, limits));
  const auto& order = mixer->plan().topological_order;
  std::optional<AudioWorkerPool> pool;
  if (setup.helpers != 0) {
    pool.emplace(setup.helpers);
    pool->start_owned_threads(setup.helpers);
  }
  std::vector<std::vector<StereoFrame>> sources(order.size(),
                                                std::vector<StereoFrame>(largest_block));
  std::vector<std::span<const StereoFrame>> spans(order.size());
  std::vector<std::vector<float>> edge_values(mixer->plan().edges.size(),
                                              std::vector<float>(largest_block));
  std::vector<EdgeGainSpan> edge_gains;
  std::vector<std::vector<StereoFrame>> node_values(order.size(),
                                                    std::vector<StereoFrame>(largest_block));
  std::vector<NodeGainSpan> node_gains;
  ScheduledFill fill{order, &sources, std::vector<std::uint64_t>(order.size(), 0)};
  std::vector<StereoFrame> output(setup.block);
  std::vector<StereoFrame> rendered;
  for (std::size_t index = 0; index < rendered_blocks; ++index) {
    const auto first = index * setup.block;
    for (std::size_t slot = 0; slot < order.size(); ++slot) {
      spans[slot] = std::span<const StereoFrame>(sources[slot]).first(setup.block);
      if (!setup.fill_in_schedule)
        for (std::size_t frame = 0; frame < setup.block; ++frame)
          sources[slot][frame] = tone(order[slot], first + frame);
    }
    edge_gains.clear();
    node_gains.clear();
    if (setup.automation) {
      for (std::size_t edge = 0; edge < edge_values.size(); ++edge) {
        for (std::size_t frame = 0; frame < setup.block; ++frame)
          edge_values[edge][frame] = automated(first + frame, edge);
        edge_gains.push_back({mixer->plan().edges[edge].id,
                              std::span<const float>(edge_values[edge]).first(setup.block)});
      }
      for (std::size_t node = 0; node < node_values.size(); ++node) {
        for (std::size_t frame = 0; frame < setup.block; ++frame)
          node_values[node][frame] = {automated(first + frame, node + edge_values.size()),
                                      automated(first + frame, node)};
        node_gains.push_back({mixer->plan().nodes[node].id,
                              std::span<const StereoFrame>(node_values[node]).first(setup.block)});
      }
    }
    if (index == edit_block) {
      // A mute fading in the middle of the render, and a send dragged.
      std::vector<RoutedNodeControl> controls;
      for (const auto& node : mixer->plan().nodes)
        controls.push_back({node.id, node.id == key_track, StereoFrame{1.0F, 1.0F}});
      mixer->apply_node_controls(controls);
      (void)mixer->apply_live_edge_gain(mixer->plan().edges.front().id, live_send_gain);
    }
    const RoutedNodeFill node_fill =
        setup.fill_in_schedule ? RoutedNodeFill{&fill, &ScheduledFill::fill} : RoutedNodeFill{};
    require(mixer->mix_block(spans, output, {first, transport_generation}, edge_gains, node_gains,
                             pool ? &*pool : nullptr, node_fill),
            "mix_block refused a block");
    rendered.insert(rendered.end(), output.begin(), output.end());
  }
  require(!fill.out_of_order.load(), "a node's parts were filled out of order");
  last_render_parts = mixer->last_subblocks();
  if (pool)
    pool->stop_owned_threads();
  return rendered;
}

void require_same(std::span<const StereoFrame> reference, std::span<const StereoFrame> other,
                  const std::string& what) {
  require(reference.size() == other.size(), what + ": the render length changed");
  for (std::size_t frame = 0; frame < reference.size(); ++frame)
    require(std::bit_cast<std::uint32_t>(reference[frame].left) ==
                    std::bit_cast<std::uint32_t>(other[frame].left) &&
                std::bit_cast<std::uint32_t>(reference[frame].right) ==
                    std::bit_cast<std::uint32_t>(other[frame].right),
            what + ": differs at frame " + std::to_string(frame));
}

std::string describe(const Setup& setup) {
  return "shape " + std::to_string(static_cast<int>(setup.shape)) + ", block " +
         std::to_string(setup.block) + ", part " + std::to_string(setup.part) + ", helpers " +
         std::to_string(setup.helpers) + (setup.fill_in_schedule ? ", scheduled fill" : "") +
         (setup.automation ? ", automation" : "");
}

void parts_match_whole_blocks(Shape shape, bool automation) {
  for (const auto block : block_sizes) {
    const Setup whole{shape, block, RoutedSubblocks::off, 0, 0, false, automation};
    const auto reference = render(whole);
    for (const auto helpers : helper_counts) {
      for (const bool scheduled : {false, true}) {
        for (const auto part : part_sizes) {
          const Setup split{shape,     block,     RoutedSubblocks::fixed, part, helpers,
                            scheduled, automation};
          require_same(reference, render(split), describe(split));
        }
        for (const auto mode : {RoutedSubblocks::adaptive, RoutedSubblocks::off}) {
          const Setup other{shape, block, mode, 0, helpers, scheduled, automation};
          require_same(reference, render(other), describe(other));
        }
      }
    }
  }
}

void session_parts_match_whole_blocks() { parts_match_whole_blocks(Shape::session, false); }
void session_with_automation_matches() { parts_match_whole_blocks(Shape::session, true); }
void nested_groups_match_whole_blocks() { parts_match_whole_blocks(Shape::nested, true); }
void flat_mix_matches_whole_blocks() { parts_match_whole_blocks(Shape::flat, false); }
void plugins_render_whole_in_a_split_block() {
  parts_match_whole_blocks(Shape::plugin, true);
  // The rest of the graph still runs in parts around the plugins.
  constexpr std::size_t part = 64;
  const Setup split{Shape::plugin, largest_block, RoutedSubblocks::fixed, part, some_helpers,
                    true,          true};
  (void)render(split);
  REQUIRE_EQ(last_render_parts, largest_block / part, "a graph with plugins was not split");
}

void staged_master_matches_whole_blocks() {
  Chain::staged_runs = 0;
  parts_match_whole_blocks(Shape::staged, true);
  REQUIRE_GE(Chain::staged_runs.load(), 1, "the master never ran its stages apart");
}

// Each part reaches a processor at its own position; a loop wrap flags only
// the block's first part.
void parts_carry_their_positions() {
  constexpr std::size_t block = 256;
  constexpr std::size_t part = 64;
  constexpr std::uint64_t first = 4096;
  auto recorder = std::make_shared<Recorder>();
  std::vector<RoutedNode> nodes;
  nodes.emplace_back(RoutingNode{1, 0}, std::make_shared<Colour>(track_smoothing));
  nodes.emplace_back(RoutingNode{master_id, 0}, recorder);
  const std::vector<RoutingEdge> edges{{1, 1, master_id}};
  RoutedMixerLimits limits{{master_id, compensation_limit},
                           block,
                           (nodes.size() + 1) * block * sizeof(StereoFrame),
                           edges.size() * (compensation_limit + 1) * sizeof(StereoFrame)};
  limits.subblocks = RoutedSubblocks::fixed;
  limits.fixed_subblock_frames = part;
  auto mixer = take(prepare_routed_mixer(nodes, edges, limits));
  AudioWorkerPool pool(few_helpers);
  pool.start_owned_threads(few_helpers);
  std::vector<StereoFrame> output(block);
  const std::vector<std::span<const StereoFrame>> silent(nodes.size());
  require(mixer->mix_block(silent, output, {first, wrapped_generation, 0, true}, {}, {}, &pool),
          "the wrapped block was refused");
  pool.stop_owned_threads();
  REQUIRE_EQ(mixer->last_subblocks(), block / part, "the block was not cut into its parts");
  const auto seen = recorder->seen();
  REQUIRE_EQ(seen.size(), block / part, "the master did not see every part");
  for (std::size_t index = 0; index < seen.size(); ++index) {
    REQUIRE_EQ(seen[index].context.project_sample_position, first + index * part,
               "a part's position");
    REQUIRE_EQ(seen[index].frames, part, "a part's length");
    require(seen[index].context.planned_loop_wrap == (index == 0),
            "only the first part starts the loop wrap");
  }
}

// A source that fails part way fails the block.
void a_failed_fill_fails_the_block() {
  constexpr std::size_t block = 256;
  auto graph = make_graph(Shape::flat);
  RoutedMixerLimits limits{{master_id, compensation_limit},
                           block,
                           (graph.nodes.size() + 1) * block * sizeof(StereoFrame),
                           graph.edges.size() * (compensation_limit + 1) * sizeof(StereoFrame)};
  limits.subblocks = RoutedSubblocks::fixed;
  limits.fixed_subblock_frames = block / quarter_parts;
  auto mixer = take(prepare_routed_mixer(graph.nodes, graph.edges, limits));
  AudioWorkerPool pool(some_helpers);
  pool.start_owned_threads(some_helpers);
  std::vector<std::vector<StereoFrame>> sources(graph.nodes.size(),
                                                std::vector<StereoFrame>(block));
  std::vector<std::span<const StereoFrame>> spans(sources.begin(), sources.end());
  std::vector<StereoFrame> output(block);
  const RoutedNodeFill failing{
      nullptr, [](void*, std::size_t node, BlockContext, std::size_t offset, std::size_t,
                  AudioWorkerPool*) noexcept { return !(node == 0 && offset != 0); }};
  require(!mixer->mix_block(spans, output, {0, transport_generation}, {}, {}, &pool, failing),
          "a failed fill rendered as if it had worked");
  pool.stop_owned_threads();
}

// Splitting allocates nothing on the callback.
void parts_never_allocate() {
  constexpr std::size_t block = 512;
  auto graph = make_graph(Shape::session);
  RoutedMixerLimits limits{{master_id, compensation_limit},
                           block,
                           (graph.nodes.size() + 1) * block * sizeof(StereoFrame) * budget_factor,
                           graph.edges.size() * (compensation_limit + 1) * sizeof(StereoFrame) *
                               budget_factor};
  limits.subblocks = RoutedSubblocks::fixed;
  limits.fixed_subblock_frames = block / eighth_parts;
  auto mixer = take(prepare_routed_mixer(graph.nodes, graph.edges, limits));
  AudioWorkerPool pool(some_helpers);
  pool.start_owned_threads(some_helpers);
  std::vector<std::vector<StereoFrame>> sources(mixer->plan().topological_order.size(),
                                                std::vector<StereoFrame>(block));
  std::vector<std::span<const StereoFrame>> spans(sources.begin(), sources.end());
  std::vector<StereoFrame> output(block);
  oscillate::test::counting_process_allocations = true;
  for (std::size_t index = 0; index < rendered_blocks; ++index)
    require(mixer->mix_block(spans, output, {index * block, transport_generation}, {}, {}, &pool),
            "a block was refused");
  oscillate::test::counting_process_allocations = false;
  pool.stop_owned_threads();
  REQUIRE_EQ(oscillate::test::process_allocations.load(), 0, "a split block allocated");
}

// Adaptive, on a mixer: cheap levels after the tracks leave nothing to
// overlap, so blocks stay whole.
void cheap_levels_after_the_tracks_stay_whole() {
  constexpr std::size_t block = 1024;
  constexpr std::size_t settle_blocks = std::size_t{4} * probe_blocks;
  constexpr std::chrono::microseconds costly{400};
  std::vector<RoutedNode> nodes;
  std::vector<RoutingEdge> edges;
  for (RoutingNodeId id = 1; id <= small_tracks; ++id) {
    nodes.emplace_back(RoutingNode{id, 0}, std::make_shared<Busy>(costly));
    edges.push_back({id, id, master_id});
  }
  nodes.emplace_back(RoutingNode{master_id, 0}, nullptr);
  RoutedMixerLimits limits{{master_id, compensation_limit},
                           block,
                           (nodes.size() + 1) * block * sizeof(StereoFrame),
                           edges.size() * (compensation_limit + 1) * sizeof(StereoFrame)};
  auto mixer = take(prepare_routed_mixer(nodes, edges, limits));
  AudioWorkerPool pool(few_helpers);
  pool.start_owned_threads(few_helpers);
  const std::vector<std::span<const StereoFrame>> silent(nodes.size());
  std::vector<StereoFrame> output(block);
  for (std::size_t index = 0; index < settle_blocks; ++index)
    require(mixer->mix_block(silent, output, {index * block, transport_generation}, {}, {}, &pool),
            "a block was refused");
  pool.stop_owned_threads();
  REQUIRE_EQ(mixer->last_subblocks(), 1, "a block whose cost is all in the tracks was split");
}

// The policy on measured shares and times: render `blocks` blocks, each
// taking `whole` or `split` ns a frame by the way the policy chose.
struct Timing {
  double share;
  double whole;
  double split;
};
constexpr double heavy_share = 0.5;
constexpr double light_share = 0.01;
constexpr double full_share = 1.0;
constexpr double slow_ns = 100.0;
constexpr double fast_ns = 60.0;
constexpr double near_ns = 95.0;

void observe(SubblockPolicy& policy, Timing timing, std::uint32_t blocks) {
  for (std::uint32_t block = 0; block < blocks; ++block)
    policy.observe(timing.share, policy.split() ? timing.split : timing.whole);
}

void the_policy_splits_only_for_a_clear_gain() {
  // Long enough for the shares to settle, a first probe and a decision.
  constexpr std::uint32_t settle_rounds = 4;
  constexpr std::uint32_t settle =
      settle_rounds * (probe_blocks + static_cast<std::uint32_t>(share_smoothing_blocks));
  SubblockPolicy idle;
  observe(idle, {light_share, slow_ns, fast_ns}, settle);
  require(!idle.split(), "split with nothing after the tracks");
  SubblockPolicy gaining;
  observe(gaining, {heavy_share, slow_ns, fast_ns}, settle);
  require(gaining.split(), "did not split for a 40% faster render");
  REQUIRE_EQ(gaining.parts(largest_block), max_subblocks, "a heavy serial share takes every part");
  REQUIRE_EQ(gaining.parts(2 * min_subblock_frames), 2, "parts stay at least the shortest");
  // A share high from the first block: the whole blocks' mean starts at the
  // first one timed, not at zero.
  SubblockPolicy at_once;
  observe(at_once, {full_share, slow_ns, fast_ns}, settle);
  require(at_once.split(), "did not split when the serial share was high from the start");
  SubblockPolicy marginal;
  observe(marginal, {heavy_share, slow_ns, near_ns}, settle);
  require(!marginal.split(), "split for a gain under the threshold");
  // Splitting stops paying: the next probe of whole blocks returns to them.
  observe(gaining, {heavy_share, fast_ns, slow_ns}, reprobe_blocks + 2 * probe_blocks);
  require(!gaining.split(), "kept splitting after whole blocks became faster");
}

} // namespace

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "routed wavefront: ok",
      {{"session_parts_match_whole_blocks", session_parts_match_whole_blocks},
       {"session_with_automation_matches", session_with_automation_matches},
       {"nested_groups_match_whole_blocks", nested_groups_match_whole_blocks},
       {"flat_mix_matches_whole_blocks", flat_mix_matches_whole_blocks},
       {"staged_master_matches_whole_blocks", staged_master_matches_whole_blocks},
       {"plugins_render_whole_in_a_split_block", plugins_render_whole_in_a_split_block},
       {"parts_carry_their_positions", parts_carry_their_positions},
       {"a_failed_fill_fails_the_block", a_failed_fill_fails_the_block},
       {"parts_never_allocate", parts_never_allocate},
       {"cheap_levels_after_the_tracks_stay_whole", cheap_levels_after_the_tracks_stay_whole},
       {"the_policy_splits_only_for_a_clear_gain", the_policy_splits_only_for_a_clear_gain}});
}
