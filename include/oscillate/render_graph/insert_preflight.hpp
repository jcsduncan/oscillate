#pragma once

#include "oscillate/project/insert_projection.hpp"
#include "oscillate/routing/project_route_plan.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <utility>
#include <variant>
#include <vector>

namespace oscillate::render_graph {

// `native`: the offline master renders built-in instruments and effects only.
// `vst3`: a VST3 host prepares the VST3 stages too, so every saved chain
// renders: VST3 instruments and effects on any track or bus and on the master.
enum class ExportSource : std::uint8_t { native, vst3_instrument, vst3 };

// Non-owning view of validated schema-1.13 intent. The caller retains the
// Snapshot until preparation completes. Unsupported processors fail before
// opening an output writer; active native master slots retain saved order.
struct ExportInserts {
  struct NativeTrackChain {
    project::TrackId owner;
    std::vector<const project::EffectInsert*> effects;
  };
  std::vector<const project::Vst3PluginInstance*> vst3_instruments;
  std::vector<NativeTrackChain> native_track_chains;
  std::vector<const project::EffectInsert*> track_chain;
  std::optional<project::TrackId> track_chain_owner;
  std::optional<project::TrackId> second_track_chain_owner;
  const project::NativeConvolution* master_convolution = nullptr;
  std::vector<std::uint8_t> master_order;
  // Active master slots in saved order.
  std::vector<const project::EffectInsert*> master_chain;
};

[[nodiscard]] inline foundation::Result<ExportInserts>
preflight_inserts(const project::Snapshot& snapshot, ExportSource source,
                  bool allow_mixed_vst3_effect = false, bool convolution_supported = false,
                  bool allow_native_synth_beside_vst3 = false,
                  bool allow_two_instrument_effect_paths = false) {
  auto valid = project::project_insert_chains(snapshot);
  if (!valid)
    return foundation::Result<ExportInserts>::failure(valid.error());

  ExportInserts plan;
  if (project::has_convolution_insert(snapshot) && !convolution_supported)
    return foundation::Result<ExportInserts>::failure(
        {foundation::ErrorCode::unsupported,
         "Saved convolution insert is not available for export yet"});
  std::size_t active_convolutions = 0;
  std::array<bool, std::variant_size_v<project::EffectProcessor>> active_kinds{};
  const bool any_chain = source == ExportSource::vst3;
  for (const auto& slot : snapshot.master_effects) {
    // A bypassed slot remains saved, but does not enter the rendered chain.
    // In particular, no plugin binary is needed to render past a bypassed
    // VST3 slot.
    if (!slot.enabled)
      continue;
    const auto kind = slot.processor.index();
    if (std::holds_alternative<project::Vst3PluginInstance>(slot.processor) && !any_chain)
      return foundation::Result<ExportInserts>::failure(
          {foundation::ErrorCode::unsupported, "Export cannot render a VST3 master insert"});
    const bool built_in = std::holds_alternative<project::NativeEffectPatch>(slot.processor) ||
                          std::holds_alternative<project::Vst3PluginInstance>(slot.processor);
    if (!built_in && active_kinds[kind])
      return foundation::Result<ExportInserts>::failure(
          {foundation::ErrorCode::unsupported,
           "Export cannot render duplicate native master kinds"});
    active_kinds[kind] = true;
    plan.master_order.push_back(static_cast<std::uint8_t>(kind));
    plan.master_chain.push_back(&slot);
    if (const auto* convolution = std::get_if<project::NativeConvolution>(&slot.processor)) {
      plan.master_convolution = convolution;
      ++active_convolutions;
    }
  }

  for (const auto& track : snapshot.tracks) {
    if (any_chain) {
      // The track's chain: its VST3 instrument, if any, then each enabled
      // effect (ADR-0050). A track of only bypassed VST3 slots has none.
      const bool vst3_instrument =
          std::holds_alternative<project::Vst3PluginInstance>(track.instrument_source);
      const bool chain =
          vst3_instrument || std::ranges::any_of(track.effects, [](const auto& slot) {
            return slot.enabled ||
                   !std::holds_alternative<project::Vst3PluginInstance>(slot.processor);
          });
      if (!chain)
        continue;
      ExportInserts::NativeTrackChain owned{track.id, {}};
      for (const auto& slot : track.effects) {
        owned.effects.push_back(&slot);
        if (slot.enabled && std::holds_alternative<project::NativeConvolution>(slot.processor))
          ++active_convolutions;
      }
      plan.native_track_chains.push_back(std::move(owned));
      continue;
    }
    if (!track.effects.empty()) {
      const auto native_slot = [](const project::EffectInsert& slot) {
        return std::holds_alternative<project::NativeConvolution>(slot.processor) ||
               std::holds_alternative<project::NativeEffectPatch>(slot.processor);
      };
      const auto native_slots =
          static_cast<std::size_t>(std::ranges::count_if(track.effects, native_slot));
      if (allow_mixed_vst3_effect && source == ExportSource::vst3_instrument &&
          (track.kind == project::TrackKind::audio || track.kind == project::TrackKind::group_bus ||
           track.kind == project::TrackKind::return_bus ||
           (track.kind == project::TrackKind::instrument &&
            std::holds_alternative<project::Vst3PluginInstance>(track.instrument_source))) &&
          track.effects.size() <= (track.kind == project::TrackKind::instrument ? 2U : 1U) &&
          std::ranges::all_of(track.effects, [](const auto& slot) {
            return slot.enabled &&
                   std::holds_alternative<project::Vst3PluginInstance>(slot.processor);
          })) {
        if (plan.track_chain_owner) {
          if (!allow_two_instrument_effect_paths || plan.second_track_chain_owner ||
              track.kind != project::TrackKind::instrument || track.effects.size() != 1)
            return foundation::Result<ExportInserts>::failure(
                {foundation::ErrorCode::unsupported, "VST3 export supports one affected track"});
          plan.second_track_chain_owner = track.id;
        } else {
          plan.track_chain_owner = track.id;
        }
        for (const auto& effect : track.effects)
          plan.track_chain.push_back(&effect);
      } else if (((source == ExportSource::native &&
                   (track.kind == project::TrackKind::audio ||
                    (track.kind == project::TrackKind::instrument &&
                     std::holds_alternative<project::NativeInstrumentPatch>(
                         track.instrument_source)) ||
                    track.kind == project::TrackKind::group_bus ||
                    track.kind == project::TrackKind::return_bus)) ||
                  (source == ExportSource::vst3_instrument &&
                   track.kind == project::TrackKind::instrument &&
                   std::holds_alternative<project::Vst3PluginInstance>(track.instrument_source))) &&
                 std::ranges::all_of(track.effects, [&](const auto& slot) {
                   // Native export can skip a saved, bypassed VST3 effect. It
                   // consumes no native stage, latency or tail, but an enabled
                   // VST3 slot still makes this entire path unsupported.
                   return native_slot(slot) ||
                          (source == ExportSource::native && !slot.enabled &&
                           std::holds_alternative<project::Vst3PluginInstance>(slot.processor));
                 })) {
        if (native_slots != 0) {
          ExportInserts::NativeTrackChain native{track.id, {}};
          native.effects.reserve(track.effects.size());
          for (const auto& slot : track.effects) {
            native.effects.push_back(&slot);
            if (slot.enabled && std::holds_alternative<project::NativeConvolution>(slot.processor))
              ++active_convolutions;
          }
          plan.native_track_chains.push_back(std::move(native));
        }
      } else {
        const auto missing = std::ranges::any_of(track.effects, [](const auto& slot) {
          const auto* plugin = std::get_if<project::Vst3PluginInstance>(&slot.processor);
          return slot.enabled && plugin && plugin->missing;
        });
        const auto bypassed =
            std::ranges::any_of(track.effects, [](const auto& slot) { return !slot.enabled; });
        return foundation::Result<ExportInserts>::failure(
            {foundation::ErrorCode::unsupported,
             missing    ? "Saved VST3 audio effect is missing"
             : bypassed ? "Export cannot render bypassed track or bus effects"
                        : "Export cannot render saved track or bus effects"});
      }
    }
    if (source == ExportSource::native) {
      if (std::holds_alternative<project::Vst3PluginInstance>(track.instrument_source))
        return foundation::Result<ExportInserts>::failure(
            {foundation::ErrorCode::unsupported,
             "Native export cannot render a saved VST3 instrument"});
    } else if (std::holds_alternative<project::NativeInstrumentPatch>(track.instrument_source) &&
               !allow_native_synth_beside_vst3) {
      return foundation::Result<ExportInserts>::failure(
          {foundation::ErrorCode::unsupported,
           "VST3 instrument export cannot also render a built-in synth"});
    } else if (const auto* instrument =
                   std::get_if<project::Vst3PluginInstance>(&track.instrument_source)) {
      if (plan.vst3_instruments.size() == 2)
        return foundation::Result<ExportInserts>::failure(
            {foundation::ErrorCode::unsupported,
             "VST3 export supports at most two instrument sources"});
      // A saved missing flag can recover after the caller's fresh exact scan.
      plan.vst3_instruments.push_back(instrument);
    }
  }
  if (active_convolutions > 1)
    return foundation::Result<ExportInserts>::failure(
        {foundation::ErrorCode::resource_exhausted,
         "Offline export admits one active convolution insert"});
  if (source == ExportSource::vst3_instrument && plan.vst3_instruments.empty())
    return foundation::Result<ExportInserts>::failure(
        {foundation::ErrorCode::unsupported, "VST3 export has no instrument source"});
  return foundation::Result<ExportInserts>::success(plan);
}

} // namespace oscillate::render_graph
