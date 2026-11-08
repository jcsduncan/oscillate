#include "oscillate/application/routed_playback_control.hpp"

#include "oscillate/application/routed_audio_source.hpp"
#include "oscillate/audio_io/stereo_block.hpp"
#include "oscillate/automation_project/preparation.hpp"
#include "oscillate/engine/chunked_processor.hpp"
#include "oscillate/engine/midi_events.hpp"
#include "oscillate/foundation/trace.hpp"
#include "oscillate/native_dsp/convolution_runtime.hpp"
#include "oscillate/native_dsp/synth/project_binding.hpp"
#include "oscillate/native_inserts/effect_binding.hpp"
#include "oscillate/native_inserts/native_stage.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/routing/route_gain_blocks.hpp"
#include "oscillate/track_chain/track_chain.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <iterator>
#include <limits>
#include <new>
#include <ranges>
#include <string>
#include <type_traits>
#include <unistd.h>
#include <utility>

namespace oscillate::application {
namespace {
using foundation::Error;
using foundation::ErrorCode;
using foundation::Result;

[[nodiscard]] Result<void> rejected(ErrorCode code, const char* message) {
  return Result<void>::failure({code, message});
}

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

template <typename TrackRef>
[[nodiscard]] auto* saved_vst3(TrackRef& track, project::PluginInstanceId plugin) {
  auto* found = std::get_if<project::Vst3PluginInstance>(&track.instrument_source);
  if (found && found->id == plugin)
    return found;
  for (auto& effect : track.effects) {
    found = std::get_if<project::Vst3PluginInstance>(&effect.processor);
    if (found && found->id == plugin)
      return found;
  }
  return decltype(found){nullptr};
}

// The saved plugin on `track`'s chain, or on the master's with no track.
[[nodiscard]] const project::Vst3PluginInstance* saved_vst3(const project::Snapshot& snapshot,
                                                            std::optional<project::TrackId> track,
                                                            project::PluginInstanceId plugin) {
  if (track) {
    const auto owner = std::ranges::find(snapshot.tracks, *track, &project::Track::id);
    return owner == snapshot.tracks.end() ? nullptr : saved_vst3(*owner, plugin);
  }
  for (const auto& effect : snapshot.master_effects) {
    const auto* found = std::get_if<project::Vst3PluginInstance>(&effect.processor);
    if (found && found->id == plugin)
      return found;
  }
  return nullptr;
}

// True when after differs from before only in one saved VST3 plugin's opaque
// component state and parameter references. Identity, slot, enablement,
// automation and every other track or project field must be unchanged.
[[nodiscard]] bool manual_vst3_state_only(const project::Snapshot& before,
                                          const project::Snapshot& after,
                                          project::PluginInstanceId plugin) {
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
    const auto& old_track = before.tracks[index];
    const auto& next_track = after.tracks[index];
    if (old_track == next_track)
      continue;
    const auto* old_plugin = saved_vst3(old_track, plugin);
    const auto* next_plugin = saved_vst3(next_track, plugin);
    if (changed || !old_plugin || !next_plugin)
      return false;
    auto restored = next_track;
    auto* target = saved_vst3(restored, plugin);
    target->component_state = old_plugin->component_state;
    target->parameters = old_plugin->parameters;
    if (restored != old_track)
      return false;
    changed = true;
  }
  return changed;
}

[[nodiscard]] bool same_track_outside_effects(const project::Track& left,
                                              const project::Track& right) {
  return left.id == right.id && left.clips == right.clips && left.kind == right.kind &&
         left.instrument_source == right.instrument_source && left.midi_clips == right.midi_clips &&
         left.main_output == right.main_output && left.sends == right.sends &&
         left.automation == right.automation && left.muted == right.muted &&
         left.soloed == right.soloed && left.level == right.level && left.pan == right.pan &&
         left.note_effects == right.note_effects && left.macros == right.macros &&
         left.frozen == right.frozen;
}

[[nodiscard]] bool native_synth_automation_changed(const project::Snapshot& before,
                                                   const project::Snapshot& after) {
  const auto native_lane = [](const project::AutomationLane& lane) {
    // A macro lane may drive the synth's parameters (ADR-0059).
    return std::holds_alternative<project::NativeParameterTarget>(lane.target) ||
           std::holds_alternative<project::MacroTarget>(lane.target);
  };
  for (const auto& old_track : before.tracks) {
    if (!std::holds_alternative<project::NativeInstrumentPatch>(old_track.instrument_source))
      continue;
    const auto next = std::ranges::find(after.tracks, old_track.id, &project::Track::id);
    if (next != after.tracks.end() &&
        !std::ranges::equal(old_track.automation | std::views::filter(native_lane),
                            next->automation | std::views::filter(native_lane)))
      return true;
  }
  return false;
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

// An edit the routed graph can switch to without a hold: the same tracks in
// the same order and unchanged sources (clips, built-in instruments, MIDI).
// Chains, routing, levels and sends may differ; the callback hands the
// running state over at the switching block (ADR-0049, ADR-0050). A VST3
// instrument is a chain stage, so it compares against the audible graph,
// which follows its live parameter edits.
[[nodiscard]] bool live_switch_eligible(const project::Snapshot& published,
                                        const project::Snapshot& source,
                                        const project::Snapshot& candidate) {
  if (published.tracks.size() != candidate.tracks.size() ||
      source.tracks.size() != candidate.tracks.size())
    return false;
  for (std::size_t index = 0; index < candidate.tracks.size(); ++index) {
    const auto& next = candidate.tracks[index];
    const auto& old = published.tracks[index];
    const auto& sounding = source.tracks[index];
    const auto& instrument =
        std::holds_alternative<project::Vst3PluginInstance>(next.instrument_source) ? old
                                                                                    : sounding;
    if (old.id != next.id || sounding.id != next.id ||
        instrument.instrument_source != next.instrument_source ||
        sounding.midi_clips != next.midi_clips)
      return false;
  }
  return true;
}

// A native synth player belongs to the prepared source bank, not to the
// immutable mixer graph. Rebuilding only the graph cannot replace its engine.
[[nodiscard]] bool native_synth_engine_changed(const project::Snapshot& source,
                                               const project::Snapshot& candidate) {
  for (const auto& old_track : source.tracks) {
    const auto* old_patch =
        std::get_if<project::NativeInstrumentPatch>(&old_track.instrument_source);
    if (!old_patch)
      continue;
    const auto next = std::ranges::find(candidate.tracks, old_track.id, &project::Track::id);
    if (next == candidate.tracks.end())
      continue;
    const auto* next_patch = std::get_if<project::NativeInstrumentPatch>(&next->instrument_source);
    if (next_patch && (old_patch->engine != next_patch->engine ||
                       old_patch->engine_version != next_patch->engine_version))
      return true;
  }
  return false;
}

// A source-bank replacement may change exactly one saved native instrument.
// Everything else, including MIDI and routing, remains byte-for-byte project
// intent. The audio-only clip predicate below admits its own separate case.
[[nodiscard]] bool native_synth_switch_only(const project::Snapshot& before,
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
    if (before.tracks[index] == after.tracks[index])
      continue;
    if (changed || before.tracks[index].id != after.tracks[index].id)
      return false;
    const auto* old_patch =
        std::get_if<project::NativeInstrumentPatch>(&before.tracks[index].instrument_source);
    const auto* new_patch =
        std::get_if<project::NativeInstrumentPatch>(&after.tracks[index].instrument_source);
    if (!old_patch || !new_patch ||
        (old_patch->engine == new_patch->engine &&
         old_patch->engine_version == new_patch->engine_version))
      return false;
    auto normalized = after.tracks[index];
    normalized.instrument_source = before.tracks[index].instrument_source;
    if (normalized != before.tracks[index])
      return false;
    changed = true;
  }
  return changed;
}

// Only the project key changed (and with it nothing but presentation): the
// running synths hear the new key; no graph, chain or source changes.
[[nodiscard]] bool project_key_only(const project::Snapshot& before,
                                    const project::Snapshot& after) {
  return after.revision > before.revision && before.key != after.key && before.id == after.id &&
         before.sample_rate == after.sample_rate && same_timing(before, after) &&
         before.sources == after.sources && before.master_effects == after.master_effects &&
         before.master_level == after.master_level &&
         before.source_tempo_reviews == after.source_tempo_reviews &&
         before.source_key_reviews == after.source_key_reviews &&
         before.next_insert_slot_serial == after.next_insert_slot_serial &&
         before.tracks == after.tracks;
}

[[nodiscard]] bool channel_controls_only(const project::Snapshot& before,
                                         const project::Snapshot& after) {
  if (after.revision <= before.revision || before.id != after.id ||
      before.sample_rate != after.sample_rate || !same_timing(before, after) ||
      before.sources != after.sources || before.master_effects != after.master_effects ||
      before.source_tempo_reviews != after.source_tempo_reviews ||
      before.source_key_reviews != after.source_key_reviews ||
      before.next_insert_slot_serial != after.next_insert_slot_serial ||
      before.tracks.size() != after.tracks.size())
    return false;
  bool changed = before.master_level != after.master_level;
  for (std::size_t index = 0; index < before.tracks.size(); ++index) {
    const auto& old_track = before.tracks[index];
    const auto& new_track = after.tracks[index];
    if (old_track.id != new_track.id || old_track.clips != new_track.clips ||
        old_track.kind != new_track.kind ||
        old_track.instrument_source != new_track.instrument_source ||
        old_track.midi_clips != new_track.midi_clips || old_track.effects != new_track.effects ||
        old_track.main_output != new_track.main_output || old_track.sends != new_track.sends ||
        old_track.automation != new_track.automation)
      return false;
    changed |= old_track.muted != new_track.muted || old_track.soloed != new_track.soloed ||
               old_track.level != new_track.level || old_track.pan != new_track.pan;
  }
  return changed;
}

[[nodiscard]] bool send_gains_only(const project::Snapshot& before,
                                   const project::Snapshot& after) {
  if (after.revision <= before.revision || before.id != after.id ||
      before.sample_rate != after.sample_rate || !same_timing(before, after) ||
      before.sources != after.sources || before.master_effects != after.master_effects ||
      before.master_level != after.master_level ||
      before.source_tempo_reviews != after.source_tempo_reviews ||
      before.source_key_reviews != after.source_key_reviews ||
      before.next_insert_slot_serial != after.next_insert_slot_serial ||
      before.tracks.size() != after.tracks.size())
    return false;
  bool changed = false;
  for (std::size_t index = 0; index < before.tracks.size(); ++index) {
    const auto& old_track = before.tracks[index];
    const auto& new_track = after.tracks[index];
    if (old_track.id != new_track.id || old_track.clips != new_track.clips ||
        old_track.kind != new_track.kind ||
        old_track.instrument_source != new_track.instrument_source ||
        old_track.midi_clips != new_track.midi_clips || old_track.effects != new_track.effects ||
        old_track.main_output != new_track.main_output ||
        old_track.automation != new_track.automation || old_track.muted != new_track.muted ||
        old_track.soloed != new_track.soloed || old_track.level != new_track.level ||
        old_track.pan != new_track.pan || old_track.sends.size() != new_track.sends.size())
      return false;
    for (std::size_t send = 0; send < old_track.sends.size(); ++send) {
      const auto& old_route = old_track.sends[send];
      const auto& new_route = new_track.sends[send];
      if (old_route.id != new_route.id || old_route.destination != new_route.destination ||
          old_route.purpose != new_route.purpose ||
          old_route.target_insert != new_route.target_insert)
        return false;
      changed |= old_route.gain != new_route.gain;
    }
  }
  return changed;
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

[[nodiscard]] bool source_overlay_clip_only(const project::Snapshot& before,
                                            const project::Snapshot& after) {
  const auto stable_native_chain = [](std::span<const project::EffectInsert> effects) {
    return std::ranges::all_of(effects, [](const auto& insert) {
      return std::holds_alternative<project::NativeEffectPatch>(insert.processor);
    });
  };
  // This bounded source-only plan shares the active mixer and its processor
  // owners. Source bank order is the unchanged track order. A single exact
  // clip partition may add one source view inside an existing audio slot.
  if (!audio_clip_edit_only(before, after) || before.tracks.empty() || before.tracks.size() > 4 ||
      before.sources.empty() || before.sources.size() > 16 ||
      !stable_native_chain(before.master_effects))
    return false;
  std::size_t clip_count = 0;
  std::size_t changed_clips = 0;
  bool changed = false;
  bool split = false;
  for (std::size_t index = 0; index < before.tracks.size(); ++index) {
    const auto& old_track = before.tracks[index];
    const auto& new_track = after.tracks[index];
    if (old_track.kind != project::TrackKind::audio ||
        new_track.clips.size() < old_track.clips.size() ||
        new_track.clips.size() > old_track.clips.size() + 1 ||
        !stable_native_chain(old_track.effects) || !old_track.automation.empty() ||
        !old_track.sends.empty() || old_track.main_output || !new_track.automation.empty() ||
        !new_track.sends.empty() || new_track.main_output)
      return false;
    clip_count += new_track.clips.size();
    if (clip_count > 16)
      return false;
    const bool split_track = new_track.clips.size() == old_track.clips.size() + 1;
    if (split_track && split)
      return false;
    const project::Clip* original_split = nullptr;
    const project::Clip* left_split = nullptr;
    for (const auto& original : old_track.clips) {
      const auto found = std::ranges::find(new_track.clips, original.id, &project::Clip::id);
      if (found == new_track.clips.end() || original.source != found->source ||
          !original.automation.empty() || !found->automation.empty() ||
          original.processing.requires_rendering() || found->processing.requires_rendering() ||
          original.processing != found->processing || original.fades != found->fades)
        return false;
      const auto source =
          std::ranges::find(before.sources, original.source, &project::SourceRecord::id);
      if (source == before.sources.end() || source->encoding != project::SourceEncoding::wav ||
          source->external_link || source->sample_rate != before.sample_rate)
        return false;
      const bool geometry_changed =
          original.placement != found->placement || original.source_range != found->source_range;
      changed_clips += geometry_changed ? 1U : 0U;
      if (split_track && geometry_changed) {
        if (original_split)
          return false;
        original_split = &original;
        left_split = &*found;
      }
      changed |= geometry_changed;
    }
    if (split_track) {
      // Source bank topology is unchanged: one existing clip becomes two
      // contiguous views of the same managed frames in the same audio slot.
      // Keep this separate from general clip insertion or duplication.
      if (!original_split || !left_split || original_split->fades != project::ClipFades{} ||
          !original_split->automation.empty())
        return false;
      const auto new_clip = std::ranges::find_if(new_track.clips, [&](const auto& clip) {
        return std::ranges::none_of(old_track.clips,
                                    [&](const auto& old) { return old.id == clip.id; });
      });
      if (new_clip == new_track.clips.end() || new_clip->source != original_split->source ||
          new_clip->fades != original_split->fades ||
          new_clip->processing != original_split->processing ||
          new_clip->processing.requires_rendering() || !new_clip->automation.empty())
        return false;
      const auto* original_time = std::get_if<project::ElapsedInterval>(&original_split->placement);
      const auto* left_time = std::get_if<project::ElapsedInterval>(&left_split->placement);
      const auto* right_time = std::get_if<project::ElapsedInterval>(&new_clip->placement);
      if (!original_time || !left_time || !right_time || left_time->start != original_time->start ||
          left_time->end != right_time->start || right_time->end != original_time->end ||
          left_split->source_range.start != original_split->source_range.start ||
          left_split->source_range.end != new_clip->source_range.start ||
          new_clip->source_range.end != original_split->source_range.end ||
          original_time->start.value < 0 ||
          original_time->end.value - original_time->start.value !=
              original_split->source_range.end.value - original_split->source_range.start.value ||
          left_time->end.value - left_time->start.value !=
              left_split->source_range.end.value - left_split->source_range.start.value ||
          right_time->end.value - right_time->start.value !=
              new_clip->source_range.end.value - new_clip->source_range.start.value)
        return false;
      split = true;
    }
  }
  return changed && (!split || changed_clips == 1);
}

[[nodiscard]] std::optional<std::uint64_t>
sum_export_tail(std::optional<std::uint64_t> total, std::optional<std::uint64_t> stage) noexcept {
  if (!total || !stage || *stage > std::numeric_limits<std::uint64_t>::max() - *total)
    return std::nullopt;
  return *total + *stage;
}

// How long a removed stage that cannot bound its tail rings out before the
// mixer fades it (ADR-0050).
constexpr std::uint64_t unbounded_ring_out_seconds = 4;

// A prepared VST3 binding as a chain stage. Only an independently qualified
// tail bounds export; a declared one still bounds a live ring-out.
[[nodiscard]] track_chain::Stage vst3_stage(const track_chain::SavedStage& saved,
                                            const PreparedTrackInsert& prepared) {
  std::optional<std::uint64_t> declared;
  if (prepared.finite_tail_frames)
    declared = *prepared.finite_tail_frames;
  return {saved,
          prepared.processor,
          prepared.latency_frames,
          prepared.fixed_tail_qualified ? declared : std::nullopt,
          declared,
          prepared.routed_sidechain};
}

// True when `chain` runs exactly the `saved` stages.
[[nodiscard]] bool runs(const track_chain::PreparedChain& chain,
                        std::span<const track_chain::SavedStage> saved) {
  return std::ranges::equal(chain.stages, saved, {}, &track_chain::Stage::saved);
}

#if OSCILLATE_HAVE_CONVOLUTION
[[nodiscard]] Result<std::shared_ptr<native_dsp::ConvolutionRuntime>>
prepare_live_convolution(const project::Snapshot& snapshot, const project::NativeConvolution& saved,
                         const LiveConvolutionAdmission& admission) {
  using Runtime = native_dsp::ConvolutionRuntime;
  using RuntimeResult = Result<std::shared_ptr<Runtime>>;
  if (!admission.store)
    return RuntimeResult::failure(
        {ErrorCode::unavailable,
         "Live convolution needs a verified IR store and measured headroom"});
  const auto record =
      std::ranges::find(snapshot.sources, saved.ir.source, &project::SourceRecord::id);
  if (record == snapshot.sources.end() || record->external_link ||
      record->encoding != project::SourceEncoding::wav)
    return RuntimeResult::failure(
        {ErrorCode::corrupt_data, "Live convolution IR has no managed WAV source"});
  auto descriptor = admission.store->open_managed_source(*record);
  if (!descriptor)
    return RuntimeResult::failure(descriptor.error());
  auto prepared = native_dsp::prepare_convolution_ir(
      descriptor.value(),
      {saved.ir.sha256, saved.ir.encoded_bytes, saved.ir.sample_rate,
       static_cast<std::uint16_t>(saved.ir.channels), saved.ir.frames},
      admission.budget, snapshot.sample_rate, static_cast<float>(saved.wet));
  const int closed = ::close(descriptor.value());
  if (!prepared)
    return RuntimeResult::failure(prepared.error());
  if (closed != 0)
    return RuntimeResult::failure({ErrorCode::io_failure, "Cannot close verified live IR"});
  try {
    return RuntimeResult::success(std::make_shared<Runtime>(std::move(prepared.value().processor)));
  } catch (const std::bad_alloc&) {
    return RuntimeResult::failure(
        {ErrorCode::resource_exhausted, "Live convolution runtime allocation failed"});
  }
}
#endif
} // namespace

Result<std::uint64_t> automation_sample_limit(const project::Snapshot& snapshot,
                                              std::size_t max_frames) {
  if (max_frames == 0 || max_frames > engine::max_midi_block_frames)
    return Result<std::uint64_t>::failure(
        {ErrorCode::invalid_argument, "Automation block capacity is invalid"});
  long double peak_bpm = 0.0L;
  for (const auto& event : snapshot.tempo.events())
    peak_bpm = std::max(peak_bpm, static_cast<long double>(event.bpm));
  const long double bound =
      (static_cast<long double>(std::numeric_limits<std::int64_t>::max()) / 4.0L) * 60.0L *
      snapshot.sample_rate / (peak_bpm * static_cast<long double>(timeline::ticks_per_quarter));
  if (!std::isfinite(bound) || bound < static_cast<long double>(max_frames))
    return Result<std::uint64_t>::failure(
        {ErrorCode::unsupported, "Automation tempo exceeds live sample-coordinate headroom"});
  const auto maximum =
      static_cast<long double>(std::numeric_limits<std::int64_t>::max() - max_frames);
  return Result<std::uint64_t>::success(static_cast<std::uint64_t>(std::min(bound, maximum)));
}

RoutedPlaybackControl::RoutedPlaybackControl(project::ProjectModel& model,
                                             engine::RoutedMixerLimits limits,
                                             TrackInsertProvider* inserts,
                                             LiveConvolutionAdmission convolution)
    : model_(model), limits_(limits), inserts_(inserts), convolution_(convolution),
      revisions_(model.snapshot()->revision, 0, ChangeDescriptor{"open project", {}}),
      source_snapshot_(model.snapshot()) {
  limits_.latency.master = 1;
}

std::optional<Error> RoutedPlaybackControl::processor_fault() const {
  for (const auto& stage : vst3_stages_) {
    if (stage.prepared.process_failed && stage.prepared.process_failed())
      return Error{
          ErrorCode::unavailable,
          "Active VST3 stage on " +
              (stage.track ? "track " + stage.track->to_string() : std::string("the master")) +
              " failed during processing; stop and restart playback after resolving "
              "the plugin"};
  }
  return std::nullopt;
}

std::optional<Error> RoutedPlaybackControl::processor_fault(project::TrackId track) const {
  for (const auto& stage : vst3_stages_) {
    if (stage.track == track && stage.prepared.process_failed && stage.prepared.process_failed())
      return Error{ErrorCode::unavailable,
                   "Active VST3 stage on track " + track.to_string() +
                       " failed during processing; stop and restart playback after resolving "
                       "the plugin"};
  }
  return std::nullopt;
}

const RoutedPlaybackControl::ActiveChain*
RoutedPlaybackControl::active_chain(std::optional<project::TrackId> track) const {
  const auto found = std::ranges::find(active_chains_, track, &ActiveChain::track);
  return found == active_chains_.end() ? nullptr : &*found;
}

RoutedPlaybackControl::ActiveChain*
RoutedPlaybackControl::active_chain(std::optional<project::TrackId> track) {
  const auto found = std::ranges::find(active_chains_, track, &ActiveChain::track);
  return found == active_chains_.end() ? nullptr : &*found;
}

track_chain::Stage* RoutedPlaybackControl::active_stage(std::optional<project::TrackId> track,
                                                        project::InsertSlotId insert) {
  auto* chain = active_chain(track);
  if (chain == nullptr)
    return nullptr;
  const auto found =
      std::ranges::find(chain->chain.stages, std::optional(insert),
                        [](const track_chain::Stage& stage) { return stage.saved.slot; });
  return found == chain->chain.stages.end() ? nullptr : &*found;
}

Result<void> RoutedPlaybackControl::fail(std::uint64_t revision, Error error) {
  (void)revisions_.fail_revision(revision, error);
  return Result<void>::failure(std::move(error));
}

namespace {

// A built-in effect's values edit offered to its running stage (ADR-0043):
// no rebuild, the kernel glides. Declines (false) when the stage is not the
// one prepared for this slot or the values would change its latency.
[[nodiscard]] Result<bool> offer_effect_values(engine::ProcessorRuntime* stage,
                                               const project::NativeEffectPatch& old_patch,
                                               const project::NativeEffectPatch& next,
                                               std::uint64_t revision,
                                               native_inserts::NativeEffectStage*& offered_to) {
  auto* runtime = dynamic_cast<native_inserts::NativeEffectStage*>(stage);
  if (!runtime || next.engine != old_patch.engine ||
      next.engine_version != old_patch.engine_version || next.generation <= old_patch.generation)
    return Result<bool>::success(false);
  auto resolved = native_inserts::resolve_native_effect(next);
  if (!resolved)
    return Result<bool>::failure(resolved.error());
  if (resolved.value().engine->descriptor != runtime->engine().descriptor)
    return Result<bool>::success(false);
  // `latency_frames` is fixed at creation; a latency change needs a rebuild.
  // The stage's own rate makes the comparison exact.
  if (resolved.value().engine->latency_frames(resolved.value().values, runtime->sample_rate()) !=
      runtime->latency_frames())
    return Result<bool>::success(false);
  if (next.document != old_patch.document) {
    if (auto offered = runtime->offer_document(next.document, revision); !offered)
      return Result<bool>::failure(offered.error());
  }
  if (!runtime->offer_values(resolved.value().values, revision))
    return Result<bool>::failure(
        {ErrorCode::conflict, "Live effect parameter mailbox is full; retry the saved edit"});
  offered_to = runtime;
  return Result<bool>::success(true);
}

} // namespace

Result<bool>
RoutedPlaybackControl::try_queue_live_native(std::shared_ptr<const project::Snapshot> snapshot) {
  // A newer value may replace an unacknowledged offer only on the same
  // processor instance. The active graph still owns that instance and all
  // downstream state; a second graph publication is neither needed nor safe.
  const bool superseding =
      pending_native_revision_ && pending_native_revision_->track &&
      pending_native_revision_->group.empty() &&
      std::holds_alternative<native_inserts::NativeEffectStage*>(pending_native_revision_->runtime);
  const auto* previous =
      superseding ? pending_native_revision_->snapshot.get() : published_snapshot_.get();
  if (!previous || !published_snapshot_ || publisher_.active_revision() == 0 ||
      publisher_.has_pending() || publisher_.held_boundary() ||
      (pending_native_revision_ && !superseding) ||
      (!superseding && revisions_.snapshot().active_graph_revision != previous->revision))
    return Result<bool>::success(false);
  if (previous->id != snapshot->id || previous->sample_rate != snapshot->sample_rate ||
      previous->sources != snapshot->sources ||
      previous->master_effects != snapshot->master_effects ||
      previous->source_tempo_reviews != snapshot->source_tempo_reviews ||
      previous->source_key_reviews != snapshot->source_key_reviews ||
      previous->next_insert_slot_serial != snapshot->next_insert_slot_serial ||
      previous->master_level != snapshot->master_level ||
      previous->tracks.size() != snapshot->tracks.size() || !same_timing(*previous, *snapshot))
    return Result<bool>::success(false);
  if (published_snapshot_->tracks.size() != previous->tracks.size())
    return Result<bool>::success(false);

  const project::Track* changed = nullptr;
  const project::Track* old_track = nullptr;
  for (std::size_t index = 0; index < snapshot->tracks.size(); ++index) {
    const auto& old = previous->tracks[index];
    const auto& next = snapshot->tracks[index];
    if (!same_track_outside_effects(old, next))
      return Result<bool>::success(false);
    if (old.effects != next.effects) {
      if (changed)
        return Result<bool>::success(false);
      old_track = &old;
      changed = &next;
    }
  }
  if (!changed ||
      (changed->kind != project::TrackKind::audio &&
       changed->kind != project::TrackKind::instrument &&
       changed->kind != project::TrackKind::group_bus &&
       changed->kind != project::TrackKind::return_bus) ||
      old_track->effects.empty() || old_track->effects.size() != changed->effects.size())
    return Result<bool>::success(false);
  const auto changed_slot = std::ranges::mismatch(old_track->effects, changed->effects);
  if (changed_slot.in1 == old_track->effects.end() ||
      !std::ranges::equal(std::next(changed_slot.in1), old_track->effects.end(),
                          std::next(changed_slot.in2), changed->effects.end()) ||
      changed_slot.in1->id != changed_slot.in2->id || !changed_slot.in1->enabled ||
      !changed_slot.in2->enabled)
    return Result<bool>::success(false);
  // A value offer never touches the detector wiring, so a built-in effect on
  // a sidechained chain still edits live.
  auto* running = active_stage(changed->id, changed_slot.in1->id);
  if (!running ||
      !(running->saved ==
        track_chain::SavedStage{changed_slot.in1->id, changed_slot.in1->processor}) ||
      (superseding && (pending_native_revision_->track != changed->id ||
                       pending_native_revision_->effects != old_track->effects)))
    return Result<bool>::success(false);
  try {
    PendingNativeRevision pending{{}, changed->id, changed->effects, std::move(snapshot), {}};
    if (const auto* old_patch =
            std::get_if<project::NativeEffectPatch>(&changed_slot.in1->processor)) {
      const auto* next = std::get_if<project::NativeEffectPatch>(&changed_slot.in2->processor);
      if (!next)
        return Result<bool>::success(false);
      auto* stage = running->runtime.get();
      if (superseding &&
          pending_native_revision_->runtime !=
              decltype(pending.runtime){dynamic_cast<native_inserts::NativeEffectStage*>(stage)})
        return Result<bool>::success(false);
      native_inserts::NativeEffectStage* offered = nullptr;
      auto queued =
          offer_effect_values(stage, *old_patch, *next, pending.snapshot->revision, offered);
      if (!queued || !queued.value())
        return queued;
      pending.runtime = offered;
    } else {
      return Result<bool>::success(false);
    }
    if (superseding) {
      pending_native_revision_->effects = std::move(pending.effects);
      pending_native_revision_->snapshot = std::move(pending.snapshot);
    } else {
      pending_native_revision_.emplace(std::move(pending));
    }
    return Result<bool>::success(true);
  } catch (const std::bad_alloc&) {
    return Result<bool>::failure(
        {ErrorCode::resource_exhausted, "Live native parameter preparation ran out of memory"});
  }
}

Result<bool> RoutedPlaybackControl::try_queue_live_effect_group(
    std::shared_ptr<const project::Snapshot> snapshot) {
  // A linked group's shape edit changes built-in effects on several chains
  // at once. Each running stage takes the offer through its own mailbox;
  // the revision is active once every one has applied it. A newer group
  // edit to the same stages supersedes an unacknowledged one.
  const bool superseding = pending_native_revision_ && !pending_native_revision_->group.empty();
  const auto* previous =
      superseding ? pending_native_revision_->snapshot.get() : published_snapshot_.get();
  if (!previous || !published_snapshot_ || publisher_.active_revision() == 0 ||
      publisher_.has_pending() || publisher_.held_boundary() ||
      (pending_native_revision_ && !superseding) ||
      (!superseding && revisions_.snapshot().active_graph_revision != previous->revision))
    return Result<bool>::success(false);
  if (previous->id != snapshot->id || previous->sample_rate != snapshot->sample_rate ||
      previous->sources != snapshot->sources ||
      previous->master_effects != snapshot->master_effects ||
      previous->source_tempo_reviews != snapshot->source_tempo_reviews ||
      previous->source_key_reviews != snapshot->source_key_reviews ||
      previous->next_insert_slot_serial != snapshot->next_insert_slot_serial ||
      previous->master_level != snapshot->master_level ||
      previous->tracks.size() != snapshot->tracks.size() || !same_timing(*previous, *snapshot))
    return Result<bool>::success(false);

  struct Change {
    const project::Track* track;
    const project::NativeEffectPatch* old_patch;
    const project::NativeEffectPatch* next;
    project::InsertSlotId slot;
  };
  std::vector<Change> changes;
  for (std::size_t index = 0; index < snapshot->tracks.size(); ++index) {
    const auto& old = previous->tracks[index];
    const auto& next = snapshot->tracks[index];
    if (!same_track_outside_effects(old, next))
      return Result<bool>::success(false);
    if (old.effects == next.effects)
      continue;
    if (old.effects.size() != next.effects.size())
      return Result<bool>::success(false);
    for (std::size_t e = 0; e < old.effects.size(); ++e) {
      const auto& before = old.effects[e];
      const auto& after = next.effects[e];
      if (before == after)
        continue;
      const auto* old_patch = std::get_if<project::NativeEffectPatch>(&before.processor);
      const auto* next_patch = std::get_if<project::NativeEffectPatch>(&after.processor);
      if (!old_patch || !next_patch || before.id != after.id || !before.enabled || !after.enabled)
        return Result<bool>::success(false);
      changes.push_back({&next, old_patch, next_patch, after.id});
    }
  }
  if (changes.size() < 2)
    return Result<bool>::success(false);
  if (superseding && (pending_native_revision_->group.size() != changes.size() ||
                      !std::ranges::equal(changes, pending_native_revision_->group,
                                          [](const Change& change, const PendingGroupStage& stage) {
                                            return change.track->id == stage.track &&
                                                   change.slot == stage.slot;
                                          })))
    return Result<bool>::success(false);
  try {
    std::vector<PendingGroupStage> group;
    group.reserve(changes.size());
    for (std::size_t i = 0; i < changes.size(); ++i) {
      const auto& change = changes[i];
      auto* running = active_stage(change.track->id, change.slot);
      // A superseded offer has not landed, so its stage still holds the
      // published settings: it must be the same instance.
      if (!running ||
          (!superseding &&
           !(running->saved ==
             track_chain::SavedStage{change.slot, project::EffectProcessor{*change.old_patch}})))
        return Result<bool>::success(false);
      auto* stage = dynamic_cast<native_inserts::NativeEffectStage*>(running->runtime.get());
      if (!stage || stage->slot() != change.slot ||
          (superseding && pending_native_revision_->group[i].stage != stage))
        return Result<bool>::success(false);
      group.push_back({change.track->id, change.slot, stage});
    }
    // Every stage must accept before any is offered, so a refusal leaves
    // them all as they were and the edit rebuilds instead.
    for (std::size_t i = 0; i < changes.size(); ++i) {
      const auto& change = changes[i];
      const auto& member = group[i];
      auto resolved = native_inserts::resolve_native_effect(*change.next);
      if (!resolved)
        return Result<bool>::failure(resolved.error());
      if (change.next->engine != change.old_patch->engine ||
          change.next->engine_version != change.old_patch->engine_version ||
          change.next->generation <= change.old_patch->generation ||
          resolved.value().engine->descriptor != member.stage->engine().descriptor ||
          resolved.value().engine->latency_frames(resolved.value().values,
                                                  member.stage->sample_rate()) !=
              member.stage->latency_frames())
        return Result<bool>::success(false);
    }
    for (std::size_t i = 0; i < changes.size(); ++i) {
      const auto& change = changes[i];
      native_inserts::NativeEffectStage* offered = nullptr;
      auto queued = offer_effect_values(group[i].stage, *change.old_patch, *change.next,
                                        snapshot->revision, offered);
      if (!queued)
        return queued;
      if (!queued.value())
        return Result<bool>::failure(
            {ErrorCode::internal, "A linked effect stage declined an accepted edit"});
    }
    const auto first = changes.front().track;
    PendingNativeRevision pending{group.front().stage, first->id, first->effects,
                                  std::move(snapshot), std::move(group)};
    pending_native_revision_ = std::move(pending);
    return Result<bool>::success(true);
  } catch (const std::bad_alloc&) {
    return Result<bool>::failure(
        {ErrorCode::resource_exhausted, "Live linked effect preparation ran out of memory"});
  }
}

Result<bool> RoutedPlaybackControl::try_queue_live_master_native(
    std::shared_ptr<const project::Snapshot> snapshot) {
  // The same stateful master processor can consume a newer bounded mailbox
  // offer without rebuilding its chain or discarding downstream DSP history.
  const bool superseding =
      pending_native_revision_ && !pending_native_revision_->track &&
      std::holds_alternative<native_inserts::NativeEffectStage*>(pending_native_revision_->runtime);
  const auto* previous =
      superseding ? pending_native_revision_->snapshot.get() : published_snapshot_.get();
  if (!previous || !published_snapshot_ || publisher_.active_revision() == 0 ||
      publisher_.has_pending() || publisher_.held_boundary() ||
      (pending_native_revision_ && !superseding) ||
      (!superseding && revisions_.snapshot().active_graph_revision != previous->revision) ||
      (superseding && pending_native_revision_->effects != previous->master_effects))
    return Result<bool>::success(false);
  if (previous->id != snapshot->id || previous->sample_rate != snapshot->sample_rate ||
      previous->sources != snapshot->sources || previous->tracks != snapshot->tracks ||
      previous->source_tempo_reviews != snapshot->source_tempo_reviews ||
      previous->source_key_reviews != snapshot->source_key_reviews ||
      previous->next_insert_slot_serial != snapshot->next_insert_slot_serial ||
      previous->master_level != snapshot->master_level || !same_timing(*previous, *snapshot) ||
      previous->master_effects.empty() ||
      previous->master_effects.size() != snapshot->master_effects.size())
    return Result<bool>::success(false);
  const auto changed = std::ranges::mismatch(previous->master_effects, snapshot->master_effects);
  if (changed.in1 == previous->master_effects.end() ||
      !std::ranges::equal(std::next(changed.in1), previous->master_effects.end(),
                          std::next(changed.in2), snapshot->master_effects.end()) ||
      changed.in1->id != changed.in2->id || !changed.in1->enabled || !changed.in2->enabled)
    return Result<bool>::success(false);
  auto* running = active_stage(std::nullopt, changed.in1->id);
  if (!running ||
      !(running->saved == track_chain::SavedStage{changed.in1->id, changed.in1->processor}))
    return Result<bool>::success(false);
  try {
    PendingNativeRevision pending{
        {}, std::nullopt, snapshot->master_effects, std::move(snapshot), {}};
    if (const auto* old_patch = std::get_if<project::NativeEffectPatch>(&changed.in1->processor)) {
      const auto* next = std::get_if<project::NativeEffectPatch>(&changed.in2->processor);
      if (!next)
        return Result<bool>::success(false);
      auto* stage = running->runtime.get();
      if (superseding &&
          pending_native_revision_->runtime !=
              decltype(pending.runtime){dynamic_cast<native_inserts::NativeEffectStage*>(stage)})
        return Result<bool>::success(false);
      native_inserts::NativeEffectStage* offered = nullptr;
      auto queued =
          offer_effect_values(stage, *old_patch, *next, pending.snapshot->revision, offered);
      if (!queued || !queued.value())
        return queued;
      pending.runtime = offered;
    } else {
      return Result<bool>::success(false);
    }
    if (superseding) {
      pending_native_revision_->effects = std::move(pending.effects);
      pending_native_revision_->snapshot = std::move(pending.snapshot);
    } else {
      pending_native_revision_.emplace(std::move(pending));
    }
    return Result<bool>::success(true);
  } catch (const std::bad_alloc&) {
    return Result<bool>::failure(
        {ErrorCode::resource_exhausted, "Live master effect preparation ran out of memory"});
  }
}

Result<void> RoutedPlaybackControl::build_and_publish(
    std::shared_ptr<const project::Snapshot> prepared_snapshot) {
  OSCILLATE_TRACE_SPAN("edit.build_and_publish");
  const auto& snapshot = *prepared_snapshot;
  if (project::has_convolution_insert(snapshot)) {
#if OSCILLATE_HAVE_CONVOLUTION
    if (snapshot.master_effects.size() != 1 || !snapshot.master_effects.front().enabled ||
        !std::holds_alternative<project::NativeConvolution>(
            snapshot.master_effects.front().processor) ||
        std::ranges::any_of(snapshot.tracks, [](const auto& track) {
          return std::ranges::any_of(track.effects, [](const auto& effect) {
            return std::holds_alternative<project::NativeConvolution>(effect.processor);
          });
        }))
      return fail(snapshot.revision,
                  {ErrorCode::unsupported, "Live convolution admits one sole master insert"});
    if ((limits_.max_frames != 64 && limits_.max_frames != 128) ||
        convolution_.budget.measured_block_frames != limits_.max_frames)
      return fail(snapshot.revision,
                  {ErrorCode::resource_exhausted,
                   "Live convolution has no measurement at the prepared callback block size"});
    if (live_convolution_ && published_snapshot_ &&
        published_snapshot_->master_effects != snapshot.master_effects)
      return fail(snapshot.revision,
                  {ErrorCode::unsupported,
                   "Live convolution IR changes need a stopped, freshly measured graph"});
#else
    return fail(snapshot.revision,
                {ErrorCode::unsupported, "This build has no live convolution runtime"});
#endif
  } else if (live_convolution_) {
    return fail(snapshot.revision,
                {ErrorCode::unsupported,
                 "Removing live convolution needs a stopped, freshly measured graph"});
  }
  if (limits_.max_frames == 0 || limits_.max_frames > 8192) {
    return fail(snapshot.revision,
                {ErrorCode::invalid_argument, "Routed live block limit must be 1..8192"});
  }
  std::shared_ptr<native_dsp::ConvolutionRuntime> prepared_convolution;
#if OSCILLATE_HAVE_CONVOLUTION
  if (project::has_convolution_insert(snapshot) && !live_convolution_) {
    const auto& saved =
        std::get<project::NativeConvolution>(snapshot.master_effects.front().processor);
    auto admitted = prepare_live_convolution(snapshot, saved, convolution_);
    if (!admitted)
      return fail(snapshot.revision, admitted.error());
    prepared_convolution = std::move(admitted).value();
  }
#endif
  if (!same_audio_content(*source_snapshot_, snapshot) &&
      !(awaiting_source_replacement_ && published_snapshot_ &&
        (audio_clip_edit_only(*published_snapshot_, snapshot) ||
         native_synth_switch_only(*published_snapshot_, snapshot)))) {
    return fail(snapshot.revision,
                {ErrorCode::unsupported,
                 "Routed live source content changed; prepare and restart playback"});
  }
  auto automation = automation_project::prepare_project_automation(snapshot, snapshot.revision);
  if (!automation)
    return fail(snapshot.revision, automation.error());
  // Every native stage this revision prepares sees the project's tempo and
  // its slot's automation, bounded like the synths' (ADR-0043).
  auto automation_limit = automation_sample_limit(snapshot, limits_.max_frames);
  if (!automation_limit)
    return fail(snapshot.revision, automation_limit.error());
  native_inserts::StageContext stage_context{snapshot.sample_rate};
  stage_context.snapshot = &snapshot;
  stage_context.automation = automation.value();
  stage_context.max_frames = limits_.max_frames;
  stage_context.last_safe_automation_sample = automation_limit.value();
  std::vector<std::pair<project::Vst3ParameterTarget, std::uint64_t>> vst3_automation;
  for (const auto& item : automation.value()) {
    if (const auto* target = std::get_if<project::Vst3ParameterTarget>(&item.target()))
      vst3_automation.emplace_back(*target, item.semantics_key());
  }
  std::ranges::sort(vst3_automation, {}, [](const auto& item) {
    return std::pair{item.first.plugin.bits(), item.first.parameter_id};
  });
  if (publisher_.active_revision() != 0 && vst3_automation != active_vst3_automation_)
    return fail(snapshot.revision, {ErrorCode::unsupported,
                                    "Live VST3 automation changed; restart playback to rebind"});
  if (std::ranges::any_of(automation.value(), [&snapshot](const auto& item) {
        if (std::holds_alternative<project::SendGainTarget>(item.target()))
          return false;
        if (std::holds_alternative<project::Vst3ParameterTarget>(item.target()))
          return false;
        if (std::holds_alternative<project::NativeParameterTarget>(item.target()))
          return false; // Preparation verified the owning built-in synth.
        if (std::holds_alternative<project::NativeEffectParameterTarget>(item.target()))
          return false; // Preparation verified the owning built-in effect slot.
        const auto* control = std::get_if<project::TrackControlTarget>(&item.target());
        return control == nullptr || !project::is_channel_control(control->key);
      }))
    return fail(snapshot.revision, {ErrorCode::unsupported,
                                    "Routed live playback cannot apply this automation target"});
  // Every track and the master run one chain (ADR-0050). A chain whose saved
  // stages are unchanged keeps running as it is; a changed one is prepared
  // again, carrying over each unchanged running stage, and switches live.
  const bool playing = publisher_.active_revision() != 0;
  const auto unbounded_ring_out = unbounded_ring_out_seconds * snapshot.sample_rate;
  std::vector<ActiveChain> chains_next;
  std::vector<Vst3Stage> vst3_next;
  struct Switched {
    std::optional<project::TrackId> track;
    track_chain::ChainSwitch change;
  };
  std::vector<Switched> switches;
  const auto build_chain =
      [&](std::optional<project::TrackId> track, std::vector<track_chain::SavedStage> saved,
          std::optional<project::InsertSlotId> sidechain,
          const std::shared_ptr<native_dsp::ConvolutionRuntime>& convolution) -> Result<void> {
    const auto* prior = active_chain(track);
    if (prior && prior->sidechain_target == sidechain && runs(prior->chain, saved)) {
      chains_next.push_back(*prior);
      return Result<void>::success();
    }
    if (saved.empty() && !prior)
      return Result<void>::success();
    // A VST3 instrument is the track's source: replacing it needs a voice
    // handoff, which is not part of a chain switch.
    if (playing && track && prior) {
      const auto* old_source = prior->chain.stages.empty() || prior->chain.stages.front().saved.slot
                                   ? nullptr
                                   : &prior->chain.stages.front().saved;
      const auto* next_source = saved.empty() || saved.front().slot ? nullptr : &saved.front();
      if ((old_source || next_source) &&
          (!old_source || !next_source || !(*old_source == *next_source)))
        return Result<void>::failure(
            {ErrorCode::unsupported, "A VST3 instrument change on track " + track->to_string() +
                                         " is saved and plays from the next Play"});
    }
    track_chain::ChainContext context{stage_context};
    context.detector_slot = sidechain;
#if OSCILLATE_HAVE_CONVOLUTION
    if (convolution) {
      context.native.convolution =
          [&](const project::NativeConvolution& saved_ir) -> Result<native_inserts::PreparedStage> {
        native_inserts::PreparedStage stage;
        stage.kind = native_inserts::StageKind::convolution;
        stage.generation = saved_ir.generation;
        stage.export_tail_frames = saved_ir.wet == 0.0 ? 0 : convolution->tail_frames();
        stage.transition_tail_frames =
            saved_ir.wet == 0.0 ? 1 : std::max<std::uint64_t>(1, convolution->tail_frames());
        stage.runtime = convolution;
        return Result<native_inserts::PreparedStage>::success(std::move(stage));
      };
    }
#else
    (void)convolution;
#endif
    if (inserts_) {
      const auto owner = track ? "track " + track->to_string() : std::string("the master");
      context.vst3 = [&,
                      owner](const track_chain::SavedStage& stage) -> Result<track_chain::Stage> {
        const auto& plugin = std::get<project::Vst3PluginInstance>(stage.processor);
        const auto host_frames =
            std::min(static_cast<std::uint32_t>(limits_.max_frames), vst3_host_block_frames);
        auto prepared = inserts_->prepare(snapshot, plugin, host_frames);
        if (!prepared)
          return Result<track_chain::Stage>::failure(
              {prepared.error().code, "VST3 stage on " + owner + ": " + prepared.error().message});
        if (!prepared.value().processor || !prepared.value().process_failed)
          return Result<track_chain::Stage>::failure(
              {ErrorCode::corrupt_data,
               "VST3 provider returned an incomplete processor for " + owner});
        if (prepared.value().instrument_source != !stage.slot)
          return Result<track_chain::Stage>::failure(
              {ErrorCode::unsupported,
               "Verified VST3 processor kind does not match the saved stage on " + owner});
        if (limits_.max_frames > host_frames) {
          auto chunked =
              engine::ChunkedProcessorRuntime::prepare(prepared.value().processor, host_frames);
          if (!chunked)
            return Result<track_chain::Stage>::failure(chunked.error());
          prepared.value().processor = std::move(chunked).value();
        }
        auto result = vst3_stage(stage, prepared.value());
        vst3_next.push_back({track, stage.slot, plugin, std::move(prepared).value()});
        return Result<track_chain::Stage>::success(std::move(result));
      };
    }
    auto prepared = track_chain::prepare_chain(saved, context, prior ? &prior->chain : nullptr);
    if (!prepared)
      return Result<void>::failure(prepared.error());
    // A held switch keeps exact time, so it cannot carry a latency change.
    const auto prior_latency = prior ? prior->chain.latency_frames() : 0;
    if (playing && !publish_live_ && prior_latency != prepared.value().latency_frames())
      return Result<void>::failure({ErrorCode::unsupported,
                                    "This edit changes a chain's latency while its graph is held; "
                                    "it is saved and plays from the next Play"});
    if (prior && prior->chain.runtime) {
      switches.push_back(
          {track, track_chain::switch_chain(prior->chain, prepared.value(), unbounded_ring_out)});
    }
    if (prepared.value().runtime)
      chains_next.push_back({track, sidechain, std::move(prepared).value()});
    return Result<void>::success();
  };
  try {
    for (const auto& track : snapshot.tracks) {
      if (track.kind != project::TrackKind::instrument &&
          !std::holds_alternative<std::monostate>(track.instrument_source))
        return fail(snapshot.revision,
                    {ErrorCode::unsupported, "Routed effect path has an instrument source"});
      auto built = build_chain(track.id, track_chain::saved_chain(track),
                               routing::sidechain_target(snapshot, track.id), nullptr);
      if (!built)
        return fail(snapshot.revision, built.error());
    }
    auto built =
        build_chain(std::nullopt, track_chain::saved_chain(snapshot.master_effects), std::nullopt,
                    prepared_convolution ? prepared_convolution : live_convolution_);
    if (!built)
      return fail(snapshot.revision, built.error());
    // Every running VST3 stage keeps its host record; new ones were recorded
    // as they were prepared.
    for (const auto& chain : chains_next) {
      for (const auto& stage : chain.chain.stages) {
        if (!std::holds_alternative<project::Vst3PluginInstance>(stage.saved.processor) ||
            std::ranges::any_of(vst3_next, [&](const Vst3Stage& record) {
              return record.prepared.processor == stage.runtime;
            }))
          continue;
        const auto kept = std::ranges::find_if(vst3_stages_, [&](const Vst3Stage& record) {
          return record.prepared.processor == stage.runtime;
        });
        if (kept == vst3_stages_.end())
          return fail(snapshot.revision,
                      {ErrorCode::internal, "Running VST3 stage lost its host record"});
        if (kept->prepared.process_failed && kept->prepared.process_failed())
          return fail(snapshot.revision, {ErrorCode::unavailable,
                                          "An active VST3 stage has failed during processing"});
        vst3_next.push_back(*kept);
      }
    }
    std::erase_if(vst3_next, [&](const Vst3Stage& record) {
      return std::ranges::none_of(chains_next, [&](const ActiveChain& chain) {
        return std::ranges::any_of(chain.chain.stages, [&](const track_chain::Stage& stage) {
          return stage.runtime == record.prepared.processor;
        });
      });
    });
  } catch (const std::bad_alloc&) {
    return fail(snapshot.revision,
                {ErrorCode::resource_exhausted, "Chain preparation ran out of memory"});
  }
  const auto chain_of = [&](std::optional<project::TrackId> track) -> const ActiveChain* {
    const auto found = std::ranges::find(chains_next, track, &ActiveChain::track);
    return found == chains_next.end() ? nullptr : &*found;
  };
  const auto switch_of = [&](std::optional<project::TrackId> track) -> const Switched* {
    const auto found = std::ranges::find(switches, track, &Switched::track);
    return found == switches.end() ? nullptr : &*found;
  };
  std::vector<routing::BoundStage> receipts;
  for (const auto& track : snapshot.tracks) {
    if (const auto* chain = chain_of(track.id)) {
      auto stages = track_chain::receipts(track.id, chain->chain);
      receipts.insert(receipts.end(), stages.begin(), stages.end());
    }
  }
  auto route = registry_.compile(snapshot, limits_.latency.max_compensation_frames, receipts,
                                 static_cast<bool>(live_convolution_) ||
                                     static_cast<bool>(prepared_convolution));
  if (!route) {
    return fail(snapshot.revision, route.error());
  }
  if (!topological_order_.empty() &&
      (topological_order_ != route.value().latency.topological_order ||
       !same_sources(sources_, route.value().sources))) {
    return fail(snapshot.revision,
                {ErrorCode::conflict,
                 "Routed live source layout changed; restart playback with prepared sources"});
  }
  const auto* master_chain = chain_of(std::nullopt);
  const auto master_latency = master_chain ? master_chain->chain.latency_frames() : 0;
  // The graph's nodes as export builds them, with the master chain on the
  // master node and each switched chain's predecessor retiring beside it.
  auto graph = routing::graph_nodes(
      snapshot, route.value(),
      {.track_processor = [&](project::TrackId track) -> std::shared_ptr<engine::ProcessorRuntime> {
         const auto* chain = chain_of(track);
         return chain ? chain->chain.runtime : nullptr;
       },
       .master_at_node = true,
       .master_processor = master_chain ? master_chain->chain.runtime : nullptr,
       .master_latency_frames = master_latency});
  if (!graph)
    return fail(snapshot.revision, graph.error());
  auto& nodes = graph.value().nodes;
  for (std::size_t index = 0; index < nodes.size(); ++index) {
    if (const auto* switched = switch_of(graph.value().owners[index])) {
      nodes[index].replacement = switched->change.drain;
      nodes[index].state_handoff_prior = switched->change.handoff;
    }
  }
  auto gains = routing::PreparedRouteGainBlocks::prepare(snapshot, route.value(),
                                                         automation.value(), limits_.max_frames);
  if (!gains)
    return fail(snapshot.revision, gains.error());
  if (route.value().latency.master_output_latency_frames >
      std::numeric_limits<std::uint32_t>::max() - master_latency)
    return fail(snapshot.revision,
                {ErrorCode::resource_exhausted, "Routed output latency exceeds 32-bit range"});
  auto published =
      publish_live_
          ? publisher_.prepare_and_publish_live(snapshot.revision, nodes, route.value().edges,
                                                limits_, std::move(gains).value())
          : publisher_.prepare_and_publish(snapshot.revision, nodes, route.value().edges, limits_,
                                           std::move(gains).value());
  if (!published)
    return fail(snapshot.revision, published.error());
  if (publish_live_) {
    live_candidate_ =
        LiveCandidate{.revision = snapshot.revision,
                      .passed_at_publication = publisher_.live_candidate_passed_blocks(),
                      .published_snapshot = published_snapshot_,
                      .route_plan = published_route_plan_,
                      .convolution = live_convolution_,
                      .chains = active_chains_,
                      .vst3_stages = vst3_stages_,
                      .vst3_automation = active_vst3_automation_,
                      .prepared_limiter = prepared_limiter_,
                      .master_output_latency_frames = master_output_latency_frames_};
  }
  const auto route_master_output_latency = route.value().latency.master_output_latency_frames;
  if (topological_order_.empty()) {
    sources_ = std::move(route.value().sources);
    topological_order_ = route.value().latency.topological_order;
    initial_route_revision_ = snapshot.revision;
    initial_route_plan_ = route.value();
  }
  published_route_plan_ = std::move(route.value());
  published_snapshot_ = std::move(prepared_snapshot);
  if (prepared_convolution)
    live_convolution_ = std::move(prepared_convolution);
  active_chains_ = std::move(chains_next);
  follow_published_notes();
  vst3_stages_ = std::move(vst3_next);
  active_vst3_automation_ = std::move(vst3_automation);
  // A lookahead stage (a limiter such as Ceiling) holds audio Stop must drain.
  prepared_limiter_ = std::ranges::any_of(
      active_chains_, [](const ActiveChain& chain) { return chain.chain.latency_frames() != 0; });
  master_output_latency_frames_ =
      master_latency + static_cast<std::uint32_t>(route_master_output_latency);
  return Result<void>::success();
}

void RoutedPlaybackControl::follow_published_notes() {
  refresh_effect_key_notes();
  refresh_synth_locks();
}

void RoutedPlaybackControl::refresh_effect_key_notes() {
  // Effects that read their keying tracks' notes ahead (Pump's MIDI
  // trigger, ADR-0058) take the new notes on a kept stage, no rebuild. A
  // failure keeps the notes it had.
  for (auto& chain : active_chains_) {
    for (auto& stage : chain.chain.stages) {
      if (auto* effect = dynamic_cast<native_inserts::NativeEffectStage*>(stage.runtime.get()))
        (void)effect->refresh_key_notes(*published_snapshot_);
    }
  }
}

void RoutedPlaybackControl::settle_live_candidate() {
  if (!live_candidate_)
    return;
  // Pending first: the callback clears it only after storing an adopted
  // candidate as active, so "not pending and not active" means withdrawn.
  const bool pending = publisher_.has_pending();
  if (publisher_.active_revision() >= live_candidate_->revision) {
    live_candidate_.reset(); // Adopted.
    return;
  }
  if (pending) {
    if (!live_candidate_->withdrawing &&
        publisher_.live_candidate_passed_blocks() - live_candidate_->passed_at_publication >=
            live_candidate_passed_block_limit)
      live_candidate_->withdrawing = publisher_.withdraw_pending();
    return;
  }
  // Withdrawn, or discarded by the callback as prepared against another
  // graph: the old graph played on, so the control state it replaced returns.
  auto prior = std::move(*live_candidate_);
  live_candidate_.reset();
  published_snapshot_ = std::move(prior.published_snapshot);
  published_route_plan_ = std::move(prior.route_plan);
  live_convolution_ = std::move(prior.convolution);
  active_chains_ = std::move(prior.chains);
  vst3_stages_ = std::move(prior.vst3_stages);
  active_vst3_automation_ = std::move(prior.vst3_automation);
  prepared_limiter_ = prior.prepared_limiter;
  master_output_latency_frames_ = prior.master_output_latency_frames;
  if (published_snapshot_)
    follow_published_notes();
  (void)revisions_.fail_revision(
      prior.revision, {ErrorCode::unavailable,
                       "The edit's graph could not take over playback; the previous graph plays "
                       "on. The edit is saved: edit again or retry to apply it"});
}

void RoutedPlaybackControl::refresh_synth_locks() {
  if (live_source_ != nullptr && published_snapshot_) {
    live_source_->follow_project_key(published_snapshot_->key);
    live_source_->refresh_synth_locks(*published_snapshot_);
  }
}

Result<void> RoutedPlaybackControl::submit_current() {
  if (publisher_.active_revision() != 0 || publisher_.has_pending() || awaiting_) {
    return rejected(ErrorCode::conflict, "Routed playback is already starting or active");
  }
  const auto current = model_.snapshot();
  if (current->revision > revisions_.snapshot().model_revision) {
    (void)revisions_.accept_model_revision(current->revision,
                                           ChangeDescriptor{"project refresh", {}});
  }
  return build_and_publish(current);
}

Result<PreparedRouteTailReceipt>
RoutedPlaybackControl::prepared_route_tail_receipt(std::uint64_t expected_revision,
                                                   std::optional<std::uint64_t> export_end) const {
  using Receipt = Result<PreparedRouteTailReceipt>;
  const auto current = model_.snapshot();
  const auto active = publisher_.active_revision();
  const bool initial_pending = active == 0 && publisher_.has_pending();
  if (!current || !published_snapshot_ || !initial_route_plan_ || expected_revision == 0 ||
      current->revision != expected_revision ||
      published_snapshot_->revision != expected_revision ||
      initial_route_revision_ != expected_revision ||
      (active != expected_revision && !initial_pending) ||
      (active != 0 && publisher_.has_pending()) || awaiting_ || preparing_source_ ||
      pending_native_revision_ || pending_manual_ || publisher_.held_boundary())
    return Receipt::failure({ErrorCode::conflict, "Prepared route tail revision is not stable"});
  if (!active_vst3_automation_.empty())
    return Receipt::failure(
        {ErrorCode::unsupported, "Real-time VST3 export cannot qualify automated plugin tails"});

  PreparedRouteTailReceipt receipt{
      initial_route_plan_->latency, initial_route_plan_->master, expected_revision, {}};
  receipt.nodes.reserve(initial_route_plan_->nodes.size());
  for (const auto& node : initial_route_plan_->nodes) {
    std::optional<std::uint64_t> total = 0;
    std::optional<project::TrackId> track_id;
    std::vector<track_chain::SavedStage> saved;
    if (node.id == receipt.master) {
      saved = track_chain::saved_chain(published_snapshot_->master_effects);
    } else {
      const auto owner = std::ranges::find(initial_route_plan_->track_nodes, node.id,
                                           &routing::ProjectRouteTrackNode::node);
      if (owner == initial_route_plan_->track_nodes.end())
        return Receipt::failure({ErrorCode::corrupt_data, "Prepared tail node has no track"});
      const auto track =
          std::ranges::find(published_snapshot_->tracks, owner->track, &project::Track::id);
      if (track == published_snapshot_->tracks.end())
        return Receipt::failure({ErrorCode::corrupt_data, "Prepared tail track is missing"});
      track_id = owner->track;
      saved = track_chain::saved_chain(*track);
      if (std::holds_alternative<project::Vst3PluginInstance>(track->instrument_source)) {
        // The instrument's release follows its exact MIDI cut.
        const auto source = std::ranges::find_if(vst3_stages_, [&](const Vst3Stage& stage) {
          return stage.track == owner->track && !stage.slot;
        });
        if (!export_end || *export_end == 0 || source == vst3_stages_.end() ||
            !source->prepared.prepared_export_end ||
            source->prepared.prepared_export_end() != export_end)
          return Receipt::failure(
              {ErrorCode::unsupported, "Prepared VST3 instrument release cut is unqualified"});
      }
      if (track->kind == project::TrackKind::instrument && !track->midi_clips.empty()) {
        if (!std::holds_alternative<project::NativeInstrumentPatch>(track->instrument_source) &&
            !std::holds_alternative<project::Vst3PluginInstance>(track->instrument_source))
          return Receipt::failure(
              {ErrorCode::unsupported, "Instrument note releases have no real-time export bound"});
      }
      if (std::holds_alternative<project::NativeInstrumentPatch>(track->instrument_source)) {
        const auto source_tail =
            live_source_ && export_end && *export_end > 0
                ? live_source_->prepared_synth_export_tail(owner->track, *export_end)
                : std::nullopt;
        if (!source_tail)
          return Receipt::failure(
              {ErrorCode::unsupported, "Prepared native synth release cut is unavailable"});
        total = sum_export_tail(total, source_tail);
      }
    }
    // The chain's stages ring out in series: a VST3 stage counts only with an
    // independently qualified fixed tail.
    const auto* chain = active_chain(track_id);
    if (chain ? !runs(chain->chain, saved) : !saved.empty())
      return Receipt::failure({ErrorCode::conflict, "Prepared chain tail has changed"});
    if (chain) {
      if (std::ranges::any_of(vst3_stages_, [&](const Vst3Stage& stage) {
            return stage.track == track_id && stage.prepared.process_failed &&
                   stage.prepared.process_failed();
          }))
        return Receipt::failure({ErrorCode::unavailable, "A prepared VST3 stage has failed"});
      total = sum_export_tail(total, chain->chain.export_tail_frames());
    }
    if (!total)
      return Receipt::failure({ErrorCode::unsupported, "Prepared route tail is unbounded"});
    receipt.nodes.push_back({node.id, total});
  }
  return Receipt::success(std::move(receipt));
}

void RoutedPlaybackControl::bind_live_source(RoutedAudioSource& source) noexcept {
  live_source_ = &source;
}

void RoutedPlaybackControl::unbind_live_source() noexcept {
  // Audio is quiescent. Do not let a later control-side status poll dereference
  // the retired synth runtime while its final mailbox offer is pending.
  if (pending_native_revision_ && (std::holds_alternative<native_dsp::synth::NativeSynthRuntime*>(
                                       pending_native_revision_->runtime) ||
                                   std::holds_alternative<native_dsp::synth::NativeSynthPlayer*>(
                                       pending_native_revision_->runtime))) {
    pending_native_revision_.reset();
  }
  retry_live_after_ack_ = false;
  live_source_ = nullptr;
}

Result<void> RoutedPlaybackControl::reset_convolution_after_quiesce(std::uint64_t generation) {
  if (generation == 0 || graph_handoff_in_progress())
    return rejected(ErrorCode::conflict,
                    "Convolution reset requires a quiescent graph and nonzero generation");
#if OSCILLATE_HAVE_CONVOLUTION
  if (live_convolution_ && !live_convolution_->reset_after_quiesce(generation))
    return rejected(ErrorCode::conflict,
                    "Convolution reset generation is stale; current IR history was preserved");
#endif
  return Result<void>::success();
}

Result<void>
RoutedPlaybackControl::seek_convolution_after_quiesce(audio_io::TransportCursor& cursor,
                                                      std::uint64_t first_sample) {
  if (graph_handoff_in_progress())
    return rejected(ErrorCode::conflict,
                    "Convolution seek requires a quiescent graph after the device callback joins");
  const auto position = cursor.seek_after_quiesce(first_sample);
  if (!position)
    return rejected(ErrorCode::resource_exhausted, "Transport cannot seek to that sample");
  return reset_convolution_after_quiesce(position->generation);
}

// A built-in synth edit that keeps the engine and every other project field
// is offered to the running synth: continuous values glide, next-note values
// apply to new notes, and sounding voices are never reset.
Result<bool>
RoutedPlaybackControl::try_queue_live_synth(std::shared_ptr<const project::Snapshot> snapshot) {
  const bool superseding =
      pending_native_revision_ && std::holds_alternative<native_dsp::synth::NativeSynthRuntime*>(
                                      pending_native_revision_->runtime);
  const auto* previous =
      superseding ? pending_native_revision_->snapshot.get() : published_snapshot_.get();
  if (!previous || !live_source_ || publisher_.active_revision() == 0 || publisher_.has_pending() ||
      publisher_.held_boundary() || (pending_native_revision_ && !superseding) ||
      (!superseding && revisions_.snapshot().active_graph_revision != previous->revision) ||
      previous->id != snapshot->id || previous->sample_rate != snapshot->sample_rate ||
      previous->sources != snapshot->sources ||
      previous->master_effects != snapshot->master_effects ||
      previous->master_level != snapshot->master_level ||
      previous->source_tempo_reviews != snapshot->source_tempo_reviews ||
      previous->source_key_reviews != snapshot->source_key_reviews ||
      previous->next_insert_slot_serial != snapshot->next_insert_slot_serial ||
      previous->tracks.size() != snapshot->tracks.size() || !same_timing(*previous, *snapshot))
    return Result<bool>::success(false);
  const project::Track* changed = nullptr;
  const project::NativeInstrumentPatch* patch = nullptr;
  for (std::size_t index = 0; index < previous->tracks.size(); ++index) {
    const auto& old_track = previous->tracks[index];
    const auto& next_track = snapshot->tracks[index];
    if (old_track == next_track)
      continue;
    if (changed)
      return Result<bool>::success(false);
    const auto* old_patch =
        std::get_if<project::NativeInstrumentPatch>(&old_track.instrument_source);
    const auto* next_patch =
        std::get_if<project::NativeInstrumentPatch>(&next_track.instrument_source);
    if (!old_patch || !next_patch || old_patch->engine != next_patch->engine ||
        old_patch->engine_version != next_patch->engine_version ||
        old_track.kind != project::TrackKind::instrument)
      return Result<bool>::success(false);
    auto masked = old_track;
    masked.instrument_source = next_track.instrument_source;
    if (masked != next_track)
      return Result<bool>::success(false);
    changed = &next_track;
    patch = next_patch;
  }
  if (!changed || !patch)
    return Result<bool>::success(false);
  auto* runtime = live_source_->synth_runtime(changed->id);
  if (!runtime)
    return Result<bool>::failure(
        {ErrorCode::unavailable, "Live synth is unavailable; saved edit is not active"});
  if (superseding && (pending_native_revision_->track != changed->id ||
                      std::get<native_dsp::synth::NativeSynthRuntime*>(
                          pending_native_revision_->runtime) != runtime))
    return Result<bool>::success(false);
  auto resolved = native_dsp::synth::resolve_native_instrument(*patch);
  if (!resolved)
    return Result<bool>::failure(resolved.error());
  if (resolved.value().engine->descriptor != runtime->engine().descriptor)
    return Result<bool>::success(false);
  // A document goes ahead of its revision's values (ADR-0057). New managed
  // media need decoding, so those edits rebuild the source instead.
  if (resolved.value().document != runtime->document()) {
    auto offered = runtime->offer_document(resolved.value().document, snapshot->revision);
    if (!offered)
      return Result<bool>::failure(offered.error());
    if (!offered.value())
      return Result<bool>::success(false);
  }
  if (!runtime->offer_values(resolved.value().values, snapshot->revision)) {
    // A drag may end on this value; retry after the callback drains an offer.
    retry_live_after_ack_ = true;
    return Result<bool>::failure(
        {ErrorCode::conflict, "Live synth mailbox is full; saved edit is not active"});
  }
  retry_live_after_ack_ = false;
  if (superseding) {
    pending_native_revision_->snapshot = std::move(snapshot);
  } else {
    PendingNativeRevision pending{{}, changed->id, {}, std::move(snapshot), {}};
    pending.runtime = runtime;
    pending_native_revision_.emplace(std::move(pending));
  }
  return Result<bool>::success(true);
}

// A note effect edit (values or a switch, and a macro turned with them)
// that keeps the chain's slots goes to the running player inside the
// revision's automation plan: no rebuild and no gap (ADR-0059).
Result<bool> RoutedPlaybackControl::try_queue_live_note_effects(
    std::shared_ptr<const project::Snapshot> snapshot) {
  const bool superseding =
      pending_native_revision_ && std::holds_alternative<native_dsp::synth::NativeSynthPlayer*>(
                                      pending_native_revision_->runtime);
  const auto* previous =
      superseding ? pending_native_revision_->snapshot.get() : published_snapshot_.get();
  if (!previous || !live_source_ || publisher_.active_revision() == 0 || publisher_.has_pending() ||
      publisher_.held_boundary() || (pending_native_revision_ && !superseding) ||
      (!superseding && revisions_.snapshot().active_graph_revision != previous->revision) ||
      previous->id != snapshot->id || previous->sample_rate != snapshot->sample_rate ||
      previous->sources != snapshot->sources ||
      previous->master_effects != snapshot->master_effects ||
      previous->master_level != snapshot->master_level ||
      previous->source_tempo_reviews != snapshot->source_tempo_reviews ||
      previous->source_key_reviews != snapshot->source_key_reviews ||
      previous->next_insert_slot_serial != snapshot->next_insert_slot_serial ||
      previous->tracks.size() != snapshot->tracks.size() || !same_timing(*previous, *snapshot))
    return Result<bool>::success(false);
  const project::Track* changed = nullptr;
  for (std::size_t index = 0; index < previous->tracks.size(); ++index) {
    const auto& old_track = previous->tracks[index];
    const auto& next_track = snapshot->tracks[index];
    if (old_track == next_track)
      continue;
    if (changed || old_track.kind != project::TrackKind::instrument ||
        old_track.note_effects == next_track.note_effects ||
        !std::holds_alternative<project::NativeInstrumentPatch>(old_track.instrument_source) ||
        old_track.macros.size() != next_track.macros.size())
      return Result<bool>::success(false);
    // A macro's value and name are heard only through the parameters it
    // sets, which are in this edit; its mappings shape its lanes.
    for (std::size_t macro = 0; macro < old_track.macros.size(); ++macro) {
      if (old_track.macros[macro].mappings != next_track.macros[macro].mappings)
        return Result<bool>::success(false);
    }
    auto masked = old_track;
    masked.note_effects = next_track.note_effects;
    masked.macros = next_track.macros;
    if (masked != next_track)
      return Result<bool>::success(false);
    changed = &next_track;
  }
  if (!changed)
    return Result<bool>::success(false);
  auto* player = live_source_->synth_player(changed->id);
  // A new chain layout (an effect added, moved or removed) rebuilds the source.
  if (!player || !player->same_note_effects(changed->note_effects))
    return Result<bool>::success(false);
  if (superseding && (pending_native_revision_->track != changed->id ||
                      std::get<native_dsp::synth::NativeSynthPlayer*>(
                          pending_native_revision_->runtime) != player))
    return Result<bool>::success(false);
  auto prepared = automation_project::prepare_project_automation(*snapshot, snapshot->revision);
  if (!prepared)
    return Result<bool>::failure(prepared.error());
  auto limit = automation_sample_limit(*snapshot, limits_.max_frames);
  if (!limit)
    return Result<bool>::failure(limit.error());
  auto offered = player->offer_automation(*snapshot, *changed, prepared.value(), limit.value());
  if (!offered) {
    if (offered.error().code == ErrorCode::resource_exhausted)
      retry_live_after_ack_ = true;
    return Result<bool>::failure(offered.error());
  }
  retry_live_after_ack_ = false;
  if (superseding) {
    pending_native_revision_->snapshot = std::move(snapshot);
  } else {
    PendingNativeRevision pending{{}, changed->id, {}, std::move(snapshot), {}};
    pending.runtime = player;
    pending_native_revision_.emplace(std::move(pending));
  }
  return Result<bool>::success(true);
}

Result<bool> RoutedPlaybackControl::try_queue_live_synth_automation(
    std::shared_ptr<const project::Snapshot> snapshot) {
  const bool superseding =
      pending_native_revision_ && std::holds_alternative<native_dsp::synth::NativeSynthPlayer*>(
                                      pending_native_revision_->runtime);
  const auto* previous =
      superseding ? pending_native_revision_->snapshot.get() : published_snapshot_.get();
  if (!previous || !live_source_ || publisher_.active_revision() == 0 || publisher_.has_pending() ||
      publisher_.held_boundary() || (pending_native_revision_ && !superseding) ||
      (!superseding && revisions_.snapshot().active_graph_revision != previous->revision) ||
      previous->id != snapshot->id || previous->sample_rate != snapshot->sample_rate ||
      previous->sources != snapshot->sources ||
      previous->master_effects != snapshot->master_effects ||
      previous->master_level != snapshot->master_level ||
      previous->source_tempo_reviews != snapshot->source_tempo_reviews ||
      previous->source_key_reviews != snapshot->source_key_reviews ||
      previous->next_insert_slot_serial != snapshot->next_insert_slot_serial ||
      previous->tracks.size() != snapshot->tracks.size() || !same_timing(*previous, *snapshot))
    return Result<bool>::success(false);
  const project::Track* changed = nullptr;
  for (std::size_t index = 0; index < previous->tracks.size(); ++index) {
    const auto& old_track = previous->tracks[index];
    const auto& next_track = snapshot->tracks[index];
    if (old_track == next_track)
      continue;
    if (changed || old_track.kind != project::TrackKind::instrument ||
        !std::holds_alternative<project::NativeInstrumentPatch>(old_track.instrument_source))
      return Result<bool>::success(false);
    const auto native_lane = [](const project::AutomationLane& lane) {
      // A macro lane may drive the synth's parameters (ADR-0059).
      return std::holds_alternative<project::NativeParameterTarget>(lane.target) ||
             std::holds_alternative<project::MacroTarget>(lane.target);
    };
    if (std::ranges::equal(old_track.automation | std::views::filter(native_lane),
                           next_track.automation | std::views::filter(native_lane)) ||
        !std::ranges::equal(old_track.automation | std::views::filter(std::not_fn(native_lane)),
                            next_track.automation | std::views::filter(std::not_fn(native_lane))))
      return Result<bool>::success(false);
    auto masked = old_track;
    masked.automation = next_track.automation;
    if (masked != next_track)
      return Result<bool>::success(false);
    changed = &next_track;
  }
  if (!changed)
    return Result<bool>::success(false);
  auto* player = live_source_->synth_player(changed->id);
  if (!player)
    return Result<bool>::failure(
        {ErrorCode::unavailable,
         "Live synth player is unavailable; automation edit is not active"});
  if (superseding && (pending_native_revision_->track != changed->id ||
                      std::get<native_dsp::synth::NativeSynthPlayer*>(
                          pending_native_revision_->runtime) != player))
    return Result<bool>::success(false);
  auto prepared = automation_project::prepare_project_automation(*snapshot, snapshot->revision);
  if (!prepared)
    return Result<bool>::failure(prepared.error());
  auto limit = automation_sample_limit(*snapshot, limits_.max_frames);
  if (!limit)
    return Result<bool>::failure(limit.error());
  auto offered = player->offer_automation(*snapshot, *changed, prepared.value(), limit.value());
  if (!offered) {
    if (offered.error().code == ErrorCode::resource_exhausted)
      retry_live_after_ack_ = true;
    return Result<bool>::failure(offered.error());
  }
  retry_live_after_ack_ = false;
  if (superseding) {
    pending_native_revision_->snapshot = std::move(snapshot);
  } else {
    PendingNativeRevision pending{{}, changed->id, {}, std::move(snapshot), {}};
    pending.runtime = player;
    pending_native_revision_.emplace(std::move(pending));
  }
  return Result<bool>::success(true);
}

Result<bool> RoutedPlaybackControl::try_queue_live_effect_automation(
    std::shared_ptr<const project::Snapshot> snapshot) {
  const bool superseding = pending_native_revision_ && pending_native_revision_->track &&
                           std::holds_alternative<native_inserts::NativeEffectStage*>(
                               pending_native_revision_->runtime) &&
                           pending_native_revision_->effects.empty();
  const auto* previous =
      superseding ? pending_native_revision_->snapshot.get() : published_snapshot_.get();
  if (!previous || publisher_.active_revision() == 0 || publisher_.has_pending() ||
      publisher_.held_boundary() || (pending_native_revision_ && !superseding) ||
      (!superseding && revisions_.snapshot().active_graph_revision != previous->revision) ||
      previous->id != snapshot->id || previous->sample_rate != snapshot->sample_rate ||
      previous->sources != snapshot->sources ||
      previous->master_effects != snapshot->master_effects ||
      previous->master_level != snapshot->master_level ||
      previous->source_tempo_reviews != snapshot->source_tempo_reviews ||
      previous->source_key_reviews != snapshot->source_key_reviews ||
      previous->next_insert_slot_serial != snapshot->next_insert_slot_serial ||
      previous->tracks.size() != snapshot->tracks.size() || !same_timing(*previous, *snapshot))
    return Result<bool>::success(false);
  const project::Track* changed = nullptr;
  std::optional<project::InsertSlotId> slot;
  for (std::size_t index = 0; index < previous->tracks.size(); ++index) {
    const auto& old_track = previous->tracks[index];
    const auto& next_track = snapshot->tracks[index];
    if (old_track == next_track)
      continue;
    if (changed)
      return Result<bool>::success(false);
    auto masked = old_track;
    masked.automation = next_track.automation;
    if (masked != next_track)
      return Result<bool>::success(false);
    // Every differing lane must target one built-in effect slot.
    const auto lane_slot = [](const project::AutomationLane& lane) {
      const auto* target = std::get_if<project::NativeEffectParameterTarget>(&lane.target);
      return target ? std::optional(target->slot) : std::nullopt;
    };
    const auto note = [&](const project::AutomationLane& lane) {
      const auto target = lane_slot(lane);
      if (!target || (slot && *slot != *target))
        return false;
      slot = target;
      return true;
    };
    for (const auto& lane : old_track.automation) {
      if (std::ranges::find(next_track.automation, lane) == next_track.automation.end() &&
          !note(lane))
        return Result<bool>::success(false);
    }
    for (const auto& lane : next_track.automation) {
      if (std::ranges::find(old_track.automation, lane) == old_track.automation.end() &&
          !note(lane))
        return Result<bool>::success(false);
    }
    changed = &next_track;
  }
  if (!changed || !slot)
    return Result<bool>::success(false);
  const auto* chain = active_chain(changed->id);
  if (!chain || !runs(chain->chain, track_chain::saved_chain(*changed)))
    return Result<bool>::success(false);
  auto* running = active_stage(changed->id, *slot);
  auto* stage =
      running ? dynamic_cast<native_inserts::NativeEffectStage*>(running->runtime.get()) : nullptr;
  if (!stage || stage->slot() != *slot)
    return Result<bool>::success(false);
  if (superseding &&
      (pending_native_revision_->track != changed->id ||
       std::get<native_inserts::NativeEffectStage*>(pending_native_revision_->runtime) != stage))
    return Result<bool>::success(false);
  auto prepared = automation_project::prepare_project_automation(*snapshot, snapshot->revision);
  if (!prepared)
    return Result<bool>::failure(prepared.error());
  auto limit = automation_sample_limit(*snapshot, limits_.max_frames);
  if (!limit)
    return Result<bool>::failure(limit.error());
  auto offered = stage->offer_automation(*snapshot, prepared.value(), limit.value());
  if (!offered) {
    if (offered.error().code == ErrorCode::resource_exhausted)
      retry_live_after_ack_ = true;
    return Result<bool>::failure(offered.error());
  }
  retry_live_after_ack_ = false;
  if (superseding) {
    pending_native_revision_->snapshot = std::move(snapshot);
  } else {
    // No effects: the chain's saved slots are unchanged by a lane edit.
    PendingNativeRevision pending{{}, changed->id, {}, std::move(snapshot), {}};
    pending.runtime = stage;
    pending_native_revision_.emplace(std::move(pending));
  }
  return Result<bool>::success(true);
}

std::optional<project::TrackId> RoutedPlaybackControl::bound_instrument_track() const noexcept {
  const auto found = std::ranges::find_if(
      vst3_stages_, [](const Vst3Stage& stage) { return stage.track && !stage.slot; });
  if (found != vst3_stages_.end())
    return found->track;
  return std::nullopt;
}

bool RoutedPlaybackControl::has_bound_instrument_track(project::TrackId track) const noexcept {
  if (!published_snapshot_)
    return false;
  const auto owner = std::ranges::find(published_snapshot_->tracks, track, &project::Track::id);
  if (owner == published_snapshot_->tracks.end())
    return false;
  const auto* saved = std::get_if<project::Vst3PluginInstance>(&owner->instrument_source);
  return saved && std::ranges::any_of(vst3_stages_, [track, saved](const Vst3Stage& stage) {
           return stage.track == track && !stage.slot && stage.saved == *saved;
         });
}

std::optional<ActiveVst3EffectManualEvents> RoutedPlaybackControl::active_vst3_effect_manual_events(
    project::TrackId track, project::InsertSlotId insert, project::PluginInstanceId plugin,
    std::uint64_t graph_revision) const {
  if (graph_revision == 0 || graph_revision != publisher_.active_revision() ||
      graph_handoff_in_progress())
    return std::nullopt;
  const auto found = std::ranges::find_if(vst3_stages_, [&](const Vst3Stage& candidate) {
    return candidate.track == track && candidate.slot == insert && candidate.saved.id == plugin;
  });
  if (found == vst3_stages_.end() || !found->prepared.manual_events ||
      !found->prepared.active_manual_graph ||
      (found->prepared.process_failed && found->prepared.process_failed()))
    return std::nullopt;

  // The model may have advanced while the previous render graph continues.
  // Never hand out that graph's slot for a changed saved insert.
  const auto current = model_.snapshot();
  const auto owner = std::ranges::find(current->tracks, track, &project::Track::id);
  if (owner == current->tracks.end())
    return std::nullopt;
  const auto effect = std::ranges::find(owner->effects, insert, &project::EffectInsert::id);
  const auto* saved = effect == owner->effects.end()
                          ? nullptr
                          : std::get_if<project::Vst3PluginInstance>(&effect->processor);
  if (!saved || !effect->enabled || saved->missing || *saved != found->saved)
    return std::nullopt;
  const auto identity = found->prepared.active_manual_graph();
  if (identity.graph_revision != graph_revision || identity.instance_id == 0 ||
      identity.compatibility_key == 0)
    return std::nullopt;
  return ActiveVst3EffectManualEvents{found->prepared.manual_events, identity};
}

std::optional<ActiveVst3InstrumentManualEvents>
RoutedPlaybackControl::active_vst3_instrument_manual_events(project::TrackId track,
                                                            project::PluginInstanceId plugin,
                                                            std::uint64_t graph_revision) const {
  if (graph_revision == 0 || graph_revision != publisher_.active_revision() ||
      graph_handoff_in_progress())
    return std::nullopt;
  const auto found = std::ranges::find_if(vst3_stages_, [&](const Vst3Stage& candidate) {
    return candidate.track == track && !candidate.slot && candidate.saved.id == plugin;
  });
  if (found == vst3_stages_.end() || !found->prepared.manual_events ||
      !found->prepared.active_manual_graph ||
      (found->prepared.process_failed && found->prepared.process_failed()))
    return std::nullopt;

  const auto current = model_.snapshot();
  const auto owner = std::ranges::find(current->tracks, track, &project::Track::id);
  if (owner == current->tracks.end() || owner->kind != project::TrackKind::instrument)
    return std::nullopt;
  const auto* saved = std::get_if<project::Vst3PluginInstance>(&owner->instrument_source);
  if (!saved || saved->missing || *saved != found->saved)
    return std::nullopt;
  const auto identity = found->prepared.active_manual_graph();
  if (identity.graph_revision != graph_revision || identity.instance_id == 0 ||
      identity.compatibility_key == 0)
    return std::nullopt;
  return ActiveVst3InstrumentManualEvents{found->prepared.manual_events, identity};
}

std::optional<PendingVst3ManualPreviewEvents>
RoutedPlaybackControl::pending_vst3_manual_preview_events(
    project::TrackId track, std::optional<project::InsertSlotId> insert,
    project::PluginInstanceId plugin) const {
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  if (!pending_manual_ || pending_manual_->plugin != plugin ||
      pending_manual_->snapshot != model_.snapshot() || awaiting_ || publisher_.has_pending() ||
      publisher_.held_boundary() || pending_native_revision_ || publisher_.active_revision() == 0)
    return std::nullopt;
  const auto found = std::ranges::find_if(vst3_stages_, [&](const Vst3Stage& candidate) {
    return candidate.track == track && candidate.slot == insert && candidate.saved.id == plugin;
  });
  if (found == vst3_stages_.end() || !found->prepared.manual_events ||
      !found->prepared.active_manual_graph ||
      (found->prepared.process_failed && found->prepared.process_failed()))
    return std::nullopt;
  const auto identity = found->prepared.active_manual_graph();
  if (identity.graph_revision != pending_manual_->identity.graph_revision ||
      identity.instance_id != pending_manual_->identity.instance_id ||
      identity.compatibility_key != pending_manual_->identity.compatibility_key ||
      identity.graph_revision != publisher_.active_revision())
    return std::nullopt;
  const auto receipt = found->prepared.manual_events->receipt();
  if (receipt.revision != pending_manual_->snapshot->revision)
    return std::nullopt;
  const auto phase = receipt.phase;
  if (phase != plugins::Vst3ManualEventPhase::pending &&
      phase != plugins::Vst3ManualEventPhase::processing &&
      phase != plugins::Vst3ManualEventPhase::applied)
    return std::nullopt;
  return PendingVst3ManualPreviewEvents{found->prepared.manual_events, identity};
#else
  (void)track;
  (void)insert;
  (void)plugin;
  return std::nullopt;
#endif
}

Result<const RoutedPlaybackControl::Vst3Stage*>
RoutedPlaybackControl::manual_slot_ready(project::PluginInstanceId plugin,
                                         plugins::Vst3ActiveGraphIdentity expected) const {
  const auto refuse = [](ErrorCode code, const char* message) {
    return Result<const Vst3Stage*>::failure({code, message});
  };
  const auto status = revisions_.snapshot();
  // Audible state must equal the latest saved revision. After an unapplied
  // manual value the audible plugin lacks it, so a later event cannot make
  // the newer saved state truthfully audible.
  if (graph_handoff_in_progress() || pending_native_revision_ || !published_snapshot_ ||
      publisher_.active_revision() == 0 || status.application != PlaybackApplication::active ||
      status.active_graph_revision != status.model_revision ||
      published_snapshot_->revision != status.active_graph_revision)
    return refuse(ErrorCode::conflict, "Live VST3 parameter needs a settled audible revision");
  const auto found = std::ranges::find_if(
      vst3_stages_, [&](const Vst3Stage& stage) { return stage.saved.id == plugin; });
  if (found == vst3_stages_.end() || !found->prepared.manual_events ||
      !found->prepared.active_manual_graph ||
      (found->prepared.process_failed && found->prepared.process_failed()))
    return refuse(ErrorCode::unavailable, "VST3 plugin has no active qualified manual slot");
  const auto identity = found->prepared.active_manual_graph();
  if (expected.graph_revision != publisher_.active_revision() || identity.instance_id == 0 ||
      identity.graph_revision != expected.graph_revision ||
      identity.instance_id != expected.instance_id ||
      identity.compatibility_key != expected.compatibility_key)
    return refuse(ErrorCode::conflict, "VST3 manual edit names a stale active instance");
  const auto* audible = saved_vst3(*published_snapshot_, found->track, plugin);
  if (!audible || *audible != found->saved)
    return refuse(ErrorCode::conflict, "Audible VST3 state differs from its active plugin");
  return Result<const Vst3Stage*>::success(&*found);
}

bool RoutedPlaybackControl::published_snapshot_audible() const {
  // The callback-accepted topology carries the published snapshot, or an
  // acknowledged live parameter revision advanced it past that topology.
  return published_snapshot_ &&
         published_snapshot_->revision ==
             std::max(publisher_.active_revision(), revisions_.snapshot().active_graph_revision);
}

Result<void>
RoutedPlaybackControl::preflight_vst3_manual_edit(project::PluginInstanceId plugin,
                                                  plugins::Vst3ActiveGraphIdentity expected) const {
  if (model_.snapshot()->revision != revisions_.snapshot().model_revision)
    return rejected(ErrorCode::conflict, "Routed playback has not accepted the saved revision");
  auto ready = manual_slot_ready(plugin, expected);
  if (!ready)
    return Result<void>::failure(ready.error());
  return Result<void>::success();
}

Result<void> RoutedPlaybackControl::begin_vst3_manual_edit(
    const project::AppliedRevision& applied, std::string_view command_label,
    project::PluginInstanceId plugin, plugins::Vst3ActiveGraphIdentity expected) {
  observe();
  const auto current = model_.snapshot();
  if (applied.snapshot != current || current->revision <= revisions_.snapshot().model_revision)
    return rejected(ErrorCode::conflict, "Routed VST3 manual edit was superseded");
  // Readiness describes the previous saved revision, so check it first.
  auto ready = manual_slot_ready(plugin, expected);
  (void)revisions_.accept_model_revision(
      current->revision, ChangeDescriptor{std::string(command_label), applied.changes});
  preparing_source_.reset();
  if (!ready)
    return fail(current->revision, ready.error());
  if (!manual_vst3_state_only(*published_snapshot_, *current, plugin))
    return fail(current->revision,
                {ErrorCode::unsupported,
                 "Live VST3 revision changes more than one plugin's parameter state"});
  pending_manual_.emplace(PendingManualRevision{current, plugin, expected});
  return Result<void>::success();
}

Result<void>
RoutedPlaybackControl::settle_vst3_manual_edit(plugins::Vst3ManualEventReceipt receipt) {
  if (!pending_manual_ || receipt.revision != pending_manual_->snapshot->revision ||
      (receipt.phase != plugins::Vst3ManualEventPhase::applied &&
       receipt.phase != plugins::Vst3ManualEventPhase::failed))
    return rejected(ErrorCode::conflict, "VST3 manual receipt does not match the pending edit");
  auto pending = std::move(*pending_manual_);
  pending_manual_.reset();
  if (receipt.phase != plugins::Vst3ManualEventPhase::applied || receipt.project_sample < 0)
    return fail(pending.snapshot->revision,
                {ErrorCode::unavailable,
                 "Live VST3 parameter was not applied; the saved revision is not audible"});
  const auto found = std::ranges::find_if(
      vst3_stages_, [&](const Vst3Stage& stage) { return stage.saved.id == pending.plugin; });
  const project::Vst3PluginInstance* saved =
      found == vst3_stages_.end() ? nullptr
                                  : saved_vst3(*pending.snapshot, found->track, pending.plugin);
  if (!saved || !found->prepared.active_manual_graph)
    return fail(pending.snapshot->revision,
                {ErrorCode::internal, "Applied VST3 manual revision lost its active plugin"});
  const auto identity = found->prepared.active_manual_graph();
  if (identity.graph_revision != pending.identity.graph_revision ||
      identity.instance_id != pending.identity.instance_id ||
      identity.compatibility_key != pending.identity.compatibility_key)
    return fail(pending.snapshot->revision,
                {ErrorCode::conflict, "Active VST3 instance changed before its receipt"});
  // The running plugin now holds the saved state, so its chain stage does too.
  found->saved = *saved;
  if (auto* chain = active_chain(found->track)) {
    for (auto& stage : chain->chain.stages) {
      if (stage.runtime == found->prepared.processor)
        stage.saved.processor = *saved;
    }
  }
  published_snapshot_ = std::move(pending.snapshot);
  (void)revisions_.observe_active_graph(published_snapshot_->revision);
  return Result<void>::success();
}

std::optional<engine::RoutedNodeMeterReading>
RoutedPlaybackControl::active_track_meter(project::TrackId track) const noexcept {
  if (!published_route_plan_)
    return std::nullopt;
  const auto route = std::ranges::find(published_route_plan_->track_nodes, track,
                                       &routing::ProjectRouteTrackNode::track);
  if (route == published_route_plan_->track_nodes.end())
    return std::nullopt;
  return publisher_.active_node_meter(route->node);
}

bool RoutedPlaybackControl::set_live_channel(std::optional<project::TrackId> track, double level,
                                             double pan) noexcept {
  if (!published_route_plan_ || !std::isfinite(level) || !std::isfinite(pan))
    return false;
  if (!track)
    return publisher_.push_live_node_gain(published_route_plan_->master,
                                          {static_cast<float>(level), static_cast<float>(level)});
  const auto route = std::ranges::find(published_route_plan_->track_nodes, *track,
                                       &routing::ProjectRouteTrackNode::track);
  if (route == published_route_plan_->track_nodes.end())
    return false;
  const auto gain = routing::channel_stereo_gain(level, std::clamp(pan, -1.0, 1.0));
  return publisher_.push_live_node_gain(route->node, {gain.left, gain.right});
}

bool RoutedPlaybackControl::set_live_send(project::TrackId track, project::SendId send,
                                          double gain) noexcept {
  if (!published_route_plan_ || !std::isfinite(gain))
    return false;
  const auto route = std::ranges::find_if(published_route_plan_->sends, [&](const auto& item) {
    return item.track == track && item.send == send;
  });
  if (route == published_route_plan_->sends.end())
    return false;
  // A silent key stays silent while its gain is dragged.
  float heard = static_cast<float>(gain);
  if (published_snapshot_) {
    const auto& tracks = published_snapshot_->tracks;
    const auto source = std::ranges::find(tracks, track, &project::Track::id);
    if (source != tracks.end()) {
      const auto saved = std::ranges::find(source->sends, send, &project::SendRoute::id);
      if (saved != source->sends.end() && saved->purpose == project::SendPurpose::sidechain) {
        try {
          if (routing::silent_key_sources(*published_snapshot_).contains(track))
            heard = 0.0F;
        } catch (const std::bad_alloc&) {
          return false;
        }
      }
    }
  }
  return publisher_.push_live_edge_gain(route->edge, heard);
}

bool RoutedPlaybackControl::set_live_synth_values(project::TrackId track,
                                                  const native_dsp::synth::EngineDescriptor& engine,
                                                  std::span<const double> values) noexcept {
  auto* runtime = live_source_ ? live_source_->synth_runtime(track) : nullptr;
  return runtime && runtime->engine().descriptor == &engine && runtime->offer_live_values(values);
}

bool RoutedPlaybackControl::live_note(project::TrackId track, std::uint8_t key,
                                      std::uint8_t velocity, bool on) noexcept {
  auto* player = live_source_ ? live_source_->synth_player(track) : nullptr;
  return player && player->queue_live_note(key, velocity, on);
}

bool RoutedPlaybackControl::set_live_effect_values(
    std::optional<project::TrackId> track, project::InsertSlotId slot,
    const native_dsp::synth::EngineDescriptor& engine, std::span<const double> values) noexcept {
  auto* stage = active_effect_stage(track, slot);
  if (!stage || stage->engine().descriptor != &engine)
    return false;
  // Latency is fixed at creation: a latency change waits for the saved
  // edit, which rebuilds the stage.
  const auto latency = stage->engine().latency_frames
                           ? stage->engine().latency_frames(values, stage->sample_rate())
                           : 0U;
  return latency == stage->latency_frames() && stage->offer_live_values(values);
}

native_inserts::NativeEffectStage*
RoutedPlaybackControl::active_effect_stage(std::optional<project::TrackId> track,
                                           project::InsertSlotId insert) const noexcept {
  const auto* chain = active_chain(track);
  if (chain == nullptr)
    return nullptr;
  for (const auto& stage : chain->chain.stages) {
    auto* effect = dynamic_cast<native_inserts::NativeEffectStage*>(stage.runtime.get());
    if (effect && effect->slot() == insert)
      return effect;
  }
  return nullptr;
}

bool RoutedPlaybackControl::active_effect_telemetry(std::optional<project::TrackId> track,
                                                    project::InsertSlotId insert,
                                                    std::vector<float>& frame) {
  auto* effect = active_effect_stage(track, insert);
  return effect != nullptr && effect->read_telemetry(frame);
}

bool RoutedPlaybackControl::set_active_effect_key_listen(project::TrackId track,
                                                         project::InsertSlotId insert,
                                                         bool listen) noexcept {
  auto* effect = active_effect_stage(track, insert);
  if (effect == nullptr)
    return false;
  effect->set_key_listen(listen);
  return true;
}

std::optional<float>
RoutedPlaybackControl::active_effect_key_peak(project::TrackId track,
                                              project::InsertSlotId insert) const noexcept {
  const auto* effect = active_effect_stage(track, insert);
  return effect == nullptr ? std::nullopt : std::optional(effect->key_peak());
}

Result<void> RoutedPlaybackControl::begin_current() {
  if (awaiting_ || publisher_.has_pending()) {
    return rejected(ErrorCode::conflict, "A routed graph update is already pending");
  }
  const auto current = model_.snapshot();
  if (publisher_.active_revision() == 0) {
    return build_and_publish(current);
  }
  if (source_snapshot_ && native_synth_engine_changed(*source_snapshot_, *current)) {
    return fail(current->revision,
                {ErrorCode::unsupported,
                 "Native synth engine change is saved but not audible; live source replacement "
                 "is unavailable. Stop and restart playback to apply it"});
  }
  (void)publisher_.collect_retired();
  // Built-in chain, routing and level edits switch at the next block while
  // the current graph keeps playing; anything touching sources or VST3
  // instances takes the held path.
  if (live_switching_ && published_snapshot_ && source_snapshot_ && published_snapshot_audible() &&
      !pending_native_revision_ && !pending_manual_ &&
      same_audio_content(*source_snapshot_, *current) &&
      live_switch_eligible(*published_snapshot_, *source_snapshot_, *current)) {
    publish_live_ = true;
    auto published = build_and_publish(current);
    publish_live_ = false;
    return published;
  }
  if (!publisher_.request_hold()) {
    return fail(current->revision,
                {ErrorCode::conflict, "Routed graph could not request a callback hold"});
  }
  awaiting_ = current;
  return Result<void>::success();
}

Result<void> RoutedPlaybackControl::reject_edit(const project::AppliedRevision& applied,
                                                std::string_view command_label, Error error) {
  observe();
  const auto current = model_.snapshot();
  if (applied.snapshot != current || current->revision <= revisions_.snapshot().model_revision)
    return rejected(ErrorCode::conflict, "Routed project edit was superseded");
  (void)revisions_.accept_model_revision(
      current->revision, ChangeDescriptor{std::string(command_label), applied.changes});
  return fail(current->revision, std::move(error));
}

Result<void> RoutedPlaybackControl::begin_edit(const project::AppliedRevision& applied,
                                               std::string_view command_label) {
  OSCILLATE_TRACE_SPAN("edit.begin");
  observe();
  const auto current = model_.snapshot();
  if (applied.snapshot != current || current->revision <= revisions_.snapshot().model_revision) {
    return rejected(ErrorCode::conflict, "Routed project edit was superseded");
  }
  (void)revisions_.accept_model_revision(
      current->revision, ChangeDescriptor{std::string(command_label), applied.changes});
  preparing_source_.reset(); // A newer saved edit invalidates any worker capture.
  if (awaiting_ || publisher_.has_pending()) {
    return fail(current->revision,
                {ErrorCode::conflict, "Previous routed graph update must activate first"});
  }
  if (pending_manual_)
    return fail(current->revision,
                {ErrorCode::conflict, "Previous live VST3 parameter must be acknowledged first"});
  if (published_snapshot_ && published_snapshot_audible() && !pending_native_revision_ &&
      project_key_only(*published_snapshot_, *current)) {
    // Every running synth adopts the key at its next block; nothing else
    // that plays changed.
    if (live_source_ != nullptr)
      live_source_->follow_project_key(current->key);
    published_snapshot_ = current;
    source_snapshot_ = current;
    (void)revisions_.observe_active_graph(current->revision);
    return Result<void>::success();
  }
  if (published_snapshot_ && published_route_plan_ && published_snapshot_audible() &&
      !pending_native_revision_ && channel_controls_only(*published_snapshot_, *current)) {
    auto controls = routing::project_node_controls(*current, *published_route_plan_);
    if (!controls)
      return fail(current->revision, controls.error());
    // A mute or solo also decides which sidechain keys sound.
    auto edges = routing::project_edge_controls(*current, *published_route_plan_);
    if (!edges)
      return fail(current->revision, edges.error());
    (void)publisher_.collect_retired();
    auto offered =
        publisher_.prepare_mix_overlay(current->revision, controls.value(), edges.value());
    if (offered) {
      // The source bank and every processor remain active. Only the post-insert
      // controls and audible revision change at the next callback boundary.
      published_snapshot_ = current;
      refresh_effect_key_notes();
      return Result<void>::success();
    }
    // Revision-sensitive VST3 owners or channel automation retain the normal
    // guarded graph path. Its status still distinguishes saved from audible.
  }
  if (published_snapshot_ && published_route_plan_ && published_snapshot_audible() &&
      !pending_native_revision_ && send_gains_only(*published_snapshot_, *current)) {
    auto edges = routing::project_edge_controls(*current, *published_route_plan_);
    if (!edges)
      return fail(current->revision, edges.error());
    (void)publisher_.collect_retired();
    auto offered = publisher_.prepare_edge_overlay(current->revision, edges.value());
    if (offered) {
      published_snapshot_ = current;
      refresh_effect_key_notes(); // A key at zero gain keys no notes.
      return Result<void>::success();
    }
    // A bound automation edge or a revision-sensitive processor follows the
    // guarded route path, with its saved-versus-audible status preserved.
  }
  auto fast_synth = try_queue_live_synth(current);
  if (!fast_synth)
    return fail(current->revision, fast_synth.error());
  if (fast_synth.value())
    return Result<void>::success();
  auto fast_synth_automation = try_queue_live_synth_automation(current);
  if (!fast_synth_automation)
    return fail(current->revision, fast_synth_automation.error());
  if (fast_synth_automation.value())
    return Result<void>::success();
  auto fast_note_effects = try_queue_live_note_effects(current);
  if (!fast_note_effects)
    return fail(current->revision, fast_note_effects.error());
  if (fast_note_effects.value())
    return Result<void>::success();
  if (published_snapshot_ && native_synth_automation_changed(*published_snapshot_, *current))
    return fail(current->revision,
                {ErrorCode::unsupported, "Live synth automation must be an isolated lane edit; "
                                         "other saved changes need a restart"});
  auto fast_effect_automation = try_queue_live_effect_automation(current);
  if (!fast_effect_automation)
    return fail(current->revision, fast_effect_automation.error());
  if (fast_effect_automation.value())
    return Result<void>::success();
  auto fast_master = try_queue_live_master_native(current);
  if (!fast_master)
    return fail(current->revision, fast_master.error());
  if (fast_master.value())
    return Result<void>::success();
  auto fast_native = try_queue_live_native(current);
  if (!fast_native)
    return fail(current->revision, fast_native.error());
  if (fast_native.value())
    return Result<void>::success();
  auto fast_group = try_queue_live_effect_group(current);
  if (!fast_group)
    return fail(current->revision, fast_group.error());
  if (fast_group.value())
    return Result<void>::success();
  if (pending_native_revision_)
    return fail(current->revision,
                {ErrorCode::conflict, "Previous live parameter update must activate first"});
  return begin_current();
}

Result<RoutedSourcePreparationCapture>
RoutedPlaybackControl::capture_source_edit(const project::AppliedRevision& applied,
                                           std::string_view command_label) {
  observe();
  const auto current = model_.snapshot();
  if (applied.snapshot != current || current->revision <= revisions_.snapshot().model_revision) {
    return Result<RoutedSourcePreparationCapture>::failure(
        {ErrorCode::conflict, "Routed source edit was superseded"});
  }
  (void)revisions_.accept_model_revision(
      current->revision, ChangeDescriptor{std::string(command_label), applied.changes});
  preparing_source_.reset();
  const auto rejected_capture = [&](Error error) {
    (void)fail(current->revision, error);
    return Result<RoutedSourcePreparationCapture>::failure(std::move(error));
  };
  if (publisher_.active_revision() == 0 || publisher_.has_pending() || awaiting_ ||
      pending_native_revision_ || pending_manual_ || !published_snapshot_audible()) {
    return rejected_capture(
        {ErrorCode::conflict, "Routed source edit needs an active, settled graph"});
  }
  const bool synth_switch = native_synth_switch_only(*published_snapshot_, *current);
  if (!audio_clip_edit_only(*published_snapshot_, *current) && !synth_switch) {
    return rejected_capture(
        {ErrorCode::unsupported, "Live source preparation requires an isolated source edit"});
  }
  try {
    RoutedSourcePreparationCapture capture{
        current,
        sources_,
        topological_order_,
        limits_.max_frames,
        publisher_.active_revision(),
        synth_switch,
        !synth_switch && source_overlay_clip_only(*published_snapshot_, *current)};
    preparing_source_ = current;
    return Result<RoutedSourcePreparationCapture>::success(std::move(capture));
  } catch (const std::bad_alloc&) {
    return rejected_capture(
        {ErrorCode::resource_exhausted, "Routed source preparation capture ran out of memory"});
  }
}

bool RoutedPlaybackControl::can_capture_source_edit(const project::Snapshot& candidate) const {
  return !pending_manual_ && published_snapshot_audible() &&
         (audio_clip_edit_only(*published_snapshot_, candidate) ||
          native_synth_switch_only(*published_snapshot_, candidate));
}

Result<void> RoutedPlaybackControl::publish_captured_source_overlay(
    const RoutedSourcePreparationCapture& capture) {
  observe();
  const auto current = model_.snapshot();
  if (!capture.source_overlay_eligible || current != capture.snapshot ||
      current != preparing_source_ || revisions_.snapshot().model_revision != current->revision ||
      revisions_.snapshot().application != PlaybackApplication::pending ||
      publisher_.active_revision() != capture.active_graph_revision ||
      !same_sources(sources_, capture.source_layout) ||
      !std::ranges::equal(topological_order_, capture.topological_order) ||
      limits_.max_frames != capture.max_frames || awaiting_ || publisher_.has_pending() ||
      pending_native_revision_ || pending_manual_ ||
      !source_overlay_clip_only(*published_snapshot_, *current))
    return rejected(ErrorCode::conflict, "Routed source overlay capture is stale or busy");
  (void)publisher_.collect_retired();
  auto offered = publisher_.prepare_source_overlay(current->revision);
  if (!offered)
    return fail(current->revision, offered.error());
  published_snapshot_ = current;
  source_snapshot_ = current;
  preparing_source_.reset();
  return Result<void>::success();
}

Result<void>
RoutedPlaybackControl::request_captured_source_hold(const RoutedSourcePreparationCapture& capture) {
  observe();
  const auto current = model_.snapshot();
  if (current != preparing_source_ && current->revision > revisions_.snapshot().model_revision) {
    preparing_source_.reset();
    (void)revisions_.accept_model_revision(current->revision,
                                           ChangeDescriptor{"project refresh", {}});
    return fail(current->revision, {ErrorCode::conflict,
                                    "Routed source preparation was superseded by a project edit"});
  }
  if (current != preparing_source_ || current != capture.snapshot ||
      revisions_.snapshot().model_revision != current->revision ||
      revisions_.snapshot().application != PlaybackApplication::pending ||
      publisher_.active_revision() != capture.active_graph_revision ||
      !same_sources(sources_, capture.source_layout) ||
      !std::ranges::equal(topological_order_, capture.topological_order) ||
      limits_.max_frames != capture.max_frames || awaiting_ || publisher_.has_pending() ||
      pending_native_revision_ || pending_manual_) {
    return rejected(ErrorCode::conflict, "Routed source preparation capture is stale or busy");
  }
  (void)publisher_.collect_retired();
  if (!publisher_.request_hold()) {
    return fail(current->revision,
                {ErrorCode::conflict, "Routed source graph could not request a callback hold"});
  }
  awaiting_ = current;
  awaiting_source_replacement_ = true;
  preparing_source_.reset();
  return Result<void>::success();
}

Result<void>
RoutedPlaybackControl::fail_captured_source_edit(const RoutedSourcePreparationCapture& capture,
                                                 Error error) {
  const auto current = model_.snapshot();
  if (current != capture.snapshot || current != preparing_source_ ||
      revisions_.snapshot().model_revision != current->revision ||
      revisions_.snapshot().application != PlaybackApplication::pending) {
    return rejected(ErrorCode::conflict, "Routed source preparation failure is stale");
  }
  preparing_source_.reset();
  return fail(current->revision, std::move(error));
}

Result<void> RoutedPlaybackControl::publish_held() {
  if (!awaiting_ || !publisher_.held_boundary()) {
    return rejected(ErrorCode::conflict, "Routed graph has not acknowledged a callback hold");
  }
  auto current = model_.snapshot();
  if (current != awaiting_) {
    (void)publisher_.cancel_hold();
    awaiting_.reset();
    awaiting_source_replacement_ = false;
    if (current->revision > revisions_.snapshot().model_revision) {
      (void)revisions_.accept_model_revision(current->revision,
                                             ChangeDescriptor{"project refresh", {}});
    }
    return fail(current->revision,
                {ErrorCode::conflict, "Routed project changed during graph preparation"});
  }
  if (awaiting_source_replacement_) {
    (void)publisher_.cancel_hold();
    awaiting_.reset();
    awaiting_source_replacement_ = false;
    return fail(
        current->revision,
        {ErrorCode::unsupported, "Routed source switching is not connected to graph publication"});
  }
  auto result = build_and_publish(current);
  if (!result) {
    (void)publisher_.cancel_hold();
  }
  awaiting_.reset();
  return result;
}

Result<void> RoutedPlaybackControl::publish_held_with_source() {
  if (!awaiting_source_replacement_ || !awaiting_ || !publisher_.held_boundary())
    return rejected(ErrorCode::conflict, "Routed source graph has no held boundary");
  auto current = model_.snapshot();
  if (current != awaiting_) {
    (void)publisher_.cancel_hold();
    awaiting_.reset();
    awaiting_source_replacement_ = false;
    return fail(current->revision,
                {ErrorCode::conflict, "Routed project changed during source handoff"});
  }
  auto result = build_and_publish(current);
  if (result)
    source_snapshot_ = std::move(current);
  if (!result)
    (void)publisher_.cancel_hold();
  awaiting_.reset();
  awaiting_source_replacement_ = false;
  return result;
}

Result<void> RoutedPlaybackControl::cancel_captured_source_hold(Error error) {
  if (!awaiting_source_replacement_ || !awaiting_ || publisher_.has_pending())
    return rejected(ErrorCode::conflict, "Routed source hold cannot be cancelled now");
  const auto revision = awaiting_->revision;
  (void)publisher_.cancel_hold();
  awaiting_.reset();
  awaiting_source_replacement_ = false;
  return fail(revision, std::move(error));
}

Result<void> RoutedPlaybackControl::retry() {
  if (revisions_.snapshot().application != PlaybackApplication::failed ||
      revisions_.snapshot().model_revision != model_.snapshot()->revision) {
    return rejected(ErrorCode::conflict, "Only the failed current routed revision can be retried");
  }
  (void)revisions_.retry_current_revision();
  return begin_current();
}

void RoutedPlaybackControl::observe() {
  OSCILLATE_TRACE_SPAN("edit.observe");
  settle_live_candidate();
  (void)publisher_.collect_retired();
  // A group is applied when its slowest stage is.
  const auto group_applied = [this] {
    std::uint64_t applied = std::numeric_limits<std::uint64_t>::max();
    for (const auto& member : pending_native_revision_->group)
      applied = std::min(applied, std::max(member.stage->applied_values_revision(),
                                           member.stage->applied_automation_revision()));
    return applied;
  };
  if (pending_native_revision_ && !pending_native_revision_->group.empty()) {
    if (group_applied() >= pending_native_revision_->snapshot->revision) {
      // The running stages now hold the offered settings, so their saved
      // identities follow.
      bool chains = true;
      for (const auto& member : pending_native_revision_->group) {
        const auto track = std::ranges::find(pending_native_revision_->snapshot->tracks,
                                             member.track, &project::Track::id);
        auto* running = active_stage(member.track, member.slot);
        if (!running || track == pending_native_revision_->snapshot->tracks.end()) {
          chains = false;
          continue;
        }
        const auto effect =
            std::ranges::find(track->effects, member.slot, &project::EffectInsert::id);
        if (effect == track->effects.end()) {
          chains = false;
          continue;
        }
        running->saved.processor = effect->processor;
      }
      if (chains) {
        published_snapshot_ = std::move(pending_native_revision_->snapshot);
        (void)revisions_.observe_active_graph(published_snapshot_->revision);
      } else {
        (void)revisions_.fail_revision(
            pending_native_revision_->snapshot->revision,
            {ErrorCode::internal, "Applied linked revision lost an active track chain"});
      }
      pending_native_revision_.reset();
    }
  } else if (pending_native_revision_ &&
             std::visit(
                 [](auto* runtime) {
                   using Runtime = std::remove_pointer_t<decltype(runtime)>;
                   if constexpr (std::is_same_v<Runtime, native_dsp::synth::NativeSynthRuntime>)
                     return runtime->applied_revision();
                   else if constexpr (std::is_same_v<Runtime, native_dsp::synth::NativeSynthPlayer>)
                     return runtime->applied_automation_revision();
                   else if constexpr (std::is_same_v<Runtime, native_inserts::NativeEffectStage>)
                     // A revision offers either values or lanes; the other stays older.
                     return std::max(runtime->applied_values_revision(),
                                     runtime->applied_automation_revision());
                   else
                     return runtime->applied_prepared_revision();
                 },
                 pending_native_revision_->runtime) >=
                 pending_native_revision_->snapshot->revision) {
    if (std::holds_alternative<native_dsp::synth::NativeSynthRuntime*>(
            pending_native_revision_->runtime) ||
        std::holds_alternative<native_dsp::synth::NativeSynthPlayer*>(
            pending_native_revision_->runtime)) {
      published_snapshot_ = std::move(pending_native_revision_->snapshot);
      source_snapshot_ = published_snapshot_;
      (void)revisions_.observe_active_graph(published_snapshot_->revision);
      // A Lock switch or a followed track's values changed on one synth.
      refresh_synth_locks();
    } else {
      if (auto* chain = active_chain(pending_native_revision_->track)) {
        // The running stages now hold the offered values, so their saved
        // identities follow. A lane-only effect edit carries no slots.
        for (const auto& effect : pending_native_revision_->effects) {
          for (auto& stage : chain->chain.stages) {
            if (stage.saved.slot == effect.id)
              stage.saved.processor = effect.processor;
          }
        }
        published_snapshot_ = std::move(pending_native_revision_->snapshot);
        (void)revisions_.observe_active_graph(published_snapshot_->revision);
      } else {
        (void)revisions_.fail_revision(
            pending_native_revision_->snapshot->revision,
            {ErrorCode::internal, "Applied native revision lost its active track chain"});
      }
    }
    pending_native_revision_.reset();
  }
  (void)revisions_.observe_active_graph(publisher_.active_revision());
  if (retry_live_after_ack_ && !pending_native_revision_ && live_source_) {
    const auto current = model_.snapshot();
    if (current->revision > revisions_.snapshot().active_graph_revision) {
      auto retried = try_queue_live_synth(current);
      if (retried && !retried.value())
        retried = try_queue_live_synth_automation(current);
      if (retried && !retried.value())
        retried = try_queue_live_note_effects(current);
      if (retried && !retried.value())
        retried = try_queue_live_effect_automation(current);
      if (retried && retried.value()) {
        (void)revisions_.retry_current_revision();
        retry_live_after_ack_ = false;
      } else if (retried && !retried.value()) {
        retry_live_after_ack_ = false; // Saved edit is incompatible; retain failed status.
      }
    } else {
      retry_live_after_ack_ = false;
    }
  }
}

} // namespace oscillate::application
