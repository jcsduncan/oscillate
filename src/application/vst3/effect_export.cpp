#include "oscillate/application/vst3/effect_export.hpp"
#include "oscillate/export/master_output.hpp"
#include "oscillate/export/processed_clip.hpp"
#include "oscillate/project/clip_fade.hpp"

#include "oscillate/application/routed_playback_control.hpp"
#include "oscillate/application/vst3/track_insert_provider.hpp"
#include "oscillate/media/audio_reader.hpp"
#include "oscillate/media/source_rate_reader.hpp"
#include "oscillate/native_inserts/native_stage.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/plugins/project_binding.hpp"
#include "oscillate/plugins/vst3_bundle_loader.hpp"
#include "oscillate/project/insert_projection.hpp"
#include "oscillate/routing/project_route_plan.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
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

constexpr std::size_t block_frames = 128;
constexpr std::size_t max_clips = 32;
constexpr std::size_t max_sources = 32;
constexpr std::uint64_t max_render_seconds = 3'600;
constexpr std::uint64_t max_tail_seconds = 10;
// This experimental exporter caps the preallocated graph history separately
// from the eventual reference-hardware PDC qualification limit.
constexpr std::uint32_t max_effect_compensation_frames = 8'192;

class OfflineGate final : public project::TransportGate {
public:
  [[nodiscard]] foundation::Result<std::unique_ptr<project::StoppedTransportLease>>
  acquire_stopped() override {
    return foundation::Result<std::unique_ptr<project::StoppedTransportLease>>::failure(
        {foundation::ErrorCode::conflict, "Offline render already owns the transport lease"});
  }
};

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

[[nodiscard]] foundation::Result<PublishOutcome> fail(foundation::ErrorCode code,
                                                      std::string message) {
  return foundation::Result<PublishOutcome>::failure({code, std::move(message)});
}

[[nodiscard]] foundation::Result<std::pair<std::uint64_t, std::uint64_t>>
clip_interval(const project::Snapshot& snapshot, const project::Placement& placement) {
  timeline::SamplePosition first{0};
  timeline::SamplePosition last{0};
  if (const auto* musical = std::get_if<project::MusicalInterval>(&placement)) {
    const auto start = snapshot.tempo.tick_to_sample(musical->start, snapshot.sample_rate);
    const auto end = snapshot.tempo.tick_to_sample(musical->end, snapshot.sample_rate);
    if (!start || !end)
      return foundation::Result<std::pair<std::uint64_t, std::uint64_t>>::failure(
          !start ? start.error() : end.error());
    first = start.value();
    last = end.value();
  } else {
    const auto& elapsed = std::get<project::ElapsedInterval>(placement);
    first = elapsed.start;
    last = elapsed.end;
  }
  if (first.value < 0 || last.value <= first.value)
    return foundation::Result<std::pair<std::uint64_t, std::uint64_t>>::failure(
        {foundation::ErrorCode::unsupported, "VST3 effect export clip placement is invalid"});
  return foundation::Result<std::pair<std::uint64_t, std::uint64_t>>::success(
      {static_cast<std::uint64_t>(first.value), static_cast<std::uint64_t>(last.value)});
}

[[nodiscard]] foundation::Result<std::vector<PreparedClip>>
prepare_clips(const project::Snapshot& snapshot, const persistence::ProjectStore& store,
              const project::Track& track, std::vector<PreparedSource>& sources) {
  using Clips = foundation::Result<std::vector<PreparedClip>>;
  if (track.clips.empty() || track.clips.size() > max_clips)
    return Clips::failure(
        {foundation::ErrorCode::unsupported, "VST3 effect export needs 1..32 audio clips"});
  std::vector<PreparedClip> clips;
  clips.reserve(track.clips.size());
  for (const auto& clip : track.clips) {
    const auto record =
        std::ranges::find(snapshot.sources, clip.source, &project::SourceRecord::id);
    if (record == snapshot.sources.end() ||
        std::ranges::count(snapshot.sources, clip.source, &project::SourceRecord::id) != 1)
      return Clips::failure(
          {foundation::ErrorCode::corrupt_data, "VST3 effect export source identity is ambiguous"});
    if (record->encoding != project::SourceEncoding::wav || record->channels < 1 ||
        record->channels > 2 || clip.source_range.start.value < 0 ||
        clip.source_range.end.value <= clip.source_range.start.value ||
        static_cast<std::uint64_t>(clip.source_range.end.value) > record->frames)
      return Clips::failure({foundation::ErrorCode::unsupported,
                             "VST3 effect export needs bounded managed WAV sources"});
    auto processed = prepare_processed_export_clip(snapshot, store, clip);
    if (!processed)
      return Clips::failure(processed.error());
    if (processed.value()) {
      auto& warped = *processed.value();
      auto fades = project::prepare_clip_fades(clip.fades, warped.last - warped.first);
      if (!fades)
        return Clips::failure(fades.error());
      clips.push_back({warped.first, warped.last, 0, 0, fades.value(), std::move(warped.reader)});
      continue;
    }
    const auto source_first =
        media::source_rate_frame_count(static_cast<std::uint64_t>(clip.source_range.start.value),
                                       record->sample_rate, snapshot.sample_rate);
    const auto source_last =
        media::source_rate_frame_count(static_cast<std::uint64_t>(clip.source_range.end.value),
                                       record->sample_rate, snapshot.sample_rate);
    const auto interval = clip_interval(snapshot, clip.placement);
    if (!source_first || !source_last || !interval)
      return Clips::failure(!source_first  ? source_first.error()
                            : !source_last ? source_last.error()
                                           : interval.error());
    const auto [first, last] = interval.value();
    if (source_last.value() <= source_first.value() ||
        source_last.value() - source_first.value() != last - first)
      return Clips::failure({foundation::ErrorCode::unsupported,
                             "VST3 effect export cannot render stretched or warped clips"});
    if (clip.clay)
      return Clips::failure({foundation::ErrorCode::unsupported,
                             "VST3 effect export does not render Clay note edits yet"});
    const auto existing = std::ranges::find(sources, clip.source, &PreparedSource::id);
    std::size_t index = static_cast<std::size_t>(existing - sources.begin());
    if (existing == sources.end()) {
      if (sources.size() == max_sources)
        return Clips::failure({foundation::ErrorCode::resource_exhausted,
                               "VST3 effect export source limit exceeded"});
      auto descriptor = store.open_managed_source(*record);
      if (!descriptor)
        return Clips::failure(descriptor.error());
      auto opened = media::AudioReader::open_fd(descriptor.value(), *record);
      const auto closed = ::close(descriptor.value());
      if (!opened)
        return Clips::failure(opened.error());
      if (closed != 0)
        return Clips::failure({foundation::ErrorCode::io_failure,
                               "Cannot close verified VST3 effect audio descriptor"});
      if (opened.value().sample_rate() != record->sample_rate ||
          opened.value().channels() != record->channels ||
          opened.value().frame_count() != record->frames)
        return Clips::failure(
            {foundation::ErrorCode::corrupt_data, "Managed VST3 effect WAV metadata changed"});
      auto converted =
          media::SourceRateReader::open(std::move(opened).value(), snapshot.sample_rate);
      if (!converted)
        return Clips::failure(converted.error());
      sources.push_back({record->id, std::move(converted).value()});
      index = sources.size() - 1;
    }
    if (source_last.value() > sources[index].reader.frame_count())
      return Clips::failure(
          {foundation::ErrorCode::corrupt_data, "VST3 effect clip exceeds managed media"});
    auto fades = project::prepare_clip_fades(clip.fades, last - first);
    if (!fades)
      return Clips::failure(fades.error());
    clips.push_back({first, last, source_first.value(), index, fades.value(), nullptr});
  }
  std::ranges::sort(clips, {}, &PreparedClip::first);
  for (std::size_t index = 1; index < clips.size(); ++index)
    if (clips[index - 1].last > clips[index].first)
      return Clips::failure({foundation::ErrorCode::unsupported,
                             "VST3 effect export cannot overlap same-track clips"});
  return Clips::success(std::move(clips));
}

[[nodiscard]] foundation::Result<PublishOutcome>
render(const project::Snapshot& snapshot, const persistence::ProjectStore& store,
       StoppedExportAuthority authority, plugins::Vst3Scanner& scanner,
       const std::filesystem::path& bundle, std::optional<project::TrackId> stem,
       const MasterOutputOptions& options) {
  if (project::has_convolution_insert(snapshot))
    return fail(foundation::ErrorCode::unsupported,
                "Saved convolution insert is not available for VST3 effect export yet");
  if (!authority.matches(snapshot))
    return fail(foundation::ErrorCode::conflict, "VST3 effect export authority is stale");
  auto projection = project::project_insert_chains(snapshot);
  if (!projection)
    return foundation::Result<PublishOutcome>::failure(projection.error());
  const auto* wav = std::get_if<MasterRenderOptions>(&options);
  const auto requested_rate =
      std::visit([](const auto& value) { return value.sample_rate; }, options);
  if ((wav && wav->loudness_report != nullptr) ||
      !timeline::supported_sample_rate(snapshot.sample_rate) ||
      (requested_rate != 0 && !timeline::supported_sample_rate(requested_rate)))
    return fail(foundation::ErrorCode::unsupported,
                "VST3 effect export requires supported project and output rates without a "
                "loudness report");
  if (!snapshot.master_effects.empty())
    return fail(foundation::ErrorCode::unsupported,
                "VST3 effect export cannot render master inserts");
  const project::Track* wet = nullptr;
  std::vector<const project::EffectInsert*> native_effects;
  std::vector<const project::EffectInsert*> inserts;
  std::vector<const project::Track*> audio_tracks;
  for (const auto& track : snapshot.tracks) {
    const bool bus =
        track.kind == project::TrackKind::group_bus || track.kind == project::TrackKind::return_bus;
    if (bus) {
      if (!track.clips.empty() || !track.midi_clips.empty() ||
          !std::holds_alternative<std::monostate>(track.instrument_source))
        return fail(foundation::ErrorCode::unsupported,
                    "VST3 effect export cannot render bus source content");
    } else {
      if (track.kind != project::TrackKind::audio || !track.midi_clips.empty() ||
          !std::holds_alternative<std::monostate>(track.instrument_source))
        return fail(foundation::ErrorCode::unsupported,
                    "VST3 effect export needs one plain audio track");
      if (audio_tracks.size() == 2)
        return fail(foundation::ErrorCode::unsupported,
                    "VST3 effect export supports at most two audio tracks");
      audio_tracks.push_back(&track);
    }
    if (!track.effects.empty()) {
      if (wet)
        return fail(foundation::ErrorCode::unsupported,
                    "VST3 effect export needs one affected path");
      wet = &track;
      // The path's chain in saved order, built-in and VST3 (ADR-0050).
      for (const auto& effect : track.effects) {
        if (!effect.enabled)
          continue;
        if (std::holds_alternative<project::Vst3PluginInstance>(effect.processor))
          inserts.push_back(&effect);
        else if (std::holds_alternative<project::NativeEffectPatch>(effect.processor))
          native_effects.push_back(&effect);
        else
          return fail(foundation::ErrorCode::unsupported,
                      "VST3 effect export cannot render this effect slot");
      }
    }
  }
  if (!wet || audio_tracks.empty() ||
      (stem && std::ranges::none_of(audio_tracks,
                                    [stem](const auto* track) { return track->id == *stem; })))
    return fail(foundation::ErrorCode::invalid_argument,
                "VST3 effect export source track is absent");
  std::optional<project::TrackId> detector_track;
  for (const auto& track : snapshot.tracks) {
    for (const auto& send : track.sends) {
      if (send.purpose != project::SendPurpose::sidechain)
        continue;
      if (detector_track || !native_effects.empty() || inserts.size() != 1 ||
          track.kind != project::TrackKind::audio || track.id == wet->id ||
          send.destination != wet->id || send.target_insert != inserts.front()->id ||
          track.sends.size() != 1)
        return fail(foundation::ErrorCode::unsupported,
                    "VST3 effect export needs one direct audio detector send to its sole effect");
      detector_track = track.id;
    }
  }
  std::vector<PreparedSource> sources;
  sources.reserve(max_sources);
  std::vector<PreparedAudio> audio;
  audio.reserve(audio_tracks.size());
  for (const auto* track : audio_tracks) {
    auto clips = prepare_clips(snapshot, store, *track, sources);
    if (!clips)
      return foundation::Result<PublishOutcome>::failure(clips.error());
    audio.push_back({track->id, std::move(clips).value()});
  }
  plugins::Vst3TrackInsertProvider provider(plugins::Vst3ProcessingMode::offline);
  // One receipt per stage of the wet path, in saved order.
  std::vector<std::optional<routing::BoundStage>> staged(wet->effects.size());
  std::uint64_t tail_frames = 0;
  const auto stage_index = [&](const project::EffectInsert* effect) {
    return static_cast<std::size_t>(effect - wet->effects.data());
  };
  for (const auto* native_effect : native_effects) {
    native_inserts::StageContext context{snapshot.sample_rate};
    context.snapshot = &snapshot;
    auto prepared = native_inserts::prepare_native_stage(*native_effect, context);
    if (!prepared)
      return foundation::Result<PublishOutcome>::failure(prepared.error());
    const auto& stage = prepared.value();
    if (!stage.export_tail_frames)
      return foundation::Result<PublishOutcome>::failure(*stage.tail_error);
    const auto tail_limit = static_cast<std::uint64_t>(snapshot.sample_rate) * max_tail_seconds;
    if (*stage.export_tail_frames > tail_limit - tail_frames)
      return fail(foundation::ErrorCode::resource_exhausted,
                  "Native effect tail exceeds ten-second export limit");
    // Latent samples drain within graph latency before the writer strips it;
    // only the ring-out extends the output.
    tail_frames += *stage.export_tail_frames;
    staged[stage_index(native_effect)] =
        routing::BoundStage{.track = wet->id,
                            .slot = native_effect->id,
                            .processor = routing::StageProcessor::native_effect,
                            .plugin = std::nullopt,
                            .generation = stage.generation,
                            .latency_frames = stage.latency_frames,
                            .detector = stage.sidechain_capable};
  }
  for (std::size_t index = 0; index < inserts.size(); ++index) {
    const auto* insert = inserts[index];
    const auto& saved = std::get<project::Vst3PluginInstance>(insert->processor);
    // The explicit bundle argument anchors the first saved slot. Later slots
    // are scanned from their own persisted paths and independently verified.
    auto scanned = scanner.scan(index == 0 ? bundle : std::filesystem::path(saved.bundle_path));
    if (!scanned.ok())
      return fail(foundation::ErrorCode::unavailable,
                  "VST3 effect fresh scan failed: " + scanned.diagnostic);
    std::error_code canonical_error;
    const auto saved_bundle = std::filesystem::canonical(saved.bundle_path, canonical_error);
    if (canonical_error || saved_bundle != scanned.binary.canonical_bundle ||
        saved.binary_sha256 != scanned.binary.sha256)
      return fail(foundation::ErrorCode::unavailable,
                  "VST3 effect binary differs from saved slot; explicit relink required");
    const auto klass = std::ranges::find_if(scanned.metadata.classes, [&](const auto& candidate) {
      return candidate.id == saved.class_id && candidate.vendor == saved.vendor &&
             candidate.name == saved.name && candidate.version == saved.version;
    });
    if (klass == scanned.metadata.classes.end())
      return fail(foundation::ErrorCode::unavailable, "VST3 effect class differs from saved slot");
    const bool routed_sidechain = detector_track && insert == inserts.front();
    auto binding = plugins::bind_vst3_project_effect(
        saved, scanned, snapshot.sample_rate, block_frames, plugins::Vst3ProcessingMode::offline,
        plugins::default_vst3_parameter_events, routed_sidechain);
    if (!binding.ready() || !binding.unresolved_parameter_ids.empty()) {
      const auto reason =
          binding.status == plugins::Vst3BindingStatus::state_rejected ? "rejected saved state: "
          : binding.status == plugins::Vst3BindingStatus::activation_failed ? "failed to activate: "
          : binding.status == plugins::Vst3BindingStatus::load_failed       ? "could not load: "
          : !binding.unresolved_parameter_ids.empty() ? "has unavailable parameter references: "
                                                      : "could not bind: ";
      return fail(foundation::ErrorCode::unavailable,
                  "VST3 effect tail probe " + std::string(reason) + binding.diagnostic);
    }
    auto probe = std::move(binding.effect);
    if (routed_sidechain && !probe->host().routed_sidechain_enabled())
      return fail(foundation::ErrorCode::unavailable,
                  "VST3 effect did not activate its routed stereo detector bus");
    const auto tail = probe->host().finite_tail_samples();
    if (!tail)
      return fail(foundation::ErrorCode::unsupported,
                  "VST3 effect reports infinite or unavailable tail");
    const auto tail_limit = static_cast<std::uint64_t>(snapshot.sample_rate) * max_tail_seconds;
    if (*tail > tail_limit - tail_frames)
      return fail(foundation::ErrorCode::resource_exhausted,
                  "VST3 effect tail exceeds ten-second export limit");
    tail_frames += *tail;
    staged[stage_index(insert)] =
        routing::BoundStage{.track = wet->id,
                            .slot = insert->id,
                            .processor = routing::StageProcessor::vst3,
                            .plugin = saved.id,
                            .generation = 0,
                            .latency_frames = probe->host().latency_samples(),
                            .detector = routed_sidechain};
    probe.reset();
    provider.set_scan_receipt(saved.id, std::move(scanned));
  }

  // Disabled slots have no stage.
  std::vector<routing::BoundStage> bound;
  for (auto& stage : staged) {
    if (stage)
      bound.push_back(std::move(*stage));
  }
  std::uint32_t compensation_frames = 0;
  for (const auto& insert : bound) {
    if (insert.latency_frames > max_effect_compensation_frames - compensation_frames)
      return fail(foundation::ErrorCode::resource_exhausted,
                  "Serial effect latency exceeds 8192-frame export compensation limit");
    compensation_frames += insert.latency_frames;
  }
  routing::ProjectRouteIdRegistry route_ids;
  auto route = route_ids.compile(snapshot, compensation_frames, bound);
  if (!route)
    return foundation::Result<PublishOutcome>::failure(route.error());
  const auto graph_latency = route.value().latency.master_output_latency_frames;
  const auto max_frames = static_cast<std::uint64_t>(snapshot.sample_rate) * max_render_seconds;
  std::uint64_t source_end = 0;
  for (const auto& track : audio)
    source_end = std::max(source_end, track.clips.back().last);
  if (graph_latency > max_frames || tail_frames > max_frames - graph_latency ||
      source_end > max_frames - graph_latency - tail_frames)
    return fail(foundation::ErrorCode::resource_exhausted,
                "VST3 effect export exceeds one-hour output limit");
  const auto render_frames = source_end + graph_latency + tail_frames;
  const auto output_frames = source_end + tail_frames;

  OfflineGate gate;
  auto render_snapshot = snapshot;
  if (stem) {
    for (auto& track : render_snapshot.tracks) {
      if (track.kind != project::TrackKind::audio || track.id == *stem)
        continue;
      if (!detector_track || track.id != *detector_track || *stem != wet->id) {
        track.muted = true;
        continue;
      }
      // The selected wet stem still needs the detector's post-fader signal.
      // Route its audible main output into a private muted sink; muting the
      // detector itself would also silence its sidechain edge.
      std::optional<project::TrackId> sink_id;
      for (std::uint64_t serial = 1; serial <= snapshot.tracks.size() + 1; ++serial) {
        auto candidate = project::TrackId::from_bits({0x5349444543484149ULL, serial});
        if (!candidate)
          return foundation::Result<PublishOutcome>::failure(candidate.error());
        if (std::ranges::find(render_snapshot.tracks, candidate.value(), &project::Track::id) ==
            render_snapshot.tracks.end()) {
          sink_id = candidate.value();
          break;
        }
      }
      if (!sink_id)
        return fail(foundation::ErrorCode::resource_exhausted,
                    "VST3 detector stem has no private sink identity");
      track.main_output = *sink_id;
      project::Track sink(*sink_id, {}, project::TrackKind::group_bus);
      sink.muted = true;
      render_snapshot.tracks.push_back(std::move(sink));
      break;
    }
  }
  auto model = project::ProjectModel::open(std::move(render_snapshot), gate);
  if (!model)
    return foundation::Result<PublishOutcome>::failure(model.error());
  application::RoutedPlaybackControl control(
      model.value(), {{1, compensation_frames}, block_frames, 512ULL * 1024, 512ULL * 1024},
      &provider);
  auto submitted = control.submit_current();
  if (!submitted)
    return foundation::Result<PublishOutcome>::failure(submitted.error());
  if (control.master_output_latency_frames() != graph_latency)
    return fail(foundation::ErrorCode::unsupported,
                "VST3 detector stem changed master latency after isolation");
  for (auto& track : audio) {
    const auto source = std::ranges::find(control.source_layout(), track.track,
                                          &routing::ProjectRouteSource::track);
    if (source == control.source_layout().end() ||
        source->topological_index >= control.topological_order().size())
      return fail(foundation::ErrorCode::corrupt_data, "VST3 effect routed source slot is absent");
    track.source_slot = source->topological_index;
  }
  auto writer = detail::MasterOutput::begin(options, snapshot.sample_rate, output_frames);
  if (!writer)
    return foundation::Result<PublishOutcome>::failure(writer.error());
  std::vector<std::span<const engine::StereoFrame>> inputs(control.topological_order().size());
  std::vector<std::array<engine::StereoFrame, block_frames>> source_frames(audio.size());
  std::array<engine::StereoFrame, block_frames> output{};
  std::array<media::StereoFrame, block_frames> decoded{};
  std::array<float, block_frames * 2> interleaved{};
  for (std::uint64_t first = 0; first < render_frames;) {
    const auto count =
        static_cast<std::size_t>(std::min<std::uint64_t>(block_frames, render_frames - first));
    for (std::size_t track_index = 0; track_index < audio.size(); ++track_index) {
      auto& track = audio[track_index];
      auto& frames = source_frames[track_index];
      std::ranges::fill(std::span(frames).first(count), engine::StereoFrame{});
      if (!stem || *stem == track.track ||
          (detector_track && *stem == wet->id && track.track == *detector_track)) {
        while (track.next_clip < track.clips.size() && track.clips[track.next_clip].last <= first)
          ++track.next_clip;
        for (std::size_t index = track.next_clip; index < track.clips.size(); ++index) {
          const auto& clip = track.clips[index];
          if (clip.first >= first + count)
            break;
          const auto overlap_first = std::max(first, clip.first);
          const auto overlap_last = std::min(first + count, clip.last);
          if (overlap_first >= overlap_last)
            continue;
          const auto length = static_cast<std::size_t>(overlap_last - overlap_first);
          const auto destination = std::span(decoded).first(length);
          auto read = clip.warp ? clip.warp->read_at(overlap_first - clip.first, destination)
                                : sources[clip.source_index].reader.read_at(
                                      clip.source_first + overlap_first - clip.first, destination);
          if (!read)
            return foundation::Result<PublishOutcome>::failure(read.error());
          if (read.value() != length)
            return fail(foundation::ErrorCode::io_failure,
                        "Managed WAV ended during VST3 effect export");
          const auto offset = static_cast<std::size_t>(overlap_first - first);
          for (std::size_t frame = 0; frame < length; ++frame) {
            const auto gain =
                project::clip_fade_gain(clip.fades, overlap_first - clip.first + frame);
            frames[offset + frame] = {static_cast<float>(decoded[frame].left * gain),
                                      static_cast<float>(decoded[frame].right * gain)};
          }
        }
      }
      inputs[track.source_slot] = std::span(frames).first(count);
    }
    const auto status = control.render_block(inputs, std::span(output).first(count), {first, 1});
    if (status != engine::RoutedRenderStatus::rendered || control.processor_fault())
      return fail(foundation::ErrorCode::unavailable, "VST3 effect failed during offline render");
    for (std::size_t frame = 0; frame < count; ++frame) {
      interleaved[frame * 2] = output[frame].left;
      interleaved[frame * 2 + 1] = output[frame].right;
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
  return writer.value().finish();
}

} // namespace

foundation::Result<PublishOutcome>
render_vst3_effect_master(const project::Snapshot& snapshot, const persistence::ProjectStore& store,
                          StoppedExportAuthority authority, plugins::Vst3Scanner& scanner,
                          const std::filesystem::path& bundle, const MasterOutputOptions& options) {
  return render(snapshot, store, std::move(authority), scanner, bundle, std::nullopt, options);
}

foundation::Result<PublishOutcome> render_vst3_effect_wav_master(
    const project::Snapshot& snapshot, const persistence::ProjectStore& store,
    StoppedExportAuthority authority, plugins::Vst3Scanner& scanner,
    const std::filesystem::path& bundle, const MasterRenderOptions& options) {
  return render(snapshot, store, std::move(authority), scanner, bundle, std::nullopt,
                MasterOutputOptions{options});
}

foundation::Result<PublishOutcome>
render_vst3_effect_wav_stem(const project::Snapshot& snapshot,
                            const persistence::ProjectStore& store,
                            StoppedExportAuthority authority, plugins::Vst3Scanner& scanner,
                            const std::filesystem::path& bundle, project::TrackId source_track,
                            const MasterRenderOptions& options) {
  return render(snapshot, store, std::move(authority), scanner, bundle, source_track,
                MasterOutputOptions{options});
}

} // namespace oscillate::exporting
