#include "edit_rules.hpp"

#include <algorithm>
#include <cstddef>
#include <ranges>
#include <utility>
#include <variant>
#include <vector>

namespace oscillate::application::routed_playback {

namespace {

// A lane a built-in instrument's player evaluates: its synth's parameters,
// a macro (ADR-0059) or one of the track's note effects.
[[nodiscard]] bool player_lane(const project::Track& track, const project::AutomationLane& lane) {
  if (std::holds_alternative<project::NativeParameterTarget>(lane.target) ||
      std::holds_alternative<project::MacroTarget>(lane.target))
    return true;
  const auto* effect = std::get_if<project::NativeEffectParameterTarget>(&lane.target);
  return effect != nullptr &&
         std::ranges::find(track.note_effects, effect->slot, &project::NoteEffectInsert::id) !=
             track.note_effects.end();
}

} // namespace

[[nodiscard]] bool same_sources(std::span<const routing::ProjectRouteSource> left,
                                std::span<const routing::ProjectRouteSource> right) {
  return std::ranges::equal(left, right, [](const auto& a, const auto& b) {
    return a.track == b.track && a.node == b.node && a.topological_index == b.topological_index;
  });
}

[[nodiscard]] bool same_timing(const project::Snapshot& left, const project::Snapshot& right) {
  return left.tempo.revision() == right.tempo.revision() &&
         left.meter.revision() == right.meter.revision() &&
         std::ranges::equal(left.tempo.events(), right.tempo.events(),
                            [](const auto& a, const auto& b) {
                              return a.tick == b.tick && a.bpm == b.bpm &&
                                     a.shape_to_next == b.shape_to_next;
                            }) &&
         std::ranges::equal(left.meter.events(), right.meter.events(),
                            [](const auto& a, const auto& b) {
                              return a.tick == b.tick && a.numerator == b.numerator &&
                                     a.denominator == b.denominator;
                            });
}

[[nodiscard]] bool player_edit_only(const project::Track& before, const project::Track& after) {
  if (before.id != after.id || before.kind != project::TrackKind::instrument ||
      !std::holds_alternative<project::NativeInstrumentPatch>(before.instrument_source) ||
      before.macros.size() != after.macros.size())
    return false;
  // A macro's value is heard only through the parameters it sets; its
  // mappings shape its lanes.
  for (std::size_t macro = 0; macro < before.macros.size(); ++macro) {
    if (before.macros[macro].mappings != after.macros[macro].mappings)
      return false;
  }
  const auto other_lane = [](const project::Track& track) {
    return [&track](const project::AutomationLane& lane) { return !player_lane(track, lane); };
  };
  if (!std::ranges::equal(before.automation | std::views::filter(other_lane(before)),
                          after.automation | std::views::filter(other_lane(after))))
    return false;
  // Only a player lane may ride on a MIDI clip that changes.
  for (const auto* track : {&before, &after}) {
    for (const auto& clip : track->midi_clips) {
      if (!std::ranges::all_of(clip.automation, [track](const project::AutomationLane& lane) {
            return player_lane(*track, lane);
          }))
        return false;
    }
  }
  auto masked = before;
  masked.midi_clips = after.midi_clips;
  masked.note_effects = after.note_effects;
  masked.macros = after.macros;
  masked.automation = after.automation;
  return masked == after;
}

[[nodiscard]] bool vst3_midi_edit_only(const project::Track& before, const project::Track& after) {
  const auto unautomated = [](const project::Track& track) {
    return std::ranges::all_of(track.midi_clips,
                               [](const auto& clip) { return clip.automation.empty(); });
  };
  if (before.id != after.id || before.kind != project::TrackKind::instrument ||
      !std::holds_alternative<project::Vst3PluginInstance>(before.instrument_source) ||
      !unautomated(before) || !unautomated(after))
    return false;
  auto masked = before;
  masked.midi_clips = after.midi_clips;
  return masked == after;
}

[[nodiscard]] bool same_audio_content(const project::Snapshot& prepared,
                                      const project::Snapshot& candidate) {
  if (prepared.sample_rate != candidate.sample_rate ||
      prepared.tempo.revision() != candidate.tempo.revision() ||
      prepared.sources != candidate.sources) {
    return false;
  }
  std::vector<std::pair<project::TrackId, project::Clip>> original;
  std::vector<std::pair<project::TrackId, project::Clip>> current;
  for (const auto& track : prepared.tracks) {
    for (const auto& clip : track.clips)
      original.emplace_back(track.id, clip);
  }
  for (const auto& track : candidate.tracks) {
    for (const auto& clip : track.clips)
      current.emplace_back(track.id, clip);
  }
  const auto by_id = [](const auto& a, const auto& b) { return a.second.id < b.second.id; };
  std::ranges::sort(original, by_id);
  std::ranges::sort(current, by_id);
  if (!std::ranges::equal(original, current, [](const auto& a, const auto& b) {
        const auto& left = a.second;
        const auto& right = b.second;
        return a.first == b.first && left.id == right.id && left.source == right.source &&
               left.source_range == right.source_range && left.placement == right.placement &&
               left.settings_generation == right.settings_generation && left.fades == right.fades &&
               left.processing == right.processing;
      }))
    return false;
  for (const auto& track : prepared.tracks) {
    const auto found = std::ranges::find(candidate.tracks, track.id, &project::Track::id);
    if (found == candidate.tracks.end() || found->midi_clips != track.midi_clips)
      return false;
    // Note effects play before the source, a frozen render replaces it, and
    // macros feed its parameters (ADR-0059).
    if (found->note_effects != track.note_effects || found->macros != track.macros ||
        found->frozen != track.frozen)
      return false;
    // A VST3 instrument is its chain's first stage, which checks its saved
    // state; its live parameter edits never change the source bank.
    const auto* old_plugin = std::get_if<project::Vst3PluginInstance>(&track.instrument_source);
    const auto* new_plugin = std::get_if<project::Vst3PluginInstance>(&found->instrument_source);
    if (old_plugin && new_plugin ? old_plugin->id != new_plugin->id
                                 : found->instrument_source != track.instrument_source)
      return false;
    // Synth lanes are bound into its player when the source is prepared.
    const auto native_lane = [](const project::AutomationLane& lane) {
      // A macro lane may drive the synth's parameters (ADR-0059).
      return std::holds_alternative<project::NativeParameterTarget>(lane.target) ||
             std::holds_alternative<project::MacroTarget>(lane.target);
    };
    if (!std::ranges::equal(track.automation | std::views::filter(native_lane),
                            found->automation | std::views::filter(native_lane)))
      return false;
  }
  return true;
}

[[nodiscard]] bool needs_whole_switch(const project::Snapshot& published,
                                      const project::Snapshot& candidate) {
  const auto ids = [](const project::Snapshot& snapshot) {
    std::vector<project::TrackId> result;
    result.reserve(snapshot.tracks.size());
    for (const auto& track : snapshot.tracks)
      result.push_back(track.id);
    std::ranges::sort(result);
    return result;
  };
  if (ids(published) != ids(candidate) || !same_timing(published, candidate) ||
      published.sources != candidate.sources)
    return true;
  for (const auto& after : candidate.tracks) {
    const auto& before = *std::ranges::find(published.tracks, after.id, &project::Track::id);
    const auto* old_plugin = std::get_if<project::Vst3PluginInstance>(&before.instrument_source);
    const auto* new_plugin = std::get_if<project::Vst3PluginInstance>(&after.instrument_source);
    // A VST3 instrument's own state edits switch its chain stage, and a
    // built-in synth's values and document go to its running runtime.
    const auto* old_patch = std::get_if<project::NativeInstrumentPatch>(&before.instrument_source);
    const auto* new_patch = std::get_if<project::NativeInstrumentPatch>(&after.instrument_source);
    const bool same_instrument =
        old_plugin && new_plugin
            ? old_plugin->id == new_plugin->id
            : (old_patch && new_patch
                   ? old_patch->engine == new_patch->engine &&
                         old_patch->engine_version == new_patch->engine_version &&
                         // New media need decoding into a new player.
                         old_patch->document.media == new_patch->document.media
                   : before.instrument_source == after.instrument_source);
    if (!same_instrument)
      return true;
    if (std::holds_alternative<project::NativeInstrumentPatch>(before.instrument_source) &&
        before.midi_clips != after.midi_clips && !player_edit_only(before, after))
      return true;
    if (std::holds_alternative<project::Vst3PluginInstance>(before.instrument_source) &&
        before.midi_clips != after.midi_clips && !vst3_midi_edit_only(before, after))
      return true;
  }
  return false;
}

[[nodiscard]] bool audio_clip_edit_only(const project::Snapshot& before,
                                        const project::Snapshot& after) {
  if (before.id != after.id || before.sample_rate != after.sample_rate ||
      !same_timing(before, after) || before.sources != after.sources ||
      before.master_effects != after.master_effects || before.master_level != after.master_level ||
      before.source_tempo_reviews != after.source_tempo_reviews ||
      before.source_key_reviews != after.source_key_reviews ||
      before.next_insert_slot_serial != after.next_insert_slot_serial ||
      before.tracks.size() != after.tracks.size())
    return false;
  bool changed = false;
  for (std::size_t index = 0; index < before.tracks.size(); ++index) {
    const auto& prior = before.tracks[index];
    const auto& next = after.tracks[index];
    // An instrument track the edit leaves alone keeps playing (its player
    // carries over to the replacement source).
    if (prior == next)
      continue;
    if (prior.kind == project::TrackKind::instrument ||
        next.kind == project::TrackKind::instrument ||
        !std::holds_alternative<std::monostate>(prior.instrument_source) ||
        !std::holds_alternative<std::monostate>(next.instrument_source) ||
        !prior.midi_clips.empty() || !next.midi_clips.empty() || prior.id != next.id ||
        prior.kind != next.kind || prior.effects != next.effects ||
        prior.main_output != next.main_output || prior.sends != next.sends ||
        prior.automation != next.automation || prior.muted != next.muted ||
        prior.soloed != next.soloed || prior.level != next.level || prior.pan != next.pan)
      return false;
    for (const auto& old_clip : prior.clips) {
      const auto found = std::ranges::find(next.clips, old_clip.id, &project::Clip::id);
      if (found == next.clips.end()) {
        changed = true;
        continue;
      }
      const auto& new_clip = *found;
      if (old_clip.source != new_clip.source || old_clip.automation != new_clip.automation)
        return false;
      // Saved warp/pitch settings are clip content like placement: the
      // replacement source renders them through the processed-clip reader.
      const bool clip_changed = old_clip.placement != new_clip.placement ||
                                old_clip.source_range != new_clip.source_range ||
                                old_clip.fades != new_clip.fades ||
                                old_clip.processing != new_clip.processing;
      if (clip_changed) {
        if (new_clip.settings_generation <= old_clip.settings_generation)
          return false;
        changed = true;
      } else if (old_clip.settings_generation != new_clip.settings_generation) {
        return false;
      }
    }
    for (const auto& new_clip : next.clips) {
      if (std::ranges::find(prior.clips, new_clip.id, &project::Clip::id) == prior.clips.end())
        changed = true;
    }
  }
  return changed;
}

[[nodiscard]] bool runs(const track_chain::PreparedChain& chain,
                        std::span<const track_chain::SavedStage> saved) {
  return std::ranges::equal(chain.stages, saved, {}, &track_chain::Stage::saved);
}

} // namespace oscillate::application::routed_playback
