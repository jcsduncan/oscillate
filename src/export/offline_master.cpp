#include "oscillate/export/offline_master.hpp"

#include "oscillate/engine/denormals.hpp"
#include "oscillate/export/insert_preflight.hpp"
#include "oscillate/export/master_output.hpp"
#include "oscillate/export/processed_clip.hpp"
#include "oscillate/foundation/file_io.hpp"
#include "oscillate/native_inserts/native_stage.hpp"
#include "oscillate/processing/clay_source.hpp"
#include "oscillate/project/clip_fade.hpp"
#include "oscillate/project/frozen_view.hpp"
#include "private.hpp"

#include "oscillate/automation_project/preparation.hpp"
#include "oscillate/engine/midi_events.hpp"
#include "oscillate/engine/routed_mixer.hpp"
#include "oscillate/media/audio_reader.hpp"
#include "oscillate/media/source_rate_reader.hpp"
#include "oscillate/native_dsp/loudness.hpp"
#include "oscillate/native_dsp/synth/player.hpp"
#include "oscillate/native_dsp/synth/project_binding.hpp"
#include "oscillate/native_dsp/true_peak.hpp"
#if OSCILLATE_HAVE_CONVOLUTION
#include "oscillate/native_dsp/convolution_ir.hpp"
#include "oscillate/native_dsp/convolution_runtime.hpp"
#endif
#include "oscillate/persistence/store.hpp"
#include "oscillate/routing/project_route_plan.hpp"
#include "oscillate/routing/route_gain_blocks.hpp"
#include "oscillate/source_prep/synth_media.hpp"
#include "oscillate/track_chain/track_chain.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace oscillate::exporting {
namespace {

constexpr std::size_t block_frames = 1'024;
constexpr std::size_t max_export_clips = 4'096;
constexpr std::size_t max_export_instruments = 128;
// A note-off's ring-out on an instrument without a declared tail.
constexpr std::uint32_t fallback_release_frames = 64;

// A track render (freeze, bounce, ADR-0059) narrows a solo render: its
// length comes from the solo track's own content (a key's track does not
// lengthen it), or from an explicit range, and the file starts at the first
// frame the track sounds. Progress is offered every few blocks; false cancels.
struct RenderControls {
  bool solo_extent_only = false;
  std::optional<std::pair<std::uint64_t, std::uint64_t>> range;
  std::function<bool(std::uint64_t, std::uint64_t)> progress;
  std::uint64_t first_frame = 0; // Out: the project frame of the file's frame 0.
  // Mix Check's taps (render_master_taps): no file is written, the render is
  // routed, and every source track's block and the master go to the taps.
  const RenderTaps* taps = nullptr;
};

struct OpenSource {
  project::SourceId id;
  // The audio actually read: the source's hash, or its Clay render's key.
  std::string sha256;
  media::SourceRateReader reader;
};

// A raw clip reads sources[source_index] from source_first; a processed clip
// reads its own Q0 warp render (`warp`), whose frame 0 is `first`.
struct PreparedClip {
  std::size_t source_index;
  std::size_t track_index;
  std::uint64_t first;
  std::uint64_t last;
  std::uint64_t source_first;
  project::PreparedClipFades fades;
  std::unique_ptr<processing::OfflineWarpClip> warp;
};

// Built-in synths render through the same player as live playback.
struct PreparedSynth {
  native_dsp::synth::NativeSynthPlayer player;
  std::size_t track_index;
};

// Where the delivered master goes: the output writer, or Mix Check's master
// tap (deinterleaved, at the project frame it plays at).
class MasterSink {
public:
  explicit MasterSink(detail::MasterOutput& writer) noexcept : writer_(&writer) {}
  MasterSink(const RenderTaps& taps, std::uint64_t first_frame) noexcept
      : taps_(&taps), next_(first_frame) {}

  [[nodiscard]] foundation::Result<void> write_interleaved_stereo(std::span<const float> samples) {
    if (writer_ != nullptr)
      return writer_->write_interleaved_stereo(samples);
    const auto frames = samples.size() / 2;
    for (std::size_t offset = 0; offset < frames; offset += left_.size()) {
      const auto count = std::min(left_.size(), frames - offset);
      for (std::size_t index = 0; index < count; ++index) {
        left_[index] = samples[(offset + index) * 2];
        right_[index] = samples[(offset + index) * 2 + 1];
      }
      if (taps_->master)
        taps_->master(std::span<const float>(left_).first(count),
                      std::span<const float>(right_).first(count), next_);
      next_ += count;
    }
    return foundation::Result<void>::success();
  }
  // The writer ends early; a tap has simply had nothing more.
  [[nodiscard]] foundation::Result<void> shorten(std::uint64_t project_frames) {
    return writer_ != nullptr ? writer_->shorten(project_frames)
                              : foundation::Result<void>::success();
  }

private:
  detail::MasterOutput* writer_ = nullptr;
  const RenderTaps* taps_ = nullptr;
  std::uint64_t next_ = 0;
  std::array<float, block_frames> left_{};
  std::array<float, block_frames> right_{};
};

// Holds back what the master plays after the music ends and, at the end,
// writes it only up to its last stretch within tail_quiet_below_music_db of
// the music's loudest stretch, fading out over tail_fade_seconds.
class TailTrim {
public:
  TailTrim(std::uint32_t rate, std::uint64_t music_frames)
      : music_frames_(music_frames),
        window_(std::max<std::uint64_t>(1, static_cast<std::uint64_t>(rate * tail_window_seconds))),
        fade_(static_cast<std::uint64_t>(rate * tail_fade_seconds)) {}

  [[nodiscard]] foundation::Result<void> write(MasterSink& writer, std::span<const float> samples) {
    const auto frames = samples.size() / 2;
    const auto music =
        written_ < music_frames_
            ? static_cast<std::size_t>(std::min<std::uint64_t>(frames, music_frames_ - written_))
            : 0;
    if (music != 0) {
      const auto part = samples.first(music * 2);
      for (std::size_t index = 0; index < music; ++index) {
        sum_ += static_cast<double>(part[index * 2]) * part[index * 2] +
                static_cast<double>(part[index * 2 + 1]) * part[index * 2 + 1];
        if (++counted_ == window_)
          close_window();
      }
      auto wrote = writer.write_interleaved_stereo(part);
      if (!wrote)
        return wrote;
      written_ += music;
    }
    held_.insert(held_.end(), samples.begin() + static_cast<std::ptrdiff_t>(music * 2),
                 samples.end());
    return foundation::Result<void>::success();
  }

  [[nodiscard]] foundation::Result<void> finish(MasterSink& writer) {
    if (counted_ != 0)
      close_window();
    const auto floor = std::pow(10.0, tail_floor_dbfs / 20.0);
    const auto threshold =
        std::max(floor, loudest_ * std::pow(10.0, -tail_quiet_below_music_db / 20.0));
    const auto held = held_.size() / 2;
    std::size_t keep = 0;
    for (std::size_t first = 0; first < held; first += window_) {
      const auto last = std::min<std::size_t>(held, first + window_);
      double sum = 0.0;
      for (std::size_t index = first * 2; index < last * 2; ++index)
        sum += static_cast<double>(held_[index]) * held_[index];
      if (std::sqrt(sum / static_cast<double>((last - first) * 2)) >= threshold)
        keep = last;
    }
    // At least one held frame (faded to nothing), so the shortened output
    // still ends on a write.
    keep = std::max(keep, std::min<std::size_t>(held, 1));
    if (keep == 0)
      return foundation::Result<void>::success();
    if (keep < held) {
      keep = std::min<std::size_t>(held, keep + fade_);
      const auto fade = std::min<std::size_t>(keep, fade_);
      for (std::size_t index = 0; index < fade; ++index) {
        const auto gain = static_cast<float>(
            0.5 * (1.0 + std::cos(std::numbers::pi * static_cast<double>(index + 1) /
                                  static_cast<double>(fade))));
        held_[(keep - fade + index) * 2] *= gain;
        held_[(keep - fade + index) * 2 + 1] *= gain;
      }
    }
    auto shortened = writer.shorten(written_ + keep);
    if (!shortened)
      return shortened;
    return writer.write_interleaved_stereo(std::span<const float>(held_).first(keep * 2));
  }

private:
  void close_window() {
    loudest_ = std::max(loudest_, std::sqrt(sum_ / static_cast<double>(counted_ * 2)));
    sum_ = 0.0;
    counted_ = 0;
  }

  std::uint64_t music_frames_;
  std::uint64_t window_;
  std::uint64_t fade_;
  std::uint64_t written_ = 0;
  std::uint64_t counted_ = 0;
  double sum_ = 0.0;
  double loudest_ = 0.0;
  std::vector<float> held_;
};

// Last MIDI or pedal event plus the synth's release and effect tail.
[[nodiscard]] std::uint64_t synth_event_extent(const engine::InstrumentEvents& events,
                                               std::uint64_t tail_frames) noexcept {
  std::uint64_t last = 0;
  for (const auto& event : events.events()) {
    last = std::max(last, static_cast<std::uint64_t>(event.sample.value));
  }
  // A pedal lifted after the last note-off is when the release starts.
  for (const auto& event : events.pedals()) {
    last = std::max(last, static_cast<std::uint64_t>(event.sample.value));
  }
  return last + tail_frames;
}

// A source track's node in the routed mixer: its topological index and the
// latency its own and upstream processors add to its output.
struct RoutedSource {
  project::TrackId track;
  std::size_t index;
  std::uint64_t output_latency_frames;
};

struct OfflineRouting {
  std::unique_ptr<engine::RoutedMixer> mixer;
  std::vector<RoutedSource> sources;
  std::shared_ptr<routing::PreparedRouteGainBlocks> gains;
  std::vector<std::size_t> track_slots;
  std::vector<std::vector<engine::StereoFrame>> inputs;
  std::vector<std::span<const engine::StereoFrame>> views;
  // Inputs of nodes nobody can hear: their sources need not render.
  std::vector<bool> silent_inputs;
  std::vector<std::size_t> isolated_edges;
  std::vector<engine::EdgeGainSpan> isolated_gains;
  std::array<float, block_frames> silent_gain{};
  std::uint64_t output_latency_frames = 0;
  std::uint64_t native_tail_frames = 0;
};

// True when a stage held its output after a non-finite frame; unknown stages
// count as guarded so a render never passes them silently.
[[nodiscard]] bool guarded_stage(const engine::ProcessorRuntime& stage) noexcept {
  if (const auto frames = native_inserts::guarded_frames_after_quiesce(stage))
    return *frames != 0;
#if OSCILLATE_HAVE_CONVOLUTION
  if (const auto* convolution = dynamic_cast<const native_dsp::ConvolutionRuntime*>(&stage))
    return convolution->guarded_frames_after_quiesce() != 0;
#endif
  return true;
}

struct NativeRouteRuntime {
  project::TrackId track;
  track_chain::PreparedChain chain;
};

[[nodiscard]] std::optional<project::Snapshot>
native_export_view(const project::Snapshot& snapshot) {
  // Route compilation and automation preparation require every referenced
  // VST3 effect to have a runtime. Bypassed effects have none. Remove those
  // effects and their inactive lanes only from this private render view.
  if (std::ranges::none_of(snapshot.tracks, [](const auto& track) {
        return std::ranges::any_of(track.effects, [](const auto& slot) {
          return !slot.enabled &&
                 std::holds_alternative<project::Vst3PluginInstance>(slot.processor);
        });
      }))
    return std::nullopt;
  auto view = snapshot;
  for (auto& track : view.tracks) {
    std::vector<project::PluginInstanceId> bypassed_plugins;
    for (const auto& slot : track.effects)
      if (!slot.enabled)
        if (const auto* plugin = std::get_if<project::Vst3PluginInstance>(&slot.processor))
          bypassed_plugins.push_back(plugin->id);
    if (bypassed_plugins.empty())
      continue;
    std::erase_if(track.effects, [](const auto& slot) {
      return !slot.enabled && std::holds_alternative<project::Vst3PluginInstance>(slot.processor);
    });
    const auto bypassed_lane = [&](const project::AutomationLane& lane) {
      const auto* target = std::get_if<project::Vst3ParameterTarget>(&lane.target);
      return target &&
             std::ranges::find(bypassed_plugins, target->plugin) != bypassed_plugins.end();
    };
    std::erase_if(track.automation, bypassed_lane);
    for (auto& clip : track.clips)
      std::erase_if(clip.automation, bypassed_lane);
  }
  return view;
}

[[nodiscard]] foundation::Result<OfflineRouting>
prepare_offline_routing(const project::Snapshot& snapshot,
                        std::span<const automation_project::PreparedTargetAutomation> automation,
                        std::span<const routing::BoundStage> native_chain = {},
                        std::span<const NativeRouteRuntime> native_runtimes = {},
                        std::optional<project::TrackId> solo_track = std::nullopt,
                        std::span<const project::TrackId> detector_buses = {}) {
  routing::ProjectRouteIdRegistry ids;
  // Each native chain runs after its own incoming main/send contributions.
  // Lookahead belongs to that node and parallel paths receive bounded PDC.
  const bool latent_stage = std::ranges::any_of(
      native_chain, [](const auto& receipt) { return receipt.latency_frames != 0; });
  const std::uint32_t compensation_limit = latent_stage ? 8192 : 0;
  const auto route_view = native_export_view(snapshot);
  auto plan = ids.compile(route_view ? *route_view : snapshot, compensation_limit, native_chain,
                          OSCILLATE_HAVE_CONVOLUTION);
  if (!plan) {
    return foundation::Result<OfflineRouting>::failure(plan.error());
  }
  if (native_chain.empty() != native_runtimes.empty())
    return foundation::Result<OfflineRouting>::failure(
        {foundation::ErrorCode::corrupt_data, "Native route processor count is inconsistent"});
  for (const auto& receipt : native_chain) {
    const auto owner =
        std::ranges::find(native_runtimes, receipt.track, &NativeRouteRuntime::track);
    if (owner == native_runtimes.end() || !owner->chain.runtime)
      return foundation::Result<OfflineRouting>::failure(
          {foundation::ErrorCode::corrupt_data, "Native route has no prepared processor"});
  }
  std::map<engine::RoutingNodeId, std::uint64_t> own_tails;
  for (const auto& runtime : native_runtimes) {
    const auto owner = std::ranges::find(plan.value().track_nodes, runtime.track,
                                         &routing::ProjectRouteTrackNode::track);
    const auto tail = runtime.chain.export_tail_frames();
    if (owner == plan.value().track_nodes.end() || !runtime.chain.runtime || !tail ||
        !own_tails.emplace(owner->node, *tail).second)
      return foundation::Result<OfflineRouting>::failure(
          {foundation::ErrorCode::corrupt_data, "Native route owner is absent or duplicated"});
  }
  std::map<engine::RoutingNodeId, std::vector<engine::RoutingNodeId>> outgoing;
  for (const auto& edge : plan.value().edges)
    outgoing[edge.source].push_back(edge.destination);
  std::map<engine::RoutingNodeId, std::uint64_t> path_tails;
  for (const auto node : plan.value().latency.topological_order) {
    auto& extent = path_tails[node];
    const auto own = own_tails.find(node);
    if (own != own_tails.end()) {
      if (own->second > std::numeric_limits<std::uint64_t>::max() - extent)
        return foundation::Result<OfflineRouting>::failure(
            {foundation::ErrorCode::resource_exhausted, "Native route tail exceeds frame range"});
      extent += own->second;
    }
    for (const auto destination : outgoing[node])
      path_tails[destination] = std::max(path_tails[destination], extent);
  }
  const auto tail = path_tails.find(plan.value().master);
  if (tail == path_tails.end())
    return foundation::Result<OfflineRouting>::failure(
        {foundation::ErrorCode::corrupt_data, "Native route has no master tail path"});
  std::vector<engine::RoutedNode> nodes;
  nodes.reserve(plan.value().nodes.size());
  const auto track_mutes = routing::effective_track_mutes(snapshot, plan.value());
  // A track nobody can hear is muted at its node, which then skips its work.
  const auto silent_tracks = routing::silent_track_nodes(snapshot, plan.value());
  std::set<engine::RoutingNodeId> silent_nodes;
  for (const auto node : plan.value().nodes) {
    std::shared_ptr<engine::ProcessorRuntime> processor;
    bool muted = false;
    engine::StereoFrame output_gain{1.0F, 1.0F};
    if (const auto owner = std::ranges::find(plan.value().track_nodes, node.id,
                                             &routing::ProjectRouteTrackNode::node);
        owner != plan.value().track_nodes.end()) {
      if (const auto native =
              std::ranges::find(native_runtimes, owner->track, &NativeRouteRuntime::track);
          native != native_runtimes.end())
        processor = native->chain.runtime;
      const auto track_index = static_cast<std::size_t>(owner - plan.value().track_nodes.begin());
      muted = track_mutes[track_index] || silent_tracks[track_index];
      if (silent_tracks[track_index])
        silent_nodes.insert(node.id);
      if (const auto track = std::ranges::find(snapshot.tracks, owner->track, &project::Track::id);
          track != snapshot.tracks.end()) {
        const auto gain = routing::track_stereo_gain(*track);
        output_gain = {gain.left, gain.right};
      }
    }
    nodes.push_back({node, std::move(processor), {}, muted, output_gain});
  }
  // A keyed node has a detector buffer; a key's source keeps its pre-fader block.
  std::set<engine::RoutingNodeId> sidechain_destinations;
  std::set<engine::RoutingNodeId> sidechain_sources;
  for (const auto& edge : plan.value().edges) {
    if (edge.kind == engine::RoutingEdgeKind::sidechain) {
      sidechain_destinations.insert(edge.destination);
      sidechain_sources.insert(edge.source);
    }
  }
  const auto buffer_bytes = (static_cast<std::uint64_t>(nodes.size()) + 1 +
                             sidechain_destinations.size() + sidechain_sources.size()) *
                            block_frames * sizeof(engine::StereoFrame);
  constexpr std::uint64_t history_budget = 64ULL * 1024 * 1024;
  auto mixer = engine::prepare_routed_mixer(nodes, plan.value().edges,
                                            {.latency = {plan.value().master, compensation_limit},
                                             .max_frames = block_frames,
                                             .max_buffer_bytes = buffer_bytes,
                                             .max_history_bytes = history_budget,
                                             .skip_muted_nodes = true});
  if (!mixer) {
    return foundation::Result<OfflineRouting>::failure(mixer.error());
  }
  auto gains =
      routing::PreparedRouteGainBlocks::prepare(snapshot, plan.value(), automation, block_frames);
  if (!gains)
    return foundation::Result<OfflineRouting>::failure(gains.error());
  OfflineRouting route;
  route.mixer = std::move(mixer).value();
  route.gains = std::move(gains).value();
  route.track_slots.resize(snapshot.tracks.size());
  route.inputs.resize(nodes.size(), std::vector<engine::StereoFrame>(block_frames));
  route.views.resize(nodes.size());
  // Inputs are in topological order, as the plan's sources index them.
  route.silent_inputs.resize(nodes.size());
  for (std::size_t index = 0; index < plan.value().latency.topological_order.size(); ++index)
    route.silent_inputs[index] =
        silent_nodes.contains(plan.value().latency.topological_order[index]);
  if (solo_track) {
    std::set<engine::RoutingNodeId> other_sources;
    std::set<engine::RoutingNodeId> detector_nodes;
    for (const auto& source : plan.value().sources) {
      if (source.track != *solo_track)
        other_sources.insert(source.node);
    }
    for (const auto& owner : plan.value().track_nodes) {
      if (std::ranges::find(detector_buses, owner.track) != detector_buses.end())
        detector_nodes.insert(owner.node);
    }
    for (std::size_t index = 0; index < plan.value().latency.edges.size(); ++index) {
      const auto& edge = plan.value().latency.edges[index];
      if (edge.kind != engine::RoutingEdgeKind::sidechain &&
          (detector_nodes.contains(edge.source) ||
           (other_sources.contains(edge.source) && !detector_nodes.contains(edge.destination))))
        route.isolated_edges.push_back(index);
    }
    route.isolated_gains.resize(plan.value().latency.edges.size());
  }
  route.output_latency_frames = plan.value().latency.master_output_latency_frames;
  route.native_tail_frames = tail->second;
  for (const auto& source : plan.value().sources) {
    const auto track = std::ranges::find_if(
        snapshot.tracks, [&](const auto& item) { return item.id == source.track; });
    if (track == snapshot.tracks.end() || source.topological_index >= route.inputs.size()) {
      return foundation::Result<OfflineRouting>::failure(
          {foundation::ErrorCode::corrupt_data, "Offline route source mapping is invalid"});
    }
    route.track_slots[static_cast<std::size_t>(track - snapshot.tracks.begin())] =
        source.topological_index;
    route.sources.push_back(
        {source.track, source.topological_index,
         plan.value().latency.nodes[source.topological_index].output_latency_frames});
  }
  // Snapshot order, as the taps report them.
  std::ranges::sort(route.sources, {}, [&](const RoutedSource& source) {
    return std::ranges::find(snapshot.tracks, source.track, &project::Track::id) -
           snapshot.tracks.begin();
  });
  return foundation::Result<OfflineRouting>::success(std::move(route));
}

#if OSCILLATE_HAVE_CONVOLUTION
[[nodiscard]] foundation::Result<native_dsp::PreparedConvolutionIr>
prepare_saved_convolution(const project::Snapshot& snapshot, const persistence::ProjectStore& store,
                          const project::NativeConvolution& effect) {
  const auto record =
      std::ranges::find(snapshot.sources, effect.ir.source, &project::SourceRecord::id);
  if (record == snapshot.sources.end() || record->external_link ||
      record->encoding != project::SourceEncoding::wav)
    return foundation::Result<native_dsp::PreparedConvolutionIr>::failure(
        {foundation::ErrorCode::corrupt_data, "Convolution IR has no managed WAV source"});
  auto descriptor = store.open_managed_source(*record);
  if (!descriptor)
    return foundation::Result<native_dsp::PreparedConvolutionIr>::failure(descriptor.error());
  constexpr std::uint64_t preparation_budget = 12ULL * 1024 * 1024;
  auto prepared = native_dsp::prepare_convolution_ir_offline(
      descriptor.value(),
      {effect.ir.sha256, effect.ir.encoded_bytes, effect.ir.sample_rate,
       static_cast<std::uint16_t>(effect.ir.channels), effect.ir.frames},
      preparation_budget, snapshot.sample_rate, static_cast<float>(effect.wet));
  const int close_result = ::close(descriptor.value());
  if (!prepared)
    return foundation::Result<native_dsp::PreparedConvolutionIr>::failure(prepared.error());
  if (close_result != 0)
    return foundation::Result<native_dsp::PreparedConvolutionIr>::failure(
        {foundation::ErrorCode::io_failure, "Cannot close verified convolution IR"});
  return prepared;
}
#endif

[[nodiscard]] foundation::Result<std::pair<std::uint64_t, std::uint64_t>>
sample_interval(const project::Snapshot& snapshot, const project::Placement& placement) {
  timeline::SamplePosition first{0};
  timeline::SamplePosition last{0};
  if (const auto* musical = std::get_if<project::MusicalInterval>(&placement)) {
    auto converted_first = snapshot.tempo.tick_to_sample(musical->start, snapshot.sample_rate);
    if (!converted_first) {
      return foundation::Result<std::pair<std::uint64_t, std::uint64_t>>::failure(
          converted_first.error());
    }
    auto converted_last = snapshot.tempo.tick_to_sample(musical->end, snapshot.sample_rate);
    if (!converted_last) {
      return foundation::Result<std::pair<std::uint64_t, std::uint64_t>>::failure(
          converted_last.error());
    }
    first = converted_first.value();
    last = converted_last.value();
  } else {
    const auto& elapsed = std::get<project::ElapsedInterval>(placement);
    first = elapsed.start;
    last = elapsed.end;
  }
  if (first.value < 0 || last.value <= first.value) {
    return foundation::Result<std::pair<std::uint64_t, std::uint64_t>>::failure(
        {foundation::ErrorCode::unsupported, "Master export requires positive clip placement"});
  }
  return foundation::Result<std::pair<std::uint64_t, std::uint64_t>>::success(
      {static_cast<std::uint64_t>(first.value), static_cast<std::uint64_t>(last.value)});
}

[[nodiscard]] foundation::Result<std::size_t>
prepare_source(const project::Snapshot& snapshot, const persistence::ProjectStore& store,
               const project::Clip& clip, std::vector<OpenSource>& open_sources) {
  const auto id = clip.source;
  const auto found =
      std::ranges::find_if(snapshot.sources, [id](const auto& source) { return source.id == id; });
  if (found == snapshot.sources.end()) {
    return foundation::Result<std::size_t>::failure(
        {foundation::ErrorCode::unavailable, "Master export source is unresolved"});
  }
  // A Clay take plays its render (ADR-0081): same frames, rate and channels.
  auto audio = processing::clay::clip_audio_record(clip, *found);
  if (!audio)
    return foundation::Result<std::size_t>::failure(audio.error());
  for (std::size_t index = 0; index < open_sources.size(); ++index) {
    if (open_sources[index].id == id && open_sources[index].sha256 == audio.value().sha256) {
      return foundation::Result<std::size_t>::success(index);
    }
  }
  const auto* record = &audio.value();
  if (record->channels < 1 || record->channels > 2) {
    return foundation::Result<std::size_t>::failure(
        {foundation::ErrorCode::unsupported, "Master export requires mono/stereo source"});
  }
  std::optional<processing::clay::RenderedAudio> rendered;
  int descriptor_fd = -1;
  if (clip.clay) {
    auto opened = processing::clay::open_rendered(store, *found, *clip.clay);
    if (!opened)
      return foundation::Result<std::size_t>::failure(opened.error());
    rendered.emplace(std::move(opened).value());
    descriptor_fd = ::dup(rendered->fd());
    if (descriptor_fd < 0)
      return foundation::Result<std::size_t>::failure(
          {foundation::ErrorCode::io_failure, "Cannot duplicate the Clay render descriptor"});
  } else {
    auto descriptor = store.open_managed_source(*record);
    if (!descriptor) {
      return foundation::Result<std::size_t>::failure(descriptor.error());
    }
    descriptor_fd = descriptor.value();
  }
  auto reader = media::AudioReader::open_fd(descriptor_fd, *record);
  const int close_result = ::close(descriptor_fd);
  if (!reader) {
    return foundation::Result<std::size_t>::failure(reader.error());
  }
  if (close_result != 0) {
    return foundation::Result<std::size_t>::failure(
        {foundation::ErrorCode::io_failure, "Cannot close verified source descriptor"});
  }
  if (reader.value().sample_rate() != record->sample_rate ||
      reader.value().channels() != record->channels ||
      reader.value().frame_count() != record->frames) {
    return foundation::Result<std::size_t>::failure(
        {foundation::ErrorCode::corrupt_data, "Managed source metadata changed"});
  }
  auto converted = media::SourceRateReader::open(std::move(reader).value(), snapshot.sample_rate);
  if (!converted)
    return foundation::Result<std::size_t>::failure(converted.error());
  open_sources.push_back(OpenSource{id, record->sha256, std::move(converted).value()});
  return foundation::Result<std::size_t>::success(open_sources.size() - 1);
}

} // namespace

foundation::Result<StoppedExportAuthority>
StoppedExportAuthority::acquire(const project::Snapshot& snapshot, project::TransportGate& gate) {
  auto acquired = gate.acquire_stopped();
  if (!acquired) {
    return foundation::Result<StoppedExportAuthority>::failure(acquired.error());
  }
  if (!acquired.value()) {
    return foundation::Result<StoppedExportAuthority>::failure(
        {foundation::ErrorCode::conflict, "Transport gate returned an empty stopped lease"});
  }
  return foundation::Result<StoppedExportAuthority>::success(
      {snapshot.id, snapshot.revision, std::move(acquired.value())});
}

bool StoppedExportAuthority::matches(const project::Snapshot& snapshot) const noexcept {
  return lease_ != nullptr && project_id_ == snapshot.id && revision_ == snapshot.revision;
}

namespace {

using WriterOptions = MasterOutputOptions;

[[nodiscard]] std::uint32_t requested_rate(const WriterOptions& options,
                                           std::uint32_t project_rate) {
  const auto chosen = std::visit([](const auto& value) { return value.sample_rate; }, options);
  return chosen == 0 ? project_rate : chosen;
}

[[nodiscard]] foundation::Result<PublishOutcome> render_master_impl(
    const project::Snapshot& snapshot, const persistence::ProjectStore& store,
    const WriterOptions& options, const std::function<void()>& after_writer_begin,
    std::optional<project::TrackId> solo_track = std::nullopt,
    std::uint64_t source_extent_floor = 0, const engine::CompiledMidiPlan* compiled_midi = nullptr,
    const std::vector<automation_project::PreparedTargetAutomation>* prepared_automation = nullptr,
    std::span<const project::TrackId> detector_buses = {}, RenderControls* controls = nullptr) {
  // The render computes as the device callback does (engine/denormals.hpp).
  const engine::DenormalsFlushed flushed;
  // Frozen tracks play their renders (ADR-0059). Prepared plans come from
  // the caller's view, which is never frozen.
  if (project::has_frozen_track(snapshot)) {
    if (compiled_midi != nullptr || prepared_automation != nullptr)
      return foundation::Result<PublishOutcome>::failure(
          {foundation::ErrorCode::corrupt_data, "A prepared render plan names a frozen track"});
    const auto view = project::audible_snapshot(snapshot);
    return render_master_impl(view, store, options, after_writer_begin, solo_track,
                              source_extent_floor, nullptr, nullptr, detector_buses, controls);
  }
#if !OSCILLATE_HAVE_CONVOLUTION
  if (project::has_convolution_insert(snapshot))
    return foundation::Result<PublishOutcome>::failure(
        {foundation::ErrorCode::unsupported,
         "Saved convolution insert is not available for export yet"});
#endif
  // Only the solo track's own content sets a track render's length.
  const bool solo_extent_only = controls != nullptr && controls->solo_extent_only && solo_track;
  const auto sets_extent = [&](const project::Track& track) {
    return !solo_extent_only || track.id == *solo_track;
  };
  std::optional<std::uint64_t> solo_first;
  const auto sounds_from = [&](const project::Track& track, std::uint64_t frame) {
    if (solo_track && track.id == *solo_track)
      solo_first = std::min(solo_first.value_or(frame), frame);
  };
  std::optional<std::vector<automation_project::PreparedTargetAutomation>> local_automation;
  if (prepared_automation == nullptr) {
    const auto view = native_export_view(snapshot);
    auto prepared =
        automation_project::prepare_project_automation(view ? *view : snapshot, snapshot.revision);
    if (!prepared) {
      return foundation::Result<PublishOutcome>::failure(prepared.error());
    }
    local_automation.emplace(std::move(prepared).value());
    prepared_automation = &*local_automation;
  }
  const bool has_track_native = std::ranges::any_of(snapshot.tracks, [](const auto& track) {
    return std::ranges::any_of(track.effects, [](const auto& slot) {
      return slot.enabled || !std::holds_alternative<project::Vst3PluginInstance>(slot.processor);
    });
  });
  std::optional<OfflineRouting> routed;
  if (project::requires_routed_rendering(snapshot) && !has_track_native) {
    auto prepared =
        prepare_offline_routing(snapshot, *prepared_automation, {}, {}, solo_track, detector_buses);
    if (!prepared)
      return foundation::Result<PublishOutcome>::failure(prepared.error());
    routed.emplace(std::move(prepared).value());
  }
  if (!timeline::supported_sample_rate(snapshot.sample_rate)) {
    return foundation::Result<PublishOutcome>::failure(
        {foundation::ErrorCode::unsupported, "Master export sample rate is unsupported"});
  }
  auto inserts = detail::preflight_inserts(snapshot, detail::ExportSource::native, false,
                                           OSCILLATE_HAVE_CONVOLUTION);
  if (!inserts)
    return foundation::Result<PublishOutcome>::failure(inserts.error());
  native_inserts::StageContext stage_context{snapshot.sample_rate};
  stage_context.snapshot = &snapshot;
  stage_context.automation = *prepared_automation;
  stage_context.max_frames = block_frames;
#if OSCILLATE_HAVE_CONVOLUTION
  stage_context.convolution = [&](const project::NativeConvolution& saved)
      -> foundation::Result<native_inserts::PreparedStage> {
    auto prepared = prepare_saved_convolution(snapshot, store, saved);
    if (!prepared)
      return foundation::Result<native_inserts::PreparedStage>::failure(prepared.error());
    native_inserts::PreparedStage stage;
    stage.kind = native_inserts::StageKind::convolution;
    stage.generation = saved.generation;
    const auto tail = prepared.value().processor->tail_frames();
    stage.export_tail_frames = saved.wet == 0.0 ? 0 : tail;
    stage.transition_tail_frames = saved.wet == 0.0 ? 1 : std::max<std::uint64_t>(1, tail);
    stage.transition_rule = native_inserts::TransitionRule::override;
    stage.runtime =
        std::make_shared<native_dsp::ConvolutionRuntime>(std::move(prepared.value().processor));
    return foundation::Result<native_inserts::PreparedStage>::success(std::move(stage));
  };
#endif
  // Offline renders need every tail bounded.
  const auto prepare_offline_stage =
      [&](const project::EffectInsert& slot) -> foundation::Result<native_inserts::PreparedStage> {
    auto prepared = native_inserts::prepare_native_stage(slot, stage_context);
    if (prepared && !prepared.value().export_tail_frames)
      return foundation::Result<native_inserts::PreparedStage>::failure(
          *prepared.value().tail_error);
    return prepared;
  };
  std::vector<NativeRouteRuntime> track_chains;
  std::uint64_t track_chain_tail_frames = 0;
  std::vector<routing::BoundStage> receipts;
  for (const auto& saved_chain : inserts.value().native_track_chains) {
    const auto owner = std::ranges::find(snapshot.tracks, saved_chain.owner, &project::Track::id);
    if (owner == snapshot.tracks.end())
      return foundation::Result<PublishOutcome>::failure(
          {foundation::ErrorCode::corrupt_data, "Offline chain has no saved track"});
    track_chain::ChainContext context{stage_context};
    context.detector_slot = routing::sidechain_target(snapshot, owner->id);
    const auto saved = track_chain::saved_chain(*owner);
    auto prepared = track_chain::prepare_chain(saved, context);
    if (!prepared)
      return foundation::Result<PublishOutcome>::failure(prepared.error());
    // Offline renders need every tail bounded.
    for (const auto& stage : prepared.value().stages) {
      if (!stage.export_tail_frames)
        return foundation::Result<PublishOutcome>::failure(
            {foundation::ErrorCode::unsupported, "Offline effect tail is unbounded"});
    }
    if (!prepared.value().runtime)
      continue;
    auto stages = track_chain::receipts(owner->id, prepared.value());
    receipts.insert(receipts.end(), stages.begin(), stages.end());
    track_chains.push_back({owner->id, std::move(prepared).value()});
  }
  if (!inserts.value().native_track_chains.empty()) {
    auto prepared = prepare_offline_routing(snapshot, *prepared_automation, receipts, track_chains,
                                            solo_track, detector_buses);
    if (!prepared)
      return foundation::Result<PublishOutcome>::failure(prepared.error());
    track_chain_tail_frames = prepared.value().native_tail_frames;
    routed.emplace(std::move(prepared).value());
  }
  // Taps read each track's node of the routed mixer, so a tapped render is
  // routed even with nothing on its tracks.
  const bool tapping = controls != nullptr && controls->taps != nullptr;
  if (tapping && !routed) {
    auto prepared =
        prepare_offline_routing(snapshot, *prepared_automation, {}, {}, solo_track, detector_buses);
    if (!prepared)
      return foundation::Result<PublishOutcome>::failure(prepared.error());
    routed.emplace(std::move(prepared).value());
  }
  // A routed render runs its instruments, and each dependency level of its
  // mixer, on worker threads as live playback does (ADR-0048). Every task
  // writes only its own track's buffers and every node sums its inputs in a
  // fixed order, so the result is bit-identical to rendering on one thread.
  std::optional<engine::AudioWorkerPool> workers;
  if (routed) {
    workers.emplace(engine::AudioWorkerPool::default_worker_count());
    try {
      workers->start_owned_threads(workers->wanted_workers());
    } catch (const std::system_error&) {
      workers.reset(); // Without threads the same work runs here, in order.
    }
  }
  engine::AudioWorkerPool* const pool = workers ? &*workers : nullptr;
  // The master chain in saved order. Tails ring out after the mix; latency
  // drains internally and is trimmed at the writer.
  std::vector<std::shared_ptr<engine::ProcessorRuntime>> master_stages;
  std::uint64_t master_tail_frames = 0;
  std::uint64_t master_latency_frames = 0;
  for (const auto* slot : inserts.value().master_chain) {
    auto prepared = prepare_offline_stage(*slot);
    if (!prepared)
      return foundation::Result<PublishOutcome>::failure(prepared.error());
    auto& stage = prepared.value();
    if (*stage.export_tail_frames > std::numeric_limits<std::uint64_t>::max() - master_tail_frames)
      return foundation::Result<PublishOutcome>::failure(
          {foundation::ErrorCode::resource_exhausted, "Master effect tail exceeds render range"});
    master_tail_frames += *stage.export_tail_frames;
    master_latency_frames += stage.latency_frames;
    master_stages.push_back(std::move(stage.runtime));
  }

  std::vector<OpenSource> sources;
  std::vector<PreparedClip> clips;
  std::vector<PreparedSynth> synths;
  std::optional<engine::CompiledMidiPlan> local_midi;
  if (compiled_midi == nullptr) {
    auto compiled = engine::compile_midi_events(snapshot);
    if (!compiled) {
      return foundation::Result<PublishOutcome>::failure(compiled.error());
    }
    local_midi.emplace(std::move(compiled).value());
    compiled_midi = &*local_midi;
  }
  std::uint64_t total_frames = source_extent_floor;
  // Where the notes and clips end, before any release or effect tail.
  std::uint64_t music_end = source_extent_floor;
  std::set<project::TrackId> detector_tracks;
  if (solo_track) {
    for (const auto& track : snapshot.tracks) {
      if (std::ranges::any_of(
              track.sends,
              [](const auto& send) { return send.purpose == project::SendPurpose::sidechain; }) ||
          std::ranges::any_of(track.sends, [&](const auto& send) {
            return send.purpose == project::SendPurpose::audible &&
                   std::ranges::find(detector_buses, send.destination) != detector_buses.end();
          }))
        detector_tracks.insert(track.id);
    }
  }
  for (std::size_t track_index = 0; track_index < snapshot.tracks.size(); ++track_index) {
    const auto& track = snapshot.tracks[track_index];
    if (solo_track && track.id != *solo_track && !detector_tracks.contains(track.id)) {
      continue;
    }
    if (track.kind == project::TrackKind::instrument &&
        std::holds_alternative<project::NativeInstrumentPatch>(track.instrument_source)) {
      if (!track.clips.empty()) {
        return foundation::Result<PublishOutcome>::failure(
            {foundation::ErrorCode::unsupported,
             "Master export requires no audio clips on instrument tracks"});
      }
      const auto plan =
          std::ranges::find_if(compiled_midi->instruments(),
                               [&track](const auto& item) { return item.track() == track.id; });
      if (plan == compiled_midi->instruments().end()) {
        return foundation::Result<PublishOutcome>::failure(
            {foundation::ErrorCode::corrupt_data, "Instrument MIDI event plan is missing"});
      }
      if (plan->events().empty()) {
        continue;
      }
      if (synths.size() == max_export_instruments) {
        return foundation::Result<PublishOutcome>::failure(
            {foundation::ErrorCode::resource_exhausted, "Master export instrument limit exceeded"});
      }
      auto media = source_prep::load_synth_media(
          store, snapshot,
          std::get<project::NativeInstrumentPatch>(track.instrument_source).document);
      if (!media) {
        return foundation::Result<PublishOutcome>::failure(media.error());
      }
      auto player = native_dsp::synth::NativeSynthPlayer::prepare(
          snapshot, track, *plan, *prepared_automation, block_frames,
          std::numeric_limits<std::uint64_t>::max(), std::move(media).value());
      if (!player) {
        return foundation::Result<PublishOutcome>::failure(player.error());
      }
      if (sets_extent(track)) {
        total_frames =
            std::max(total_frames, synth_event_extent(*plan, player.value().tail_frames()));
        music_end = std::max(music_end, synth_event_extent(*plan, 0));
      }
      sounds_from(track, static_cast<std::uint64_t>(plan->events().front().sample.value));
      synths.push_back({std::move(player).value(), track_index});
      continue;
    }
    if (track.kind == project::TrackKind::instrument) {
      // An empty instrument track is silent; any other source has no
      // offline renderer here.
      if (!track.clips.empty() || !track.midi_clips.empty() ||
          !std::holds_alternative<std::monostate>(track.instrument_source)) {
        return foundation::Result<PublishOutcome>::failure(
            {foundation::ErrorCode::unsupported,
             "Master export requires a built-in instrument and no audio clips on instrument "
             "tracks"});
      }
      continue;
    }
    if (track.kind == project::TrackKind::group_bus ||
        track.kind == project::TrackKind::return_bus) {
      continue;
    }
    if (track.kind != project::TrackKind::audio ||
        !std::holds_alternative<std::monostate>(track.instrument_source) ||
        !track.midi_clips.empty()) {
      return foundation::Result<PublishOutcome>::failure(
          {foundation::ErrorCode::unsupported,
           "Master export track kind or content is unsupported"});
    }
    for (const auto& clip : track.clips) {
      if (clips.size() == max_export_clips) {
        return foundation::Result<PublishOutcome>::failure(
            {foundation::ErrorCode::resource_exhausted, "Master export clip limit exceeded"});
      }
      auto processed = prepare_processed_export_clip(snapshot, store, clip);
      if (!processed)
        return foundation::Result<PublishOutcome>::failure(processed.error());
      if (processed.value()) {
        auto& warped = *processed.value();
        auto fades = project::prepare_clip_fades(clip.fades, warped.last - warped.first);
        if (!fades)
          return foundation::Result<PublishOutcome>::failure(fades.error());
        if (sets_extent(track)) {
          total_frames = std::max(total_frames, warped.last);
          music_end = std::max(music_end, warped.last);
        }
        sounds_from(track, warped.first);
        clips.push_back(PreparedClip{0, track_index, warped.first, warped.last, 0, fades.value(),
                                     std::move(warped.reader)});
        continue;
      }
      auto interval = sample_interval(snapshot, clip.placement);
      if (!interval) {
        return foundation::Result<PublishOutcome>::failure(interval.error());
      }
      const auto [first, last] = interval.value();
      if (clip.source_range.start.value < 0 ||
          clip.source_range.end.value <= clip.source_range.start.value) {
        return foundation::Result<PublishOutcome>::failure(
            {foundation::ErrorCode::unsupported, "Master export source range is invalid"});
      }
      const auto source_record = std::ranges::find_if(
          snapshot.sources, [&clip](const auto& source) { return source.id == clip.source; });
      if (source_record == snapshot.sources.end() ||
          static_cast<std::uint64_t>(clip.source_range.end.value) > source_record->frames) {
        return foundation::Result<PublishOutcome>::failure(
            {foundation::ErrorCode::corrupt_data, "Clip source range exceeds managed media"});
      }
      auto source_first =
          media::source_rate_frame_count(static_cast<std::uint64_t>(clip.source_range.start.value),
                                         source_record->sample_rate, snapshot.sample_rate);
      auto source_last =
          media::source_rate_frame_count(static_cast<std::uint64_t>(clip.source_range.end.value),
                                         source_record->sample_rate, snapshot.sample_rate);
      if (!source_first || !source_last) {
        return foundation::Result<PublishOutcome>::failure(!source_first ? source_first.error()
                                                                         : source_last.error());
      }
      if (source_last.value() <= source_first.value() ||
          source_last.value() - source_first.value() != last - first) {
        return foundation::Result<PublishOutcome>::failure(
            {foundation::ErrorCode::unsupported,
             "Master export does not yet render stretched or warped clips"});
      }
      auto source_index = prepare_source(snapshot, store, clip, sources);
      if (!source_index) {
        return foundation::Result<PublishOutcome>::failure(source_index.error());
      }
      if (source_last.value() > sources[source_index.value()].reader.frame_count()) {
        return foundation::Result<PublishOutcome>::failure(
            {foundation::ErrorCode::corrupt_data, "Clip source range exceeds managed media"});
      }
      auto fades = project::prepare_clip_fades(clip.fades, last - first);
      if (!fades)
        return foundation::Result<PublishOutcome>::failure(fades.error());
      clips.push_back(PreparedClip{source_index.value(), track_index, first, last,
                                   source_first.value(), fades.value(), nullptr});
      if (sets_extent(track)) {
        total_frames = std::max(total_frames, last);
        music_end = std::max(music_end, last);
      }
      sounds_from(track, first);
    }
  }
  if (clips.empty() && synths.empty() && !solo_track) {
    return foundation::Result<PublishOutcome>::failure(
        {foundation::ErrorCode::unsupported, "Master export requires audible clips or MIDI notes"});
  }
  if (track_chain_tail_frames > std::numeric_limits<std::uint64_t>::max() - total_frames) {
    return foundation::Result<PublishOutcome>::failure(
        {foundation::ErrorCode::resource_exhausted, "Native route tail exceeds render range"});
  }
  total_frames += track_chain_tail_frames;
  if (master_tail_frames > std::numeric_limits<std::uint64_t>::max() - total_frames) {
    return foundation::Result<PublishOutcome>::failure(
        {foundation::ErrorCode::resource_exhausted, "Master effect tail exceeds render range"});
  }
  total_frames += master_tail_frames;
  const auto route_latency_frames = routed ? routed->output_latency_frames : 0;
  if (route_latency_frames > std::numeric_limits<std::uint64_t>::max() - total_frames) {
    return foundation::Result<PublishOutcome>::failure(
        {foundation::ErrorCode::resource_exhausted, "Routed latency exceeds render range"});
  }
  total_frames += route_latency_frames;
  if (master_latency_frames > std::numeric_limits<std::uint64_t>::max() - total_frames) {
    return foundation::Result<PublishOutcome>::failure(
        {foundation::ErrorCode::resource_exhausted, "Master effect latency exceeds render range"});
  }
  total_frames += master_latency_frames;
  // A latent stage (the limiter's lookahead) emits each input frame late.
  // Render the extra drain internally, then remove the leading latency at the
  // writer boundary, keeping the published timeline and upstream tails in place.
  const auto leading_latency_frames = route_latency_frames + master_latency_frames;
  // A track render keeps only what the track sounds: from its first frame to
  // the end of its tail, or exactly the requested range.
  std::uint64_t content_first = 0;
  if (controls != nullptr) {
    if (controls->range) {
      const auto [first, last] = *controls->range;
      if (last <= first ||
          last > std::numeric_limits<std::uint64_t>::max() - leading_latency_frames)
        return foundation::Result<PublishOutcome>::failure(
            {foundation::ErrorCode::invalid_argument, "The render range is empty"});
      content_first = first;
      total_frames = last + leading_latency_frames;
    } else if (solo_track) {
      if (!solo_first)
        return foundation::Result<PublishOutcome>::failure(
            {foundation::ErrorCode::unsupported, "The track has nothing to render"});
      content_first = *solo_first;
    }
    if (content_first >= total_frames - leading_latency_frames)
      return foundation::Result<PublishOutcome>::failure(
          {foundation::ErrorCode::unsupported, "The track has nothing to render"});
    controls->first_frame = content_first;
  }
  const auto leading_skip_frames = leading_latency_frames + content_first;
  const auto delivered_frames = total_frames - leading_skip_frames;
  const auto output_rate = requested_rate(options, snapshot.sample_rate);
  if (!timeline::supported_sample_rate(output_rate)) {
    return foundation::Result<PublishOutcome>::failure(
        {foundation::ErrorCode::invalid_argument, "Export output sample rate is unsupported"});
  }
  auto output_frames =
      detail::converted_frame_count(delivered_frames, snapshot.sample_rate, output_rate);
  if (!output_frames) {
    return foundation::Result<PublishOutcome>::failure(output_frames.error());
  }
  RenderLoudnessReport* loudness_report = nullptr;
  if (const auto* wav = std::get_if<MasterRenderOptions>(&options)) {
    loudness_report = wav->loudness_report;
    if (loudness_report != nullptr && wav->format != WavFormat::float32) {
      return foundation::Result<PublishOutcome>::failure(
          {foundation::ErrorCode::unsupported,
           "Integrated loudness report requires float32 WAV output"});
    }
    if (loudness_report != nullptr &&
        output_frames.value() > static_cast<std::uint64_t>(output_rate) * 3'600ULL) {
      return foundation::Result<PublishOutcome>::failure(
          {foundation::ErrorCode::resource_exhausted,
           "Export loudness report exceeds the one-hour analysis limit"});
    }
  }
  std::optional<native_dsp::IntegratedLoudnessAnalyzer> loudness;
  std::optional<native_dsp::StereoTruePeakMeter> true_peak;
  if (loudness_report != nullptr) {
    loudness.emplace(output_rate, native_dsp::LoudnessLayout::stereo);
    if (loudness->status() != native_dsp::LoudnessStatus::ok) {
      return foundation::Result<PublishOutcome>::failure(
          {foundation::ErrorCode::unsupported, "Cannot prepare export loudness measurement"});
    }
    true_peak.emplace();
    if (!true_peak->prepare(output_rate)) {
      return foundation::Result<PublishOutcome>::failure(
          {foundation::ErrorCode::unsupported, "Cannot prepare export true-peak measurement"});
    }
  }
  std::array<double, block_frames * 2> mix{};
  std::array<float, block_frames * 2> output{};
  std::array<engine::StereoFrame, block_frames> master_frames{};
  std::array<media::StereoFrame, block_frames> scratch{};
  std::array<engine::StereoFrame, block_frames> instrument_mix{};
  const auto measure_emitted = [&](std::span<const float> emitted) {
    if (!loudness) {
      return true;
    }
    if (loudness->append(emitted) != native_dsp::LoudnessStatus::ok) {
      return false;
    }
    std::array<engine::StereoFrame, block_frames> decoded{};
    for (std::size_t offset = 0; offset < emitted.size() / 2; offset += decoded.size()) {
      const auto count = std::min(decoded.size(), emitted.size() / 2 - offset);
      for (std::size_t index = 0; index < count; ++index) {
        decoded[index] = {emitted[(offset + index) * 2], emitted[(offset + index) * 2 + 1]};
      }
      if (!true_peak->process(std::span(decoded).first(count))) {
        return false;
      }
    }
    return true;
  };
  std::optional<detail::MasterOutput> writer;
  if (!tapping) {
    auto begun = detail::MasterOutput::begin(options, snapshot.sample_rate, delivered_frames,
                                             measure_emitted);
    if (!begun) {
      return foundation::Result<PublishOutcome>::failure(begun.error());
    }
    writer.emplace(std::move(begun).value());
  }
  auto sink = writer ? MasterSink(*writer) : MasterSink(*controls->taps, content_first);
  if (after_writer_begin) {
    after_writer_begin();
  }
  // Each tapped track's node output, delayed by what its latency is short of
  // the latest tapped track's, so every tap lines up at block_first -
  // tap_latency. The first frames of each delay are silence.
  struct TapLane {
    std::size_t node;
    std::vector<engine::StereoFrame> pending;
    std::vector<float> left;
    std::vector<float> right;
  };
  std::vector<TapLane> tap_lanes;
  std::vector<TrackTap> track_taps;
  std::uint64_t tap_latency = 0;
  if (tapping) {
    for (const auto& source : routed->sources)
      tap_latency = std::max(tap_latency, source.output_latency_frames);
    for (const auto& source : routed->sources) {
      tap_lanes.push_back({source.index,
                           std::vector<engine::StereoFrame>(static_cast<std::size_t>(
                               tap_latency - source.output_latency_frames)),
                           std::vector<float>(block_frames), std::vector<float>(block_frames)});
      track_taps.push_back({source.track, {}, {}});
    }
  }
  // A trimmed tail: what follows the music is held back, then cut where it
  // falls quiet (see tail_quiet_below_music_db).
  const bool trim_tail =
      std::visit([](const auto& chosen) { return chosen.trim_silent_tail; }, options);
  TailTrim tail(snapshot.sample_rate, music_end > content_first ? music_end - content_first : 0);
  std::uint64_t blocks = 0;
  // Each audio clip into its track's input (or the plain mix), for one block.
  const auto read_clips = [&](std::uint64_t block_first,
                              std::size_t count) -> foundation::Result<void> {
    for (const auto& clip : clips) {
      if (routed && routed->silent_inputs[routed->track_slots[clip.track_index]])
        continue;
      const auto overlap_first = std::max(block_first, clip.first);
      const auto overlap_last = std::min(block_first + count, clip.last);
      if (overlap_first >= overlap_last) {
        continue;
      }
      const auto overlap_count = static_cast<std::size_t>(overlap_last - overlap_first);
      const auto destination = std::span<media::StereoFrame>(scratch).first(overlap_count);
      auto read = clip.warp ? clip.warp->read_at(overlap_first - clip.first, destination)
                            : sources[clip.source_index].reader.read_at(
                                  clip.source_first + overlap_first - clip.first, destination);
      if (!read) {
        return foundation::Result<void>::failure(read.error());
      }
      if (read.value() != overlap_count) {
        return foundation::Result<void>::failure(
            {foundation::ErrorCode::io_failure, "Managed source ended during master render"});
      }
      const auto offset = static_cast<std::size_t>(overlap_first - block_first);
      for (std::size_t index = 0; index < overlap_count; ++index) {
        const auto gain = project::clip_fade_gain(clip.fades, overlap_first - clip.first + index);
        if (routed) {
          auto& frame = routed->inputs[routed->track_slots[clip.track_index]][offset + index];
          frame.left += static_cast<float>(scratch[index].left * gain);
          frame.right += static_cast<float>(scratch[index].right * gain);
        } else {
          mix[(offset + index) * 2] += scratch[index].left * gain;
          mix[(offset + index) * 2 + 1] += scratch[index].right * gain;
        }
      }
    }
    return foundation::Result<void>::success();
  };
  // Each instrument into its own track's input, one task each, for one block.
  // Each instrument renders into its own track's input, one task each.
  struct SynthJob {
    std::span<PreparedSynth> synths;
    OfflineRouting* route;
    std::uint64_t first;
    std::size_t count;
    engine::AudioWorkerPool* pool;
    std::atomic<bool> failed{false};
  };
  const auto render_one = [](void* context, std::size_t task, std::size_t) noexcept {
    auto& synth_job = *static_cast<SynthJob*>(context);
    auto& synth = synth_job.synths[task];
    const auto slot = synth_job.route->track_slots[synth.track_index];
    const auto output = std::span(synth_job.route->inputs[slot]).first(synth_job.count);
    std::ranges::fill(output, engine::StereoFrame{});
    if (synth_job.route->silent_inputs[slot])
      return;
    if (!synth.player.render(output, synth_job.first, 1, synth_job.pool))
      synth_job.failed.store(true, std::memory_order_relaxed);
  };
  const auto render_routed_synths = [&](std::uint64_t block_first,
                                        std::size_t count) -> foundation::Result<void> {
    for (auto& synth : synths)
      synth.player.await_content();
    SynthJob job{synths, &*routed, block_first, count, pool};
    if (pool != nullptr)
      pool->run(synths.size(), render_one, &job);
    else
      for (std::size_t index = 0; index < synths.size(); ++index)
        render_one(&job, index, 0);
    if (job.failed.load(std::memory_order_relaxed))
      return foundation::Result<void>::failure(
          {foundation::ErrorCode::corrupt_data, "Built-in synth block failed"});
    return foundation::Result<void>::success();
  };
  // A pipeline's inputs belong to synths or to clips, never both (an
  // instrument track has no audio clips), so clips can fill theirs while the
  // synths render the next block.
  std::vector<bool> synth_slot(routed ? routed->inputs.size() : 0);
  if (routed)
    for (const auto& synth : synths)
      synth_slot[routed->track_slots[synth.track_index]] = true;
  // Without taps, a routed render runs as a pipeline (RoutedMixer::mix_step):
  // every node at once, each dependency level a block behind the one before,
  // so buses and returns stop waiting on their tracks. Each node sees the same
  // inputs in the same order, so the output is bit-identical.
  const auto pipeline_depth =
      routed && !tapping ? routed->mixer->prepare_pipeline() : std::optional<std::size_t>{};
  std::uint64_t input_first = 0; // The next block a pipeline is fed.
  for (std::uint64_t block_first = 0; block_first < total_frames;) {
    if (controls != nullptr && controls->progress && (blocks++ % 16) == 0 &&
        !controls->progress(block_first, total_frames))
      return foundation::Result<PublishOutcome>::failure(
          {foundation::ErrorCode::cancelled, "Render cancelled"});
    const auto count =
        static_cast<std::size_t>(std::min<std::uint64_t>(block_frames, total_frames - block_first));
    if (pipeline_depth) {
      // Feed blocks until the pipeline emits this one; past the end, silent
      // blocks drain it.
      std::size_t emitted = 0;
      while (emitted == 0) {
        const auto block_at = [&](std::uint64_t first) {
          return static_cast<std::size_t>(
              first < total_frames ? std::min<std::uint64_t>(block_frames, total_frames - first)
                                   : block_frames);
        };
        const bool real = input_first < total_frames;
        const auto fed = block_at(input_first);
        if (input_first == 0) {
          // The first block's inputs; each step then renders the next.
          for (auto& input : routed->inputs)
            std::fill_n(input.begin(), fed, engine::StereoFrame{});
          if (auto read = read_clips(input_first, fed); !read)
            return foundation::Result<PublishOutcome>::failure(read.error());
          if (auto rendered = render_routed_synths(input_first, fed); !rendered)
            return foundation::Result<PublishOutcome>::failure(rendered.error());
        }
        engine::RoutedGainSpans gains;
        if (real && !routed->gains->fill_block({input_first, 1}, fed, gains))
          return foundation::Result<PublishOutcome>::failure(
              {foundation::ErrorCode::corrupt_data,
               "Offline send gain automation rejected a block"});
        for (std::size_t index = 0; index < routed->inputs.size(); ++index)
          routed->views[index] = std::span(routed->inputs[index]).first(fed);
        auto edge_gains = gains.edges;
        if (solo_track && !routed->isolated_edges.empty()) {
          if (edge_gains.empty()) {
            // A silent drain block still keeps other sources out of the stem.
            for (std::size_t index = 0; index < routed->isolated_gains.size(); ++index)
              routed->isolated_gains[index] = {routed->mixer->plan().edges[index].id, {}};
          } else {
            std::ranges::copy(gains.edges, routed->isolated_gains.begin());
          }
          for (const auto index : routed->isolated_edges)
            routed->isolated_gains[index].values = std::span(routed->silent_gain).first(fed);
          edge_gains = routed->isolated_gains;
        }
        // The step holds its own copy of this block's inputs before any task
        // runs, so the synths render the next block into them alongside.
        const auto next_first = input_first + fed;
        const bool next_real = next_first < total_frames;
        const auto next = block_at(next_first);
        if (next_real)
          for (auto& synth : synths)
            synth.player.await_content();
        SynthJob job{synths, &*routed, next_first, next, pool};
        const auto stepped = routed->mixer->mix_step(
            routed->views, fed, {input_first, 1}, edge_gains, gains.nodes, master_frames, pool,
            next_real ? engine::RoutedStepTasks{synths.size(), render_one, &job}
                      : engine::RoutedStepTasks{});
        if (!stepped)
          return foundation::Result<PublishOutcome>::failure(
              {foundation::ErrorCode::corrupt_data, "Offline route mixer rejected a block"});
        if (job.failed.load(std::memory_order_relaxed))
          return foundation::Result<PublishOutcome>::failure(
              {foundation::ErrorCode::corrupt_data, "Built-in synth block failed"});
        for (std::size_t index = 0; index < routed->inputs.size(); ++index)
          if (!next_real || !synth_slot[index])
            std::fill_n(routed->inputs[index].begin(), next, engine::StereoFrame{});
        if (next_real)
          if (auto read = read_clips(next_first, next); !read)
            return foundation::Result<PublishOutcome>::failure(read.error());
        emitted = *stepped;
        input_first = next_first;
      }
      if (emitted != count)
        return foundation::Result<PublishOutcome>::failure(
            {foundation::ErrorCode::corrupt_data, "Offline route pipeline lost a block"});
      if (std::ranges::any_of(track_chains, [](const auto& chain) {
            return std::ranges::any_of(chain.chain.stages, [](const auto& stage) {
              return guarded_stage(*stage.runtime);
            });
          }))
        return foundation::Result<PublishOutcome>::failure(
            {foundation::ErrorCode::corrupt_data,
             "Native route chain encountered a non-finite frame"});
      for (std::size_t index = 0; index < count; ++index) {
        output[index * 2] = master_frames[index].left;
        output[index * 2 + 1] = master_frames[index].right;
      }
    } else {
      std::fill_n(mix.begin(), count * 2, 0.0);
      if (routed) {
        for (auto& input : routed->inputs) {
          std::fill_n(input.begin(), count, engine::StereoFrame{});
        }
      }
      if (auto read = read_clips(block_first, count); !read)
        return foundation::Result<PublishOutcome>::failure(read.error());
      std::fill_n(instrument_mix.begin(), count, engine::StereoFrame{});
      if (routed) {
        if (auto rendered = render_routed_synths(block_first, count); !rendered)
          return foundation::Result<PublishOutcome>::failure(rendered.error());
      } else {
        for (auto& synth : synths) {
          auto output = std::span(instrument_mix).first(count);
          synth.player.await_content();
          if (!synth.player.render(output, block_first, 1)) {
            return foundation::Result<PublishOutcome>::failure(
                {foundation::ErrorCode::corrupt_data, "Built-in synth block failed"});
          }
        }
      }
      if (routed) {
        for (std::size_t index = 0; index < routed->inputs.size(); ++index) {
          routed->views[index] = std::span(routed->inputs[index]).first(count);
        }
        engine::RoutedGainSpans gains;
        if (!routed->gains->fill_block({block_first, 1}, count, gains)) {
          return foundation::Result<PublishOutcome>::failure(
              {foundation::ErrorCode::corrupt_data,
               "Offline send gain automation rejected a block"});
        }
        auto edge_gains = gains.edges;
        if (solo_track && !routed->isolated_edges.empty()) {
          std::ranges::copy(gains.edges, routed->isolated_gains.begin());
          for (const auto index : routed->isolated_edges)
            routed->isolated_gains[index].values = std::span(routed->silent_gain).first(count);
          edge_gains = routed->isolated_gains;
        }
        if (!routed->mixer->mix_block(routed->views, std::span(master_frames).first(count),
                                      {block_first, 1}, edge_gains, gains.nodes, pool)) {
          return foundation::Result<PublishOutcome>::failure(
              {foundation::ErrorCode::corrupt_data, "Offline route mixer rejected a block"});
        }
        if (tapping && controls->taps->tracks) {
          // Frames before project frame 0 (the delay's silence) are left out.
          const auto skip = block_first < tap_latency
                                ? static_cast<std::size_t>(
                                      std::min<std::uint64_t>(count, tap_latency - block_first))
                                : 0;
          for (std::size_t lane = 0; lane < tap_lanes.size(); ++lane) {
            auto& tap = tap_lanes[lane];
            const auto produced = routed->mixer->node_output(tap.node, count);
            tap.pending.insert(tap.pending.end(), produced.begin(), produced.end());
            for (std::size_t index = 0; index < count; ++index) {
              tap.left[index] = tap.pending[index].left;
              tap.right[index] = tap.pending[index].right;
            }
            tap.pending.erase(tap.pending.begin(),
                              tap.pending.begin() + static_cast<std::ptrdiff_t>(count));
            track_taps[lane].left = std::span<const float>(tap.left).subspan(skip, count - skip);
            track_taps[lane].right = std::span<const float>(tap.right).subspan(skip, count - skip);
          }
          if (skip < count)
            controls->taps->tracks(track_taps, block_first + skip - tap_latency);
        }
        if (std::ranges::any_of(track_chains, [](const auto& chain) {
              return std::ranges::any_of(chain.chain.stages, [](const auto& stage) {
                return guarded_stage(*stage.runtime);
              });
            }))
          return foundation::Result<PublishOutcome>::failure(
              {foundation::ErrorCode::corrupt_data,
               "Native route chain encountered a non-finite frame"});
        for (std::size_t index = 0; index < count; ++index) {
          output[index * 2] = master_frames[index].left;
          output[index * 2 + 1] = master_frames[index].right;
        }
      } else {
        for (std::size_t index = 0; index < count; ++index) {
          mix[index * 2] += instrument_mix[index].left;
          mix[index * 2 + 1] += instrument_mix[index].right;
        }
        for (std::size_t index = 0; index < count * 2; ++index) {
          if (!std::isfinite(mix[index]) ||
              std::abs(mix[index]) > static_cast<double>(std::numeric_limits<float>::max())) {
            return foundation::Result<PublishOutcome>::failure(
                {foundation::ErrorCode::unsupported, "Master mix exceeds finite float range"});
          }
          output[index] = static_cast<float>(mix[index]);
        }
      }
    }
    if (!master_stages.empty()) {
      for (std::size_t index = 0; index < count; ++index) {
        master_frames[index] = {output[index * 2], output[index * 2 + 1]};
      }
      for (const auto& stage : master_stages) {
        stage->process(std::span(master_frames).first(count), {block_first, 1});
        if (guarded_stage(*stage))
          return foundation::Result<PublishOutcome>::failure(
              {foundation::ErrorCode::corrupt_data,
               "Master effect encountered a non-finite frame"});
      }
      for (std::size_t index = 0; index < count; ++index) {
        output[index * 2] = master_frames[index].left;
        output[index * 2 + 1] = master_frames[index].right;
      }
    }
    if (snapshot.master_level != 1.0) {
      for (std::size_t index = 0; index < count * 2; ++index) {
        const double scaled = static_cast<double>(output[index]) * snapshot.master_level;
        if (!std::isfinite(scaled) ||
            std::abs(scaled) > static_cast<double>(std::numeric_limits<float>::max()))
          return foundation::Result<PublishOutcome>::failure(
              {foundation::ErrorCode::unsupported, "Master level exceeds finite float range"});
        output[index] = static_cast<float>(scaled);
      }
    }
    const auto skipped = block_first < leading_skip_frames
                             ? static_cast<std::size_t>(std::min<std::uint64_t>(
                                   count, leading_skip_frames - block_first))
                             : 0;
    const auto write_count = count - skipped;
    if (write_count == 0) {
      block_first += count;
      continue;
    }
    const auto delivered = std::span<const float>(output).subspan(skipped * 2, write_count * 2);
    auto wrote = trim_tail ? tail.write(sink, delivered) : sink.write_interleaved_stereo(delivered);
    if (!wrote) {
      return foundation::Result<PublishOutcome>::failure(wrote.error());
    }
    block_first += count;
  }
  if (trim_tail) {
    auto trimmed = tail.finish(sink);
    if (!trimmed)
      return foundation::Result<PublishOutcome>::failure(trimmed.error());
  }
  if (!writer) // Taps only: nothing was written.
    return foundation::Result<PublishOutcome>::success(PublishOutcome::durable);
  const auto measurement = loudness ? std::optional{loudness->finish()} : std::nullopt;
  if (true_peak && !true_peak->finish()) {
    return foundation::Result<PublishOutcome>::failure(
        {foundation::ErrorCode::corrupt_data, "Export true-peak measurement could not finish"});
  }
  auto published = writer.value().finish();
  if (published && measurement && loudness_report != nullptr) {
    const auto peak = true_peak->reading();
    *loudness_report = {loudness->frames(),          measurement->integrated_lkfs,
                        measurement->momentary_lkfs, measurement->short_term_lkfs,
                        peak.left_sample_peak,       peak.right_sample_peak,
                        peak.left_true_peak,         peak.right_true_peak};
  }
  return published;
}

[[nodiscard]] foundation::Result<std::uint64_t>
stem_source_extent(const project::Snapshot& snapshot, const engine::CompiledMidiPlan& midi_plan) {
  std::uint64_t extent = 0;
  std::size_t clip_count = 0;
  std::size_t instrument_count = 0;
  for (const auto& track : snapshot.tracks) {
    if (track.kind == project::TrackKind::instrument) {
      const auto release_frames = fallback_release_frames;
      const auto plan = std::ranges::find_if(
          midi_plan.instruments(), [&track](const auto& item) { return item.track() == track.id; });
      if (plan == midi_plan.instruments().end()) {
        return foundation::Result<std::uint64_t>::failure(
            {foundation::ErrorCode::corrupt_data, "Stem MIDI plan is missing a track"});
      }
      if (!plan->events().empty() && ++instrument_count > max_export_instruments) {
        return foundation::Result<std::uint64_t>::failure(
            {foundation::ErrorCode::resource_exhausted, "Stem instrument limit exceeded"});
      }
      if (const auto* native =
              std::get_if<project::NativeInstrumentPatch>(&track.instrument_source);
          native != nullptr && !plan->events().empty()) {
        auto resolved = native_dsp::synth::resolve_native_instrument(*native);
        if (!resolved) {
          return foundation::Result<std::uint64_t>::failure(resolved.error());
        }
        const auto tail = resolved.value().engine->tail_frames(
            resolved.value().values, static_cast<double>(snapshot.sample_rate));
        extent = std::max(extent, synth_event_extent(*plan, tail));
      } else {
        for (const auto& event : plan->events()) {
          if (event.kind == engine::MidiEventKind::note_off) {
            if (event.sample.value < 0 ||
                static_cast<std::uint64_t>(event.sample.value) >
                    std::numeric_limits<std::uint64_t>::max() - release_frames) {
              return foundation::Result<std::uint64_t>::failure(
                  {foundation::ErrorCode::resource_exhausted, "Stem instrument tail overflows"});
            }
            extent =
                std::max(extent, static_cast<std::uint64_t>(event.sample.value) + release_frames);
          }
        }
      }
    }
    for (const auto& clip : track.clips) {
      if (++clip_count > max_export_clips) {
        return foundation::Result<std::uint64_t>::failure(
            {foundation::ErrorCode::resource_exhausted, "Stem clip limit exceeded"});
      }
      auto interval = sample_interval(snapshot, clip.placement);
      if (!interval) {
        return foundation::Result<std::uint64_t>::failure(interval.error());
      }
      extent = std::max(extent, interval.value().second);
    }
  }
  if (extent == 0) {
    return foundation::Result<std::uint64_t>::failure(
        {foundation::ErrorCode::unsupported, "Stem export requires clips or MIDI notes"});
  }
  return foundation::Result<std::uint64_t>::success(extent);
}

struct StemStage {
  foundation::UniqueFd parent_fd;
  foundation::UniqueFd stage_fd;
  std::string stage_name;
  std::vector<std::string> files;
  bool published = false;

  StemStage(const StemStage&) = delete;
  StemStage& operator=(const StemStage&) = delete;
  StemStage() = default;
  ~StemStage() {
    if (stage_fd && !published) {
      for (const auto& name : files) {
        (void)::unlinkat(stage_fd.get(), name.c_str(), 0);
      }
    }
    stage_fd.reset();
    if (parent_fd && !published && !stage_name.empty()) {
      (void)::unlinkat(parent_fd.get(), stage_name.c_str(), AT_REMOVEDIR);
    }
  }
};

[[nodiscard]] foundation::Result<std::vector<project::TrackId>>
stem_detector_buses(const project::Snapshot& snapshot) {
  std::vector<project::TrackId> buses;
  for (const auto& track : snapshot.tracks) {
    if (track.kind != project::TrackKind::group_bus && track.kind != project::TrackKind::return_bus)
      continue;
    if (std::ranges::any_of(track.sends, [](const auto& send) {
          return send.purpose == project::SendPurpose::sidechain;
        }))
      buses.push_back(track.id);
  }
  for (const auto bus : buses) {
    const auto owner = std::ranges::find(snapshot.tracks, bus, &project::Track::id);
    if (owner->main_output || std::ranges::any_of(owner->sends, [](const auto& send) {
          return send.purpose == project::SendPurpose::audible;
        }))
      return foundation::Result<std::vector<project::TrackId>>::failure(
          {foundation::ErrorCode::unsupported,
           "Processed stem detector bus cannot have an audible bus output"});
    for (const auto& send : owner->sends) {
      if (send.purpose != project::SendPurpose::sidechain)
        continue;
      const auto target = std::ranges::find(snapshot.tracks, send.destination, &project::Track::id);
      if (target == snapshot.tracks.end() || target->kind != project::TrackKind::audio)
        return foundation::Result<std::vector<project::TrackId>>::failure(
            {foundation::ErrorCode::unsupported,
             "Processed stem detector bus target must be an audio source track"});
    }
    for (const auto& track : snapshot.tracks) {
      if (track.main_output == bus)
        return foundation::Result<std::vector<project::TrackId>>::failure(
            {foundation::ErrorCode::unsupported,
             "Processed stem detector bus cannot be an audible main destination"});
      for (const auto& send : track.sends) {
        if (send.destination != bus)
          continue;
        if (send.purpose != project::SendPurpose::audible ||
            (track.kind != project::TrackKind::audio &&
             track.kind != project::TrackKind::instrument))
          return foundation::Result<std::vector<project::TrackId>>::failure(
              {foundation::ErrorCode::unsupported,
               "Processed stem detector bus requires direct source-track audible sends"});
      }
    }
  }
  return foundation::Result<std::vector<project::TrackId>>::success(std::move(buses));
}

[[nodiscard]] foundation::Result<PublishOutcome>
render_stems_impl(const project::Snapshot& snapshot, const persistence::ProjectStore& store,
                  const StemRenderOptions& options,
                  const std::function<foundation::Result<void>(std::size_t)>& after_stem) {
  // A frozen track's stem is its render through its channel (ADR-0059).
  if (project::has_frozen_track(snapshot))
    return render_stems_impl(project::audible_snapshot(snapshot), store, options, after_stem);
#if !OSCILLATE_HAVE_CONVOLUTION
  if (project::has_convolution_insert(snapshot))
    return foundation::Result<PublishOutcome>::failure(
        {foundation::ErrorCode::unsupported,
         "Saved convolution insert is not available for stem export yet"});
#endif
  auto detector_buses = stem_detector_buses(snapshot);
  if (!detector_buses)
    return foundation::Result<PublishOutcome>::failure(detector_buses.error());
  if (options.loudness_reports != nullptr && options.format != WavFormat::float32) {
    return foundation::Result<PublishOutcome>::failure(
        {foundation::ErrorCode::unsupported,
         "Integrated loudness report requires float32 WAV stems"});
  }
  const auto view = native_export_view(snapshot);
  auto prepared_automation =
      automation_project::prepare_project_automation(view ? *view : snapshot, snapshot.revision);
  if (!prepared_automation) {
    return foundation::Result<PublishOutcome>::failure(prepared_automation.error());
  }
  const auto target_name = options.target_directory.filename().string();
  if (target_name.empty() || target_name == "." || target_name == ".." || snapshot.tracks.empty()) {
    return foundation::Result<PublishOutcome>::failure(
        {foundation::ErrorCode::invalid_argument, "Stem target and tracks are required"});
  }
  auto compiled_midi = engine::compile_midi_events(snapshot);
  if (!compiled_midi) {
    return foundation::Result<PublishOutcome>::failure(compiled_midi.error());
  }
  auto extent = stem_source_extent(snapshot, compiled_midi.value());
  if (!extent) {
    return foundation::Result<PublishOutcome>::failure(extent.error());
  }
  const auto parent_path = options.target_directory.parent_path().empty()
                               ? std::filesystem::path{"."}
                               : options.target_directory.parent_path();
  StemStage stage;
  stage.parent_fd.reset(::open(parent_path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (!stage.parent_fd) {
    return foundation::Result<PublishOutcome>::failure(
        {foundation::ErrorCode::io_failure, "Cannot open stem parent directory"});
  }
  static std::atomic<std::uint64_t> sequence{0};
  for (unsigned attempt = 0; attempt < 16; ++attempt) {
    stage.stage_name = ".oscillate-stems-" + std::to_string(::getpid()) + "-" +
                       std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
    if (::mkdirat(stage.parent_fd.get(), stage.stage_name.c_str(), 0700) == 0) {
      break;
    }
    if (errno != EEXIST || attempt == 15) {
      stage.stage_name.clear();
      return foundation::Result<PublishOutcome>::failure(
          {foundation::ErrorCode::io_failure, "Cannot create private stem directory"});
    }
  }
  stage.stage_fd.reset(::openat(stage.parent_fd.get(), stage.stage_name.c_str(),
                                O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  if (!stage.stage_fd) {
    return foundation::Result<PublishOutcome>::failure(
        {foundation::ErrorCode::io_failure, "Cannot open private stem directory"});
  }
  std::vector<StemLoudnessReport> measured_stems;
  if (options.loudness_reports != nullptr) {
    measured_stems.reserve(snapshot.tracks.size());
  }
  for (std::size_t index = 0; index < snapshot.tracks.size(); ++index) {
    const auto& track = snapshot.tracks[index];
    if (track.kind == project::TrackKind::group_bus ||
        track.kind == project::TrackKind::return_bus) {
      continue;
    }
    const auto file_name = "track-" + track.id.to_string() + ".wav";
    stage.files.push_back(file_name);
    // The writer opens this held directory descriptor, not a path that could
    // be rebound between staging and publication.
    const auto target =
        std::filesystem::path{"/proc/self/fd"} / std::to_string(stage.stage_fd.get()) / file_name;
    RenderLoudnessReport measurement;
    auto rendered = render_master_impl(
        snapshot, store,
        MasterRenderOptions{target, options.format, options.dither_seed, options.sample_rate,
                            options.loudness_reports != nullptr ? &measurement : nullptr},
        {}, track.id, extent.value(), &compiled_midi.value(), &prepared_automation.value(),
        detector_buses.value());
    if (!rendered) {
      return rendered;
    }
    if (rendered.value() != PublishOutcome::durable) {
      return foundation::Result<PublishOutcome>::failure(
          {foundation::ErrorCode::io_failure, "Private stem durability is uncertain"});
    }
    if (options.loudness_reports != nullptr) {
      measured_stems.push_back({track.id, measurement});
    }
    if (after_stem) {
      auto injected = after_stem(index);
      if (!injected) {
        return foundation::Result<PublishOutcome>::failure(injected.error());
      }
    }
  }
  if (auto synced = foundation::sync(stage.stage_fd.get(), "Cannot sync private stem directory");
      !synced) {
    return foundation::Result<PublishOutcome>::failure(std::move(synced.error()));
  }
  // Linux RENAME_NOREPLACE prevents another worker's target from being
  // overwritten. There is no portable multi-file atomic publication primitive.
  if (auto renamed = foundation::rename_in(stage.parent_fd.get(), stage.stage_name, target_name,
                                           foundation::PublishMode::no_replace,
                                           "Cannot publish stem directory");
      !renamed) {
    return foundation::Result<PublishOutcome>::failure(std::move(renamed.error()));
  }
  stage.published = true;
  if (::fsync(stage.parent_fd.get()) != 0) {
    if (options.loudness_reports != nullptr) {
      *options.loudness_reports = std::move(measured_stems);
    }
    return foundation::Result<PublishOutcome>::success(PublishOutcome::durability_uncertain);
  }
  if (options.loudness_reports != nullptr) {
    *options.loudness_reports = std::move(measured_stems);
  }
  return foundation::Result<PublishOutcome>::success(PublishOutcome::durable);
}

} // namespace

foundation::Result<PublishOutcome> detail::render_wav_master_with_start_hook(
    const project::Snapshot& snapshot, const persistence::ProjectStore& store,
    const LiveRevisionState& live, const MasterRenderOptions& options,
    const std::function<void()>& after_writer_begin) {
  if (live.application != RevisionApplication::active || live.model_revision != snapshot.revision ||
      live.active_graph_revision != snapshot.revision) {
    const std::string detail = live.failure ? live.failure->message : "graph is pending or stale";
    return foundation::Result<PublishOutcome>::failure(
        {foundation::ErrorCode::conflict, "Master export rejected: " + detail});
  }
  return render_master_impl(snapshot, store, options, after_writer_begin);
}

foundation::Result<PublishOutcome> detail::render_wav_master_with_start_hook(
    const project::Snapshot& snapshot, const persistence::ProjectStore& store,
    StoppedExportAuthority authority, const MasterRenderOptions& options,
    const std::function<void()>& after_writer_begin) {
  if (!authority.matches(snapshot)) {
    return foundation::Result<PublishOutcome>::failure(
        {foundation::ErrorCode::conflict, "Master export rejected: stopped authority is stale"});
  }
  return render_master_impl(snapshot, store, options, after_writer_begin);
}

foundation::Result<PublishOutcome> render_wav_master(const project::Snapshot& snapshot,
                                                     const persistence::ProjectStore& store,
                                                     const LiveRevisionState& live,
                                                     const MasterRenderOptions& options) {
  return detail::render_wav_master_with_start_hook(snapshot, store, live, options, {});
}

foundation::Result<PublishOutcome> render_wav_master(const project::Snapshot& snapshot,
                                                     const persistence::ProjectStore& store,
                                                     StoppedExportAuthority authority,
                                                     const MasterRenderOptions& options) {
  return detail::render_wav_master_with_start_hook(snapshot, store, std::move(authority), options,
                                                   {});
}

foundation::Result<PublishOutcome> detail::render_wav_stems_with_after_stem_hook(
    const project::Snapshot& snapshot, const persistence::ProjectStore& store,
    const LiveRevisionState& live, const StemRenderOptions& options,
    const std::function<foundation::Result<void>(std::size_t)>& after_stem) {
  if (live.application != RevisionApplication::active || live.model_revision != snapshot.revision ||
      live.active_graph_revision != snapshot.revision) {
    const std::string reason = live.failure ? live.failure->message : "graph is pending or stale";
    return foundation::Result<PublishOutcome>::failure(
        {foundation::ErrorCode::conflict, "Stem export rejected: " + reason});
  }
  return render_stems_impl(snapshot, store, options, after_stem);
}

foundation::Result<PublishOutcome> render_wav_stems(const project::Snapshot& snapshot,
                                                    const persistence::ProjectStore& store,
                                                    const LiveRevisionState& live,
                                                    const StemRenderOptions& options) {
  return detail::render_wav_stems_with_after_stem_hook(snapshot, store, live, options, {});
}

foundation::Result<PublishOutcome> render_wav_stems(const project::Snapshot& snapshot,
                                                    const persistence::ProjectStore& store,
                                                    StoppedExportAuthority authority,
                                                    const StemRenderOptions& options) {
  if (!authority.matches(snapshot)) {
    return foundation::Result<PublishOutcome>::failure(
        {foundation::ErrorCode::conflict, "Stem export rejected: stopped authority is stale"});
  }
  return render_stems_impl(snapshot, store, options, {});
}

foundation::Result<PublishOutcome> render_encoded_master(const project::Snapshot& snapshot,
                                                         const persistence::ProjectStore& store,
                                                         const LiveRevisionState& live,
                                                         const EncodedMasterOptions& options) {
  if (live.application != RevisionApplication::active || live.model_revision != snapshot.revision ||
      live.active_graph_revision != snapshot.revision) {
    const std::string detail = live.failure ? live.failure->message : "graph is pending or stale";
    return foundation::Result<PublishOutcome>::failure(
        {foundation::ErrorCode::conflict, "Master export rejected: " + detail});
  }
#if !OSCILLATE_HAVE_CODEC
  (void)store;
  (void)options;
  return foundation::Result<PublishOutcome>::failure(
      {foundation::ErrorCode::unsupported, "Encoded export requires codec adapter build"});
#else
  return render_master_impl(snapshot, store, options, {});
#endif
}

foundation::Result<PublishOutcome> render_encoded_master(const project::Snapshot& snapshot,
                                                         const persistence::ProjectStore& store,
                                                         StoppedExportAuthority authority,
                                                         const EncodedMasterOptions& options) {
  if (!authority.matches(snapshot)) {
    return foundation::Result<PublishOutcome>::failure(
        {foundation::ErrorCode::conflict, "Master export rejected: stopped authority is stale"});
  }
#if !OSCILLATE_HAVE_CODEC
  (void)store;
  (void)options;
  return foundation::Result<PublishOutcome>::failure(
      {foundation::ErrorCode::unsupported, "Encoded export requires codec adapter build"});
#else
  return render_master_impl(snapshot, store, options, {});
#endif
}

foundation::Result<void> render_master_taps(const project::Snapshot& snapshot,
                                            const persistence::ProjectStore& store,
                                            StoppedExportAuthority authority,
                                            const RenderTaps& taps) {
  if (!authority.matches(snapshot))
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::conflict, "Render rejected: stopped authority is stale"});
  MasterRenderOptions options;
  options.trim_silent_tail = taps.trim_silent_tail;
  RenderControls controls;
  controls.progress = taps.progress;
  controls.taps = &taps;
  auto rendered = render_master_impl(snapshot, store, options, {}, std::nullopt, 0, nullptr,
                                     nullptr, {}, &controls);
  if (!rendered)
    return foundation::Result<void>::failure(rendered.error());
  return foundation::Result<void>::success();
}

foundation::Result<TrackRender> render_track(const project::Snapshot& snapshot,
                                             const persistence::ProjectStore& store,
                                             const TrackRenderOptions& options) {
  using R = foundation::Result<TrackRender>;
  auto view = project::audible_snapshot(snapshot);
  const auto target = std::ranges::find(view.tracks, options.track, &project::Track::id);
  if (target == view.tracks.end())
    return R::failure({foundation::ErrorCode::not_found, "Track to render not found"});
  if (target->kind != project::TrackKind::audio && target->kind != project::TrackKind::instrument)
    return R::failure(
        {foundation::ErrorCode::unsupported, "Only audio and instrument tracks render"});
  // The track alone, before its channel: unity, centred, unmuted, straight
  // to a master with no chain. Keys into its inserts stay.
  for (auto& track : view.tracks)
    track.soloed = false;
  target->muted = false;
  target->level = 1.0;
  target->pan = 0.0;
  target->main_output.reset();
  target->sends.clear();
  std::erase_if(target->automation, [](const project::AutomationLane& lane) {
    const auto* control = std::get_if<project::TrackControlTarget>(&lane.target);
    return (control != nullptr && project::is_channel_control(control->key)) ||
           std::holds_alternative<project::SendGainTarget>(lane.target);
  });
  view.master_effects.clear();
  view.master_level = 1.0;
  auto detector_buses = stem_detector_buses(view);
  if (!detector_buses)
    return R::failure(detector_buses.error());
  RenderControls controls{true, options.range, options.progress, 0};
  auto rendered =
      render_master_impl(view, store, MasterRenderOptions{options.target}, {}, options.track, 0,
                         nullptr, nullptr, detector_buses.value(), &controls);
  if (!rendered)
    return R::failure(rendered.error());
  return R::success({rendered.value(), controls.first_frame});
}

} // namespace oscillate::exporting
