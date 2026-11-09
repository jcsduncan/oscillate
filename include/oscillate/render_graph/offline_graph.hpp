#pragma once

#include "oscillate/automation_project/preparation.hpp"
#include "oscillate/engine/audio_worker_pool.hpp"
#include "oscillate/engine/graph.hpp"
#include "oscillate/engine/midi_events.hpp"
#include "oscillate/engine/routed_mixer.hpp"
#include "oscillate/foundation/error.hpp"
#include "oscillate/project/model.hpp"
#include "oscillate/render_graph/vst3_stage.hpp"
#include "oscillate/routing/route_gain_blocks.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace oscillate::persistence {
class ProjectStore;
}

// The graph builder for offline renders: one saved snapshot becomes the
// prepared track chains, master chain, routed mixer and built-in instruments
// that export runs. Chains come from the one-chain-per-track builder
// (ADR-0050) and instruments from the player live playback runs, so export
// prepares nothing itself.
namespace oscillate::render_graph {

// The project's automation, prepared once for every render of a snapshot.
// Bypassed VST3 effects and their lanes are left out (see OfflineGraphRequest).
[[nodiscard]] foundation::Result<std::vector<automation_project::PreparedTargetAutomation>>
prepare_offline_automation(const project::Snapshot& snapshot);

// A source track's node in the routed mixer: its topological index and the
// latency its own and upstream processors add to its output.
struct RoutedSource {
  project::TrackId track;
  std::size_t index;
  std::uint64_t output_latency_frames;
};

// The routed mixer of an offline render, with an input buffer per node.
struct OfflineRoute {
  std::unique_ptr<engine::RoutedMixer> mixer;
  // Source tracks in snapshot order.
  std::vector<RoutedSource> sources;
  std::shared_ptr<routing::PreparedRouteGainBlocks> gains;
  // Per snapshot track, its input's topological index.
  std::vector<std::size_t> track_slots;
  std::vector<std::vector<engine::StereoFrame>> inputs;
  std::vector<std::span<const engine::StereoFrame>> views;
  // Inputs of nodes nobody can hear: their sources need not render.
  std::vector<bool> silent_inputs;
  // A solo render's edges that would carry other sources into it.
  std::vector<std::size_t> isolated_edges;
  std::vector<engine::EdgeGainSpan> isolated_gains;
  // Zero gain for the isolated edges, one block long.
  std::vector<float> silent_gain;
  std::uint64_t output_latency_frames = 0;
  // The longest ring-out of the track chains on any path to the master.
  std::uint64_t native_tail_frames = 0;
};

struct OfflineGraphRequest {
  // From prepare_offline_automation for this snapshot.
  std::span<const automation_project::PreparedTargetAutomation> automation{};
  // A stem or track render: only this source track sounds, with the
  // detector buses that key its chain.
  std::optional<project::TrackId> solo_track = std::nullopt;
  std::span<const project::TrackId> detector_buses{};
  // Route the render even when nothing on its tracks needs it (taps).
  bool always_routed = false;
  std::size_t max_frames = 0;
  // Prepares the snapshot's VST3 stages. Without it a saved VST3 effect or
  // instrument fails preflight (see preflight_inserts).
  Vst3StagePreparer vst3{};
};

// One prepared stage of an offline render.
struct OfflineStage {
  std::shared_ptr<engine::ProcessorRuntime> runtime;
  // A VST3 stage's probe for a failed block (track_chain::Stage::failed).
  std::function<bool()> failed{};
  // True when the stage failed a block or held its output after a
  // non-finite frame.
  [[nodiscard]] bool faulted() const noexcept;
};

// Every processor an offline render runs, besides its sources. Tails and
// latencies are bounded: an unbounded tail fails preparation.
struct OfflineGraph {
  // Empty when the snapshot mixes plainly, with no routing or track chains.
  std::optional<OfflineRoute> route;
  // Each stage of every track chain, for the fault check.
  std::vector<OfflineStage> chain_stages;
  // The master chain in saved order: it runs after the mix.
  std::vector<OfflineStage> master_stages;
  std::uint64_t master_tail_frames = 0;
  std::uint64_t master_latency_frames = 0;
  // True when a track chain's stage faulted.
  [[nodiscard]] bool chain_faulted() const noexcept;
};

// Control or worker side. Prepares the snapshot's track chains, routing and
// master chain for blocks of at most request.max_frames. Built-in effects
// run, and VST3 effects through request.vst3; without it a saved VST3 effect
// or instrument fails (see preflight_inserts). The store decodes built-in
// synths' and effects' managed media.
[[nodiscard]] foundation::Result<OfflineGraph>
prepare_offline_graph(const project::Snapshot& snapshot, const persistence::ProjectStore& store,
                      const OfflineGraphRequest& request);

// True when a stage held its output after a non-finite frame; a stage this
// builder did not prepare counts as held, so a render never passes it silently.
[[nodiscard]] bool held_non_finite(const engine::ProcessorRuntime& stage) noexcept;

// A built-in instrument track as an offline render plays it: the same
// player live playback runs, with its MIDI, automation and media.
class OfflineInstrument final {
public:
  [[nodiscard]] static foundation::Result<OfflineInstrument>
  prepare(const project::Snapshot& snapshot, const persistence::ProjectStore& store,
          const project::Track& track, const engine::InstrumentEvents& events,
          std::span<const automation_project::PreparedTargetAutomation> automation,
          std::size_t max_frames);

  OfflineInstrument(OfflineInstrument&&) noexcept;
  OfflineInstrument& operator=(OfflineInstrument&&) noexcept;
  OfflineInstrument(const OfflineInstrument&) = delete;
  OfflineInstrument& operator=(const OfflineInstrument&) = delete;
  ~OfflineInstrument();

  // Waits until the instrument's streamed content is ready; call before
  // each render.
  void await_content() noexcept;
  // Adds one block from project frame `first`. False when the block failed.
  [[nodiscard]] bool render(std::span<engine::StereoFrame> output, std::uint64_t first,
                            engine::AudioWorkerPool* workers = nullptr) noexcept;
  // Frames a note released at the end keeps sounding.
  [[nodiscard]] std::uint64_t tail_frames() const noexcept;

private:
  struct Player;
  explicit OfflineInstrument(std::unique_ptr<Player> player) noexcept;
  std::unique_ptr<Player> player_;
};

// The release tail of a saved built-in instrument at `sample_rate`.
[[nodiscard]] foundation::Result<std::uint64_t>
instrument_tail_frames(const project::NativeInstrumentPatch& patch, std::uint32_t sample_rate);

} // namespace oscillate::render_graph
