#include "oscillate/project/model.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace oscillate;
using namespace oscillate::project;
using foundation::ErrorCode;
using foundation::Result;
using oscillate::test::require;
using oscillate::test::take;

// A depth limit small enough to cross many times in one test.
constexpr std::size_t small_depth = 8;
// Edits made past a limit: enough to trim many times.
constexpr std::size_t edits_past_limit = 24;
// A plugin state large enough that the bytes limit, not the depth, trims.
constexpr std::size_t plugin_state_bytes = std::size_t{1024} * 1024;
// The bytes budget in plugin states, and the steps kept regardless.
constexpr std::size_t budget_states = 16;
constexpr std::size_t kept_steps = 4;
// Many edits: far more than either limit keeps.
constexpr std::size_t many_edits = 200;
// A track level that changes with every edit, so each state is distinct.
constexpr double level_step = 0.001;
constexpr std::uint8_t state_fill = 0x5a;

template <typename Id> Id id(std::uint64_t low) { return take(Id::from_bits({0, low})); }

class FakeGate final : public TransportGate {
public:
  Result<std::unique_ptr<StoppedTransportLease>> acquire_stopped() override {
    return Result<std::unique_ptr<StoppedTransportLease>>::failure(
        {ErrorCode::unavailable, "no rate changes in history tests"});
  }
};

ProjectModel model(FakeGate& gate) {
  return take(ProjectModel::create(
      id<ProjectId>(1), oscillate::test::sample_rate_48k,
      take(timeline::TempoMap::create({{{0}, 120.0, timeline::TempoShape::step}}, 1)),
      take(timeline::MeterMap::create({{{0}, 4, 4}}, 1)), gate));
}

const TrackId track = id<TrackId>(1);

void edit(ProjectModel& project, std::size_t number) {
  REQUIRE_OK(
      project.apply({SetTrackLevelPan{track, level_step * static_cast<double>(number + 1), 0.0}}));
}

// Undo and redo publish a new revision of the same content.
bool same_content(const Snapshot& a, const Snapshot& b) {
  return a.id == b.id && a.sample_rate == b.sample_rate && a.tracks == b.tracks &&
         a.sources == b.sources && a.master_effects == b.master_effects &&
         a.master_level == b.master_level && a.markers == b.markers && a.key == b.key &&
         a.lead_bars == b.lead_bars && a.source_tempo_reviews == b.source_tempo_reviews &&
         a.source_key_reviews == b.source_key_reviews;
}

Vst3PluginInstance large_plugin() {
  return {id<PluginInstanceId>(1),
          std::string(32, 'a'),
          "Fixture Vendor",
          "Gain",
          "1.0",
          "/plugins/Gain.vst3",
          std::string(64, 'b'),
          std::vector<std::uint8_t>(plugin_state_bytes, state_fill),
          {},
          true};
}

// The oldest steps go first; what is left undoes and redoes exactly, and
// undo stops at the oldest kept state.
void undo_and_redo_are_exact_at_the_depth_boundary() {
  FakeGate gate;
  auto project = model(gate);
  project.set_history_limits({.steps = small_depth});
  REQUIRE_OK(project.apply({AddTrack{track}}));
  std::vector<std::shared_ptr<const Snapshot>> states{project.snapshot()};
  for (std::size_t number = 0; number < small_depth + edits_past_limit; ++number) {
    edit(project, number);
    states.push_back(project.snapshot());
    REQUIRE_TRUE(project.undo_depth() <= small_depth, "undo history grew past its depth limit");
  }
  REQUIRE_EQ(project.undo_depth(), small_depth, "undo history did not keep its depth");
  for (std::size_t step = 1; step <= small_depth; ++step) {
    auto undone = take(project.undo());
    REQUIRE_TRUE(same_content(*undone.snapshot, *states[states.size() - 1 - step]),
                 "undo inside the kept history restored another state");
  }
  const auto oldest = project.undo();
  REQUIRE_TRUE(!oldest && oldest.error().code == ErrorCode::not_found,
               "undo went past the oldest kept state");
  REQUIRE_TRUE(same_content(*project.snapshot(), *states[states.size() - 1 - small_depth]),
               "a failed undo at the boundary changed the project");
  for (std::size_t step = small_depth; step-- > 0;) {
    auto redone = take(project.redo());
    REQUIRE_TRUE(same_content(*redone.snapshot, *states[states.size() - 1 - step]),
                 "redo from the boundary restored another state");
  }
  REQUIRE_TRUE(!project.redo(), "redo went past the newest state");
}

// The bytes limit trims old snapshots, which are then freed, and keeps the
// floor of steps even when the budget is smaller.
void memory_stays_bounded_across_many_edits() {
  FakeGate gate;
  auto project = model(gate);
  const std::size_t budget = budget_states * plugin_state_bytes;
  project.set_history_limits({.bytes = budget, .kept_steps = kept_steps});
  REQUIRE_OK(project.apply({AddTrack{track, TrackKind::instrument}}));
  REQUIRE_OK(project.apply({InsertVst3Plugin{track, 0, large_plugin()}}));
  REQUIRE_TRUE(estimated_bytes(*project.snapshot()) > plugin_state_bytes,
               "the estimate missed the plugin state");
  const std::weak_ptr<const Snapshot> first = project.snapshot();
  std::size_t peak = 0;
  for (std::size_t number = 0; number < many_edits; ++number) {
    edit(project, number);
    peak = std::max(peak, project.history_bytes());
    REQUIRE_TRUE(project.history_bytes() <= budget, "undo history grew past its bytes limit");
  }
  REQUIRE_TRUE(first.expired(), "a trimmed snapshot was not freed");
  REQUIRE_TRUE(project.undo_depth() >= kept_steps && project.undo_depth() < budget_states,
               "the bytes limit did not decide the depth");
  REQUIRE_TRUE(peak > (budget_states - kept_steps) * plugin_state_bytes,
               "the history never approached its budget");

  // Undoing and redoing everything moves steps between the stacks without
  // counting a shared snapshot twice.
  // The current snapshot is then a copy, so the history alone keeps the one
  // it replaced: at most one snapshot over the budget.
  const auto depth = project.undo_depth();
  const auto one_snapshot = estimated_bytes(*project.snapshot());
  for (std::size_t step = 0; step < depth; ++step) {
    REQUIRE_OK(project.undo());
    REQUIRE_TRUE(project.history_bytes() <= budget + one_snapshot,
                 "undo counted a shared snapshot twice");
  }
  for (std::size_t step = 0; step < depth; ++step)
    REQUIRE_OK(project.redo());
  REQUIRE_TRUE(project.history_bytes() <= budget + one_snapshot,
               "redo counted a shared snapshot twice");

  // A budget below one step keeps the floor.
  project.set_history_limits({.bytes = 1, .kept_steps = kept_steps});
  REQUIRE_EQ(project.undo_depth(), kept_steps, "a lowered budget did not trim to the floor");
  for (std::size_t number = 0; number < edits_past_limit; ++number)
    edit(project, number);
  REQUIRE_EQ(project.undo_depth(), kept_steps, "the floor of kept steps was not kept");
}

// The default depth applies without any configuration.
void the_default_depth_bounds_a_long_session() {
  FakeGate gate;
  auto project = model(gate);
  REQUIRE_OK(project.apply({AddTrack{track}}));
  for (std::size_t number = 0; number < default_undo_steps + edits_past_limit; ++number)
    edit(project, number);
  REQUIRE_EQ(project.undo_depth(), default_undo_steps, "the default depth limit did not apply");
}

} // namespace

int main() {
  try {
    undo_and_redo_are_exact_at_the_depth_boundary();
    memory_stays_bounded_across_many_edits();
    the_default_depth_bounds_a_long_session();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
}
