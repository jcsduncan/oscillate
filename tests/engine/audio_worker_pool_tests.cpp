#include "oscillate/engine/audio_worker_pool.hpp"
#include "oscillate/engine/routed_mixer.hpp"
#include "oscillate/test/check.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {
using namespace oscillate;
using namespace oscillate::engine;

using oscillate::test::require;

using oscillate::test::take;

struct Tally {
  std::array<std::atomic<std::uint32_t>, 64> runs{};
  std::array<std::atomic<std::uint32_t>, 64> lane_users{};
  std::atomic<bool> lane_clash{false};
  std::atomic<bool> lane_out_of_range{false};
  std::size_t count = 0;
};

void tally_task(void* context, std::size_t task, std::size_t lane) noexcept {
  auto& tally = *static_cast<Tally*>(context);
  if (lane >= tally.count) {
    tally.lane_out_of_range.store(true);
    return;
  }
  // Two threads holding one lane at once would share scratch.
  if (tally.lane_users[lane].fetch_add(1) != 0)
    tally.lane_clash.store(true);
  tally.runs[task].fetch_add(1);
  volatile double spin = 0.0;
  for (int index = 0; index < 200; ++index)
    spin = spin + std::sqrt(static_cast<double>(index) + static_cast<double>(task));
  tally.lane_users[lane].fetch_sub(1);
}

void every_task_runs_once(std::size_t workers) {
  AudioWorkerPool pool(workers);
  pool.start_owned_threads(workers);
  REQUIRE_EQ(pool.attached_workers(), workers, "owned workers did not attach");
  for (int batch = 0; batch < 5000; ++batch) {
    Tally tally;
    tally.count = 1 + static_cast<std::size_t>(batch) % 40;
    pool.run(tally.count, &tally_task, &tally);
    for (std::size_t task = 0; task < tally.count; ++task)
      REQUIRE_EQ(tally.runs[task].load(), 1, "a task ran other than once");
    for (std::size_t task = tally.count; task < tally.runs.size(); ++task)
      REQUIRE_EQ(tally.runs[task].load(), 0, "a task outside the batch ran");
    require(!tally.lane_clash.load(), "two threads shared a lane");
    require(!tally.lane_out_of_range.load(), "a lane exceeded the task count");
  }
  pool.stop_owned_threads();
  REQUIRE_EQ(pool.attached_workers(), 0, "workers stayed attached after stop");
}

// No more than max_workers attach, whoever starts them, so lanes stay below
// max_lanes: a surplus worker returns at once and owned threads stop at the cap.
void attached_workers_are_capped() {
  AudioWorkerPool pool(AudioWorkerPool::max_workers);
  pool.start_owned_threads(AudioWorkerPool::max_workers + 1);
  REQUIRE_EQ(pool.attached_workers(), AudioWorkerPool::max_workers,
             "owned workers went past the cap");
  std::thread surplus([&pool] { pool.run_worker(); });
  surplus.join(); // Returns at once: no place is free.
  REQUIRE_EQ(pool.attached_workers(), AudioWorkerPool::max_workers,
             "a surplus worker attached past the cap");
  pool.start_owned_threads(1);
  REQUIRE_EQ(pool.attached_workers(), AudioWorkerPool::max_workers,
             "owned workers went past the cap");
  pool.stop_owned_threads();
  REQUIRE_EQ(pool.attached_workers(), 0, "workers stayed attached after stop");
}

void runs_inline_without_workers() {
  AudioWorkerPool pool(4);
  std::vector<std::size_t> order;
  struct Context {
    std::vector<std::size_t>& order;
    std::thread::id caller;
    bool other_thread = false;
  } context{order, std::this_thread::get_id()};
  order.reserve(16);
  pool.run(
      16,
      // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): AudioWorkerPool::Task.
      [](void* raw, std::size_t task, std::size_t lane) noexcept {
        auto& state = *static_cast<Context*>(raw);
        state.order.push_back(task + lane * 100);
        state.other_thread |= std::this_thread::get_id() != state.caller;
      },
      &context);
  require(!context.other_thread, "an idle pool used another thread");
  for (std::size_t task = 0; task < 16; ++task)
    REQUIRE_EQ(order[task], task, "an idle pool did not run tasks in order on lane 0");
}

// Outer tasks that each hand out a nested batch, more of them at once than
// there are nested slots, so some run inline and some share helpers.
struct NestedRound {
  AudioWorkerPool* pool = nullptr;
  std::array<Tally, 12> nested{};
};

void nested_task(void* context, std::size_t task, std::size_t) noexcept {
  auto& round = *static_cast<NestedRound*>(context);
  auto& tally = round.nested[task];
  round.pool->run_nested(tally.count, &tally_task, &tally);
}

void nested_batches_run_every_task_once(std::size_t workers) {
  AudioWorkerPool pool(workers);
  pool.start_owned_threads(workers);
  for (int batch = 0; batch < 2000; ++batch) {
    NestedRound round;
    round.pool = &pool;
    const auto outer = 1 + static_cast<std::size_t>(batch) % round.nested.size();
    for (std::size_t task = 0; task < outer; ++task)
      round.nested[task].count = 1 + (static_cast<std::size_t>(batch) + 7 * task) % 40;
    pool.run(outer, &nested_task, &round);
    for (std::size_t task = 0; task < outer; ++task) {
      const auto& tally = round.nested[task];
      for (std::size_t index = 0; index < tally.count; ++index)
        REQUIRE_EQ(tally.runs[index].load(), 1, "a nested task ran other than once");
      for (std::size_t index = tally.count; index < tally.runs.size(); ++index)
        REQUIRE_EQ(tally.runs[index].load(), 0, "a task outside the nested batch ran");
      require(!tally.lane_clash.load(), "two threads shared a nested lane");
      require(!tally.lane_out_of_range.load(), "a nested lane exceeded its task count");
    }
  }
  // Straight from the dispatching thread, outside any outer batch.
  for (int batch = 0; batch < 500; ++batch) {
    Tally tally;
    tally.count = 2 + static_cast<std::size_t>(batch) % 30;
    pool.run_nested(tally.count, &tally_task, &tally);
    for (std::size_t index = 0; index < tally.count; ++index)
      REQUIRE_EQ(tally.runs[index].load(), 1, "a direct nested task ran other than once");
  }
  pool.stop_owned_threads();
}

void nested_runs_inline_without_workers() {
  AudioWorkerPool pool(4);
  std::vector<std::size_t> order;
  order.reserve(8);
  pool.run_nested(
      8,
      // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): AudioWorkerPool::Task.
      [](void* raw, std::size_t task, std::size_t lane) noexcept {
        static_cast<std::vector<std::size_t>*>(raw)->push_back(task + lane * 100);
      },
      &order);
  for (std::size_t task = 0; task < 8; ++task)
    REQUIRE_EQ(order[task], task, "an idle pool did not run a nested batch in order on lane 0");
}

void restarts_after_stop() {
  AudioWorkerPool pool(2);
  for (int round = 0; round < 20; ++round) {
    pool.start_owned_threads(2);
    Tally tally;
    tally.count = 12;
    pool.run(tally.count, &tally_task, &tally);
    for (std::size_t task = 0; task < tally.count; ++task)
      REQUIRE_EQ(tally.runs[task].load(), 1, "a restarted pool lost a task");
    pool.stop_owned_threads();
    // With every worker gone, the caller still finishes the batch.
    Tally alone;
    alone.count = 9;
    pool.run(alone.count, &tally_task, &alone);
    for (std::size_t task = 0; task < alone.count; ++task)
      REQUIRE_EQ(alone.runs[task].load(), 1, "a stopped pool did not run inline");
  }
}

// Stateful, order-sensitive processing: any change in summing order, lost
// scratch or a race shows up as a bit difference.
class Colour final : public ProcessorRuntime {
public:
  explicit Colour(float coefficient) : coefficient_(coefficient) {}
  void process(std::span<StereoFrame> frames, BlockContext) noexcept override {
    {
      const std::lock_guard lock(threads_mutex);
      threads.insert(std::this_thread::get_id());
    }
    for (auto& frame : frames) {
      double left = frame.left;
      for (int step = 0; step < 24; ++step)
        left = std::sin(left * 0.9 + 0.01 * step);
      state_left_ += coefficient_ * (static_cast<float>(left) - state_left_);
      state_right_ += coefficient_ * (std::tanh(frame.right * 1.7F) - state_right_);
      frame = {state_left_, state_right_};
    }
  }
  static inline std::mutex threads_mutex;
  static inline std::set<std::thread::id> threads;

private:
  float coefficient_;
  float state_left_ = 0.0F;
  float state_right_ = 0.0F;
};

class Latency final : public ProcessorRuntime {
public:
  void process(std::span<StereoFrame> frames, BlockContext) noexcept override {
    for (auto& frame : frames) {
      std::swap(frame, line_[cursor_]);
      cursor_ = (cursor_ + 1) % line_.size();
    }
  }

private:
  std::array<StereoFrame, 8> line_{};
  std::size_t cursor_ = 0;
};

struct Render {
  std::vector<StereoFrame> output;
  std::vector<RoutedNodeMeterReading> meters;
};

// Eight tracks, two groups, a return fed by sends, a latent track and master.
Render render_session(AudioWorkerPool* workers) {
  std::vector<RoutedNode> nodes;
  for (RoutingNodeId track = 1; track <= 8; ++track) {
    if (track == 3)
      nodes.emplace_back(RoutingNode{track, 8}, std::make_shared<Latency>());
    else
      nodes.emplace_back(RoutingNode{track, 0},
                         std::make_shared<Colour>(0.1F + 0.05F * static_cast<float>(track)));
  }
  nodes.emplace_back(RoutingNode{9, 0}, std::make_shared<Colour>(0.6F));
  nodes.emplace_back(RoutingNode{10, 0}, std::make_shared<Colour>(0.7F));
  nodes.emplace_back(RoutingNode{11, 0}, std::make_shared<Colour>(0.3F));
  nodes.emplace_back(RoutingNode{12, 0}, nullptr);
  std::vector<RoutingEdge> edges;
  RoutingEdgeId edge = 100;
  for (RoutingNodeId track = 1; track <= 8; ++track)
    edges.push_back({edge++, track, track <= 4 ? RoutingNodeId{9} : RoutingNodeId{10}});
  edges.push_back({edge++, 1, 11, 0.4F});
  edges.push_back({edge++, 6, 11, 0.25F});
  edges.push_back({edge++, 9, 12});
  edges.push_back({edge++, 10, 12, 0.8F});
  edges.push_back({edge++, 11, 12});
  constexpr std::size_t frames = 128;
  RoutedMixerLimits limits{{12, 64},
                           frames,
                           (nodes.size() + 1) * frames * sizeof(StereoFrame),
                           edges.size() * 65 * sizeof(StereoFrame)};
  limits.node_meters = true;
  auto mixer = take(prepare_routed_mixer(nodes, edges, limits));
  const auto& order = mixer->plan().topological_order;
  std::vector<std::vector<StereoFrame>> sources(order.size(), std::vector<StereoFrame>(frames));
  std::vector<std::span<const StereoFrame>> spans(order.size());
  std::vector<StereoFrame> block(frames);
  Render result;
  for (std::uint64_t index = 0; index < 300; ++index) {
    for (std::size_t slot = 0; slot < order.size(); ++slot) {
      const auto id = order[slot];
      spans[slot] = {};
      if (id > 8)
        continue;
      for (std::size_t frame = 0; frame < frames; ++frame) {
        const auto time = static_cast<double>(index * frames + frame);
        sources[slot][frame] = {
            static_cast<float>(std::sin(time * 0.003 * static_cast<double>(id))),
            static_cast<float>(std::cos(time * 0.0021 * static_cast<double>(id)))};
      }
      spans[slot] = sources[slot];
    }
    require(mixer->mix_block(spans, block, {index * frames, 1}, {}, {}, workers),
            "mix_block refused a block");
    result.output.insert(result.output.end(), block.begin(), block.end());
  }
  for (RoutingNodeId node = 1; node <= 12; ++node) {
    const auto reading = mixer->node_meter(node);
    REQUIRE_OK(reading, "a node meter was empty");
    result.meters.push_back(*reading);
  }
  return result;
}

bool same_bits(float left, float right) {
  return std::bit_cast<std::uint32_t>(left) == std::bit_cast<std::uint32_t>(right);
}

void parallel_mix_is_bit_identical() {
  const auto reference = render_session(nullptr);
  for (const std::size_t count : {std::size_t{0}, std::size_t{1}, std::size_t{2}, std::size_t{4}}) {
    AudioWorkerPool pool(count);
    pool.start_owned_threads(count);
    {
      const std::lock_guard lock(Colour::threads_mutex);
      Colour::threads.clear();
    }
    const auto parallel = render_session(&pool);
    pool.stop_owned_threads();
    REQUIRE_EQ(parallel.output.size(), reference.output.size(), "render length changed");
    for (std::size_t frame = 0; frame < reference.output.size(); ++frame)
      require(same_bits(parallel.output[frame].left, reference.output[frame].left) &&
                  same_bits(parallel.output[frame].right, reference.output[frame].right),
              "parallel mix differs from the single-threaded mix");
    for (std::size_t node = 0; node < reference.meters.size(); ++node)
      require(same_bits(parallel.meters[node].left_peak, reference.meters[node].left_peak) &&
                  same_bits(parallel.meters[node].right_rms, reference.meters[node].right_rms),
              "parallel node meters differ");
    if (count >= 2) {
      const std::lock_guard lock(Colour::threads_mutex);
      REQUIRE_GE(Colour::threads.size(), 2, "no node ran on a worker thread");
    }
  }
}

// Every hardware thread but the callback's, SMT siblings included, up to the
// cap: a four-thread machine gets three helpers, a sixteen-thread one seven.
void default_workers_leave_the_callback_a_cpu() {
  constexpr std::size_t small_machine = 4;
  constexpr std::size_t large_machine = 16;
  REQUIRE_EQ(AudioWorkerPool::default_workers_for(0), std::size_t{0}, "no CPU count");
  REQUIRE_EQ(AudioWorkerPool::default_workers_for(1), std::size_t{0}, "one CPU");
  REQUIRE_EQ(AudioWorkerPool::default_workers_for(small_machine), small_machine - 1,
             "every other thread of a small machine");
  REQUIRE_EQ(AudioWorkerPool::default_workers_for(large_machine),
             AudioWorkerPool::default_worker_cap, "the cap on a large machine");
  static_assert(AudioWorkerPool::default_worker_cap <= AudioWorkerPool::max_workers);
  const auto workers = AudioWorkerPool::default_worker_count();
  require(workers < std::max<unsigned>(1, std::thread::hardware_concurrency()) || workers == 0,
          "the default leaves the callback a CPU");
}

} // namespace

int main() {
  try {
    runs_inline_without_workers();
    every_task_runs_once(1);
    every_task_runs_once(3);
    every_task_runs_once(7);
    restarts_after_stop();
    attached_workers_are_capped();
    nested_runs_inline_without_workers();
    nested_batches_run_every_task_once(1);
    nested_batches_run_every_task_once(5);
    parallel_mix_is_bit_identical();
    default_workers_leave_the_callback_a_cpu();
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
  std::cout << "audio worker pool: ok\n";
  return 0;
}
