#include "oscillate/project/snapshot.hpp"

#include <cstddef>
#include <string>
#include <variant>
#include <vector>

namespace oscillate::project {
namespace {

// The heap bytes of a vector's elements; their own heap content is added by
// the caller.
template <typename T> [[nodiscard]] std::size_t elements(const std::vector<T>& values) noexcept {
  return values.size() * sizeof(T);
}

[[nodiscard]] std::size_t text(const std::string& value) noexcept { return value.size(); }

[[nodiscard]] std::size_t bytes(const std::vector<NativeInstrumentParameter>& parameters) noexcept {
  std::size_t total = elements(parameters);
  for (const auto& parameter : parameters)
    total += text(parameter.id);
  return total;
}

[[nodiscard]] std::size_t bytes(const AutomationTarget& target) noexcept {
  if (const auto* native = std::get_if<NativeParameterTarget>(&target))
    return text(native->parameter_id);
  if (const auto* effect = std::get_if<NativeEffectParameterTarget>(&target))
    return text(effect->parameter_id);
  return 0;
}

[[nodiscard]] std::size_t bytes(const std::vector<AutomationLane>& lanes) noexcept {
  std::size_t total = elements(lanes);
  for (const auto& lane : lanes)
    total += bytes(lane.target) + text(lane.domain.semantic_key) + text(lane.domain.unit) +
             elements(lane.points);
  return total;
}

[[nodiscard]] std::size_t bytes(const ManagedAudioReference& audio) noexcept {
  return text(audio.sha256);
}

[[nodiscard]] std::size_t bytes(const NativeSynthDocument& document) noexcept {
  std::size_t total = elements(document.routes) + elements(document.curves) +
                      elements(document.chains) + elements(document.sequences) +
                      elements(document.media) + elements(document.labels) +
                      elements(document.regions) + elements(document.voice_lanes);
  for (const auto& route : document.routes)
    total += text(route.source) + text(route.aux) + text(route.destination);
  for (const auto& curve : document.curves)
    total += text(curve.id) + elements(curve.points);
  for (const auto& chain : document.chains) {
    total += text(chain.id) + elements(chain.modules);
    for (const auto& module : chain.modules)
      total += text(module.module);
  }
  for (const auto& sequence : document.sequences)
    total += text(sequence.id) + elements(sequence.notes);
  for (const auto& media : document.media) {
    total += text(media.id);
    if (const auto* builtin = std::get_if<NativeBuiltinMedia>(&media.media))
      total += text(builtin->name);
    else if (const auto* managed = std::get_if<ManagedAudioReference>(&media.media))
      total += bytes(*managed);
  }
  for (const auto& label : document.labels)
    total += text(label.id) + text(label.text);
  for (const auto& region : document.regions)
    total += text(region.media) + text(region.tag) + text(region.name);
  for (const auto& lane : document.voice_lanes)
    total += text(lane.id) + elements(lane.values);
  return total;
}

[[nodiscard]] std::size_t bytes(const Vst3PluginInstance& plugin) noexcept {
  return text(plugin.class_id) + text(plugin.vendor) + text(plugin.name) + text(plugin.version) +
         text(plugin.bundle_path) + text(plugin.binary_sha256) + elements(plugin.component_state) +
         elements(plugin.parameters) +
         (plugin.controller_state ? elements(*plugin.controller_state) : 0);
}

[[nodiscard]] std::size_t bytes(const InstrumentSource& source) noexcept {
  if (const auto* plugin = std::get_if<Vst3PluginInstance>(&source))
    return bytes(*plugin);
  if (const auto* patch = std::get_if<NativeInstrumentPatch>(&source))
    return text(patch->engine) + bytes(patch->parameters) + bytes(patch->document);
  return 0;
}

[[nodiscard]] std::size_t bytes(const std::vector<EffectInsert>& effects) noexcept {
  std::size_t total = elements(effects);
  for (const auto& effect : effects) {
    if (const auto* plugin = std::get_if<Vst3PluginInstance>(&effect.processor))
      total += bytes(*plugin);
    else if (const auto* patch = std::get_if<NativeEffectPatch>(&effect.processor))
      total += text(patch->engine) + bytes(patch->parameters) + bytes(patch->document) +
               text(patch->link);
  }
  return total;
}

[[nodiscard]] std::size_t bytes(const Clip& clip) noexcept {
  std::size_t total =
      bytes(clip.automation) + elements(clip.processing.reviewed_warp_markers) + text(clip.name);
  if (clip.clay)
    total += elements(clip.clay->ops) + elements(clip.clay->edits);
  return total;
}

[[nodiscard]] std::size_t bytes(const MidiClip& clip) noexcept {
  std::size_t total = elements(clip.notes) + bytes(clip.automation) + elements(clip.pedals);
  for (const auto& pedal : clip.pedals)
    total += elements(pedal.points);
  return total;
}

[[nodiscard]] std::size_t bytes(const Track& track) noexcept {
  std::size_t total = elements(track.clips) + elements(track.midi_clips) + elements(track.sends) +
                      elements(track.note_effects) + elements(track.macros) + text(track.name) +
                      bytes(track.instrument_source) + bytes(track.effects) +
                      bytes(track.automation);
  for (const auto& clip : track.clips)
    total += bytes(clip);
  for (const auto& clip : track.midi_clips)
    total += bytes(clip);
  for (const auto& effect : track.note_effects)
    total += text(effect.engine) + bytes(effect.parameters);
  for (const auto& macro : track.macros) {
    total += text(macro.name) + elements(macro.mappings);
    for (const auto& mapping : macro.mappings)
      total += text(mapping.domain.semantic_key) + text(mapping.domain.unit);
  }
  if (track.frozen)
    total += bytes(track.frozen->audio);
  return total;
}

} // namespace

std::size_t estimated_bytes(const Snapshot& snapshot) noexcept {
  std::size_t total = sizeof(Snapshot) + elements(snapshot.tempo.events()) +
                      elements(snapshot.meter.events()) + elements(snapshot.tracks) +
                      elements(snapshot.sources) + elements(snapshot.source_tempo_reviews) +
                      elements(snapshot.source_key_reviews) + elements(snapshot.markers) +
                      bytes(snapshot.master_effects);
  for (const auto& track : snapshot.tracks)
    total += bytes(track);
  for (const auto& source : snapshot.sources)
    total += text(source.sha256) +
             (source.external_link ? text(source.external_link->last_known_path) : 0);
  for (const auto& review : snapshot.source_tempo_reviews)
    total += elements(review.generated_beat_frames) + elements(review.user_beat_frames) +
             elements(review.generated_onset_frames) + elements(review.dismissed_onset_frames);
  for (const auto& review : snapshot.source_key_reviews)
    total += elements(review.compatible);
  for (const auto& marker : snapshot.markers)
    total += text(marker.name);
  return total;
}

} // namespace oscillate::project
