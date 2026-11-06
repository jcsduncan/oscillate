#include "oscillate/application/history_labels.hpp"

#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/native_dsp/synth/runtime.hpp"

#include <algorithm>
#include <cstddef>
#include <map>
#include <ranges>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace oscillate::application {
namespace {

// One thing a step did. `object` names a track or a device on one; a step
// touching many tracks the same way is summarised by its action.
struct Phrase {
  std::string action;
  std::string object;
  // How the object joins the action: " on " for something on a track,
  // " " for a track itself ("Rename track Bass").
  std::string_view join = " on ";
};

std::string track_label(const project::Snapshot& snapshot, project::TrackId id) {
  const auto found = std::ranges::find(snapshot.tracks, id, &project::Track::id);
  if (found == snapshot.tracks.end())
    return "a track";
  if (!found->name.empty())
    return found->name;
  return "Track " + std::to_string(found - snapshot.tracks.begin() + 1);
}

std::string native_effect_name(std::string_view engine) {
  if (const auto* found = native_dsp::effects::find_engine(engine);
      found != nullptr && found->descriptor != nullptr)
    return std::string(found->descriptor->name);
  return std::string(engine);
}

std::string effect_name(const project::EffectInsert& insert) {
  return std::visit(
      [](const auto& processor) -> std::string {
        using T = std::decay_t<decltype(processor)>;
        if constexpr (std::is_same_v<T, project::Vst3PluginInstance>)
          return processor.name.empty() ? std::string("plug-in") : processor.name;
        else if constexpr (std::is_same_v<T, project::NativeConvolution>)
          return "Convolution";
        else
          return native_effect_name(processor.engine);
      },
      insert.processor);
}

std::string instrument_name(const project::InstrumentSource& source) {
  return std::visit(
      [](const auto& instrument) -> std::string {
        using T = std::decay_t<decltype(instrument)>;
        if constexpr (std::is_same_v<T, std::monostate>)
          return "instrument";
        else if constexpr (std::is_same_v<T, project::Vst3PluginInstance>)
          return instrument.name.empty() ? std::string("plug-in") : instrument.name;
        else {
          const auto* engine = native_dsp::synth::find_engine(instrument.engine);
          return engine != nullptr && engine->descriptor != nullptr
                     ? std::string(engine->descriptor->name)
                     : instrument.engine;
        }
      },
      source);
}

void describe_effects(std::span<const project::EffectInsert> before,
                      std::span<const project::EffectInsert> after, const std::string& where,
                      std::vector<Phrase>& out) {
  bool changed = false;
  for (const auto& insert : after) {
    const auto old = std::ranges::find(before, insert.id, &project::EffectInsert::id);
    if (old == before.end()) {
      out.push_back({"Add " + effect_name(insert), where});
      changed = true;
    } else if (old->enabled != insert.enabled) {
      out.push_back({(insert.enabled ? "Turn on " : "Bypass ") + effect_name(insert), where});
      changed = true;
    } else if (old->processor != insert.processor) {
      out.push_back({"Edit " + effect_name(insert), where});
      changed = true;
    }
  }
  for (const auto& insert : before) {
    if (std::ranges::find(after, insert.id, &project::EffectInsert::id) == after.end()) {
      out.push_back({"Remove " + effect_name(insert), where});
      changed = true;
    }
  }
  if (!changed && !std::ranges::equal(before, after))
    out.push_back({"Reorder effects", where});
}

// A clip's change, most specific first.
std::string_view audio_clip_change(const project::Clip& before, const project::Clip& after) {
  if (before.placement != after.placement && before.source_range == after.source_range)
    return "Move clip";
  if (before.source_range != after.source_range)
    return "Trim clip";
  if (before.fades != after.fades)
    return "Edit fades";
  if (before.processing != after.processing)
    return "Edit warp";
  if (before.automation != after.automation)
    return "Edit clip automation";
  if (before.name != after.name)
    return "Rename clip";
  if (before.color != after.color)
    return "Colour clip";
  return "Edit clip";
}

std::string_view midi_clip_change(const project::MidiClip& before, const project::MidiClip& after) {
  if (before.notes != after.notes)
    return before.placement != after.placement ? "Edit clip" : "Edit notes";
  if (before.placement != after.placement || before.pattern_length != after.pattern_length)
    return before.pattern_length != after.pattern_length ? "Resize clip" : "Move clip";
  if (before.loop != after.loop)
    return "Loop clip";
  if (before.pedals != after.pedals)
    return "Edit pedals";
  if (before.automation != after.automation)
    return "Edit clip automation";
  if (before.color != after.color)
    return "Colour clip";
  return "Edit clip";
}

template <class ClipT, class Describe>
void describe_clips(const std::vector<ClipT>& before, const std::vector<ClipT>& after,
                    const std::string& where, Describe describe, std::vector<Phrase>& out) {
  std::size_t added = 0;
  std::size_t removed = 0;
  std::map<std::string_view, std::size_t> edits;
  for (const auto& clip : after) {
    const auto old = std::ranges::find(before, clip.id, &ClipT::id);
    if (old == before.end())
      ++added;
    else if (*old != clip)
      ++edits[describe(*old, clip)];
  }
  for (const auto& clip : before)
    if (std::ranges::find(after, clip.id, &ClipT::id) == after.end())
      ++removed;
  // A split or a cut leaves one clip changed and adds or removes another.
  if (added != 0 && removed == 0 && edits.size() == 1 && edits.begin()->second == 1 && added == 1 &&
      edits.begin()->first != "Edit notes") {
    out.push_back({"Split clip", where});
    return;
  }
  const auto count = [](std::string_view verb, std::size_t n) {
    return n == 1 ? std::string(verb) + " clip"
                  : std::string(verb) + " " + std::to_string(n) + " clips";
  };
  if (added != 0)
    out.push_back({count("Add", added), where});
  if (removed != 0)
    out.push_back({count("Delete", removed), where});
  for (const auto& [label, n] : edits) {
    if (n == 1 || !label.ends_with(" clip")) {
      out.push_back({std::string(label), where});
    } else {
      const auto verb = label.substr(0, label.size() - 5);
      out.push_back({count(verb, n), where});
    }
  }
}

void describe_track(const project::Track& before, const project::Track& after,
                    const std::string& where, std::vector<Phrase>& out) {
  const auto first = out.size();
  if (before.name != after.name)
    out.push_back({"Rename track", after.name.empty() ? where : after.name, " "});
  if (before.color != after.color)
    out.push_back({"Colour track", where, " "});
  if (before.muted != after.muted)
    out.push_back({after.muted ? "Mute" : "Unmute", where, " "});
  if (before.soloed != after.soloed)
    out.push_back({after.soloed ? "Solo" : "Unsolo", where, " "});
  if (before.level != after.level)
    out.push_back({"Change level", where});
  if (before.pan != after.pan)
    out.push_back({"Change pan", where});
  if (before.instrument_source != after.instrument_source) {
    const auto* old_native = std::get_if<project::NativeInstrumentPatch>(&before.instrument_source);
    const auto* new_native = std::get_if<project::NativeInstrumentPatch>(&after.instrument_source);
    const bool same_kind = before.instrument_source.index() == after.instrument_source.index() &&
                           (old_native == nullptr || old_native->engine == new_native->engine);
    out.push_back(
        {(same_kind ? "Edit " : "Load ") + instrument_name(after.instrument_source), where});
  }
  describe_effects(before.effects, after.effects, where, out);
  describe_clips(before.clips, after.clips, where, audio_clip_change, out);
  describe_clips(before.midi_clips, after.midi_clips, where, midi_clip_change, out);
  if (before.sends != after.sends) {
    const bool keys = std::ranges::any_of(after.sends, [](const project::SendRoute& send) {
      return send.purpose == project::SendPurpose::sidechain;
    });
    out.push_back({before.sends.size() < after.sends.size() && keys ? "Add sidechain"
                   : before.sends.size() < after.sends.size()       ? "Add send"
                   : before.sends.size() > after.sends.size()       ? "Remove send"
                                                                    : "Edit sends",
                   where});
  }
  if (before.main_output != after.main_output)
    out.push_back({"Route output", where});
  if (before.automation != after.automation)
    out.push_back({"Edit automation", where});
  // A change this list does not name yet still reads as the track's.
  if (out.size() == first)
    out.push_back({"Edit track", where, " "});
}

void describe_markers(const project::Snapshot& before, const project::Snapshot& after,
                      std::vector<Phrase>& out) {
  if (before.markers == after.markers)
    return;
  if (after.markers.size() > before.markers.size()) {
    out.push_back({"Add marker", {}});
  } else if (after.markers.size() < before.markers.size()) {
    out.push_back({"Delete marker", {}});
  } else {
    const bool renamed = std::ranges::any_of(after.markers, [&](const project::Marker& marker) {
      const auto old = std::ranges::find(before.markers, marker.id, &project::Marker::id);
      return old != before.markers.end() && old->name != marker.name;
    });
    out.push_back({renamed ? "Rename marker" : "Move marker", {}});
  }
}

std::string render(const Phrase& phrase) {
  if (phrase.object.empty())
    return phrase.action;
  return phrase.action + std::string(phrase.join) + phrase.object;
}

std::string summarise(const std::vector<Phrase>& phrases) {
  if (phrases.empty())
    return "Edit";
  if (phrases.size() == 1)
    return render(phrases.front());
  // The commonest action names the step; a gesture across tracks (link,
  // duck) reads as one action on N of them.
  std::map<std::string, std::vector<std::string>> by_action;
  for (const auto& phrase : phrases)
    by_action[phrase.action].push_back(phrase.object);
  const auto commonest = std::ranges::max_element(
      by_action, {}, [](const auto& entry) { return entry.second.size(); });
  const auto& [action, objects] = *commonest;
  std::string text;
  if (objects.size() == 1) {
    text = render(*std::ranges::find(phrases, action, &Phrase::action));
  } else {
    std::vector<std::string> distinct = objects;
    std::ranges::sort(distinct);
    distinct.erase(std::ranges::unique(distinct).begin(), distinct.end());
    text = distinct.size() == 1 ? action + " on " + distinct.front()
                                : action + " on " + std::to_string(distinct.size()) + " tracks";
  }
  if (objects.size() != phrases.size())
    text += " and more";
  return text;
}

} // namespace

std::string describe_change(const project::Snapshot& before, const project::Snapshot& after) {
  std::vector<Phrase> phrases;
  // Tracks: added, deleted, moved, then each one's own changes.
  std::vector<std::string> added;
  for (const auto& track : after.tracks)
    if (std::ranges::find(before.tracks, track.id, &project::Track::id) == before.tracks.end())
      added.push_back(track_label(after, track.id));
  std::vector<std::string> deleted;
  for (const auto& track : before.tracks)
    if (std::ranges::find(after.tracks, track.id, &project::Track::id) == after.tracks.end())
      deleted.push_back(track_label(before, track.id));
  if (added.size() == 1)
    phrases.push_back({"Add track", added.front(), " "});
  else if (!added.empty())
    phrases.push_back({"Add " + std::to_string(added.size()) + " tracks", {}});
  if (deleted.size() == 1)
    phrases.push_back({"Delete track", deleted.front(), " "});
  else if (!deleted.empty())
    phrases.push_back({"Delete " + std::to_string(deleted.size()) + " tracks", {}});
  if (added.empty() && deleted.empty() && before.tracks.size() == after.tracks.size() &&
      !std::ranges::equal(before.tracks, after.tracks, {}, &project::Track::id,
                          &project::Track::id))
    phrases.push_back({"Move track", {}});
  for (const auto& track : after.tracks) {
    const auto old = std::ranges::find(before.tracks, track.id, &project::Track::id);
    if (old != before.tracks.end() && *old != track)
      describe_track(*old, track, track_label(after, track.id), phrases);
  }
  // Song-wide settings.
  if (before.tempo.revision() != after.tempo.revision())
    phrases.push_back({"Change tempo", {}});
  if (before.meter.revision() != after.meter.revision())
    phrases.push_back({"Change time signature", {}});
  if (before.key != after.key)
    phrases.push_back({after.key ? "Set song key" : "Clear song key", {}});
  if (before.lead_bars != after.lead_bars)
    phrases.push_back({"Change bars before bar 1", {}});
  if (before.master_level != after.master_level)
    phrases.push_back({"Change master level", {}});
  describe_effects(before.master_effects, after.master_effects, "the master", phrases);
  describe_markers(before, after, phrases);
  if (phrases.empty()) {
    if (before.sources != after.sources)
      phrases.push_back({"Import audio", {}});
    else if (before.source_tempo_reviews != after.source_tempo_reviews)
      phrases.push_back({"Review tempo", {}});
    else if (before.source_key_reviews != after.source_key_reviews)
      phrases.push_back({"Review key", {}});
    else if (before.sample_rate != after.sample_rate)
      phrases.push_back({"Change sample rate", {}});
  }
  return summarise(phrases);
}

} // namespace oscillate::application
