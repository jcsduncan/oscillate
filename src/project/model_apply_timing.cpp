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

namespace oscillate::project::detail {

Result<void> apply_command(ApplyContext& context, const ReplaceTempoMap& command) {
  auto& candidate = context.candidate;
  auto& changes = context.changes;
  candidate.tempo = command.map;
  changes.timing = true;
  return Result<void>::success();
}

Result<void> apply_command(ApplyContext& context, const ReplaceMeterMap& command) {
  auto& candidate = context.candidate;
  auto& changes = context.changes;
  candidate.meter = command.map;
  changes.timing = true;
  return Result<void>::success();
}

Result<void> apply_command(ApplyContext& context, const SetSampleRate& command) {
  auto& candidate = context.candidate;
  auto& changes = context.changes;
  if (!timeline::supported_sample_rate(command.rate)) {
    return Result<void>::failure(invalid("unsupported project sample rate"));
  }
  if (candidate.sample_rate != command.rate) {
    auto rescale_lanes = [&](std::vector<AutomationLane>& lanes, TrackId track_id,
                             std::optional<ClipId> clip_id) -> Result<void> {
      for (auto& lane : lanes) {
        if (lane.timebase != AutomationTimebase::elapsed)
          continue;
        if (lane.settings_generation == std::numeric_limits<std::uint64_t>::max())
          return Result<void>::failure(exhausted("automation lane generation exhausted"));
        for (auto& point : lane.points) {
          auto scaled = timeline::scale_sample_position({point.coordinate}, candidate.sample_rate,
                                                        command.rate);
          if (!scaled)
            return Result<void>::failure(scaled.error());
          point.coordinate = scaled.value().value;
        }
        ++lane.settings_generation;
        changes.automation = true;
        add_unique(changes.automation_lanes, lane.id);
        add_unique(changes.tracks, track_id);
        if (clip_id)
          add_unique(changes.clips, *clip_id);
      }
      return Result<void>::success();
    };
    for (auto& track : candidate.tracks) {
      if (auto scaled = rescale_lanes(track.automation, track.id, std::nullopt); !scaled)
        return scaled;
      for (auto& clip : track.clips) {
        if (auto scaled = rescale_lanes(clip.automation, track.id, clip.id); !scaled)
          return scaled;
        if (auto* elapsed = std::get_if<ElapsedInterval>(&clip.placement)) {
          for (auto& marker : clip.processing.reviewed_warp_markers) {
            auto* offset = std::get_if<timeline::SamplePosition>(&marker.target_offset);
            if (offset == nullptr)
              return Result<void>::failure(invalid("invalid elapsed warp marker timebase"));
            auto rescaled =
                timeline::scale_sample_position(*offset, candidate.sample_rate, command.rate);
            if (!rescaled)
              return Result<void>::failure(rescaled.error());
            *offset = rescaled.value();
          }
          auto start =
              timeline::scale_sample_position(elapsed->start, candidate.sample_rate, command.rate);
          if (!start) {
            return Result<void>::failure(start.error());
          }
          auto end =
              timeline::scale_sample_position(elapsed->end, candidate.sample_rate, command.rate);
          if (!end) {
            return Result<void>::failure(end.error());
          }
          elapsed->start = start.value();
          elapsed->end = end.value();
          if (clip.settings_generation == std::numeric_limits<std::uint64_t>::max()) {
            return Result<void>::failure(exhausted("clip settings generation exhausted"));
          }
          ++clip.settings_generation;
          add_unique(changes.clips, clip.id);
          add_unique(changes.tracks, track.id);
        }
      }
      for (auto& clip : track.midi_clips) {
        if (auto scaled = rescale_lanes(clip.automation, track.id, clip.id); !scaled)
          return scaled;
        if (auto* elapsed = std::get_if<ElapsedInterval>(&clip.placement)) {
          auto start =
              timeline::scale_sample_position(elapsed->start, candidate.sample_rate, command.rate);
          auto end =
              timeline::scale_sample_position(elapsed->end, candidate.sample_rate, command.rate);
          if (!start || !end) {
            return Result<void>::failure(!start ? start.error() : end.error());
          }
          if (clip.settings_generation == std::numeric_limits<std::uint64_t>::max()) {
            return Result<void>::failure(exhausted("MIDI clip generation exhausted"));
          }
          elapsed->start = start.value();
          elapsed->end = end.value();
          ++clip.settings_generation;
          add_unique(changes.clips, clip.id);
          add_unique(changes.tracks, track.id);
        }
      }
    }
    for (auto& slot : candidate.master_effects) {
      auto bump = [&](auto& native) -> Result<void> {
        using T = std::decay_t<decltype(native)>;
        if constexpr (std::is_same_v<T, Vst3PluginInstance>) {
          return Result<void>::success();
        } else {
          if (native.generation == std::numeric_limits<std::uint64_t>::max()) {
            return Result<void>::failure(exhausted("native effect generation exhausted"));
          }
          ++native.generation;
          changes.master_effects = true;
          return Result<void>::success();
        }
      };
      auto bumped = std::visit(bump, slot.processor);
      if (!bumped)
        return bumped;
    }
    candidate.sample_rate = command.rate;
    changes.sample_rate = true;
  } else {
    return Result<void>::failure(invalid("project sample rate is unchanged"));
  }
  return Result<void>::success();
}

} // namespace oscillate::project::detail
