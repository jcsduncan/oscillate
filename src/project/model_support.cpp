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

[[nodiscard]] Error invalid(std::string message) {
  return {ErrorCode::invalid_argument, std::move(message)};
}

[[nodiscard]] Error conflict(std::string message) {
  return {ErrorCode::conflict, std::move(message)};
}

[[nodiscard]] Error exhausted(std::string message) {
  return {ErrorCode::resource_exhausted, std::move(message)};
}

[[nodiscard]] Result<InsertSlotId> allocate_insert_slot(Snapshot& snapshot) {
  auto id = command_insert_slot_id(snapshot.next_insert_slot_serial);
  if (!id)
    return id;
  ++snapshot.next_insert_slot_serial;
  return id;
}

[[nodiscard]] const NativeInstrumentPatch* native_source(const Track& track) {
  return std::get_if<NativeInstrumentPatch>(&track.instrument_source);
}

// Engine and parameter ids: lowercase ASCII letters, digits, '.', '_' and '-'.
[[nodiscard]] bool valid_native_identifier(std::string_view id) {
  return !id.empty() && id.size() <= max_native_identifier_length &&
         std::ranges::all_of(id, [](char c) {
           return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                  c == '-';
         });
}

// Built-in effect lanes belong to one slot's engine. Removing the slot or
// changing its engine removes them, and macro mappings to it. A note effect's
// slot is handled the same way.
void remove_effect_automation(Track& track, InsertSlotId slot, ChangeSet& changes) {
  remove_macro_mappings(track, [&](const MacroDestination& destination) {
    const auto* effect = std::get_if<NativeEffectParameterTarget>(&destination);
    return effect != nullptr && effect->slot == slot;
  });
  const auto refers_to_slot = [&](const AutomationLane& lane) {
    const auto* target = std::get_if<NativeEffectParameterTarget>(&lane.target);
    if (target && target->slot == slot) {
      add_unique(changes.automation_lanes, lane.id);
      return true;
    }
    return false;
  };
  std::erase_if(track.automation, refers_to_slot);
  for (auto& clip : track.clips) {
    std::erase_if(clip.automation, refers_to_slot);
  }
  for (auto& clip : track.midi_clips) {
    std::erase_if(clip.automation, refers_to_slot);
  }
  changes.automation |= !changes.automation_lanes.empty();
}

[[nodiscard]] std::vector<const Vst3PluginInstance*> track_plugins(const Track& track) {
  std::vector<const Vst3PluginInstance*> plugins;
  if (const auto* source = std::get_if<Vst3PluginInstance>(&track.instrument_source)) {
    plugins.push_back(source);
  }
  for (const auto& insert : track.effects) {
    if (const auto* plugin = std::get_if<Vst3PluginInstance>(&insert.processor)) {
      plugins.push_back(plugin);
    }
  }
  return plugins;
}

[[nodiscard]] bool lowercase_hex(std::string_view value, std::size_t length) {
  return value.size() == length && std::ranges::all_of(value, [](char ch) {
           return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
         });
}

[[nodiscard]] bool valid_beat_frames(const std::vector<std::uint64_t>& markers,
                                     std::uint64_t source_frames) {
  if (markers.size() > max_tempo_markers_per_source) {
    return false;
  }
  std::uint64_t previous = 0;
  for (std::size_t index = 0; index < markers.size(); ++index) {
    if (markers[index] >= source_frames || (index != 0 && markers[index] <= previous)) {
      return false;
    }
    previous = markers[index];
  }
  return true;
}

[[nodiscard]] bool valid_key(MusicalKey key) {
  return key.tonic < foundation::semitones_per_octave &&
         (key.mode == KeyMode::major || key.mode == KeyMode::minor);
}

[[nodiscard]] bool valid_text(std::string_view value, std::size_t limit, bool nonempty) {
  if (value.size() > limit || (nonempty && value.empty())) {
    return false;
  }
  for (std::size_t i = 0; i < value.size();) {
    const auto first = static_cast<unsigned char>(value[i]);
    if (first < 0x20 || first == 0x7f) {
      return false;
    }
    if (first < 0x80) {
      ++i;
      continue;
    }
    const std::size_t width = first >= 0xc2 && first <= 0xdf   ? 2
                              : first >= 0xe0 && first <= 0xef ? 3
                              : first >= 0xf0 && first <= 0xf4 ? 4
                                                               : 0;
    if (width == 0 || i + width > value.size()) {
      return false;
    }
    const auto second = static_cast<unsigned char>(value[i + 1]);
    if ((static_cast<unsigned int>(second) & 0xc0U) != 0x80 || (first == 0xe0 && second < 0xa0) ||
        (first == 0xed && second > 0x9f) || (first == 0xf0 && second < 0x90) ||
        (first == 0xf4 && second > 0x8f)) {
      return false;
    }
    for (std::size_t j = 2; j < width; ++j) {
      if ((static_cast<unsigned int>(static_cast<unsigned char>(value[i + j])) & 0xc0U) != 0x80) {
        return false;
      }
    }
    i += width;
  }
  return true;
}

[[nodiscard]] Result<void>
validate_parameters(const std::vector<Vst3ParameterReference>& parameters) {
  if (parameters.size() > max_vst3_parameter_references) {
    return Result<void>::failure(exhausted("VST3 parameter reference limit exceeded"));
  }
  std::set<std::uint32_t> ids;
  for (const auto& parameter : parameters) {
    if (!ids.insert(parameter.id).second || !std::isfinite(parameter.normalized_value) ||
        parameter.normalized_value < 0.0 || parameter.normalized_value > 1.0) {
      return Result<void>::failure(invalid("invalid VST3 parameter reference"));
    }
  }
  return Result<void>::success();
}

[[nodiscard]] Result<void> validate_plugin(const Vst3PluginInstance& plugin) {
  bool valid_path = !plugin.bundle_path.empty() && plugin.bundle_path.front() == '/' &&
                    plugin.bundle_path.ends_with(".vst3");
  for (std::size_t start = 1; valid_path && start < plugin.bundle_path.size();) {
    const auto end = plugin.bundle_path.find('/', start);
    const auto segment = std::string_view(plugin.bundle_path).substr(start, end - start);
    if (segment.empty() || segment == "." || segment == "..") {
      valid_path = false;
      break;
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  if (!lowercase_hex(plugin.class_id, 32) || !lowercase_hex(plugin.binary_sha256, 64) ||
      !valid_text(plugin.vendor, 128, true) || !valid_text(plugin.name, 128, true) ||
      !valid_text(plugin.version, 128, false) ||
      !valid_text(plugin.bundle_path, max_vst3_bundle_path_bytes, true) || !valid_path ||
      plugin.component_state.size() > max_vst3_state_bytes ||
      (plugin.controller_state && plugin.controller_state->size() > max_vst3_state_bytes)) {
    return Result<void>::failure(invalid("invalid VST3 plugin identity or state"));
  }
  return validate_parameters(plugin.parameters);
}

[[nodiscard]] Result<void> validate_source(const SourceRecord& source) {
  if (source.external_link) {
    const auto& path = source.external_link->last_known_path;
    if (path.empty() || path.size() > max_external_source_path_bytes || path.front() != '/' ||
        path.back() == '/' || path.find("//") != std::string::npos) {
      return Result<void>::failure(invalid("invalid external source path"));
    }
    std::size_t start = 1;
    while (start < path.size()) {
      const auto end = path.find('/', start);
      const auto component = std::string_view(path).substr(start, end - start);
      if (component == "." || component == "..")
        return Result<void>::failure(invalid("invalid external source path"));
      if (end == std::string::npos)
        break;
      start = end + 1;
    }
    // JSON requires UTF-8; reject malformed or control bytes before serialization.
    for (std::size_t index = 0; index < path.size();) {
      const auto first = static_cast<unsigned char>(path[index]);
      if (first < 0x20 || first == 0x7f)
        return Result<void>::failure(invalid("invalid external source path"));
      if (first < 0x80) {
        ++index;
        continue;
      }
      const std::size_t count = first >= 0xf0 && first <= 0xf4   ? 4
                                : first >= 0xe0 && first <= 0xef ? 3
                                : first >= 0xc2 && first <= 0xdf ? 2
                                                                 : 0;
      if (count == 0 || index + count > path.size())
        return Result<void>::failure(invalid("invalid external source path"));
      const auto second = static_cast<unsigned char>(path[index + 1]);
      if ((count == 3 && ((first == 0xe0 && second < 0xa0) || (first == 0xed && second >= 0xa0))) ||
          (count == 4 && ((first == 0xf0 && second < 0x90) || (first == 0xf4 && second >= 0x90))))
        return Result<void>::failure(invalid("invalid external source path"));
      for (std::size_t tail = 1; tail < count; ++tail) {
        if ((static_cast<unsigned int>(static_cast<unsigned char>(path[index + tail])) & 0xc0U) !=
            0x80)
          return Result<void>::failure(invalid("invalid external source path"));
      }
      index += count;
    }
  }
  if (source.encoding != SourceEncoding::wav && source.encoding != SourceEncoding::aiff &&
      source.encoding != SourceEncoding::flac && source.encoding != SourceEncoding::mp3) {
    return Result<void>::failure(invalid("unsupported source encoding"));
  }
  if (source.sha256.size() != 64 || !std::ranges::all_of(source.sha256, [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
      })) {
    return Result<void>::failure(invalid("invalid source SHA-256"));
  }
  if ((source.encoding == SourceEncoding::wav ? source.byte_length < 44
                                              : source.byte_length == 0) ||
      source.sample_rate < 8'000 || source.sample_rate > 384'000 || source.channels == 0 ||
      source.channels > 32 || source.frames == 0 ||
      source.frames > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return Result<void>::failure(invalid("invalid source media metadata"));
  }
  return Result<void>::success();
}

// The managed (not externally linked) source a reference names, when its
// recorded identity still matches; otherwise null.
[[nodiscard]] const SourceRecord* managed_audio_source(const ManagedAudioReference& reference,
                                                       const Snapshot& snapshot) {
  const auto source = std::ranges::find(snapshot.sources, reference.source, &SourceRecord::id);
  if (source == snapshot.sources.end() || source->external_link ||
      source->sha256 != reference.sha256 || source->byte_length != reference.encoded_bytes ||
      source->sample_rate != reference.sample_rate || source->channels != reference.channels ||
      source->frames != reference.frames) {
    return nullptr;
  }
  return &*source;
}

[[nodiscard]] bool valid_unit_tension(double value) {
  return std::isfinite(value) && value >= -1.0 && value <= 1.0;
}

// Structure, limits and media identity only (ADR-0057); the engine checks
// which routes, curves, chains, sequences, media, labels, regions and voice
// lanes it accepts.
[[nodiscard]] Result<void> validate_native_document(const NativeSynthDocument& document,
                                                    const Snapshot& snapshot) {
  const auto unique_ids = [](const auto& items) {
    std::set<std::string_view> ids;
    return std::ranges::all_of(items, [&](const auto& item) {
      return valid_native_identifier(item.id) && ids.insert(item.id).second;
    });
  };
  std::set<std::uint32_t> route_slots;
  if (document.routes.size() > max_native_synth_routes ||
      !std::ranges::all_of(document.routes, [&](const NativeModRoute& route) {
        return route.slot < max_native_synth_routes && route_slots.insert(route.slot).second &&
               valid_native_identifier(route.source) &&
               valid_native_identifier(route.destination) &&
               (route.aux.empty() || valid_native_identifier(route.aux)) &&
               valid_unit_tension(route.tension) && std::isfinite(route.scale) &&
               route.scale >= 0.0 && route.scale <= 1.0;
      })) {
    return Result<void>::failure(invalid("invalid synth modulation route"));
  }
  if (document.curves.size() > max_native_synth_curves || !unique_ids(document.curves) ||
      !std::ranges::all_of(document.curves, [](const NativeCurve& curve) {
        const auto& points = curve.points;
        if (points.size() < 2 || points.size() > max_native_curve_points ||
            points.front().x != 0.0 || points.back().x != 1.0)
          return false;
        for (std::size_t index = 0; index < points.size(); ++index) {
          const auto& point = points[index];
          if (!std::isfinite(point.y) || point.y < 0.0 || point.y > 1.0 ||
              !valid_unit_tension(point.tension) ||
              (point.segment != NativeCurveSegment::curve &&
               point.segment != NativeCurveSegment::step) ||
              (index > 0 && !(point.x > points[index - 1].x)))
            return false;
        }
        return true;
      })) {
    return Result<void>::failure(invalid("invalid synth curve"));
  }
  std::set<std::uint32_t> slot_uids;
  if (document.chains.size() > max_native_synth_chains || !unique_ids(document.chains) ||
      !std::ranges::all_of(document.chains, [&](const NativeChain& chain) {
        return chain.modules.size() <= max_native_chain_slots &&
               std::ranges::all_of(chain.modules, [&](const NativeChainSlot& slot) {
                 return valid_native_identifier(slot.module) && slot_uids.insert(slot.uid).second;
               });
      })) {
    return Result<void>::failure(invalid("invalid synth module chain"));
  }
  if (document.sequences.size() > max_native_synth_sequences || !unique_ids(document.sequences) ||
      !std::ranges::all_of(document.sequences, [](const NativeSequence& sequence) {
        return sequence.length.value > 0 && sequence.notes.size() <= max_native_sequence_notes &&
               std::ranges::all_of(sequence.notes, [&](const NativeSequenceNote& note) {
                 return note.start.value >= 0 && note.start.value < sequence.length.value &&
                        note.length.value > 0 && note.key <= foundation::max_midi_key &&
                        note.velocity >= foundation::min_note_velocity &&
                        note.velocity <= foundation::max_note_velocity;
               });
      })) {
    return Result<void>::failure(invalid("invalid synth sequence"));
  }
  if (document.media.size() > max_native_synth_media || !unique_ids(document.media) ||
      !std::ranges::all_of(document.media, [&](const NativeMediaSlot& slot) {
        if (slot.frame_length > 8'192)
          return false;
        if (const auto* builtin = std::get_if<NativeBuiltinMedia>(&slot.media))
          return valid_native_identifier(builtin->name);
        const auto& reference = std::get<ManagedAudioReference>(slot.media);
        return reference.frames <= max_native_synth_media_frames &&
               reference.frames >= slot.frame_length &&
               managed_audio_source(reference, snapshot) != nullptr;
      })) {
    return Result<void>::failure(invalid("invalid synth media slot"));
  }
  if (document.labels.size() > max_native_synth_labels || !unique_ids(document.labels) ||
      !std::ranges::all_of(document.labels, [](const NativeLabel& label) {
        return valid_text(label.text, max_native_label_length, true);
      })) {
    return Result<void>::failure(invalid("invalid synth label"));
  }
  // Regions (ADR-0063): unique slots, each in a media slot of this document,
  // starts strictly increasing within one media in vector order.
  std::set<std::uint32_t> region_slots;
  std::map<std::string_view, std::uint64_t> region_ends;
  if (document.regions.size() > max_native_regions ||
      !std::ranges::all_of(document.regions, [&](const NativeRegion& region) {
        if (!region_slots.insert(region.slot).second ||
            std::ranges::find(document.media, region.media, &NativeMediaSlot::id) ==
                document.media.end() ||
            !valid_text(region.tag, max_native_region_tag, false) ||
            !valid_text(region.name, max_native_region_name, false))
          return false;
        const auto [previous, first] = region_ends.try_emplace(region.media, region.start_frame);
        if (first)
          return true;
        if (!(region.start_frame > previous->second))
          return false;
        previous->second = region.start_frame;
        return true;
      })) {
    return Result<void>::failure(invalid("invalid synth region"));
  }
  // Voice lanes (ADR-0073): unique ids, voices strictly increasing, values
  // finite in -1..1.
  if (document.voice_lanes.size() > max_native_voice_lanes || !unique_ids(document.voice_lanes) ||
      !std::ranges::all_of(document.voice_lanes, [](const NativeVoiceLane& lane) {
        if (lane.values.empty() || lane.values.size() > max_native_voice_lane_values)
          return false;
        for (std::size_t index = 0; index < lane.values.size(); ++index) {
          const auto& value = lane.values[index];
          if (!valid_unit_tension(value.value) ||
              (index > 0 && !(value.voice > lane.values[index - 1].voice)))
            return false;
        }
        return true;
      })) {
    return Result<void>::failure(invalid("invalid synth voice lane"));
  }
  return Result<void>::success();
}

// Structure only: the engine resolves ids, ranges and the document's
// meaning (ADR-0040, ADR-0058). Effects have no media.
[[nodiscard]] Result<void> validate_native_effect_patch(const NativeEffectPatch& effect,
                                                        const Snapshot& snapshot) {
  std::set<std::string_view> ids;
  if (effect.generation == 0 || !valid_native_identifier(effect.engine) ||
      effect.engine_version == 0 || effect.parameters.size() > max_native_instrument_parameters ||
      !std::ranges::all_of(effect.parameters,
                           [&](const NativeInstrumentParameter& p) {
                             return valid_native_identifier(p.id) && std::isfinite(p.value) &&
                                    ids.insert(p.id).second;
                           }) ||
      (!effect.link.empty() && !valid_native_identifier(effect.link))) {
    return Result<void>::failure(invalid("invalid built-in effect patch"));
  }
  if (!effect.document.media.empty()) {
    return Result<void>::failure(invalid("built-in effects have no media"));
  }
  return validate_native_document(effect.document, snapshot);
}

[[nodiscard]] Result<void> validate_native_convolution(const NativeConvolution& effect,
                                                       const Snapshot& snapshot) {
  const auto& ir = effect.ir;
  if (effect.generation == 0 || !std::isfinite(effect.wet) || effect.wet < 0.0 ||
      effect.wet > 1.0 || ir.encoded_bytes < 44 || ir.encoded_bytes > 16ULL * 1024 * 1024 ||
      (ir.sample_rate != 44'100 && ir.sample_rate != 48'000 && ir.sample_rate != 88'200 &&
       ir.sample_rate != 96'000) ||
      (ir.channels != 1 && ir.channels != 2) || ir.frames == 0 ||
      ir.frames > 2ULL * ir.sample_rate) {
    return Result<void>::failure(invalid("invalid convolution IR identity or settings"));
  }
  const auto* source = managed_audio_source(ir, snapshot);
  if (source == nullptr || source->encoding != SourceEncoding::wav) {
    return Result<void>::failure(invalid("convolution IR does not match a managed source record"));
  }
  return Result<void>::success();
}

[[nodiscard]] bool has_clip_id(const Snapshot& state, ClipId id) {
  if (find_clip(state, id) != nullptr) {
    return true;
  }
  return std::ranges::any_of(state.tracks, [id](const Track& track) {
    return std::ranges::any_of(track.midi_clips,
                               [id](const MidiClip& clip) { return clip.id == id; });
  });
}

[[nodiscard]] std::optional<MutableMidiClip> find_mutable_midi_clip(Snapshot& state, ClipId id) {
  for (auto& track : state.tracks) {
    auto it = std::ranges::find_if(track.midi_clips,
                                   [id](const MidiClip& clip) { return clip.id == id; });
    if (it != track.midi_clips.end()) {
      return MutableMidiClip{&track, &*it};
    }
  }
  return {};
}

[[nodiscard]] std::optional<MutableClip> find_mutable_clip(Snapshot& state, ClipId id) {
  for (auto& track : state.tracks) {
    auto it = std::ranges::find_if(track.clips, [id](const Clip& clip) { return clip.id == id; });
    if (it != track.clips.end()) {
      return MutableClip{&track, &*it};
    }
  }
  return {};
}

} // namespace oscillate::project::detail
