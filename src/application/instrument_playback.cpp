#include "oscillate/application/instrument_playback.hpp"

#include "oscillate/application/clip_prefetch_worker.hpp"
#include "oscillate/application/graph_control.hpp"
#include "oscillate/application/project_session.hpp"
#include "oscillate/audio_io/graph_source.hpp"
#include "oscillate/audio_io/pipewire_output.hpp"
#include "oscillate/audio_io/stereo_block.hpp"
#include "oscillate/engine/graph.hpp"
#include "oscillate/persistence/store.hpp"
#include "session/session_state.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace oscillate::application {
namespace {

constexpr std::size_t max_live_audio_clips = 32;
constexpr std::size_t ring_frames = 16'384;
constexpr std::uint64_t audio_namespace = 1ULL << 63U;

std::uint64_t steady_nanoseconds(void*) noexcept {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count());
}

class PipeWirePlaybackOutput final : public PlaybackOutput {
public:
  PipeWirePlaybackOutput(audio_io::StereoRenderSource source, audio_io::TransportCursor& transport)
      : output_(source, transport) {}

  [[nodiscard]] foundation::Result<void>
  start(std::uint32_t rate, std::uint32_t latency_frames,
        std::optional<std::uint64_t> target_serial) override {
    return output_.start(rate, latency_frames, target_serial);
  }
  void stop() noexcept override { output_.stop(); }
  [[nodiscard]] audio_io::OutputStatus status() const noexcept override { return output_.status(); }

private:
  audio_io::PipeWireOutput output_;
};

[[nodiscard]] foundation::Result<std::pair<std::uint64_t, std::uint64_t>>
project_interval(const project::Snapshot& snapshot, const project::Clip& clip) {
  if (const auto* musical = std::get_if<project::MusicalInterval>(&clip.placement)) {
    auto first = snapshot.tempo.tick_to_sample(musical->start, snapshot.sample_rate);
    auto last = snapshot.tempo.tick_to_sample(musical->end, snapshot.sample_rate);
    if (!first || !last) {
      return foundation::Result<std::pair<std::uint64_t, std::uint64_t>>::failure(
          !first ? first.error() : last.error());
    }
    if (first.value().value < 0 || last.value() <= first.value()) {
      return foundation::Result<std::pair<std::uint64_t, std::uint64_t>>::failure(
          {foundation::ErrorCode::invalid_argument, "Audio clip has an invalid project interval"});
    }
    return foundation::Result<std::pair<std::uint64_t, std::uint64_t>>::success(
        {static_cast<std::uint64_t>(first.value().value),
         static_cast<std::uint64_t>(last.value().value)});
  }
  const auto& elapsed = std::get<project::ElapsedInterval>(clip.placement);
  if (elapsed.start.value < 0 || elapsed.end <= elapsed.start) {
    return foundation::Result<std::pair<std::uint64_t, std::uint64_t>>::failure(
        {foundation::ErrorCode::invalid_argument, "Audio clip has an invalid project interval"});
  }
  return foundation::Result<std::pair<std::uint64_t, std::uint64_t>>::success(
      {static_cast<std::uint64_t>(elapsed.start.value),
       static_cast<std::uint64_t>(elapsed.end.value)});
}

[[nodiscard]] bool audio_shape_same(const project::Snapshot& old,
                                    const project::Snapshot& current) {
  if (old.sample_rate != current.sample_rate || old.tempo.revision() != current.tempo.revision() ||
      old.sources != current.sources) {
    return false;
  }
  std::vector<project::Clip> previous;
  std::vector<project::Clip> next;
  for (const auto& track : old.tracks) {
    previous.insert(previous.end(), track.clips.begin(), track.clips.end());
  }
  for (const auto& track : current.tracks) {
    next.insert(next.end(), track.clips.begin(), track.clips.end());
  }
  return previous == next;
}

void append_clip_feed(std::vector<ClipFeed>& feeds, project::ClipId clip) {
  feeds.push_back({clip, std::make_shared<media::PrefetchRing>(clip, ring_frames),
                   std::make_shared<PlaybackReadiness>(),
                   audio_namespace + static_cast<std::uint64_t>(feeds.size())});
}

} // namespace

struct InstrumentPlayback::Run {
  explicit Run(project::ProjectModel& model) : control(model, graph, 8192) {}

  ~Run() { stop(); }

  void stop() noexcept {
    if (output) {
      output->stop();
      output.reset();
    }
    control.observe();
    if (token) {
      token->finish_after_join();
    }
  }

  engine::GraphPublisher graph;
  GraphControl control;
  audio_io::TransportCursor transport;
  std::vector<ClipFeed> clip_feeds;
  std::vector<std::unique_ptr<ClipPrefetchWorker>> clip_workers;
  std::shared_ptr<const project::Snapshot> audio_snapshot;
  std::uint32_t sample_rate = 0;
  std::optional<std::uint64_t> target_serial;
  std::optional<ControlTransportGate::PlaybackToken> token;
  std::unique_ptr<PlaybackOutput> output;
};

std::string playback_buffer_frames_text() {
  std::string text;
  for (std::size_t i = 0; i < playback_buffer_frames.size(); ++i) {
    if (i > 0)
      text += i + 1 == playback_buffer_frames.size() ? " or " : ", ";
    text += std::to_string(playback_buffer_frames[i]);
  }
  return text;
}

PlaybackOutputFactory pipewire_output_factory() {
  return [](audio_io::StereoRenderSource source,
            audio_io::TransportCursor& transport) -> std::unique_ptr<PlaybackOutput> {
    return std::make_unique<PipeWirePlaybackOutput>(source, transport);
  };
}

InstrumentPlayback::InstrumentPlayback(ProjectSession& session,
                                       PlaybackOutputFactory output_factory,
                                       std::uint32_t requested_buffer_frames)
    : session_(session), output_factory_(std::move(output_factory)),
      requested_buffer_frames_(valid_playback_buffer_frames(requested_buffer_frames)
                                   ? requested_buffer_frames
                                   : default_playback_buffer_frames) {
  if (!output_factory_)
    output_factory_ = pipewire_output_factory();
}
InstrumentPlayback::~InstrumentPlayback() { stop(); }

foundation::Result<void> InstrumentPlayback::set_requested_buffer_frames(std::uint32_t frames) {
  if (!valid_playback_buffer_frames(frames))
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::invalid_argument,
         "Requested audio buffer must be " + playback_buffer_frames_text() + " frames"});
  if (run_ || retiring())
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::conflict, "Stop playback before changing the buffer request"});
  requested_buffer_frames_ = frames;
  return foundation::Result<void>::success();
}

foundation::Result<PreparedInstrumentPlayback>
InstrumentPlaybackPreparation::run(const std::stop_token& stop) && {
  if (!snapshot) {
    return foundation::Result<PreparedInstrumentPlayback>::failure(
        {foundation::ErrorCode::invalid_argument, "Playback preparation has no project snapshot"});
  }
  if (project::requires_automation_rendering(*snapshot)) {
    return foundation::Result<PreparedInstrumentPlayback>::failure(
        {foundation::ErrorCode::unsupported, "Direct playback cannot apply automation"});
  }
  if (project::requires_routed_rendering(*snapshot)) {
    return foundation::Result<PreparedInstrumentPlayback>::failure(
        {foundation::ErrorCode::unsupported,
         "Live playback cannot render saved buses or sends yet"});
  }
  if (std::ranges::any_of(snapshot->tracks, [](const project::Track& track) {
        return !std::holds_alternative<std::monostate>(track.instrument_source) ||
               !track.effects.empty();
      })) {
    return foundation::Result<PreparedInstrumentPlayback>::failure(
        {foundation::ErrorCode::unsupported,
         "Direct playback cannot render instruments or track inserts"});
  }
  std::size_t clip_count = 0;
  for (const auto& track : snapshot->tracks) {
    if (track.clips.size() > max_live_audio_clips - clip_count) {
      return foundation::Result<PreparedInstrumentPlayback>::failure(
          {foundation::ErrorCode::resource_exhausted,
           "Live playback supports at most 32 prepared audio clips"});
    }
    clip_count += track.clips.size();
  }
  auto reader = persistence::ProjectStore::open_reader(root);
  if (!reader) {
    return foundation::Result<PreparedInstrumentPlayback>::failure(reader.error());
  }
  std::optional<PreparedAudioClip> audio_clip;
  std::vector<PreparedAudioClip> additional_audio_clips;
  for (const auto& track : snapshot->tracks) {
    for (const auto& clip : track.clips) {
      if (stop.stop_requested())
        return foundation::Result<PreparedInstrumentPlayback>::failure(
            {foundation::ErrorCode::cancelled, "playback preparation cancelled"});
      if (clip.processing.requires_rendering())
        return foundation::Result<PreparedInstrumentPlayback>::failure(
            {foundation::ErrorCode::unsupported,
             "Audio clip pitch or reviewed warp markers need a processed source"});
      if (clip.clay)
        return foundation::Result<PreparedInstrumentPlayback>::failure(
            {foundation::ErrorCode::unsupported, "Clay note edits play through routed playback"});
      const auto source = std::ranges::find_if(
          snapshot->sources, [&clip](const auto& item) { return item.id == clip.source; });
      if (source == snapshot->sources.end()) {
        return foundation::Result<PreparedInstrumentPlayback>::failure(
            {foundation::ErrorCode::unavailable, "Audio clip source has no project record"});
      }
      auto fd = reader.value().open_verified_source(*source, stop);
      if (!fd) {
        return foundation::Result<PreparedInstrumentPlayback>::failure(fd.error());
      }
      auto opened = media::AudioReader::open_fd(fd.value(), *source);
      (void)::close(fd.value());
      if (!opened) {
        return foundation::Result<PreparedInstrumentPlayback>::failure(opened.error());
      }
      if (clip.source_range.start.value < 0 || clip.source_range.end <= clip.source_range.start) {
        return foundation::Result<PreparedInstrumentPlayback>::failure(
            {foundation::ErrorCode::invalid_argument, "Audio clip source range is invalid"});
      }
      auto interval = project_interval(*snapshot, clip);
      if (!interval) {
        return foundation::Result<PreparedInstrumentPlayback>::failure(interval.error());
      }
      const auto source_length = clip.source_range.end.value - clip.source_range.start.value;
      if (static_cast<std::uint64_t>(source_length) !=
          interval.value().second - interval.value().first) {
        return foundation::Result<PreparedInstrumentPlayback>::failure(
            {foundation::ErrorCode::unsupported,
             "Audio clip requires one source frame per project frame"});
      }
      PreparedAudioClip prepared{clip.id,
                                 std::move(opened).value(),
                                 static_cast<std::uint64_t>(clip.source_range.start.value),
                                 static_cast<std::uint64_t>(clip.source_range.end.value),
                                 interval.value().first,
                                 interval.value().second};
      if (!audio_clip) {
        audio_clip.emplace(std::move(prepared));
      } else {
        additional_audio_clips.push_back(std::move(prepared));
      }
    }
  }
  return foundation::Result<PreparedInstrumentPlayback>::success(
      {std::move(snapshot), std::move(audio_clip), std::move(additional_audio_clips)});
}

foundation::Result<InstrumentPlaybackPreparation> InstrumentPlayback::prepare_start() const {
  if (run_ || retiring()) {
    return foundation::Result<InstrumentPlaybackPreparation>::failure(
        {foundation::ErrorCode::conflict, "Playback is active or stopping"});
  }
  if (!session_.state_->pending_receipts.empty()) {
    return foundation::Result<InstrumentPlaybackPreparation>::failure(
        {foundation::ErrorCode::conflict, "Save the pending managed source before playback"});
  }
  const auto snapshot = session_.state_->model->snapshot();
  return foundation::Result<InstrumentPlaybackPreparation>::success(
      {session_.state_->root, snapshot});
}

foundation::Result<InstrumentPlaybackPreparation> InstrumentPlayback::prepare_live_edit() const {
  if (!run_ || !run_->output) {
    return foundation::Result<InstrumentPlaybackPreparation>::failure(
        {foundation::ErrorCode::conflict, "Live edit requires streaming playback"});
  }
  if (!session_.state_->pending_receipts.empty()) {
    return foundation::Result<InstrumentPlaybackPreparation>::failure(
        {foundation::ErrorCode::conflict, "Save the pending managed source before live playback"});
  }
  const auto snapshot = session_.state_->model->snapshot();
  if (snapshot->id != run_->audio_snapshot->id ||
      snapshot->revision <= run_->audio_snapshot->revision) {
    return foundation::Result<InstrumentPlaybackPreparation>::failure(
        {foundation::ErrorCode::conflict, "No newer project revision is available for live edit"});
  }
  return foundation::Result<InstrumentPlaybackPreparation>::success(
      {session_.state_->root, snapshot});
}

foundation::Result<void>
InstrumentPlayback::start_prepared(PreparedInstrumentPlayback prepared,
                                   std::optional<std::uint64_t> target_serial) {
  if (!prepared.snapshot) {
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::invalid_argument, "Prepared playback has no project snapshot"});
  }
  if (run_ || retiring()) {
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::conflict, "Playback is active or stopping"});
  }
  if (!session_.state_->pending_receipts.empty() ||
      session_.state_->model->snapshot()->revision != prepared.snapshot->revision ||
      session_.state_->model->snapshot()->id != prepared.snapshot->id) {
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::conflict, "Project changed during playback preparation; retry"});
  }
  // This slice uses one producer per clip. Bound the number of workers and
  // graph nodes until a shared, transport-aware prefetch scheduler lands.
  const auto clip_count = (prepared.audio_clip ? 1U : 0U) + prepared.additional_audio_clips.size();
  if (clip_count > max_live_audio_clips) {
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::resource_exhausted,
         "Live playback supports at most 32 prepared audio clips"});
  }
  run_ = std::make_unique<Run>(*session_.state_->model);
  status_override_.reset();
  run_->sample_rate = prepared.snapshot->sample_rate;
  run_->audio_snapshot = prepared.snapshot;
  run_->target_serial = target_serial;
  run_->clip_feeds.reserve(clip_count);
  run_->clip_workers.reserve(clip_count);
  retired_workers_.reserve(clip_count);
  if (prepared.audio_clip) {
    append_clip_feed(run_->clip_feeds, prepared.audio_clip->clip);
  }
  for (const auto& clip : prepared.additional_audio_clips) {
    append_clip_feed(run_->clip_feeds, clip.clip);
  }
  auto reservation = session_.reserve_playback_start();
  if (!reservation) {
    stop();
    return foundation::Result<void>::failure(reservation.error());
  }
  run_->token.emplace(std::move(reservation).value());
  auto published = run_->control.submit_current(run_->clip_feeds);
  if (!published) {
    stop();
    return published;
  }
  const auto start_worker = [this](PreparedAudioClip& clip,
                                   const ClipFeed& feed) -> foundation::Result<void> {
    const auto length = clip.end_source_frame - clip.first_source_frame;
    const auto minimum = static_cast<std::size_t>(std::min<std::uint64_t>(length, 4'096));
    auto worker = ClipPrefetchWorker::start(std::move(clip.source), feed.ring, feed.readiness,
                                            {run_->sample_rate,
                                             1,
                                             clip.first_source_frame,
                                             clip.end_source_frame,
                                             {minimum, 2'000'000'000ULL}},
                                            {nullptr, steady_nanoseconds});
    if (!worker) {
      return foundation::Result<void>::failure(worker.error());
    }
    run_->clip_workers.push_back(std::move(worker).value());
    return foundation::Result<void>::success();
  };
  std::size_t clip_index = 0;
  if (prepared.audio_clip) {
    auto result = start_worker(*prepared.audio_clip, run_->clip_feeds[clip_index++]);
    if (!result) {
      stop();
      return result;
    }
  }
  for (auto& clip : prepared.additional_audio_clips) {
    auto result = start_worker(clip, run_->clip_feeds[clip_index++]);
    if (!result) {
      stop();
      return result;
    }
  }
  if (clip_count != 0) {
    return foundation::Result<void>::success();
  }
  run_->output = output_factory_(audio_io::graph_render_source(run_->graph), run_->transport);
  if (!run_->output) {
    stop();
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::internal, "Playback output factory returned no device"});
  }
  auto started = run_->output->start(run_->sample_rate, requested_buffer_frames_, target_serial);
  if (!started) {
    stop();
    return started;
  }
  return foundation::Result<void>::success();
}

foundation::Result<void>
InstrumentPlayback::restart_prepared_after_edit(PreparedInstrumentPlayback prepared) {
  using foundation::ErrorCode;
  if (!run_ || !run_->output || !prepared.snapshot) {
    return foundation::Result<void>::failure(
        {ErrorCode::conflict, "Live edit handoff requires active playback and prepared media"});
  }
  const auto current = session_.state_->model->snapshot();
  if (!session_.state_->pending_receipts.empty() || prepared.snapshot != current ||
      current->id != run_->audio_snapshot->id ||
      current->revision <= run_->audio_snapshot->revision ||
      current->sample_rate != run_->sample_rate) {
    return foundation::Result<void>::failure(
        {ErrorCode::conflict, "Live edit preparation is stale; prepare the current revision"});
  }
  if (run_->graph.has_pending()) {
    return foundation::Result<void>::failure(
        {ErrorCode::conflict, "Previous graph change must activate before live clip handoff"});
  }
  const auto clip_count = (prepared.audio_clip ? 1U : 0U) + prepared.additional_audio_clips.size();
  if (clip_count > max_live_audio_clips) {
    return foundation::Result<void>::failure(
        {ErrorCode::resource_exhausted, "Live playback supports at most 32 audio clips"});
  }
  // The output owns the callback join. No old ring or processor is modified
  // until stop() has returned and the last completed sample is coherent.
  run_->output->stop();
  run_->output.reset();
  run_->control.observe();
  const auto fail_after_quiesce = [this, &current](foundation::Error error) {
    auto visible = run_->control.status();
    visible.model_revision = current->revision;
    visible.application = PlaybackApplication::failed;
    visible.change = ChangeDescriptor{"audio clip live edit", {}};
    for (const auto& track : current->tracks) {
      for (const auto& clip : track.clips) {
        visible.change->affected.clips.push_back(clip.id);
      }
    }
    visible.failure = error;
    status_override_ = std::move(visible);
    stop();
    return foundation::Result<void>::failure(std::move(error));
  };
  for (auto& worker : run_->clip_workers) {
    worker->request_stop();
    retired_workers_.push_back(std::move(worker));
  }
  run_->clip_workers.clear();
  const auto completed = run_->transport.completed_snapshot();
  if (completed.generation == std::numeric_limits<std::uint64_t>::max()) {
    return fail_after_quiesce(
        {ErrorCode::resource_exhausted, "Transport generation exhausted during live edit"});
  }
  const auto resumed = run_->transport.restart_from_completed_after_quiesce();
  if (!resumed || resumed->next_sample != completed.next_sample ||
      resumed->generation != completed.generation + 1) {
    return fail_after_quiesce(
        {ErrorCode::conflict, "Transport could not resume at the completed block"});
  }
  std::vector<ClipFeed> next_feeds;
  next_feeds.reserve(clip_count);
  if (prepared.audio_clip) {
    append_clip_feed(next_feeds, prepared.audio_clip->clip);
  }
  for (const auto& clip : prepared.additional_audio_clips) {
    append_clip_feed(next_feeds, clip.clip);
  }
  std::vector<std::unique_ptr<ClipPrefetchWorker>> next_workers;
  next_workers.reserve(clip_count);
  const auto start_worker = [&](PreparedAudioClip& clip,
                                const ClipFeed& feed) -> foundation::Result<void> {
    if (clip.project_start_frame >= clip.project_end_frame ||
        clip.end_source_frame <= clip.first_source_frame ||
        clip.project_end_frame - clip.project_start_frame !=
            clip.end_source_frame - clip.first_source_frame) {
      return foundation::Result<void>::failure(
          {ErrorCode::invalid_argument, "Prepared live clip has invalid geometry"});
    }
    if (resumed->next_sample >= clip.project_end_frame) {
      feed.readiness->set_steady(true);
      return foundation::Result<void>::success();
    }
    const auto offset = resumed->next_sample > clip.project_start_frame
                            ? resumed->next_sample - clip.project_start_frame
                            : 0;
    const auto first = clip.first_source_frame + offset;
    const auto minimum =
        static_cast<std::size_t>(std::min<std::uint64_t>(clip.end_source_frame - first, 4'096));
    auto worker = ClipPrefetchWorker::start(std::move(clip.source), feed.ring, feed.readiness,
                                            {run_->sample_rate,
                                             resumed->generation,
                                             first,
                                             clip.end_source_frame,
                                             {minimum, 2'000'000'000ULL}},
                                            {nullptr, steady_nanoseconds});
    if (!worker) {
      return foundation::Result<void>::failure(worker.error());
    }
    next_workers.push_back(std::move(worker).value());
    return foundation::Result<void>::success();
  };
  std::size_t index = 0;
  if (prepared.audio_clip) {
    auto started = start_worker(*prepared.audio_clip, next_feeds[index++]);
    if (!started) {
      return fail_after_quiesce(started.error());
    }
  }
  for (auto& clip : prepared.additional_audio_clips) {
    auto started = start_worker(clip, next_feeds[index++]);
    if (!started) {
      return fail_after_quiesce(started.error());
    }
  }
  run_->clip_feeds = std::move(next_feeds);
  run_->clip_workers = std::move(next_workers);
  run_->audio_snapshot = std::move(prepared.snapshot);
  auto published = run_->control.submit_current(run_->clip_feeds);
  if (!published) {
    return fail_after_quiesce(published.error());
  }
  status_override_.reset();
  return foundation::Result<void>::success();
}

foundation::Result<void> InstrumentPlayback::start(std::optional<std::uint64_t> target_serial) {
  auto task = prepare_start();
  if (!task) {
    return foundation::Result<void>::failure(task.error());
  }
  const auto& captured = *task.value().snapshot;
  if (std::ranges::any_of(captured.sources, [&](const auto& source) {
        if (!source.external_link)
          return false;
        return std::ranges::any_of(captured.tracks, [&](const auto& track) {
          return std::ranges::any_of(track.clips,
                                     [&](const auto& clip) { return clip.source == source.id; });
        });
      })) {
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::unsupported,
         "Externally linked audio needs worker playback preparation"});
  }
  auto prepared = std::move(task).value().run();
  if (!prepared) {
    return foundation::Result<void>::failure(prepared.error());
  }
  return start_prepared(std::move(prepared).value(), target_serial);
}

foundation::Result<bool> InstrumentPlayback::poll_start() {
  if (!run_) {
    return foundation::Result<bool>::failure(
        {foundation::ErrorCode::conflict, "Playback is not reserved"});
  }
  if (run_->output) {
    return foundation::Result<bool>::success(true);
  }
  const auto fail_start = [this](foundation::Error error) {
    auto visible = run_->control.status();
    visible.model_revision = session_.state_->model->snapshot()->revision;
    visible.application = PlaybackApplication::failed;
    visible.failure = error;
    if (!visible.change) {
      visible.change = ChangeDescriptor{"playback preparation", {}};
    }
    status_override_ = std::move(visible);
    stop();
    return foundation::Result<bool>::failure(std::move(error));
  };
  bool all_ready = true;
  for (const auto& worker : run_->clip_workers) {
    auto primed = worker->poll_priming();
    if (!primed) {
      return fail_start(primed.error());
    }
    if (primed.value() == PrimingStatus::timed_out) {
      return fail_start({foundation::ErrorCode::unavailable,
                         "Audio clip did not prime before playback deadline"});
    }
    all_ready = all_ready && primed.value() == PrimingStatus::ready;
  }
  if (!all_ready) {
    return foundation::Result<bool>::success(false);
  }
  run_->output = output_factory_(audio_io::graph_render_source(run_->graph), run_->transport);
  if (!run_->output) {
    return fail_start(
        {foundation::ErrorCode::internal, "Playback output factory returned no device"});
  }
  auto started =
      run_->output->start(run_->sample_rate, requested_buffer_frames_, run_->target_serial);
  if (!started) {
    return fail_start(started.error());
  }
  return foundation::Result<bool>::success(true);
}

void InstrumentPlayback::stop() noexcept {
  if (!run_) {
    return;
  }
  run_->stop();
  for (auto& worker : run_->clip_workers) {
    worker->request_stop();
    retired_workers_.push_back(std::move(worker));
  }
  run_.reset();
  (void)poll_retirement();
}

bool InstrumentPlayback::playing() const noexcept { return run_ != nullptr; }

bool InstrumentPlayback::retiring() const noexcept { return !retired_workers_.empty(); }

bool InstrumentPlayback::poll_retirement() noexcept {
  std::erase_if(retired_workers_, [](const auto& worker) { return worker->join_finished(); });
  return retired_workers_.empty();
}

foundation::Result<void> InstrumentPlayback::submit_current() {
  if (!run_) {
    return foundation::Result<void>::success();
  }
  run_->control.observe();
  const auto current = session_.state_->model->snapshot();
  if (!audio_shape_same(*run_->audio_snapshot, *current)) {
    foundation::Error error{foundation::ErrorCode::conflict,
                            "Audio clip or source changed; prepare and re-prime before playback"};
    auto visible = run_->control.status();
    visible.model_revision = current->revision;
    visible.application = PlaybackApplication::failed;
    visible.change = ChangeDescriptor{"audio clip live edit", {}};
    for (const auto& track : current->tracks) {
      for (const auto& clip : track.clips) {
        visible.change->affected.clips.push_back(clip.id);
      }
    }
    visible.failure = error;
    status_override_ = std::move(visible);
    return foundation::Result<void>::failure(std::move(error));
  }
  status_override_.reset();
  return run_->control.submit_current(run_->clip_feeds);
}

void InstrumentPlayback::observe() {
  if (run_) {
    run_->control.observe();
  }
}

RevisionStatus InstrumentPlayback::status() const {
  return status_override_.value_or(run_ ? run_->control.status() : RevisionStatus{});
}

audio_io::OutputState InstrumentPlayback::output_state() const noexcept {
  return output_status().state;
}

audio_io::OutputStatus InstrumentPlayback::output_status() const noexcept {
  return run_ && run_->output ? run_->output->status() : audio_io::OutputStatus{};
}

std::uint64_t InstrumentPlayback::completed_sample() const noexcept {
  return run_ ? run_->transport.completed_snapshot().next_sample : 0;
}

std::uint64_t InstrumentPlayback::source_underruns() const noexcept {
  std::uint64_t count = 0;
  if (run_) {
    for (const auto& feed : run_->clip_feeds) {
      const auto value = feed.ring->underrun_count();
      count += std::min(value, std::numeric_limits<std::uint64_t>::max() - count);
    }
  }
  return count;
}

std::optional<media::UnderrunEvent> InstrumentPlayback::pop_source_underrun() noexcept {
  if (!run_ || run_->clip_feeds.empty())
    return std::nullopt;
  const auto count = run_->clip_feeds.size();
  for (std::size_t offset = 0; offset < count; ++offset) {
    const auto index = (next_underrun_feed_ + offset) % count;
    if (auto event = run_->clip_feeds[index].ring->pop_underrun()) {
      next_underrun_feed_ = (index + 1) % count;
      return event;
    }
  }
  return std::nullopt;
}

std::optional<foundation::Error> InstrumentPlayback::source_failure() const {
  if (run_) {
    for (const auto& worker : run_->clip_workers) {
      if (auto failure = worker->failure()) {
        return failure;
      }
    }
  }
  return std::nullopt;
}

} // namespace oscillate::application
