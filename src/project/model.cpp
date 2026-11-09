#include "model_internal.hpp"

#include "oscillate/foundation/midi.hpp"
#include "oscillate/foundation/trace.hpp"
#include "oscillate/project/clay_limits.hpp"
#include "oscillate/project/insert_projection.hpp"
#include "oscillate/project/lookup.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace oscillate::project {

using namespace detail;

bool requires_routed_rendering(const Snapshot& snapshot) noexcept {
  if (snapshot.master_level != 1.0)
    return true;
  return std::ranges::any_of(snapshot.tracks, [](const Track& track) {
    const auto channel_lane = [](const AutomationLane& lane) {
      const auto* control = std::get_if<TrackControlTarget>(&lane.target);
      return control != nullptr && is_channel_control(control->key);
    };
    return track.kind == TrackKind::group_bus || track.kind == TrackKind::return_bus ||
           track.main_output.has_value() || !track.sends.empty() || track.muted || track.soloed ||
           track.level != 1.0 || track.pan != 0.0 ||
           std::ranges::any_of(track.automation, channel_lane) ||
           std::ranges::any_of(track.clips,
                               [&](const Clip& clip) {
                                 return std::ranges::any_of(clip.automation, channel_lane);
                               }) ||
           std::ranges::any_of(track.midi_clips, [&](const MidiClip& clip) {
             return std::ranges::any_of(clip.automation, channel_lane);
           });
  });
}

bool requires_automation_rendering(const Snapshot& snapshot) noexcept {
  return std::ranges::any_of(snapshot.tracks, [](const Track& track) {
    return !track.automation.empty() || std::ranges::any_of(track.clips, [](const Clip& clip) {
      return !clip.automation.empty();
    }) || std::ranges::any_of(track.midi_clips, [](const MidiClip& clip) {
      return !clip.automation.empty();
    });
  });
}

Result<InsertSlotId> command_insert_slot_id(std::uint64_t serial) {
  if (serial == std::numeric_limits<std::uint64_t>::max()) {
    return Result<InsertSlotId>::failure(exhausted("effect slot identity exhausted"));
  }
  return InsertSlotId::from_bits({command_slot_namespace, serial});
}

bool onset_dismissed(std::span<const std::uint64_t> dismissed, std::uint64_t frame) noexcept {
  const auto first = frame > max_onset_dismiss_distance ? frame - max_onset_dismiss_distance : 0;
  const auto found = std::ranges::lower_bound(dismissed, first);
  return found != dismissed.end() && *found <= frame + max_onset_dismiss_distance;
}

foundation::Result<void> validate_source_record(const SourceRecord& source) {
  return validate_source(source);
}

foundation::Result<InsertChainProjection> project_insert_chains(const Snapshot& snapshot) {
  using foundation::Result;
  if (snapshot.revision == 0) {
    return Result<InsertChainProjection>::failure(invalid("zero project revision is reserved"));
  }
  auto valid = validate(snapshot);
  if (!valid) {
    return Result<InsertChainProjection>::failure(valid.error());
  }

  InsertChainProjection projection{snapshot.id, snapshot.revision, {}};
  projection.paths.reserve(snapshot.tracks.size() + 1);
  const auto add_effects = [&](SignalPathProjection& path,
                               const std::vector<EffectInsert>& effects) {
    for (const auto& slot : effects) {
      std::visit(
          [&](const auto& processor) {
            using T = std::decay_t<decltype(processor)>;
            if constexpr (std::is_same_v<T, Vst3PluginInstance>) {
              path.effects.push_back(
                  {slot.id, processor.id, slot.enabled, std::nullopt, processor.missing});
              projection.has_missing_plugins |= processor.missing;
            } else {
              static_assert(std::is_same_v<T, NativeEffectPatch>);
              path.effects.push_back({slot.id, NativeInsertKind::effect, slot.enabled,
                                      processor.generation, false, processor.engine});
            }
          },
          slot.processor);
    }
  };
  for (const auto& track : snapshot.tracks) {
    projection.paths.push_back({SignalPathId{track.id}, track.kind, {}, {}});
    auto& path = projection.paths.back();
    if (const auto* native = native_source(track)) {
      path.source = ProjectedNativeInstrumentSource{native->engine};
    } else if (const auto* plugin = std::get_if<Vst3PluginInstance>(&track.instrument_source)) {
      path.source = ProjectedVst3InstrumentSource{plugin->id, plugin->missing};
      projection.has_missing_plugins |= plugin->missing;
    }
    add_effects(path, track.effects);
  }
  projection.paths.push_back({SignalPathId{MasterSignalPath{}}, std::nullopt, {}, {}});
  add_effects(projection.paths.back(), snapshot.master_effects);
  return Result<InsertChainProjection>::success(std::move(projection));
}

Result<ProjectModel> ProjectModel::create(ProjectId id, std::uint32_t rate,
                                          timeline::TempoMap tempo, timeline::MeterMap meter,
                                          TransportGate& transport) {
  return open(Snapshot{id, 1, rate, std::move(tempo), std::move(meter), {}, {}}, transport);
}

Result<ProjectModel> ProjectModel::open(Snapshot snapshot, TransportGate& transport) {
  if (snapshot.revision == 0) {
    return Result<ProjectModel>::failure(invalid("zero project revision is reserved"));
  }
  auto valid = validate(snapshot);
  if (!valid) {
    return Result<ProjectModel>::failure(valid.error());
  }
  snapshot.audible_revision = snapshot.revision;
  auto initial = std::make_shared<const Snapshot>(std::move(snapshot));
  return Result<ProjectModel>::success(ProjectModel(std::move(initial), transport));
}
} // namespace oscillate::project
