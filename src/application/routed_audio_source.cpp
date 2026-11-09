#include "oscillate/application/routed_audio_source.hpp"

#include "oscillate/foundation/trace.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/processing/clay_source.hpp"
#include "oscillate/processing/clip_frame_plan.hpp"
#include "oscillate/processing/warp_render_cache.hpp"
#include "oscillate/source_prep/synth_media.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <ranges>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace oscillate::application {
namespace {
using engine::StereoFrame;
using foundation::ErrorCode;
using foundation::Result;
using namespace std::chrono_literals;

[[nodiscard]] Result<std::unique_ptr<RoutedAudioSource>> fail(ErrorCode code, std::string message) {
  return Result<std::unique_ptr<RoutedAudioSource>>::failure({code, std::move(message)});
}

[[nodiscard]] bool supported_encoding(project::SourceEncoding encoding) noexcept {
  if (encoding == project::SourceEncoding::wav)
    return true;
#if OSCILLATE_HAVE_CODEC
  return encoding == project::SourceEncoding::aiff || encoding == project::SourceEncoding::flac ||
         encoding == project::SourceEncoding::mp3;
#else
  return false;
#endif
}
} // namespace

RoutedAudioSource::RoutedAudioSource(RoutedPlaybackControl& control, std::vector<Clip> clips,
                                     std::vector<Slot> slots, std::vector<SynthSlot> synths,
                                     // Topology slots and per-block frames are different bounds.
                                     // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
                                     std::size_t topology_size, std::size_t max_frames,
                                     std::uint64_t initial_generation,
                                     const std::atomic<bool>* worker_permit)
    : control_(control), max_frames_(max_frames), initial_generation_(initial_generation),
      clips_(std::move(clips)), slots_(std::move(slots)), synths_(std::move(synths)),
      spans_(topology_size), worker_permit_(worker_permit) {
  requested_generation_.store(initial_generation, std::memory_order_relaxed);
  acknowledged_generation_.store(initial_generation, std::memory_order_relaxed);
  discarded_generation_.store(initial_generation, std::memory_order_relaxed);
  ready_generation_.store(initial_generation, std::memory_order_relaxed);
  callback_generation_ = initial_generation;
  // Each slot's clips and synth, in the order they always mix.
  slot_work_.resize(slots_.size());
  for (std::size_t index = 0; index < clips_.size(); ++index) {
    const auto slot = std::ranges::find(slots_, clips_[index].track, &Slot::track);
    if (slot != slots_.end())
      slot_work_[static_cast<std::size_t>(slot - slots_.begin())].clips.push_back(index);
  }
  for (std::size_t index = 0; index < synths_.size(); ++index)
    slot_work_[synths_[index].slot_index].synths.push_back(index);
}

RoutedAudioSource::~RoutedAudioSource() {
  stop_.store(true, std::memory_order_release);
  if (worker_.joinable())
    worker_.join();
}

Result<std::unique_ptr<RoutedAudioSource>>
RoutedAudioSource::prepare(const std::filesystem::path& root, const project::Snapshot& snapshot,
                           RoutedPlaybackControl& control, std::size_t buffer_frame_budget,
                           const std::atomic<bool>* worker_permit, const std::stop_token& stop,
                           std::uint64_t first_sample) {
  if (control.status().model_revision != snapshot.revision ||
      control.current_snapshot().get() != &snapshot) {
    return fail(ErrorCode::invalid_argument, "Routed source has no matching prepared graph");
  }
  return prepare_with_layout(root, snapshot, control, control.source_layout(),
                             control.topological_order(), {}, false, control.max_frames(),
                             buffer_frame_budget, worker_permit, stop, first_sample, 1, nullptr);
}

Result<std::unique_ptr<RoutedAudioSource>> RoutedAudioSource::prepare(
    const std::filesystem::path& root, const RoutedSourcePreparationCapture& capture,
    RoutedPlaybackControl& control, std::size_t buffer_frame_budget,
    const std::atomic<bool>* worker_permit, const std::stop_token& stop, std::uint64_t first_sample,
    std::uint64_t first_generation, const audio_io::TransportCursor* live_cursor) {
  if (!capture.snapshot || capture.active_graph_revision == 0 ||
      capture.snapshot->revision <= capture.active_graph_revision)
    return fail(ErrorCode::invalid_argument, "Routed source capture has no matching revision");
  return prepare_with_layout(root, *capture.snapshot, control, capture.source_layout,
                             capture.topological_order, capture.kept_players, capture.structural,
                             capture.max_frames, buffer_frame_budget, worker_permit, stop,
                             first_sample, first_generation, live_cursor);
}

Result<std::unique_ptr<RoutedAudioSource>> RoutedAudioSource::prepare_with_layout(
    const std::filesystem::path& root, const project::Snapshot& snapshot,
    RoutedPlaybackControl& control, std::span<const routing::ProjectRouteSource> layout,
    std::span<const engine::RoutingNodeId> order, std::span<const KeptPlayer> kept, bool structural,
    std::size_t max_frames, std::size_t buffer_frame_budget, const std::atomic<bool>* worker_permit,
    const std::stop_token& stop,
    // Project sample and transport generation are separate coordinates.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    std::uint64_t first_sample, std::uint64_t first_generation,
    const audio_io::TransportCursor* live_cursor) {
  if (buffer_frame_budget == 0 || max_frames == 0 || max_frames > 8192 || order.empty() ||
      first_generation == 0)
    return fail(ErrorCode::invalid_argument, "Routed source has no matching prepared graph");
  std::vector<Slot> slots;
  slots.reserve(layout.size());
  for (const auto& source : layout) {
    if (source.topological_index >= order.size() ||
        order[source.topological_index] != source.node) {
      return fail(ErrorCode::conflict, "Routed source layout does not match graph order");
    }
    slots.push_back({source.track, source.topological_index, std::vector<StereoFrame>(max_frames)});
  }
  auto reader = persistence::ProjectStore::open_reader(root);
  if (!reader)
    return Result<std::unique_ptr<RoutedAudioSource>>::failure(reader.error());

  std::vector<SynthSlot> synths;
  const bool has_bound_synth = std::ranges::any_of(snapshot.tracks, [](const auto& track) {
    return std::holds_alternative<project::NativeInstrumentPatch>(track.instrument_source);
  });
  if (has_bound_synth) {
    auto midi = engine::compile_midi_events(snapshot);
    if (!midi)
      return Result<std::unique_ptr<RoutedAudioSource>>::failure(midi.error());
    auto automation = automation_project::prepare_project_automation(snapshot, snapshot.revision);
    if (!automation)
      return Result<std::unique_ptr<RoutedAudioSource>>::failure(automation.error());
    auto limit = automation_sample_limit(snapshot, max_frames);
    if (!limit)
      return Result<std::unique_ptr<RoutedAudioSource>>::failure(limit.error());
    for (const auto& track : snapshot.tracks) {
      if (!std::holds_alternative<project::NativeInstrumentPatch>(track.instrument_source))
        continue;
      const auto events =
          std::ranges::find(midi.value().instruments(), track.id, &engine::InstrumentEvents::track);
      const auto slot = std::ranges::find(layout, track.id, &routing::ProjectRouteSource::track);
      if (slot == layout.end())
        return fail(ErrorCode::conflict, "Routed synth has no prepared source slot");
      // A player the edit leaves alone keeps playing, voices and all: the
      // replacement source renders it from the block it takes over.
      const auto running = std::ranges::find_if(kept, [&track](const KeptPlayer& player) {
        return player.track == track.id && !player.retiring;
      });
      if (running != kept.end()) {
        synths.push_back({static_cast<std::size_t>(slot - layout.begin()), running->player,
                          running->staged_revision});
        continue;
      }
      // A synth with no notes still renders, so live panel edits stay audible
      // once notes are added and its effects keep their tails.
      const engine::InstrumentEvents empty{track.id, {}};
      auto media = source_prep::load_synth_media(
          reader.value(), snapshot,
          std::get<project::NativeInstrumentPatch>(track.instrument_source).document, stop);
      if (!media)
        return fail(media.error().code,
                    "Built-in synth media is unavailable: " + media.error().message);
      auto player = native_inserts::NativeSynthPlayer::prepare(
          snapshot, track, events == midi.value().instruments().end() ? empty : *events,
          automation.value(), max_frames, limit.value(), std::move(media).value());
      if (!player)
        return fail(player.error().code,
                    "Built-in synth is unavailable: " + player.error().message);
      synths.push_back(
          {static_cast<std::size_t>(slot - layout.begin()),
           std::make_shared<native_inserts::NativeSynthPlayer>(std::move(player).value())});
    }
  }

  // A replaced instrument rings out in its track's slot (ADR-0098).
  for (const auto& retiring : kept) {
    if (!retiring.retiring)
      continue;
    const auto slot =
        std::ranges::find(layout, retiring.track, &routing::ProjectRouteSource::track);
    if (slot == layout.end())
      return fail(ErrorCode::conflict, "Replaced synth has no prepared source slot");
    synths.push_back({static_cast<std::size_t>(slot - layout.begin()), retiring.player,
                      retiring.staged_revision, true});
  }

  // Processed clips prefer a validated cache render; without a cache
  // directory they render live from the verified source.
  std::optional<media::CacheStore> cache;
  bool cache_opened = false;
  const auto processed_cache = [&]() -> media::CacheStore* {
    if (!cache_opened) {
      cache_opened = true;
      if (auto directory = reader.value().open_cache_directory()) {
        if (auto adopted = media::CacheStore::adopt_directory_fd(directory.value()))
          cache.emplace(std::move(adopted).value());
      }
    }
    return cache ? &*cache : nullptr;
  };
  std::vector<Clip> clips;
  for (const auto& track : snapshot.tracks) {
    // A source-less MIDI track remains a silent placeholder beside a bound
    // native synth. A saved VST3 source must have its own exact
    // prepared graph path; another instrument cannot admit its MIDI.
    const bool silent_placeholder =
        std::holds_alternative<std::monostate>(track.instrument_source) &&
        (has_bound_synth || std::ranges::any_of(kept, [&track](const KeptPlayer& player) {
           return player.track == track.id && player.retiring;
         }));
    // A structural switch prepares a VST3 instrument's stage with its graph,
    // which verifies it before the switch can take over (ADR-0098).
    const bool graph_bound =
        structural && std::holds_alternative<project::Vst3PluginInstance>(track.instrument_source);
    if (track.kind == project::TrackKind::instrument &&
        !std::holds_alternative<project::NativeInstrumentPatch>(track.instrument_source) &&
        !track.midi_clips.empty() && !silent_placeholder && !graph_bound &&
        !control.has_bound_instrument_track(track.id)) {
      return fail(ErrorCode::unsupported,
                  "Routed managed-audio source requires a verified bound MIDI instrument");
    }
    if (track.kind != project::TrackKind::audio && !track.clips.empty()) {
      return fail(ErrorCode::unsupported, "Routed bus clips require a separate source contract");
    }
    for (const auto& clip : track.clips) {
      if (stop.stop_requested())
        return fail(ErrorCode::cancelled, "routed source preparation cancelled");
      if (std::ranges::none_of(layout,
                               [&track](const auto& source) { return source.track == track.id; })) {
        return fail(ErrorCode::conflict, "Routed audio track has no prepared source slot");
      }
      const auto source = std::ranges::find_if(
          snapshot.sources, [&clip](const auto& record) { return record.id == clip.source; });
      if (source == snapshot.sources.end())
        return fail(ErrorCode::unavailable, "Routed clip has no project source record");
      if (!supported_encoding(source->encoding) ||
          (source->channels != 1 && source->channels != 2)) {
        return fail(ErrorCode::unsupported, "Routed clip requires supported mono/stereo audio");
      }
      auto planned =
          processing::prepare_clip_frame_plan(clip, *source, snapshot.tempo, snapshot.sample_rate);
      if (!planned)
        return Result<std::unique_ptr<RoutedAudioSource>>::failure(planned.error());
      const auto& frames = planned.value();
      auto source_start = media::source_rate_frame_count(frames.source_start_frame,
                                                         source->sample_rate, snapshot.sample_rate);
      auto source_end =
          media::source_rate_frame_count(frames.source_start_frame + frames.warp.source_frames(),
                                         source->sample_rate, snapshot.sample_rate);
      if (!source_start || !source_end)
        return Result<std::unique_ptr<RoutedAudioSource>>::failure(
            !source_start ? source_start.error() : source_end.error());
      if (source_end.value() <= source_start.value())
        return fail(ErrorCode::invalid_argument, "Routed clip has an empty source range");
      auto fades = project::prepare_clip_fades(clip.fades, frames.warp.output_frames());
      if (!fades)
        return Result<std::unique_ptr<RoutedAudioSource>>::failure(fades.error());
      const auto first =
          std::clamp(first_sample, frames.project_start_frame, frames.project_end_frame());
      // Pitch, reviewed markers or a placement that differs from the
      // rate-converted source duration play through the warp renderer, never
      // as raw source audio.
      if (clip.processing.requires_rendering() ||
          source_end.value() - source_start.value() != frames.warp.output_frames()) {
        auto request = processing::warp_render_request(snapshot, clip.id);
        if (!request)
          return Result<std::unique_ptr<RoutedAudioSource>>::failure(request.error());
        auto live =
            processing::WarpLiveReader::open(root, processed_cache(), request.value(), stop);
        if (!live)
          return Result<std::unique_ptr<RoutedAudioSource>>::failure(live.error());
        clips.push_back({clip.id,
                         track.id,
                         frames.project_start_frame,
                         frames.project_end_frame(),
                         0,
                         frames.warp.output_frames(),
                         fades.value(),
                         std::nullopt,
                         std::move(live).value(),
                         {},
                         {},
                         first - frames.project_start_frame});
        continue;
      }
      // A Clay take plays its render (ADR-0081): the same frames, rate and
      // channels as the source, rendered here when the cache lacks it.
      std::optional<media::AudioReader> clay_audio;
      if (clip.clay) {
        auto rendered = processing::clay::open_rendered(reader.value(), *source, *clip.clay, stop);
        if (!rendered)
          return Result<std::unique_ptr<RoutedAudioSource>>::failure(rendered.error());
        auto decoded =
            media::AudioReader::open_fd(rendered.value().fd(), rendered.value().record());
        if (!decoded)
          return Result<std::unique_ptr<RoutedAudioSource>>::failure(decoded.error());
        clay_audio.emplace(std::move(decoded).value());
      }
      auto opened = [&]() -> Result<media::AudioReader> {
        if (clay_audio)
          return Result<media::AudioReader>::success(std::move(*clay_audio));
        auto fd = reader.value().open_verified_source(*source, stop);
        if (!fd)
          return Result<media::AudioReader>::failure(fd.error());
        auto decoded = media::AudioReader::open_fd(fd.value(), *source);
        (void)::close(fd.value());
        return decoded;
      }();
      if (!opened)
        return Result<std::unique_ptr<RoutedAudioSource>>::failure(opened.error());
      if (opened.value().frame_count() != source->frames ||
          opened.value().sample_rate() != source->sample_rate ||
          opened.value().channels() != source->channels) {
        return fail(ErrorCode::corrupt_data, "Verified audio metadata changed");
      }
      auto converted =
          media::SourceRateReader::open(std::move(opened).value(), snapshot.sample_rate);
      if (!converted)
        return Result<std::unique_ptr<RoutedAudioSource>>::failure(converted.error());
      clips.push_back({clip.id,
                       track.id,
                       frames.project_start_frame,
                       frames.project_end_frame(),
                       source_start.value(),
                       source_end.value(),
                       fades.value(),
                       std::move(converted).value(),
                       nullptr,
                       {},
                       {},
                       source_start.value() + first - frames.project_start_frame});
    }
  }
  const auto capacity = clips.empty()
                            ? buffer_frame_budget
                            : std::min<std::size_t>(65'536, buffer_frame_budget / clips.size());
  if (capacity < max_frames)
    return fail(ErrorCode::resource_exhausted, "Routed stream budget is too small for clip rings");
  // Verified source opening may take far longer than the ring's lead window.
  // Pin the actual transport position only when all readers are ready to prime.
  if (live_cursor != nullptr) {
    const auto position = live_cursor->completed_snapshot();
    if (position.generation == 0 ||
        position.next_sample == std::numeric_limits<std::uint64_t>::max())
      return fail(ErrorCode::resource_exhausted, "Live routed transport position is exhausted");
    first_sample = position.next_sample;
    first_generation = position.generation;
    for (auto& clip : clips) {
      const auto first = std::clamp(first_sample, clip.start, clip.end);
      clip.next_source = clip.source_start + first - clip.start;
    }
  }
  for (auto& clip : clips) {
    clip.limit = clip.source_end;
    clip.fill_generation = first_generation;
    clip.ring = std::make_unique<media::PrefetchRing>(clip.id, capacity);
    clip.scratch.resize(max_frames);
    std::array<media::StereoFrame, media::SourceRateReader::max_read_frames> scratch{};
    const auto wanted = static_cast<std::size_t>(
        std::min<std::uint64_t>(max_frames, clip.source_end - clip.next_source));
    while (clip.ring->occupancy() < wanted) {
      const auto count =
          std::min({scratch.size(), wanted - clip.ring->occupancy(), clip.ring->free_slots()});
      auto filled = read_clip(clip, clip.next_source, std::span(scratch).first(count));
      if (!filled)
        return Result<std::unique_ptr<RoutedAudioSource>>::failure(filled.error());
      if (filled.value() != count)
        return fail(ErrorCode::corrupt_data, "Routed managed audio ended during initial prime");
      for (std::size_t index = 0; index < count; ++index) {
        if (!clip.ring->try_push(
                {clip.id, first_generation, clip.next_source + index, scratch[index]}))
          return fail(ErrorCode::resource_exhausted, "Routed source prime ring is full");
      }
      clip.next_source += count;
    }
  }
  auto source = std::unique_ptr<RoutedAudioSource>(
      new RoutedAudioSource(control, std::move(clips), std::move(slots), std::move(synths),
                            order.size(), max_frames, first_generation, worker_permit));
  try {
    source->worker_ = std::thread([ptr = source.get()] { ptr->worker_loop(); });
  } catch (const std::system_error&) {
    return fail(ErrorCode::resource_exhausted, "Cannot start routed source worker");
  }
  return Result<std::unique_ptr<RoutedAudioSource>>::success(std::move(source));
}

// The player that plays the track's instrument, never one ringing out.
native_inserts::NativeSynthPlayer*
RoutedAudioSource::playing_synth(project::TrackId track) const noexcept {
  const auto found = std::ranges::find_if(synths_, [track](const SynthSlot& synth) {
    return synth.player->track() == track && !synth.retiring;
  });
  return found == synths_.end() ? nullptr : found->player.get();
}

native_dsp::synth::NativeSynthRuntime*
RoutedAudioSource::synth_runtime(project::TrackId track) noexcept {
  auto* player = playing_synth(track);
  return player == nullptr ? nullptr : player->runtime().get();
}

std::shared_ptr<native_inserts::NativeSynthPlayer>
RoutedAudioSource::shared_synth_player(project::TrackId track) const {
  const auto found = std::ranges::find_if(synths_, [track](const SynthSlot& synth) {
    return synth.player->track() == track && !synth.retiring;
  });
  return found == synths_.end() ? nullptr : found->player;
}

native_inserts::NativeSynthPlayer*
RoutedAudioSource::synth_player(project::TrackId track) noexcept {
  return playing_synth(track);
}

void RoutedAudioSource::refresh_synth_locks(const project::Snapshot& snapshot) {
  for (auto& synth : synths_)
    (void)synth.player->refresh_lock(snapshot);
}

void RoutedAudioSource::follow_project_key(std::optional<project::MusicalKey> key) noexcept {
  for (auto& synth : synths_)
    synth.player->follow_project_key(key);
}

std::optional<std::uint64_t>
RoutedAudioSource::synth_tail_frames(project::TrackId track) const noexcept {
  const auto* player = playing_synth(track);
  return player == nullptr ? std::nullopt : std::optional<std::uint64_t>{player->tail_frames()};
}

Result<void> RoutedAudioSource::prepare_realtime_export_end(std::uint64_t end) {
  if (end == 0 || end > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
    return Result<void>::failure(
        {ErrorCode::unsupported, "Instrument export needs a finite MIDI end"});
  for (auto& synth : synths_) {
    auto prepared = synth.player->prepare_export_end(end);
    if (!prepared)
      return prepared;
  }
  return Result<void>::success();
}

std::optional<std::uint64_t>
RoutedAudioSource::prepared_synth_export_tail(project::TrackId track,
                                              std::uint64_t end) const noexcept {
  const auto* player = playing_synth(track);
  return player == nullptr || !player->export_end_prepared(end)
             ? std::nullopt
             : std::optional<std::uint64_t>{player->tail_frames()};
}

audio_io::StereoRenderSource
RoutedAudioSource::render_source(engine::AudioWorkerPool* workers) noexcept {
  render_workers_ = workers;
  return {this, &RoutedAudioSource::render, &RoutedAudioSource::freeze_cursor, workers};
}

RoutedSourceStatus RoutedAudioSource::status() const noexcept {
  RoutedSourceStatus status{ready_generation_.load(std::memory_order_acquire),
                            missing_frames_.load(std::memory_order_relaxed),
                            worker_failed_.load(std::memory_order_acquire),
                            worker_paused_.load(std::memory_order_acquire)};
  for (const auto& clip : clips_) {
    if (!clip.warp)
      continue;
    const auto live = clip.warp->status();
    ++status.processed_clips;
    status.cache_backed_clips += live.backing == processing::WarpLiveBacking::cache ? 1U : 0U;
    status.reduced_quality_clips += live.tier != warp_dsp::QualityTier::q0 ? 1U : 0U;
    status.quality_step_downs += live.step_downs;
  }
  return status;
}

std::vector<RoutedProcessedClipStatus> RoutedAudioSource::processed_clip_status() const {
  std::vector<RoutedProcessedClipStatus> statuses;
  for (const auto& clip : clips_) {
    if (clip.warp)
      statuses.push_back({clip.id, clip.track, clip.warp->status(), clip.ring->occupancy()});
  }
  return statuses;
}

std::optional<media::UnderrunEvent> RoutedAudioSource::pop_source_underrun() noexcept {
  if (clips_.empty())
    return std::nullopt;
  for (std::size_t offset = 0; offset < clips_.size(); ++offset) {
    const auto index = (next_underrun_clip_ + offset) % clips_.size();
    if (auto event = clips_[index].ring->pop_underrun()) {
      next_underrun_clip_ = (index + 1) % clips_.size();
      return event;
    }
  }
  return std::nullopt;
}

std::optional<foundation::Error> RoutedAudioSource::worker_failure() const {
  if (!worker_failed_.load(std::memory_order_acquire))
    return std::nullopt;
  return failure_;
}

media::HandoffWindowStatus RoutedAudioSource::prime_handoff_window(std::uint64_t generation,
                                                                   std::uint64_t first_sample,
                                                                   std::size_t frames) noexcept {
  using media::HandoffWindowStatus;
  if (generation != initial_generation_ || frames == 0 || frames > max_frames_ ||
      frames > std::numeric_limits<std::uint64_t>::max() - first_sample)
    return HandoffWindowStatus::invalid;
  if (worker_failed_.load(std::memory_order_acquire))
    return HandoffWindowStatus::missed;
  bool waiting = false;
  const auto end = first_sample + frames;
  for (auto& clip : clips_) {
    if (clip.start >= end || clip.end <= first_sample)
      continue;
    const auto first = std::max(clip.start, first_sample);
    const auto last = std::min(clip.end, end);
    const auto source_frame = clip.source_start + first - clip.start;
    const auto status = clip.ring->prime_handoff_window(generation, source_frame,
                                                        static_cast<std::size_t>(last - first));
    if (status == HandoffWindowStatus::invalid || status == HandoffWindowStatus::missed)
      return status;
    waiting |= status == HandoffWindowStatus::waiting;
  }
  return waiting ? HandoffWindowStatus::waiting : HandoffWindowStatus::ready;
}

void RoutedAudioSource::request_silent_drain() noexcept {
  silent_drain_requested_.store(true, std::memory_order_release);
}

std::uint64_t RoutedAudioSource::completed_silent_drain_frames() const noexcept {
  return completed_silent_drain_frames_.load(std::memory_order_acquire);
}

bool RoutedAudioSource::render(void* context, std::span<StereoFrame> output,
                               engine::BlockContext block) noexcept {
  return static_cast<RoutedAudioSource*>(context)->render_block(output, block);
}

bool RoutedAudioSource::freeze_cursor(void* context) noexcept {
  return static_cast<RoutedAudioSource*>(context)->frozen_;
}

bool RoutedAudioSource::render_block(std::span<StereoFrame> output,
                                     engine::BlockContext block) noexcept {
  frozen_ = false;
  stream_hold_ = false;
  silent_drain_block_ = silent_drain_requested_.load(std::memory_order_acquire);
  if (output.empty() || output.size() > max_frames_ ||
      output.size() > std::numeric_limits<std::uint64_t>::max() - block.project_sample_position)
    return false;
  const std::span<const std::span<const StereoFrame>> unused{};
  const auto result = control_.render_block(
      unused, output, block,
      {this, nullptr,
       [](void* context, std::uint64_t, engine::BlockContext fill_block, std::size_t frames,
          std::span<const std::span<const StereoFrame>>& selected) noexcept {
         auto& self = *static_cast<RoutedAudioSource*>(context);
         return self.fill_source_bank(fill_block, frames, selected, self.render_workers_);
       }},
      render_workers_);
  frozen_ = stream_hold_;
  if (result == engine::RoutedRenderStatus::rendered)
    note_rendered_frames(output.size());
  return result == engine::RoutedRenderStatus::rendered ||
         (result == engine::RoutedRenderStatus::invalid_block && stream_hold_);
}

bool RoutedAudioSource::fill_source_bank(engine::BlockContext block, std::size_t frames,
                                         std::span<const std::span<const StereoFrame>>& selected,
                                         engine::AudioWorkerPool* workers) noexcept {
  return fill_source_bank_impl(block, frames, selected, false, workers);
}

bool RoutedAudioSource::fill_source_bank_exact(
    engine::BlockContext block, std::size_t frames,
    std::span<const std::span<const StereoFrame>>& selected,
    engine::AudioWorkerPool* workers) noexcept {
  return fill_source_bank_impl(block, frames, selected, true, workers);
}

bool RoutedAudioSource::fill_source_bank_impl(
    engine::BlockContext block, std::size_t frames,
    std::span<const std::span<const StereoFrame>>& selected, bool require_complete,
    engine::AudioWorkerPool* workers) noexcept {
  if (frames == 0 || frames > max_frames_)
    return false;
  silent_drain_block_ = silent_drain_requested_.load(std::memory_order_acquire);
  stream_hold_ = false;
  for (auto& slot : slots_)
    spans_[slot.index] = std::span(slot.scratch).first(frames);
  if (!fill_slots(block, frames, require_complete, workers))
    return false;
  selected = spans_;
  return true;
}

void RoutedAudioSource::note_rendered_frames(std::size_t frames) noexcept {
  if (silent_drain_block_)
    completed_silent_drain_frames_.fetch_add(frames, std::memory_order_release);
}

bool RoutedAudioSource::fill_slots(engine::BlockContext block, std::size_t frames,
                                   bool require_complete,
                                   engine::AudioWorkerPool* workers) noexcept {
  if (silent_drain_block_) {
    for (auto& slot : slots_)
      std::ranges::fill(std::span(slot.scratch).first(frames), StereoFrame{});
    return true;
  }
  if (block.transport_generation < callback_generation_) {
    return false;
  }
  if (block.transport_generation != callback_generation_) {
    callback_generation_ = block.transport_generation;
    // A planned loop wrap onto a generation the worker pre-primed for the
    // current loop configuration plays at once. A torn read, another
    // configuration, generation or position, or any other new generation is
    // a seek that holds until the worker primes it.
    if (block.planned_loop_wrap) {
      const auto config = read_loop_config();
      const auto ready = read_loop_ready();
      if (config && ready && config->enabled && ready->version == config->version &&
          ready->generation == callback_generation_ &&
          config->start == block.project_sample_position) {
        loop_accepted_generation_ = callback_generation_;
        // Published before the request below, so the worker that sees this
        // generation requested also sees that it was an accepted wrap.
        loop_accepted_wrap_.store(callback_generation_, std::memory_order_release);
      }
    }
    requested_sample_.store(block.project_sample_position, std::memory_order_relaxed);
    requested_generation_.store(callback_generation_, std::memory_order_release);
  }
  if (ready_generation_.load(std::memory_order_acquire) != callback_generation_ &&
      loop_accepted_generation_ != callback_generation_) {
    if (acknowledged_generation_.load(std::memory_order_acquire) == callback_generation_ &&
        discarded_generation_.load(std::memory_order_relaxed) != callback_generation_) {
      for (auto& clip : clips_)
        clip.ring->discard_pending();
      discarded_generation_.store(callback_generation_, std::memory_order_release);
    }
    stream_hold_ = true;
    return false;
  }
  if (!synths_.empty() &&
      block.project_sample_position >
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) - frames)
    return false;
  // Slots share nothing, so each renders as its own task (ADR-0048).
  struct SlotJob {
    RoutedAudioSource& source;
    engine::BlockContext block;
    std::size_t frames;
    bool require_complete;
    SlotPart part;
    engine::AudioWorkerPool* workers;
    std::atomic<bool> failed{false};
  };
  const auto task = [](void* context, std::size_t slot, std::size_t) noexcept {
    auto& slot_job = *static_cast<SlotJob*>(context);
    if (!slot_job.failed.load(std::memory_order_relaxed) &&
        !slot_job.source.fill_slot(slot, slot_job.block, slot_job.frames, slot_job.require_complete,
                                   slot_job.part, slot_job.workers))
      slot_job.failed.store(true, std::memory_order_relaxed);
  };
  const auto run = [&](SlotPart part) {
    SlotJob job{*this, block, frames, require_complete, part, workers};
    if (workers != nullptr)
      workers->run(slots_.size(), task, &job);
    else
      for (std::size_t slot = 0; slot < slots_.size(); ++slot)
        task(&job, slot, 0);
    return !job.failed.load(std::memory_order_relaxed);
  };
  // A complete fill decides on every clip frame before any player renders.
  if (require_complete) {
    if (!run(SlotPart::clips))
      return false;
    if (!took_over_)
      take_over(block);
    return run(SlotPart::synths);
  }
  return run(SlotPart::all);
}

// The exact fill that takes over playback: this block is the switch's.
void RoutedAudioSource::take_over(engine::BlockContext block) noexcept {
  took_over_ = true;
  for (auto& synth : synths_) {
    if (synth.staged_revision != 0)
      synth.player->release_staged_plan(synth.staged_revision);
    if (synth.retiring) {
      synth.player->retire();
      synth.retire_end = block.project_sample_position + synth.player->tail_frames();
    }
  }
}

bool RoutedAudioSource::fill_slot(std::size_t index, engine::BlockContext block, std::size_t frames,
                                  bool require_complete, SlotPart part,
                                  engine::AudioWorkerPool* workers) noexcept {
  auto& slot = slots_[index];
  OSCILLATE_TRACE_SPAN("graph.source", slot.track.bits().low);
  const auto& work = slot_work_[index];
  const auto end = block.project_sample_position + frames;
  auto buffer = std::span(slot.scratch).first(frames);
  if (part != SlotPart::synths)
    std::ranges::fill(buffer, StereoFrame{});
  for (const auto clip_index :
       part == SlotPart::synths ? std::span<const std::size_t>{} : std::span(work.clips)) {
    auto& clip = clips_[clip_index];
    if (clip.start >= end || clip.end <= block.project_sample_position)
      continue;
    const auto first = std::max(clip.start, block.project_sample_position);
    const auto last = std::min(clip.end, end);
    const auto offset = static_cast<std::size_t>(first - block.project_sample_position);
    const auto count = static_cast<std::size_t>(last - first);
    const auto source_position = clip.source_start + first - clip.start;
    const auto received =
        clip.ring->consume(block.transport_generation, source_position,
                           std::span(clip.scratch).first(count), !require_complete);
    if (require_complete && received.silent != 0)
      return false;
    for (std::size_t frame = 0; frame < count; ++frame) {
      const auto gain = project::clip_fade_gain(clip.fades, first - clip.start + frame);
      buffer[offset + frame].left += static_cast<float>(clip.scratch[frame].left * gain);
      buffer[offset + frame].right += static_cast<float>(clip.scratch[frame].right * gain);
    }
    if (received.silent != 0)
      missing_frames_.fetch_add(received.silent, std::memory_order_relaxed);
  }
  if (part == SlotPart::clips)
    return true;
  for (const auto synth_index : work.synths) {
    // A replaced instrument stops once its notes have rung out.
    if (synths_[synth_index].retiring &&
        block.project_sample_position >= synths_[synth_index].retire_end)
      continue;
    // Its voices may spread over the helpers that are free (ADR-0085).
    if (!synths_[synth_index].player->render(buffer, block.project_sample_position,
                                             block.state_generation(), workers))
      return false;
  }
  return true;
}

bool RoutedAudioSource::fill_one(Clip& clip, FillRequest request) {
  if (clip.next_source >= clip.limit || clip.ring->free_slots() == 0 || request.frames == 0)
    return false;
  std::array<media::StereoFrame, media::SourceRateReader::max_read_frames> scratch{};
  const auto wanted = static_cast<std::size_t>(std::min<std::uint64_t>(
      {scratch.size(), request.frames, clip.ring->free_slots(), clip.limit - clip.next_source}));
  auto filled = read_clip(clip, clip.next_source, std::span(scratch).first(wanted));
  if (!filled || filled.value() != wanted) {
    failure_ = filled ? foundation::Error{ErrorCode::corrupt_data,
                                          "Routed managed audio ended during streaming"}
                      : filled.error();
    worker_failed_.store(true, std::memory_order_release);
    return false;
  }
  for (std::size_t index = 0; index < wanted; ++index) {
    if (!clip.ring->try_push(
            {clip.id, clip.fill_generation, clip.next_source + index, scratch[index]})) {
      failure_ = {ErrorCode::resource_exhausted, "Routed source ring is full during streaming"};
      worker_failed_.store(true, std::memory_order_release);
      return false;
    }
  }
  clip.next_source += wanted;
  return true;
}

Result<std::size_t> RoutedAudioSource::read_clip(Clip& clip, std::uint64_t first,
                                                 std::span<media::StereoFrame> out) {
  if (clip.warp)
    return clip.warp->read_at(first, out);
  if (clip.reader)
    return clip.reader->read_at(first, out);
  return Result<std::size_t>::failure({ErrorCode::internal, "Routed clip has no frame reader"});
}

// Worker only, before each fill of a clip. After a callback underrun on the
// clip's ring the worker is behind the consumer: every frame it would queue
// next is already in the past. It resumes ahead of the consumer, far enough
// that the bounded per-block stale scan can drain what is already queued
// first; the gap in between is the reported underrun silence. A processed
// clip also steps its live render down one quality tier, on an underrun or
// when its lead drops below a quarter of the ring after having been
// three-quarters full since the last seek (re-armed once the lead recovers).
void RoutedAudioSource::manage_quality(Clip& clip) noexcept {
  if (clip.next_source >= clip.limit)
    return;
  const auto capacity = clip.ring->capacity();
  const auto occupancy = clip.ring->occupancy();
  const auto underruns = clip.ring->underrun_count();
  const bool underran = underruns != clip.underruns_seen;
  clip.underruns_seen = underruns;
  if (underran) {
    const auto consumer = clip.ring->consumer_position();
    if (consumer.generation == clip.fill_generation && consumer.next_frame >= clip.next_source) {
      // The consumer drains at most max_stale_scan_per_block stale frames per
      // block while advancing up to max_frames_; resume past that distance.
      const auto blocks = occupancy / media::PrefetchRing::max_stale_scan_per_block + 2;
      const auto ahead = std::min(
          std::max(consumer.next_frame + blocks * max_frames_, clip.next_source), clip.limit);
      if (ahead > clip.next_source && clip.ring->skip_forward(clip.fill_generation, ahead))
        clip.next_source = ahead;
    }
  }
  if (!clip.warp)
    return;
  clip.filled_since_seek |= occupancy >= capacity * 3 / 4;
  const bool low = clip.filled_since_seek && occupancy < capacity / 4;
  if (underran || (low && !clip.behind)) {
    clip.behind = true;
    (void)clip.warp->step_down();
  } else if (occupancy >= capacity * 3 / 4) {
    clip.behind = false;
  }
}

// Transport generation and project sample are separate coordinates.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void RoutedAudioSource::reset_clips(std::uint64_t generation, std::uint64_t sample) noexcept {
  for (auto& clip : clips_) {
    // The callback has discarded this ring, including any frames pre-primed
    // for a loop under the same generation number.
    clip.ring->restart_producer();
    const auto first = std::clamp(sample, clip.start, clip.end);
    clip.next_source = clip.source_start + first - clip.start;
    clip.limit = clip.source_end;
    clip.fill_generation = generation;
    clip.filled_since_seek = false;
    clip.behind = false;
    // Priming silence after a seek is not an underrun.
    clip.underruns_seen = clip.ring->underrun_count();
  }
}

// Both seqlocks avoid fences (unsupported under ThreadSanitizer). The writer
// marks the version odd (relaxed), stores every field with release, then
// publishes the next even version with release. A reader loads the version
// with acquire, every field with acquire, then the version again. A field
// value from a newer write synchronises with that write's release store, so
// the odd mark sequenced before it is visible and the second version read
// differs: a torn read is always rejected.
std::optional<RoutedAudioSource::LoopConfig> RoutedAudioSource::read_loop_config() const noexcept {
  const auto before = loop_version_.load(std::memory_order_acquire);
  if ((before & 1U) != 0)
    return std::nullopt;
  const LoopConfig config{before, loop_enabled_.load(std::memory_order_acquire),
                          loop_start_.load(std::memory_order_acquire),
                          loop_end_.load(std::memory_order_acquire)};
  if (loop_version_.load(std::memory_order_relaxed) != before)
    return std::nullopt;
  return config;
}

std::optional<RoutedAudioSource::LoopReady> RoutedAudioSource::read_loop_ready() const noexcept {
  const auto before = loop_ready_sequence_.load(std::memory_order_acquire);
  if ((before & 1U) != 0)
    return std::nullopt;
  const LoopReady ready{loop_ready_version_.load(std::memory_order_acquire),
                        loop_ready_generation_.load(std::memory_order_acquire)};
  if (loop_ready_sequence_.load(std::memory_order_relaxed) != before)
    return std::nullopt;
  return ready;
}

// Single writer per seqlock: the worker here, the control thread below.
void RoutedAudioSource::publish_loop_ready(LoopReady ready) noexcept {
  const auto sequence = loop_ready_sequence_.load(std::memory_order_relaxed);
  loop_ready_sequence_.store(sequence + 1, std::memory_order_relaxed);
  loop_ready_version_.store(ready.version, std::memory_order_release);
  loop_ready_generation_.store(ready.generation, std::memory_order_release);
  loop_ready_sequence_.store(sequence + 2, std::memory_order_release);
}

void RoutedAudioSource::publish_loop_config(bool enabled, std::uint64_t start,
                                            std::uint64_t end) noexcept {
  const auto version = loop_version_.load(std::memory_order_relaxed);
  loop_version_.store(version + 1, std::memory_order_relaxed);
  loop_enabled_.store(enabled, std::memory_order_release);
  loop_start_.store(start, std::memory_order_release);
  loop_end_.store(end, std::memory_order_release);
  loop_version_.store(version + 2, std::memory_order_release);
}

// Worker only, before each round of steady filling. Limits every clip that
// overlaps the current enabled loop to the loop end in this generation; once
// a clip reaches it, its next frames are the next generation from the loop
// start. When every such clip has a block of lead queued, publishes the next
// generation as ready for exactly this configuration version.
void RoutedAudioSource::prime_loop(std::uint64_t generation) noexcept {
  const auto config = read_loop_config();
  if (!config)
    return; // the control thread is mid-update; read it next round
  const auto next = generation + 1;
  if (config->version != loop_version_seen_) {
    loop_version_seen_ = config->version;
    // Frames queued for the next generation under an earlier configuration
    // cannot be withdrawn from the rings: this configuration needs a seek.
    if (std::ranges::any_of(
            clips_, [generation](const Clip& clip) { return clip.fill_generation > generation; })) {
      loop_missed_version_.store(config->version, std::memory_order_release);
      return;
    }
    // Only current-generation frames are queued; lift the old loop limits.
    for (auto& clip : clips_)
      clip.limit = clip.source_end;
  }
  if (!config->enabled || generation == std::numeric_limits<std::uint64_t>::max() ||
      loop_missed_version_.load(std::memory_order_relaxed) == config->version)
    return;
  if (const auto ready = read_loop_ready();
      ready && ready->version == config->version && ready->generation == next)
    return;
  const auto start = config->start;
  const auto end = config->end;
  bool primed = true;
  for (auto& clip : clips_) {
    if (clip.end <= start || clip.start >= end)
      continue;
    if (clip.fill_generation == generation) {
      const auto limit = clip.source_start + std::clamp(end, clip.start, clip.end) - clip.start;
      if (clip.next_source > limit) {
        loop_missed_version_.store(config->version, std::memory_order_release);
        return;
      }
      clip.limit = limit;
      if (clip.next_source < limit) {
        primed = false;
        continue;
      }
      clip.fill_generation = next;
      clip.loop_first = clip.source_start + std::clamp(start, clip.start, clip.end) - clip.start;
      clip.next_source = clip.loop_first;
      clip.limit = clip.source_end;
    }
    const auto lead = std::min<std::uint64_t>(max_frames_, clip.source_end - clip.loop_first);
    if (clip.next_source - clip.loop_first < lead)
      primed = false;
  }
  if (primed)
    publish_loop_ready({config->version, next});
}

bool RoutedAudioSource::set_loop(std::uint64_t start, std::uint64_t end) noexcept {
  if (end <= start)
    return false;
  publish_loop_config(true, start, end);
  return true;
}

void RoutedAudioSource::clear_loop() noexcept { publish_loop_config(false, 0, 0); }

RoutedAudioSource::LoopStatus RoutedAudioSource::loop_status() const noexcept {
  // The control thread is the only configuration writer, so its own read is
  // never torn. The worker's ready record is retried a bounded number of
  // times; a record still being written reports not ready.
  const auto config = read_loop_config();
  if (!config)
    return {};
  LoopStatus status;
  // Version 0 is "never configured", which is also the no-miss sentinel.
  status.missed = config->version != 0 &&
                  loop_missed_version_.load(std::memory_order_acquire) == config->version;
  for (int attempt = 0; attempt < 8; ++attempt) {
    if (const auto ready = read_loop_ready()) {
      if (config->enabled && !status.missed && ready->version == config->version)
        status.ready_generation = ready->generation;
      break;
    }
  }
  return status;
}

bool RoutedAudioSource::fill_lead(FillRequest request) {
  for (auto& clip : clips_) {
    const auto needed = static_cast<std::size_t>(
        std::min<std::uint64_t>(request.frames, clip.limit - clip.next_source));
    const auto goal = clip.ring->occupancy() + needed;
    while (clip.ring->occupancy() < goal && !stop_.load(std::memory_order_acquire) &&
           requested_generation_.load(std::memory_order_acquire) == request.generation &&
           !worker_failed_.load(std::memory_order_acquire)) {
      if (!fill_one(clip, {request.generation, goal - clip.ring->occupancy()}))
        return false;
    }
    if (clip.ring->occupancy() < goal)
      return false;
  }
  return true;
}

void RoutedAudioSource::worker_loop() noexcept {
  try {
    std::uint64_t generation = initial_generation_;
    while (!stop_.load(std::memory_order_acquire) &&
           !worker_failed_.load(std::memory_order_acquire)) {
      if (worker_permit_ && !worker_permit_->load(std::memory_order_acquire)) {
        worker_paused_.store(true, std::memory_order_release);
        std::this_thread::sleep_for(100us);
        continue;
      }
      worker_paused_.store(false, std::memory_order_release);
      const auto requested = requested_generation_.load(std::memory_order_acquire);
      if (requested != generation && requested == generation + 1 &&
          loop_accepted_wrap_.load(std::memory_order_acquire) == requested) {
        // The callback accepted a planned wrap onto frames this worker
        // queued behind the loop end, for the configuration it checked; they
        // stay valid even if the loop was rearmed or cleared since. With
        // those frames now current, re-evaluate the current configuration
        // (a miss caused by them no longer applies).
        loop_missed_version_.store(0, std::memory_order_release);
        loop_version_seen_ = 0;
        generation = requested;
        acknowledged_generation_.store(requested, std::memory_order_release);
        discarded_generation_.store(requested, std::memory_order_release);
        ready_generation_.store(requested, std::memory_order_release);
        continue;
      }
      if (requested != generation) {
        // A seek discards everything queued, including any pre-primed loop
        // frames; the current configuration is re-armed from scratch.
        publish_loop_ready({0, 0});
        loop_missed_version_.store(0, std::memory_order_release);
        loop_version_seen_ = 0;
        acknowledged_generation_.store(requested, std::memory_order_release);
        while (discarded_generation_.load(std::memory_order_acquire) != requested &&
               !stop_.load(std::memory_order_acquire)) {
          std::this_thread::sleep_for(100us);
        }
        if (stop_.load(std::memory_order_acquire))
          break;
        reset_clips(requested, requested_sample_.load(std::memory_order_acquire));
        generation = requested;
        if (!fill_lead({generation, max_frames_}))
          continue;
        ready_generation_.store(generation, std::memory_order_release);
        continue;
      }
      prime_loop(generation);
      bool progressed = false;
      for (auto& clip : clips_) {
        if (requested_generation_.load(std::memory_order_acquire) != generation)
          break;
        manage_quality(clip);
        progressed |= fill_one(clip, {generation, 4096});
        if (worker_failed_.load(std::memory_order_acquire))
          break;
      }
      if (!progressed)
        std::this_thread::sleep_for(1ms);
    }
  } catch (const std::exception&) {
    failure_ = {ErrorCode::internal, "Routed source worker failed unexpectedly"};
    worker_failed_.store(true, std::memory_order_release);
  } catch (...) {
    failure_ = {ErrorCode::internal, "Routed source worker failed unexpectedly"};
    worker_failed_.store(true, std::memory_order_release);
  }
}

} // namespace oscillate::application
