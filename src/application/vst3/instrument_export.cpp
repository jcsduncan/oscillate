#include "oscillate/application/vst3/instrument_export.hpp"
#include "oscillate/export/insert_preflight.hpp"
#include "oscillate/export/master_output.hpp"
#include "oscillate/export/processed_clip.hpp"
#include "oscillate/project/clip_fade.hpp"

#include "oscillate/application/routed_playback_control.hpp"
#include "oscillate/application/vst3/instrument_provider.hpp"
#include "oscillate/application/vst3/stage_provider.hpp"
#include "oscillate/application/vst3/track_insert_provider.hpp"
#include "oscillate/automation_project/preparation.hpp"
#include "oscillate/engine/midi_events.hpp"
#include "oscillate/media/audio_reader.hpp"
#include "oscillate/media/source_rate_reader.hpp"
#include "oscillate/native_dsp/synth/player.hpp"
#include "oscillate/native_inserts/native_stage.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/plugins/project_binding.hpp"
#include "oscillate/plugins/vst3_bundle_loader.hpp"
#include "oscillate/source_prep/synth_media.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace oscillate::exporting {
namespace {

constexpr std::uint32_t render_block_frames = 128;
constexpr std::uint64_t max_render_seconds = 3'600;
constexpr std::uint64_t max_tail_seconds = 10;
constexpr std::size_t max_audio_tracks = 8;
constexpr std::size_t max_audio_clips_per_track = 32;
constexpr std::size_t max_audio_sources = 64;
constexpr std::uint32_t max_effect_compensation_frames = 8'192;

enum class SourceSelection : std::uint8_t { master, instrument_stem, audio_stem };

struct PreparedSource {
  project::SourceId id;
  media::SourceRateReader reader;
};

// A raw clip reads sources[source_index] from source_first; a processed clip
// reads its own Q0 warp render (`warp`), whose frame 0 is `first`.
struct PreparedClip {
  std::uint64_t first;
  std::uint64_t last;
  std::uint64_t source_first;
  std::size_t source_index;
  project::PreparedClipFades fades;
  std::unique_ptr<processing::OfflineWarpClip> warp;
};

struct PreparedAudio {
  project::TrackId track;
  std::vector<PreparedClip> clips;
  std::size_t source_slot = 0;
  std::size_t next_clip = 0;
};

[[nodiscard]] bool supported_audio_automation(const project::AutomationLane& lane,
                                              project::TrackId track) noexcept {
  // RoutedPlaybackControl prepares the shared evaluator and binds these lanes
  // to post-insert node gains, including clip-over-track precedence. Reject
  // every other target here so a mixed render cannot silently omit a lane.
  const auto* target = std::get_if<project::TrackControlTarget>(&lane.target);
  return target != nullptr && target->track == track && project::is_channel_control(target->key);
}

class OfflineGate final : public project::TransportGate {
public:
  [[nodiscard]] foundation::Result<std::unique_ptr<project::StoppedTransportLease>>
  acquire_stopped() override {
    return foundation::Result<std::unique_ptr<project::StoppedTransportLease>>::failure(
        {foundation::ErrorCode::conflict, "Offline model cannot acquire a second transport lease"});
  }
};

[[nodiscard]] foundation::Result<PublishOutcome> fail(foundation::ErrorCode code,
                                                      std::string message) {
  return foundation::Result<PublishOutcome>::failure({code, std::move(message)});
}

// A native stage's bounded ring-out; latent stages are the caller's to refuse.
[[nodiscard]] foundation::Result<native_inserts::PreparedStage>
prepare_bounded_stage(const project::Snapshot& snapshot,
                      const project::EffectProcessor& processor) {
  native_inserts::StageContext context{snapshot.sample_rate};
  context.snapshot = &snapshot;
  auto prepared = native_inserts::prepare_native_stage(processor, context);
  if (prepared && !prepared.value().export_tail_frames)
    return foundation::Result<native_inserts::PreparedStage>::failure(*prepared.value().tail_error);
  return prepared;
}

[[nodiscard]] foundation::Result<std::pair<std::uint64_t, std::uint64_t>>
clip_interval(const project::Snapshot& snapshot, const project::Placement& placement) {
  timeline::SamplePosition start{0};
  timeline::SamplePosition end{0};
  if (const auto* musical = std::get_if<project::MusicalInterval>(&placement)) {
    auto first = snapshot.tempo.tick_to_sample(musical->start, snapshot.sample_rate);
    auto last = snapshot.tempo.tick_to_sample(musical->end, snapshot.sample_rate);
    if (!first || !last)
      return foundation::Result<std::pair<std::uint64_t, std::uint64_t>>::failure(
          !first ? first.error() : last.error());
    start = first.value();
    end = last.value();
  } else {
    const auto& elapsed = std::get<project::ElapsedInterval>(placement);
    start = elapsed.start;
    end = elapsed.end;
  }
  if (start.value < 0 || end.value <= start.value)
    return foundation::Result<std::pair<std::uint64_t, std::uint64_t>>::failure(
        {foundation::ErrorCode::unsupported, "Mixed export clip placement is invalid"});
  return foundation::Result<std::pair<std::uint64_t, std::uint64_t>>::success(
      {static_cast<std::uint64_t>(start.value), static_cast<std::uint64_t>(end.value)});
}

[[nodiscard]] foundation::Result<PreparedAudio>
prepare_audio(const project::Snapshot& snapshot, const persistence::ProjectStore& store,
              const project::Track& track, std::vector<PreparedSource>& sources,
              std::optional<project::TrackId> effect_owner) {
  if (track.clips.empty() || track.clips.size() > max_audio_clips_per_track ||
      !track.midi_clips.empty() ||
      !std::holds_alternative<std::monostate>(track.instrument_source) ||
      (!track.effects.empty() && (!effect_owner || *effect_owner != track.id)) ||
      std::ranges::any_of(track.automation, [&track](const auto& lane) {
        return !supported_audio_automation(lane, track.id);
      }))
    return foundation::Result<PreparedAudio>::failure(
        {foundation::ErrorCode::unsupported,
         "Mixed export needs bounded audio clips and backed channel-level automation"});
  PreparedAudio prepared{track.id, {}};
  prepared.clips.reserve(track.clips.size());
  for (const auto& clip : track.clips) {
    if (std::ranges::any_of(clip.automation, [&track](const auto& lane) {
          return !supported_audio_automation(lane, track.id);
        }))
      return foundation::Result<PreparedAudio>::failure(
          {foundation::ErrorCode::unsupported,
           "Mixed export audio clip automation needs a backed channel-level target"});
    const auto source = std::ranges::find_if(
        snapshot.sources, [&clip](const auto& item) { return item.id == clip.source; });
    if (source == snapshot.sources.end() ||
        std::ranges::count_if(snapshot.sources,
                              [&clip](const auto& item) { return item.id == clip.source; }) != 1)
      return foundation::Result<PreparedAudio>::failure(
          {foundation::ErrorCode::corrupt_data, "Mixed export source identity is ambiguous"});
    if (source->encoding != project::SourceEncoding::wav || source->channels < 1 ||
        source->channels > 2 || clip.source_range.start.value < 0 ||
        clip.source_range.end.value <= clip.source_range.start.value ||
        static_cast<std::uint64_t>(clip.source_range.end.value) > source->frames)
      return foundation::Result<PreparedAudio>::failure(
          {foundation::ErrorCode::unsupported, "Mixed export needs bounded managed WAV sources"});
    auto processed = prepare_processed_export_clip(snapshot, store, clip);
    if (!processed)
      return foundation::Result<PreparedAudio>::failure(processed.error());
    if (processed.value()) {
      auto& warped = *processed.value();
      auto fades = project::prepare_clip_fades(clip.fades, warped.last - warped.first);
      if (!fades)
        return foundation::Result<PreparedAudio>::failure(fades.error());
      prepared.clips.push_back(
          {warped.first, warped.last, 0, 0, fades.value(), std::move(warped.reader)});
      continue;
    }
    auto source_first =
        media::source_rate_frame_count(static_cast<std::uint64_t>(clip.source_range.start.value),
                                       source->sample_rate, snapshot.sample_rate);
    auto source_last =
        media::source_rate_frame_count(static_cast<std::uint64_t>(clip.source_range.end.value),
                                       source->sample_rate, snapshot.sample_rate);
    if (!source_first || !source_last)
      return foundation::Result<PreparedAudio>::failure(!source_first ? source_first.error()
                                                                      : source_last.error());
    auto interval = clip_interval(snapshot, clip.placement);
    if (!interval)
      return foundation::Result<PreparedAudio>::failure(interval.error());
    const auto [first, last] = interval.value();
    if (source_last.value() <= source_first.value() ||
        source_last.value() - source_first.value() != last - first)
      return foundation::Result<PreparedAudio>::failure(
          {foundation::ErrorCode::unsupported,
           "Mixed export does not render stretched or warped audio clips"});
    if (clip.clay)
      return foundation::Result<PreparedAudio>::failure(
          {foundation::ErrorCode::unsupported, "Mixed export does not render Clay note edits yet"});
    const auto existing =
        std::ranges::find_if(sources, [&clip](const auto& item) { return item.id == clip.source; });
    std::size_t source_index = sources.size();
    if (existing == sources.end()) {
      if (sources.size() >= max_audio_sources)
        return foundation::Result<PreparedAudio>::failure(
            {foundation::ErrorCode::resource_exhausted, "Mixed export source limit exceeded"});
      auto descriptor = store.open_managed_source(*source);
      if (!descriptor)
        return foundation::Result<PreparedAudio>::failure(descriptor.error());
      auto opened = media::AudioReader::open_fd(descriptor.value(), *source);
      const auto closed = ::close(descriptor.value());
      if (!opened)
        return foundation::Result<PreparedAudio>::failure(opened.error());
      if (closed != 0)
        return foundation::Result<PreparedAudio>::failure(
            {foundation::ErrorCode::io_failure, "Cannot close verified audio descriptor"});
      if (opened.value().sample_rate() != source->sample_rate ||
          opened.value().channels() != source->channels ||
          opened.value().frame_count() != source->frames)
        return foundation::Result<PreparedAudio>::failure(
            {foundation::ErrorCode::corrupt_data, "Managed WAV metadata changed"});
      auto converted =
          media::SourceRateReader::open(std::move(opened).value(), snapshot.sample_rate);
      if (!converted)
        return foundation::Result<PreparedAudio>::failure(converted.error());
      if (source_last.value() > converted.value().frame_count())
        return foundation::Result<PreparedAudio>::failure(
            {foundation::ErrorCode::corrupt_data, "Clip source range exceeds managed media"});
      sources.push_back({source->id, std::move(converted).value()});
    } else {
      source_index = static_cast<std::size_t>(existing - sources.begin());
      if (source_last.value() > sources[source_index].reader.frame_count())
        return foundation::Result<PreparedAudio>::failure(
            {foundation::ErrorCode::corrupt_data, "Clip source range exceeds managed media"});
    }
    auto fades = project::prepare_clip_fades(clip.fades, last - first);
    if (!fades)
      return foundation::Result<PreparedAudio>::failure(fades.error());
    prepared.clips.push_back(
        {first, last, source_first.value(), source_index, fades.value(), nullptr});
  }
  std::ranges::sort(prepared.clips,
                    [](const auto& left, const auto& right) { return left.first < right.first; });
  for (std::size_t index = 1; index < prepared.clips.size(); ++index) {
    if (prepared.clips[index - 1].last > prepared.clips[index].first)
      return foundation::Result<PreparedAudio>::failure(
          {foundation::ErrorCode::unsupported,
           "Mixed export does not support overlapping clips on one track"});
  }
  return foundation::Result<PreparedAudio>::success(std::move(prepared));
}

[[nodiscard]] foundation::Result<PublishOutcome>
render(const project::Snapshot& snapshot, const persistence::ProjectStore* store,
       StoppedExportAuthority authority, plugins::Vst3Scanner& scanner,
       const std::filesystem::path* bundle, const MasterOutputOptions& options,
       SourceSelection selection, std::optional<project::TrackId> stem_track) {
  if (project::has_convolution_insert(snapshot))
    return fail(foundation::ErrorCode::unsupported,
                "Saved convolution insert is not available for VST3 instrument export yet");
  if (!authority.matches(snapshot))
    return fail(foundation::ErrorCode::conflict, "VST3 export stopped authority is stale");
  const bool two_source_project =
      std::ranges::count_if(snapshot.tracks, [](const auto& track) {
        return track.kind == project::TrackKind::instrument &&
               std::holds_alternative<project::Vst3PluginInstance>(track.instrument_source);
      }) == 2;
  auto inserts = detail::preflight_inserts(snapshot, detail::ExportSource::vst3_instrument,
                                           store != nullptr || two_source_project, false, true,
                                           two_source_project);
  if (!inserts)
    return foundation::Result<PublishOutcome>::failure(inserts.error());
  std::vector<const project::Vst3PluginInstance*> effects;
  effects.reserve(inserts.value().track_chain.size());
  for (const auto* slot : inserts.value().track_chain) {
    const auto* effect = std::get_if<project::Vst3PluginInstance>(&slot->processor);
    if (!effect)
      return fail(foundation::ErrorCode::corrupt_data, "Projected VST3 effect has no plugin");
    effects.push_back(effect);
  }
  if (!effects.empty() && !inserts.value().master_order.empty())
    return fail(foundation::ErrorCode::unsupported,
                "Mixed VST3 export cannot render master inserts");
  const auto* wav = std::get_if<MasterRenderOptions>(&options);
  const auto requested_rate =
      std::visit([](const auto& value) { return value.sample_rate; }, options);
  if ((wav && wav->loudness_report != nullptr) ||
      (requested_rate != 0 && !timeline::supported_sample_rate(requested_rate)))
    return fail(foundation::ErrorCode::unsupported,
                "VST3 instrument export needs a supported output rate and no loudness report");
  if (!timeline::supported_sample_rate(snapshot.sample_rate))
    return fail(foundation::ErrorCode::unsupported, "VST3 export project rate is unsupported");
  // The routed master applies active native slots in saved order; their
  // drains add conservatively for either order.
  std::uint64_t master_tail_frames = 0;
  const auto master_tail_limit =
      static_cast<std::uint64_t>(snapshot.sample_rate) * max_render_seconds;
  for (const auto* slot : inserts.value().master_chain) {
    auto prepared = prepare_bounded_stage(snapshot, slot->processor);
    if (!prepared)
      return foundation::Result<PublishOutcome>::failure(prepared.error());
    if (prepared.value().latency_frames != 0)
      return fail(foundation::ErrorCode::unsupported,
                  "VST3 export does not support master effect latency");
    if (*prepared.value().export_tail_frames > master_tail_limit - master_tail_frames)
      return fail(foundation::ErrorCode::resource_exhausted,
                  "VST3 export master effect tail exceeds the one-hour output limit");
    master_tail_frames += *prepared.value().export_tail_frames;
  }

  std::vector<const project::Track*> instruments;
  const project::Track* native_synth = nullptr;
  std::vector<const project::Track*> audio_tracks;
  for (const auto& track : snapshot.tracks) {
    if (track.kind == project::TrackKind::group_bus ||
        track.kind == project::TrackKind::return_bus) {
      if (!track.clips.empty() || !track.midi_clips.empty() ||
          !std::holds_alternative<std::monostate>(track.instrument_source))
        return fail(foundation::ErrorCode::unsupported, "VST3 export bus content is unsupported");
      continue;
    }
    if (track.kind == project::TrackKind::audio && store != nullptr) {
      if (audio_tracks.size() >= max_audio_tracks)
        return fail(foundation::ErrorCode::resource_exhausted,
                    "Mixed export audio track limit exceeded");
      audio_tracks.push_back(&track);
      continue;
    }
    if (track.kind == project::TrackKind::instrument &&
        std::holds_alternative<project::NativeInstrumentPatch>(track.instrument_source)) {
      if (native_synth || !track.clips.empty() || !track.effects.empty() ||
          !track.automation.empty() || std::ranges::any_of(track.midi_clips, [](const auto& clip) {
            return !clip.automation.empty();
          }))
        return fail(foundation::ErrorCode::unsupported,
                    "Mixed native/VST3 export requires one bare native synth source");
      native_synth = &track;
      continue;
    }
    if (track.kind != project::TrackKind::instrument || !track.clips.empty() ||
        !std::holds_alternative<project::Vst3PluginInstance>(track.instrument_source))
      return fail(foundation::ErrorCode::unsupported,
                  "VST3 export requires VST3 instrument source tracks");
    instruments.push_back(&track);
  }
  if (instruments.empty())
    return fail(foundation::ErrorCode::unsupported, "VST3 export has no instrument source");
  if (instruments.size() > 2)
    return fail(foundation::ErrorCode::unsupported,
                "VST3 export supports at most two instrument sources");
  const bool native_with_audio = native_synth && store != nullptr;
  if (native_synth &&
      (instruments.size() != 1 ||
       (native_with_audio ? audio_tracks.size() != 1 : snapshot.tracks.size() != 2) ||
       (snapshot.master_effects.size() > 1 || (snapshot.master_effects.size() == 1 &&
                                               (!snapshot.master_effects.front().enabled ||
                                                !std::holds_alternative<project::NativeEffectPatch>(
                                                    snapshot.master_effects.front().processor)))) ||
       !effects.empty() || !inserts.value().native_track_chains.empty() ||
       std::ranges::any_of(snapshot.tracks,
                           [](const auto& track) {
                             return !track.effects.empty() || !track.automation.empty() ||
                                    std::ranges::any_of(track.midi_clips, [](const auto& clip) {
                                      return !clip.automation.empty();
                                    });
                           }) ||
       (!native_with_audio && std::ranges::any_of(snapshot.tracks, [](const auto& track) {
         return track.main_output || !track.sends.empty();
       }))))
    return fail(foundation::ErrorCode::unsupported,
                "Mixed native/VST3 export requires bare sources, at most one managed audio track, "
                "and at most one built-in master effect");
  const auto* instrument = instruments.front();
  const bool paired_effect_paths =
      instruments.size() == 2 && effects.size() == 2 &&
      inserts.value().track_chain_owner == instruments.front()->id &&
      inserts.value().second_track_chain_owner == instruments.back()->id &&
      instruments.front()->effects.size() == 1 && instruments.back()->effects.size() == 1;
  if (instruments.size() == 2 && (store != nullptr || (!effects.empty() && !paired_effect_paths) ||
                                  !inserts.value().native_track_chains.empty()))
    return fail(foundation::ErrorCode::unsupported,
                "Two VST3 instrument sources require bare paths or one effect per source");
  if (selection == SourceSelection::instrument_stem && !stem_track &&
      (instruments.size() != 1 || native_synth))
    return fail(foundation::ErrorCode::invalid_argument,
                "Multiple instrument sources need an explicit stem track");
  if (effects.size() == 2 && !paired_effect_paths &&
      inserts.value().track_chain_owner != instrument->id)
    return fail(foundation::ErrorCode::unsupported,
                "Two VST3 effects must belong to their instrument source track");
  std::uint64_t native_tail_frames = 0;
  std::uint32_t native_latency_frames = 0;
  if (!effects.empty() && !inserts.value().native_track_chains.empty())
    return fail(foundation::ErrorCode::unsupported,
                "VST3 export cannot combine a second effect path with instrument native inserts");
  for (const auto& chain : inserts.value().native_track_chains) {
    if (chain.owner != instrument->id)
      return fail(foundation::ErrorCode::unsupported,
                  "VST3 export native chain is not on its instrument source path");
    for (const auto* slot : chain.effects) {
      if (!slot->enabled)
        continue;
      auto prepared = prepare_bounded_stage(snapshot, slot->processor);
      if (!prepared)
        return foundation::Result<PublishOutcome>::failure(prepared.error());
      const auto tail = *prepared.value().export_tail_frames;
      if (prepared.value().latency_frames != 0) {
        if (native_latency_frames != 0)
          return fail(foundation::ErrorCode::unsupported,
                      "VST3 instrument native chain supports one latent stage");
        native_latency_frames = prepared.value().latency_frames;
      }
      if (tail >
          static_cast<std::uint64_t>(snapshot.sample_rate) * max_tail_seconds - native_tail_frames)
        return fail(foundation::ErrorCode::resource_exhausted,
                    "VST3 instrument native chain tail exceeds ten seconds");
      native_tail_frames += tail;
    }
  }
  if (native_latency_frames > max_effect_compensation_frames)
    return fail(foundation::ErrorCode::resource_exhausted,
                "VST3 instrument native latency exceeds 8192 frames");
  if (store != nullptr && audio_tracks.empty())
    return fail(foundation::ErrorCode::unsupported,
                "Mixed export requires a verified WAV audio source");
  if (stem_track &&
      ((selection == SourceSelection::instrument_stem &&
        std::ranges::none_of(
            instruments, [stem_track](const auto* track) { return track->id == *stem_track; }) &&
        (!native_synth || native_synth->id != *stem_track)) ||
       (selection == SourceSelection::audio_stem &&
        std::ranges::none_of(
            audio_tracks, [stem_track](const auto* track) { return track->id == *stem_track; }))))
    return fail(foundation::ErrorCode::invalid_argument,
                "Requested stem track is not a source in this project");
  std::vector<PreparedSource> sources;
  std::vector<PreparedAudio> audio;
  sources.reserve(max_audio_sources);
  audio.reserve(audio_tracks.size());
  if (store != nullptr) {
    for (const auto* track : audio_tracks) {
      auto prepared =
          prepare_audio(snapshot, *store, *track, sources, inserts.value().track_chain_owner);
      if (!prepared)
        return foundation::Result<PublishOutcome>::failure(prepared.error());
      audio.push_back(std::move(prepared).value());
    }
  }
  std::vector<std::pair<project::PluginInstanceId, plugins::ScanResult>> instrument_scans;
  instrument_scans.reserve(instruments.size());
  std::vector<std::uint64_t> instrument_tails;
  instrument_tails.reserve(instruments.size());
  for (std::size_t index = 0; index < instruments.size(); ++index) {
    const auto& saved =
        std::get<project::Vst3PluginInstance>(instruments[index]->instrument_source);
    if (index >= inserts.value().vst3_instruments.size() ||
        saved.id != inserts.value().vst3_instruments[index]->id)
      return fail(foundation::ErrorCode::corrupt_data,
                  "Projected instrument source differs from saved plugin identity");
    auto scanned = scanner.scan(
        index == 0 && bundle != nullptr ? *bundle : std::filesystem::path(saved.bundle_path));
    if (!scanned.ok())
      return fail(foundation::ErrorCode::unavailable,
                  "VST3 instrument fresh scan failed: " + scanned.diagnostic);
    std::error_code canonical_error;
    const auto saved_bundle = std::filesystem::canonical(saved.bundle_path, canonical_error);
    if (canonical_error || saved_bundle != scanned.binary.canonical_bundle)
      return fail(foundation::ErrorCode::unavailable,
                  "VST3 instrument bundle differs from saved project; explicit relink required");
    if (scanned.binary.sha256 != saved.binary_sha256)
      return fail(foundation::ErrorCode::unavailable,
                  "VST3 instrument binary differs from saved project");
    const auto klass = std::ranges::find_if(scanned.metadata.classes, [&](const auto& item) {
      return item.id == saved.class_id && item.vendor == saved.vendor && item.name == saved.name &&
             item.version == saved.version;
    });
    if (klass == scanned.metadata.classes.end())
      return fail(foundation::ErrorCode::unavailable,
                  "VST3 instrument class differs from saved project");
    std::string host_error;
    auto tail_probe = plugins::load_verified_vst3_instrument(
        {*klass, scanned.binary}, snapshot.sample_rate, render_block_frames, host_error,
        plugins::Vst3ProcessingMode::offline);
    if (!tail_probe)
      return fail(foundation::ErrorCode::unavailable,
                  "VST3 instrument tail probe could not load: " + host_error);
    if ((!saved.component_state.empty() || saved.controller_state) &&
        !tail_probe->host().restore_state_pair({saved.component_state, saved.controller_state},
                                               host_error))
      return fail(foundation::ErrorCode::unavailable,
                  "VST3 instrument tail probe rejected saved state pair: " + host_error);
    if (!tail_probe->host().activate(host_error))
      return fail(foundation::ErrorCode::unavailable,
                  "VST3 instrument tail probe failed to activate: " + host_error);
    const auto reported_tail = tail_probe->host().finite_tail_samples();
    if (!reported_tail)
      return fail(foundation::ErrorCode::unsupported,
                  "VST3 instrument reports an infinite or unavailable tail");
    if (*reported_tail > static_cast<std::uint64_t>(snapshot.sample_rate) * max_tail_seconds)
      return fail(foundation::ErrorCode::resource_exhausted,
                  "VST3 instrument tail exceeds the ten-second export limit");
    if (tail_probe->host().latency_samples() != 0)
      return fail(foundation::ErrorCode::unsupported,
                  "VST3 instrument latency needs offline alignment qualification");
    instrument_tails.push_back(*reported_tail);
    instrument_scans.emplace_back(saved.id, std::move(scanned));
  }

  std::vector<std::pair<project::PluginInstanceId, plugins::ScanResult>> effect_scans;
  effect_scans.reserve(effects.size());
  std::array<std::uint64_t, 2> effect_path_tails{};
  std::array<std::uint32_t, 2> effect_path_latencies{};
  for (std::size_t effect_index = 0; effect_index < effects.size(); ++effect_index) {
    const auto* effect = effects[effect_index];
    const auto path_index = paired_effect_paths ? effect_index : 0;
    auto scanned_effect = scanner.scan(effect->bundle_path);
    if (!scanned_effect.ok())
      return fail(foundation::ErrorCode::unavailable,
                  "VST3 audio effect fresh scan failed: " + scanned_effect.diagnostic);
    std::error_code canonical_error;
    const auto effect_bundle = std::filesystem::canonical(effect->bundle_path, canonical_error);
    if (canonical_error || effect_bundle != scanned_effect.binary.canonical_bundle ||
        effect->binary_sha256 != scanned_effect.binary.sha256)
      return fail(foundation::ErrorCode::unavailable,
                  "VST3 audio effect binary differs from saved project");
    const auto effect_class =
        std::ranges::find_if(scanned_effect.metadata.classes, [&](const auto& item) {
          return item.id == effect->class_id && item.vendor == effect->vendor &&
                 item.name == effect->name && item.version == effect->version;
        });
    if (effect_class == scanned_effect.metadata.classes.end())
      return fail(foundation::ErrorCode::unavailable,
                  "VST3 audio effect class differs from saved project");
    auto binding = plugins::bind_vst3_project_effect(*effect, scanned_effect, snapshot.sample_rate,
                                                     render_block_frames,
                                                     plugins::Vst3ProcessingMode::offline);
    if (!binding.ready() || !binding.unresolved_parameter_ids.empty())
      return fail(foundation::ErrorCode::unavailable,
                  "VST3 audio effect tail probe could not bind: " + binding.diagnostic);
    auto effect_probe = std::move(binding.effect);
    const auto tail = effect_probe->host().finite_tail_samples();
    if (!tail)
      return fail(foundation::ErrorCode::unsupported,
                  "VST3 audio effect reports an infinite or unavailable tail");
    if (*tail > static_cast<std::uint64_t>(snapshot.sample_rate) * max_tail_seconds)
      return fail(foundation::ErrorCode::resource_exhausted,
                  "VST3 audio effect tail exceeds ten-second export limit");
    const auto latency = effect_probe->host().latency_samples();
    if (latency > max_effect_compensation_frames - effect_path_latencies[path_index])
      return fail(foundation::ErrorCode::resource_exhausted,
                  "Serial VST3 effect latency exceeds 8192-frame export limit");
    effect_path_latencies[path_index] += latency;
    if (*tail > static_cast<std::uint64_t>(snapshot.sample_rate) * max_tail_seconds -
                    effect_path_tails[path_index])
      return fail(foundation::ErrorCode::resource_exhausted,
                  "Serial VST3 effect tails exceed ten-second export limit");
    effect_path_tails[path_index] += *tail;
    effect_scans.emplace_back(effect->id, std::move(scanned_effect));
  }
  const auto effect_tail_frames = *std::ranges::max_element(effect_path_tails);
  const auto effect_latency_frames = *std::ranges::max_element(effect_path_latencies);

  auto compiled = engine::compile_midi_events(snapshot);
  if (!compiled)
    return foundation::Result<PublishOutcome>::failure(compiled.error());
  std::uint64_t source_frames = 0;
  const auto max_render_frames =
      static_cast<std::uint64_t>(snapshot.sample_rate) * max_render_seconds;
  std::optional<native_dsp::synth::NativeSynthPlayer> native_player;
  if (native_synth) {
    const auto events =
        std::ranges::find_if(compiled.value().instruments(), [native_synth](const auto& item) {
          return item.track() == native_synth->id;
        });
    if (events == compiled.value().instruments().end())
      return fail(foundation::ErrorCode::corrupt_data,
                  "Native synth MIDI plan is missing from mixed export");
    const auto& document =
        std::get<project::NativeInstrumentPatch>(native_synth->instrument_source).document;
    native_dsp::synth::SynthMedia media;
    if (store != nullptr) {
      auto loaded = source_prep::load_synth_media(*store, snapshot, document);
      if (!loaded)
        return foundation::Result<PublishOutcome>::failure(loaded.error());
      media = std::move(loaded).value();
    }
    auto prepared = native_dsp::synth::NativeSynthPlayer::prepare(
        snapshot, *native_synth, *events, {}, render_block_frames,
        std::numeric_limits<std::uint64_t>::max(), std::move(media));
    if (!prepared)
      return foundation::Result<PublishOutcome>::failure(prepared.error());
    native_player.emplace(std::move(prepared).value());
    if (native_player->tail_frames() >
        static_cast<std::uint64_t>(snapshot.sample_rate) * max_tail_seconds)
      return fail(foundation::ErrorCode::resource_exhausted,
                  "Native synth tail exceeds the ten-second export limit");
    std::uint64_t last_note = 0;
    bool note_on = false;
    for (const auto& event : events->events()) {
      if (event.sample.value < 0)
        return fail(foundation::ErrorCode::unsupported,
                    "Native synth export has negative MIDI time");
      if (event.kind == engine::MidiEventKind::note_on)
        note_on = true;
      if (event.kind == engine::MidiEventKind::note_off)
        last_note = std::max(last_note, static_cast<std::uint64_t>(event.sample.value) + 1);
    }
    if (!note_on || last_note == 0 || last_note > max_render_frames - native_player->tail_frames())
      return fail(foundation::ErrorCode::resource_exhausted,
                  "Native synth export needs bounded notes within one hour");
    source_frames = last_note + native_player->tail_frames();
  }
  for (std::size_t index = 0; index < instruments.size(); ++index) {
    const auto* instrument_track = instruments[index];
    const auto events =
        std::ranges::find_if(compiled.value().instruments(), [instrument_track](const auto& item) {
          return item.track() == instrument_track->id;
        });
    if (events == compiled.value().instruments().end())
      return fail(foundation::ErrorCode::corrupt_data, "VST3 instrument MIDI plan is missing");
    std::uint64_t last_note = 0;
    bool note_on = false;
    for (const auto& event : events->events()) {
      if (event.sample.value < 0)
        return fail(foundation::ErrorCode::unsupported, "VST3 export negative MIDI time");
      if (event.kind == engine::MidiEventKind::note_on)
        note_on = true;
      if (event.kind == engine::MidiEventKind::note_off)
        last_note = std::max(last_note, static_cast<std::uint64_t>(event.sample.value) + 1);
    }
    if (!note_on || last_note == 0 || last_note > max_render_frames - instrument_tails[index])
      return fail(foundation::ErrorCode::resource_exhausted,
                  "VST3 export needs bounded sources within one hour");
    source_frames = std::max(source_frames, last_note + instrument_tails[index]);
  }
  std::uint64_t audio_last = 0;
  for (const auto& track : audio)
    audio_last = std::max(audio_last, track.clips.back().last);
  if (audio_last > max_render_frames)
    return fail(foundation::ErrorCode::resource_exhausted,
                "VST3 export needs bounded sources within one hour");
  const auto base_end = std::max(audio_last, source_frames);
  // The routed master processor applies active native slots in saved order.
  // Adding each stage's finite drain is conservative for either order and
  // keeps a processed stem's output extent equal to the master's.
  if (base_end > max_render_frames - master_tail_frames)
    return fail(foundation::ErrorCode::resource_exhausted,
                "VST3 export master effect tail exceeds the one-hour output limit");
  const auto master_end = base_end + master_tail_frames;
  if (native_tail_frames > max_render_frames - master_end)
    return fail(foundation::ErrorCode::resource_exhausted,
                "VST3 instrument native tail exceeds the one-hour output limit");
  const auto native_end = master_end + native_tail_frames;
  if (effect_tail_frames > max_render_frames - native_end)
    return fail(foundation::ErrorCode::resource_exhausted,
                "VST3 audio effect tail exceeds the one-hour output limit");
  const auto output_frames = native_end + effect_tail_frames;

  OfflineGate gate;
  auto render_snapshot = snapshot;
  if (selection != SourceSelection::master) {
    // Retain every path for routing and delay alignment, but admit exactly
    // the selected source to the temporary graph. The WAV reader below also
    // skips unselected audio, so no source reaches a stem through its sends.
    const auto selected = stem_track.value_or(instrument->id);
    for (auto& source : render_snapshot.tracks) {
      if ((source.kind == project::TrackKind::audio ||
           source.kind == project::TrackKind::instrument) &&
          source.id != selected)
        source.muted = true;
    }
    // Keep the source path and its inserts in the temporary graph so parallel
    // audio tracks retain the same compensation as the master. Muting the path
    // suppresses both direct output and sends without changing saved state.
  }
  auto model = project::ProjectModel::open(std::move(render_snapshot), gate);
  if (!model)
    return foundation::Result<PublishOutcome>::failure(model.error());
  plugins::Vst3InstrumentTrackProvider provider(plugins::Vst3ProcessingMode::offline);
  for (auto& [id, receipt] : instrument_scans)
    provider.set_scan_receipt(id, std::move(receipt));
  plugins::Vst3TrackInsertProvider effect_provider(plugins::Vst3ProcessingMode::offline);
  for (auto& [id, receipt] : effect_scans)
    effect_provider.set_scan_receipt(id, std::move(receipt));
  plugins::Vst3StageProvider stage_provider(provider, effect_provider);
  application::TrackInsertProvider* insert_provider = &stage_provider;
  application::RoutedPlaybackControl control(
      model.value(),
      {{1, std::max({64U, effect_latency_frames, native_latency_frames})},
       render_block_frames,
       512ULL * 1024,
       512ULL * 1024},
      insert_provider);
  auto submitted = control.submit_current();
  if (!submitted)
    return foundation::Result<PublishOutcome>::failure(submitted.error());
  const auto graph_latency = control.master_output_latency_frames();
  if (graph_latency > max_effect_compensation_frames ||
      graph_latency != std::max(effect_latency_frames, native_latency_frames) ||
      graph_latency > max_render_frames - output_frames)
    return fail(foundation::ErrorCode::unsupported,
                "VST3 export graph latency " + std::to_string(graph_latency) +
                    " differs from verified effect/native latency " +
                    std::to_string(std::max(effect_latency_frames, native_latency_frames)));
  const auto render_frames = output_frames + graph_latency;
  for (auto& track : audio) {
    std::optional<std::size_t> audio_slot;
    for (const auto& source : control.source_layout()) {
      if (source.track == track.track) {
        audio_slot = source.topological_index;
        break;
      }
    }
    if (!audio_slot || *audio_slot >= control.topological_order().size())
      return fail(foundation::ErrorCode::corrupt_data,
                  "Mixed export routed audio source slot is missing");
    track.source_slot = *audio_slot;
  }
  std::optional<std::size_t> native_source_slot;
  if (native_synth) {
    const auto source =
        std::ranges::find_if(control.source_layout(), [native_synth](const auto& item) {
          return item.track == native_synth->id;
        });
    if (source == control.source_layout().end() ||
        source->topological_index >= control.topological_order().size())
      return fail(foundation::ErrorCode::corrupt_data,
                  "Mixed export native synth routed source slot is missing");
    native_source_slot = source->topological_index;
  }
  auto writer = detail::MasterOutput::begin(options, snapshot.sample_rate, output_frames);
  if (!writer)
    return foundation::Result<PublishOutcome>::failure(writer.error());
  std::vector<std::span<const engine::StereoFrame>> inputs(control.topological_order().size());
  std::array<engine::StereoFrame, render_block_frames> rendered{};
  std::array<engine::StereoFrame, render_block_frames> native_frames{};
  std::vector<std::array<engine::StereoFrame, render_block_frames>> audio_frames(audio.size());
  std::array<media::StereoFrame, render_block_frames> decoded{};
  std::array<float, static_cast<std::size_t>(render_block_frames) * 2> interleaved{};
  for (std::uint64_t first = 0; first < render_frames;) {
    const auto count = static_cast<std::size_t>(
        std::min<std::uint64_t>(render_block_frames, render_frames - first));
    if (native_player) {
      std::ranges::fill(std::span(native_frames).first(count), engine::StereoFrame{});
      native_player->await_content();
      if (!native_player->render(std::span(native_frames).first(count), first, 1))
        return fail(foundation::ErrorCode::unavailable,
                    "Native synth failed during mixed offline render");
      inputs[*native_source_slot] = std::span(native_frames).first(count);
    }
    for (std::size_t track_index = 0; track_index < audio.size(); ++track_index) {
      auto& frames = audio_frames[track_index];
      std::ranges::fill(std::span(frames).first(count), engine::StereoFrame{});
      auto& track = audio[track_index];
      if (selection == SourceSelection::master ||
          (selection == SourceSelection::audio_stem && stem_track == track.track)) {
        while (track.next_clip < track.clips.size() && track.clips[track.next_clip].last <= first)
          ++track.next_clip;
        for (std::size_t clip_index = track.next_clip; clip_index < track.clips.size();
             ++clip_index) {
          const auto& clip = track.clips[clip_index];
          if (clip.first >= first + count)
            break;
          const auto overlap_first = std::max(first, clip.first);
          const auto overlap_last = std::min(first + count, clip.last);
          if (overlap_first >= overlap_last)
            continue;
          const auto read_count = static_cast<std::size_t>(overlap_last - overlap_first);
          const auto destination = std::span(decoded).first(read_count);
          auto read = clip.warp ? clip.warp->read_at(overlap_first - clip.first, destination)
                                : sources[clip.source_index].reader.read_at(
                                      clip.source_first + overlap_first - clip.first, destination);
          if (!read)
            return foundation::Result<PublishOutcome>::failure(read.error());
          if (read.value() != read_count)
            return fail(foundation::ErrorCode::io_failure, "Managed WAV ended during mixed export");
          const auto offset = static_cast<std::size_t>(overlap_first - first);
          for (std::size_t index = 0; index < read_count; ++index) {
            const auto gain =
                project::clip_fade_gain(clip.fades, overlap_first - clip.first + index);
            frames[offset + index] = {static_cast<float>(decoded[index].left * gain),
                                      static_cast<float>(decoded[index].right * gain)};
          }
        }
      }
      inputs[track.source_slot] = std::span(frames).first(count);
    }
    const auto status = control.render_block(inputs, std::span(rendered).first(count), {first, 1});
    if (status != engine::RoutedRenderStatus::rendered || control.processor_fault())
      return fail(foundation::ErrorCode::unavailable,
                  "VST3 instrument failed during offline render");
    for (std::size_t index = 0; index < count; ++index) {
      interleaved[index * 2] = rendered[index].left;
      interleaved[index * 2 + 1] = rendered[index].right;
    }
    const auto skipped =
        first < graph_latency
            ? static_cast<std::size_t>(std::min<std::uint64_t>(count, graph_latency - first))
            : 0;
    if (skipped != count) {
      auto wrote = writer.value().write_interleaved_stereo(
          std::span(interleaved).subspan(skipped * 2, (count - skipped) * 2));
      if (!wrote)
        return foundation::Result<PublishOutcome>::failure(wrote.error());
    }
    first += count;
  }
  if (selection != SourceSelection::audio_stem && !provider.stop_after_join())
    return fail(foundation::ErrorCode::unavailable, "VST3 instrument failed to stop after export");
  return writer.value().finish();
}
} // namespace

foundation::Result<PublishOutcome>
render_vst3_instrument_master(const project::Snapshot& snapshot, StoppedExportAuthority authority,
                              plugins::Vst3Scanner& scanner, const std::filesystem::path& bundle,
                              const MasterOutputOptions& options) {
  return render(snapshot, nullptr, std::move(authority), scanner, &bundle, options,
                SourceSelection::master, std::nullopt);
}

foundation::Result<PublishOutcome>
render_vst3_instrument_master(const project::Snapshot& snapshot, StoppedExportAuthority authority,
                              plugins::Vst3Scanner& scanner, const MasterOutputOptions& options) {
  return render(snapshot, nullptr, std::move(authority), scanner, nullptr, options,
                SourceSelection::master, std::nullopt);
}

foundation::Result<PublishOutcome>
render_vst3_mixed_master(const project::Snapshot& snapshot, const persistence::ProjectStore& store,
                         StoppedExportAuthority authority, plugins::Vst3Scanner& scanner,
                         const std::filesystem::path& bundle, const MasterOutputOptions& options) {
  return render(snapshot, &store, std::move(authority), scanner, &bundle, options,
                SourceSelection::master, std::nullopt);
}

foundation::Result<PublishOutcome>
render_vst3_instrument_wav_master(const project::Snapshot& snapshot,
                                  StoppedExportAuthority authority, plugins::Vst3Scanner& scanner,
                                  const std::filesystem::path& bundle,
                                  const MasterRenderOptions& options) {
  return render(snapshot, nullptr, std::move(authority), scanner, &bundle,
                MasterOutputOptions{options}, SourceSelection::master, std::nullopt);
}

foundation::Result<PublishOutcome>
render_vst3_instrument_wav_master(const project::Snapshot& snapshot,
                                  StoppedExportAuthority authority, plugins::Vst3Scanner& scanner,
                                  const MasterRenderOptions& options) {
  return render(snapshot, nullptr, std::move(authority), scanner, nullptr,
                MasterOutputOptions{options}, SourceSelection::master, std::nullopt);
}

foundation::Result<PublishOutcome>
render_vst3_instrument_wav_stem(const project::Snapshot& snapshot, StoppedExportAuthority authority,
                                plugins::Vst3Scanner& scanner, const std::filesystem::path& bundle,
                                const MasterRenderOptions& options) {
  return render(snapshot, nullptr, std::move(authority), scanner, &bundle,
                MasterOutputOptions{options}, SourceSelection::instrument_stem, std::nullopt);
}

foundation::Result<PublishOutcome>
render_vst3_instrument_wav_stem(const project::Snapshot& snapshot, StoppedExportAuthority authority,
                                plugins::Vst3Scanner& scanner, const std::filesystem::path& bundle,
                                project::TrackId source_track, const MasterRenderOptions& options) {
  return render(snapshot, nullptr, std::move(authority), scanner, &bundle,
                MasterOutputOptions{options}, SourceSelection::instrument_stem, source_track);
}

foundation::Result<PublishOutcome>
render_vst3_instrument_wav_stem(const project::Snapshot& snapshot, StoppedExportAuthority authority,
                                plugins::Vst3Scanner& scanner, project::TrackId source_track,
                                const MasterRenderOptions& options) {
  return render(snapshot, nullptr, std::move(authority), scanner, nullptr,
                MasterOutputOptions{options}, SourceSelection::instrument_stem, source_track);
}

foundation::Result<PublishOutcome> render_vst3_mixed_wav_master(
    const project::Snapshot& snapshot, const persistence::ProjectStore& store,
    StoppedExportAuthority authority, plugins::Vst3Scanner& scanner,
    const std::filesystem::path& bundle, const MasterRenderOptions& options) {
  return render(snapshot, &store, std::move(authority), scanner, &bundle,
                MasterOutputOptions{options}, SourceSelection::master, std::nullopt);
}

foundation::Result<PublishOutcome>
render_vst3_mixed_wav_stem(const project::Snapshot& snapshot,
                           const persistence::ProjectStore& store, StoppedExportAuthority authority,
                           plugins::Vst3Scanner& scanner, const std::filesystem::path& bundle,
                           project::TrackId source_track, const MasterRenderOptions& options) {
  const auto found = std::ranges::find_if(
      snapshot.tracks, [source_track](const auto& track) { return track.id == source_track; });
  if (found == snapshot.tracks.end())
    return fail(foundation::ErrorCode::invalid_argument, "Requested stem track does not exist");
  const auto selection = found->kind == project::TrackKind::instrument
                             ? SourceSelection::instrument_stem
                             : SourceSelection::audio_stem;
  return render(snapshot, &store, std::move(authority), scanner, &bundle,
                MasterOutputOptions{options}, selection, source_track);
}
} // namespace oscillate::exporting
