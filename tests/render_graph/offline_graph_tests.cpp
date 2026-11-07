#include "oscillate/application/project_session.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/render_graph/offline_graph.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

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

// What the stand-in VST3 host reports for each stage it prepares.
constexpr std::uint32_t vst3_latency_frames = 32;
constexpr std::uint64_t vst3_tail_frames = 100;

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
  (void)take(session->effects().add_effect_engine(path, 0, reverb));
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
  if (!graph.route)
    throw std::runtime_error("a track chain routes the render");
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
  REQUIRE_TRUE(!graph.master_stages.back().faulted(), "a fresh master stage has held nothing");
}

// A saved VST3 plugin with a valid identity; no binary is loaded here.
project::Vst3PluginInstance saved_plugin(std::uint64_t serial) {
  return {.id = take(project::PluginInstanceId::from_bits({0, serial})),
          .class_id = std::string(32, 'a'),
          .vendor = "Fixture",
          .name = "Stand-in",
          .version = "1",
          .bundle_path = "/plugins/StandIn.vst3",
          .binary_sha256 = std::string(64, 'b'),
          .component_state = {},
          .parameters = {}};
}

// VST3 stages come from the host's preparer (ADR-0090): a track effect joins
// its chain and the route, a master slot runs after the mix, and their
// latency, tails and failure probes count as built-in stages' do. Without a
// preparer a saved VST3 effect fails before anything is prepared.
void vst3_stages_come_from_the_host() {
  Directory directory;
  auto song = song_with_reverb(directory, false);
  auto& track = song.snapshot.tracks.front();
  constexpr std::uint64_t first_slot = 9'000;
  track.effects.push_back(
      {take(project::InsertSlotId::from_bits({0, first_slot})), true, saved_plugin(first_slot)});
  song.snapshot.master_effects.push_back(
      {take(project::InsertSlotId::from_bits({0, first_slot + 1})), true,
       saved_plugin(first_slot + 1)});
  const auto automation = take(render_graph::prepare_offline_automation(song.snapshot));
  const auto refused = render_graph::prepare_offline_graph(
      song.snapshot, song.store, {.automation = automation, .max_frames = block_frames});
  REQUIRE_TRUE(!refused, "a VST3 effect without a host is refused");

  std::vector<project::PluginInstanceId> prepared;
  auto failed = std::make_shared<std::atomic<bool>>(false);
  const render_graph::Vst3StagePreparer host =
      [&](const project::Snapshot&, const oscillate::track_chain::SavedStage& stage,
          std::size_t max_frames) -> oscillate::foundation::Result<oscillate::track_chain::Stage> {
    REQUIRE_EQ(max_frames, block_frames, "the host prepares for the render's blocks");
    prepared.push_back(std::get<project::Vst3PluginInstance>(stage.processor).id);
    return oscillate::foundation::Result<oscillate::track_chain::Stage>::success(
        {stage, std::make_shared<Foreign>(), vst3_latency_frames, vst3_tail_frames,
         vst3_tail_frames, false, [failed] { return failed->load(); }});
  };
  const auto graph = take(render_graph::prepare_offline_graph(
      song.snapshot, song.store,
      {.automation = automation, .max_frames = block_frames, .vst3 = host}));
  REQUIRE_EQ(prepared.size(), 2U, "the host prepares the track's and the master's plugin");
  REQUIRE_EQ(graph.chain_stages.size(), track_stages(song.snapshot) + 1,
             "the track's plugin is a stage of its chain");
  REQUIRE_EQ(graph.master_stages.size(), master_stages(song.snapshot),
             "the master's plugin is a master stage");
  REQUIRE_TRUE(graph.route.has_value(), "the chain routes the render");
  if (!graph.route)
    throw std::runtime_error("the chain routes the render");
  REQUIRE_EQ(graph.route->output_latency_frames, vst3_latency_frames,
             "the track plugin's latency is the route's");
  REQUIRE_EQ(graph.master_latency_frames, vst3_latency_frames,
             "the master plugin's latency runs after the mix");
  REQUIRE_GE(graph.master_tail_frames, vst3_tail_frames, "the master plugin's tail rings out");
  REQUIRE_TRUE(!graph.chain_faulted(), "a fresh chain has failed nothing");
  failed->store(true);
  REQUIRE_TRUE(graph.chain_faulted(), "a VST3 stage's failed block fails the chain");
  REQUIRE_TRUE(graph.master_stages.back().faulted(), "a VST3 master stage's failure is seen");
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
    vst3_stages_come_from_the_host();
    unknown_stage_counts_as_held();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "render graph contract passed\n";
  return 0;
}
