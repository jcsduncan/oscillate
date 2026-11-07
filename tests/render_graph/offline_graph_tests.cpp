#include "oscillate/application/project_session.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/render_graph/offline_graph.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"

#include <cstddef>
#include <exception>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <variant>

// The offline graph builder: a snapshot's chains, routing and master chain
// as export renders them.
namespace {
namespace app = oscillate::application;
namespace engine = oscillate::engine;
namespace persistence = oscillate::persistence;
namespace project = oscillate::project;
namespace render_graph = oscillate::render_graph;

using oscillate::test::take;

constexpr std::size_t block_frames = 1'024;
// A reverb rings out, so the graph has a tail to report.
constexpr std::string_view reverb = "expanse";

class Directory : public oscillate::test::TempDir {
public:
  Directory() : TempDir("oscillate-render-graph") {}
};

// A stage the builder did not prepare.
class Foreign final : public engine::ProcessorRuntime {
public:
  void process(std::span<engine::StereoFrame>, engine::BlockContext) noexcept override {}
};

struct Song {
  project::Snapshot snapshot;
  persistence::ProjectStore store;
};

Song song_with_reverb(const Directory& directory, bool on_master) {
  const auto root = directory.path() / "song";
  auto session = take(app::ProjectSession::create(root));
  const auto track = take(session->instruments().add_synth_track());
  const project::SignalPathId path =
      on_master ? project::SignalPathId{project::MasterSignalPath{}} : project::SignalPathId{track};
  (void)take(session->effects().add_effect_engine(path, 0, std::string(reverb)));
  REQUIRE_OK(session->saving().save(), "project saved");
  return {session->snapshot(), take(persistence::ProjectStore::open_reader(root))};
}

// Enabled built-in effects on the tracks (new projects have returns with
// their own) and on the master.
std::size_t track_stages(const project::Snapshot& snapshot) {
  std::size_t count = 0;
  for (const auto& track : snapshot.tracks)
    for (const auto& slot : track.effects)
      count += slot.enabled && std::holds_alternative<project::NativeEffectPatch>(slot.processor);
  return count;
}

std::size_t master_stages(const project::Snapshot& snapshot) {
  std::size_t count = 0;
  for (const auto& slot : snapshot.master_effects)
    count += slot.enabled ? 1U : 0U;
  return count;
}

render_graph::OfflineGraph
prepare(const Song& song,
        std::span<const oscillate::automation_project::PreparedTargetAutomation> automation) {
  return take(render_graph::prepare_offline_graph(
      song.snapshot, song.store, {.automation = automation, .max_frames = block_frames}));
}

void track_chain_runs_in_the_route() {
  Directory directory;
  const auto song = song_with_reverb(directory, false);
  const auto automation = take(render_graph::prepare_offline_automation(song.snapshot));
  const auto graph = prepare(song, automation);
  REQUIRE_TRUE(graph.route.has_value(), "a track chain routes the render");
  REQUIRE_EQ(graph.chain_stages.size(), track_stages(song.snapshot),
             "every enabled track effect is a chain stage");
  REQUIRE_GT(graph.route->native_tail_frames, 0U, "the reverb's ring-out reaches the master");
  REQUIRE_EQ(graph.master_stages.size(), master_stages(song.snapshot),
             "the master runs its own slots");
  REQUIRE_TRUE(!graph.chain_faulted(), "a fresh chain has held nothing");
}

void master_chain_runs_after_the_mix() {
  Directory directory;
  const auto song = song_with_reverb(directory, true);
  const auto automation = take(render_graph::prepare_offline_automation(song.snapshot));
  const auto graph = prepare(song, automation);
  REQUIRE_EQ(graph.master_stages.size(), master_stages(song.snapshot),
             "the master's reverb is prepared");
  REQUIRE_GT(graph.master_tail_frames, 0U, "the master reverb rings out");
  REQUIRE_EQ(graph.chain_stages.size(), track_stages(song.snapshot),
             "only the tracks' own effects are chain stages");
  REQUIRE_TRUE(!render_graph::held_non_finite(*graph.master_stages.back()),
               "a fresh master stage has held nothing");
}

void unknown_stage_counts_as_held() {
  const Foreign stage;
  REQUIRE_TRUE(render_graph::held_non_finite(stage),
               "a stage the builder did not prepare never passes silently");
}

} // namespace

int main() {
  try {
    track_chain_runs_in_the_route();
    master_chain_runs_after_the_mix();
    unknown_stage_counts_as_held();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "render graph contract passed\n";
  return 0;
}
