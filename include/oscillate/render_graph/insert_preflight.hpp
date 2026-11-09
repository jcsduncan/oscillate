#pragma once

#include "oscillate/project/insert_projection.hpp"
#include "oscillate/routing/project_route_plan.hpp"

#include <algorithm>
#include <cstdint>
#include <ranges>
#include <utility>
#include <variant>
#include <vector>

namespace oscillate::render_graph {

// `native`: the offline render runs built-in instruments and effects only.
// `vst3`: a VST3 host prepares the VST3 stages too, so every saved chain
// renders: VST3 instruments and effects on any track or bus and on the master.
enum class ExportSource : std::uint8_t { native, vst3 };

// Non-owning view of validated saved intent. The caller retains the Snapshot
// until preparation completes. Unsupported processors fail before an output
// writer opens; active master slots retain saved order.
struct ExportInserts {
  // A track whose chain the render prepares, with its saved effect slots.
  struct TrackChain {
    project::TrackId owner;
    std::vector<const project::EffectInsert*> effects;
  };
  std::vector<TrackChain> track_chains;
  // Active master slots in saved order.
  std::vector<const project::EffectInsert*> master_chain;
};

[[nodiscard]] inline foundation::Result<ExportInserts>
preflight_inserts(const project::Snapshot& snapshot, ExportSource source) {
  using R = foundation::Result<ExportInserts>;
  auto valid = project::project_insert_chains(snapshot);
  if (!valid)
    return R::failure(valid.error());

  ExportInserts plan;
  const bool vst3 = source == ExportSource::vst3;
  const auto vst3_slot = [](const project::EffectInsert& slot) {
    return std::holds_alternative<project::Vst3PluginInstance>(slot.processor);
  };
  for (const auto& slot : snapshot.master_effects) {
    // A bypassed slot remains saved, but does not enter the rendered chain.
    // In particular, no plugin binary is needed to render past a bypassed
    // VST3 slot.
    if (!slot.enabled)
      continue;
    if (vst3_slot(slot) && !vst3)
      return R::failure(
          {foundation::ErrorCode::unsupported, "Export cannot render a VST3 master insert"});
    plan.master_chain.push_back(&slot);
  }

  for (const auto& track : snapshot.tracks) {
    const bool vst3_instrument =
        std::holds_alternative<project::Vst3PluginInstance>(track.instrument_source);
    if (vst3_instrument && !vst3)
      return R::failure({foundation::ErrorCode::unsupported,
                         "Native export cannot render a saved VST3 instrument"});
    if (!vst3) {
      // Without a host, a bypassed VST3 effect is an inert saved slot, but an
      // enabled one, missing or not, cannot render.
      const auto enabled_vst3 = std::ranges::find_if(
          track.effects, [&](const auto& slot) { return slot.enabled && vst3_slot(slot); });
      if (enabled_vst3 != track.effects.end())
        return R::failure({foundation::ErrorCode::unsupported,
                           std::get<project::Vst3PluginInstance>(enabled_vst3->processor).missing
                               ? "Saved VST3 audio effect is missing"
                               : "Export cannot render saved track or bus effects"});
    }
    // The track's chain: its VST3 instrument, if any, then each enabled
    // effect (ADR-0050). A track of only bypassed VST3 slots has none.
    const bool chain = vst3_instrument || std::ranges::any_of(track.effects, [&](const auto& slot) {
                         return slot.enabled || !vst3_slot(slot);
                       });
    if (!chain)
      continue;
    ExportInserts::TrackChain owned{track.id, {}};
    owned.effects.reserve(track.effects.size());
    for (const auto& slot : track.effects)
      owned.effects.push_back(&slot);
    plan.track_chains.push_back(std::move(owned));
  }
  return R::success(std::move(plan));
}

} // namespace oscillate::render_graph
