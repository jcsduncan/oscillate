#include "clip_warp_controller.hpp"

#include "oscillate/application/clip_warp_edit.hpp"
#include "oscillate/processing/clip_frame_plan.hpp"
#include "oscillate/processing/warp_render_cache.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>

namespace oscillate::quick {
namespace {

// Editor order, as in the mode selector.
constexpr std::array<project::WarpMode, 6> mode_order{
    project::WarpMode::beats,   project::WarpMode::tones,   project::WarpMode::texture,
    project::WarpMode::repitch, project::WarpMode::complex, project::WarpMode::complex_pro};

[[nodiscard]] QString mode_name(project::WarpMode mode) {
  switch (mode) {
  case project::WarpMode::beats:
    return QStringLiteral("Beats");
  case project::WarpMode::tones:
    return QStringLiteral("Tones");
  case project::WarpMode::texture:
    return QStringLiteral("Texture");
  case project::WarpMode::repitch:
    return QStringLiteral("Re-Pitch");
  case project::WarpMode::complex:
    return QStringLiteral("Complex");
  case project::WarpMode::complex_pro:
    return QStringLiteral("Complex Pro");
  }
  return QStringLiteral("Unknown");
}

[[nodiscard]] int mode_index(project::WarpMode mode) {
  const auto found = std::ranges::find(mode_order, mode);
  return static_cast<int>(found - mode_order.begin());
}

[[nodiscard]] int loop_index(project::WarpBeatsLoop loop) {
  switch (loop) {
  case project::WarpBeatsLoop::off:
    return 0;
  case project::WarpBeatsLoop::forward:
    return 1;
  case project::WarpBeatsLoop::back_and_forth:
    return 2;
  }
  return 1;
}

[[nodiscard]] QString tier_name(warp_dsp::QualityTier tier) {
  switch (tier) {
  case warp_dsp::QualityTier::q0:
    return QStringLiteral("Q0");
  case warp_dsp::QualityTier::q1:
    return QStringLiteral("Q1");
  case warp_dsp::QualityTier::q2:
    return QStringLiteral("Q2");
  }
  return QStringLiteral("Q?");
}

[[nodiscard]] const project::Clip* find_clip(const project::Snapshot& snapshot,
                                             const QString& clip_id) {
  const auto id = project::ClipId::parse(clip_id.toStdString());
  if (!id)
    return nullptr;
  for (const auto& track : snapshot.tracks) {
    const auto found = std::ranges::find(track.clips, id.value(), &project::Clip::id);
    if (found != track.clips.end())
      return &*found;
  }
  return nullptr;
}

struct Range {
  double minimum;
  double maximum;
};

[[nodiscard]] std::optional<Range> setting_range(const QString& key) {
  if (key == QStringLiteral("beatsEnvelope") || key == QStringLiteral("textureFlux") ||
      key == QStringLiteral("formantAmount"))
    return Range{0.0, 100.0};
  if (key == QStringLiteral("tonesGrain"))
    return Range{5.0, 200.0};
  if (key == QStringLiteral("textureGrain"))
    return Range{10.0, 500.0};
  if (key == QStringLiteral("complexProEnvelope"))
    return Range{8.0, 256.0};
  return std::nullopt;
}

} // namespace

QString warp_mode_label(project::WarpMode mode) { return mode_name(mode); }

// One worker owns the latest onset request. Cancellation only updates its
// mailbox; no analysis thread is joined by selection or editor gestures.
// The worker never touches a QObject, so the controller can detach it safely
// during shutdown after requesting cancellation.
class WarpOnsetWorker {
public:
  struct Outcome {
    std::uint64_t serial;
    foundation::Result<application::SourceOnsets> result;
  };

  explicit WarpOnsetWorker(ClipWarpController::OnsetAnalyzer analyzer)
      : thread_([shared = shared_, analyzer = std::move(analyzer)](std::stop_token stopped) {
          while (!stopped.stop_requested()) {
            std::optional<Request> request;
            {
              std::unique_lock lock(shared->mutex);
              if (!shared->changed.wait(lock, stopped, [&] { return shared->pending.has_value(); }))
                break;
              request.emplace(std::move(*shared->pending));
              shared->pending.reset();
            }
            if (request->cancel->load(std::memory_order_acquire))
              continue;
            auto result = foundation::Result<application::SourceOnsets>::failure(
                {foundation::ErrorCode::unavailable, "Onset analysis did not run"});
            try {
              result = analyzer(request->root, request->snapshot, request->source,
                                request->cancel.get());
            } catch (const std::exception& error) {
              result = foundation::Result<application::SourceOnsets>::failure(
                  {foundation::ErrorCode::internal, error.what()});
            } catch (...) {
              result = foundation::Result<application::SourceOnsets>::failure(
                  {foundation::ErrorCode::internal, "Onset analysis failed"});
            }
            if (request->cancel->load(std::memory_order_acquire) || stopped.stop_requested())
              continue;
            std::lock_guard lock(shared->mutex);
            if (!request->cancel->load(std::memory_order_acquire))
              shared->ready.emplace(Outcome{request->serial, std::move(result)});
          }
        }) {}

  WarpOnsetWorker(const WarpOnsetWorker&) = delete;
  WarpOnsetWorker& operator=(const WarpOnsetWorker&) = delete;
  ~WarpOnsetWorker() {
    cancel();
    thread_.request_stop();
    shared_->changed.notify_all();
    thread_.detach();
  }

  void submit(std::filesystem::path root, project::Snapshot snapshot, project::SourceId source,
              std::uint64_t serial) {
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    {
      std::lock_guard lock(shared_->mutex);
      if (shared_->active_cancel)
        shared_->active_cancel->store(true, std::memory_order_release);
      shared_->active_cancel = cancel;
      shared_->pending = Request{std::move(root), std::move(snapshot), source, serial, cancel};
      shared_->ready.reset();
    }
    shared_->changed.notify_one();
  }

  void cancel() {
    std::lock_guard lock(shared_->mutex);
    if (shared_->active_cancel)
      shared_->active_cancel->store(true, std::memory_order_release);
    shared_->pending.reset();
    shared_->ready.reset();
  }

  [[nodiscard]] std::optional<Outcome> collect() {
    std::lock_guard lock(shared_->mutex);
    auto ready = std::move(shared_->ready);
    shared_->ready.reset();
    return ready;
  }

private:
  struct Request {
    std::filesystem::path root;
    project::Snapshot snapshot;
    project::SourceId source;
    std::uint64_t serial = 0;
    std::shared_ptr<std::atomic<bool>> cancel;
  };
  struct Shared {
    std::mutex mutex;
    std::condition_variable_any changed;
    std::optional<Request> pending;
    std::optional<Outcome> ready;
    std::shared_ptr<std::atomic<bool>> active_cancel;
  };
  std::shared_ptr<Shared> shared_ = std::make_shared<Shared>();
  std::jthread thread_;
};

struct ClipWarpController::Selected {
  project::Snapshot snapshot;
  project::Clip clip;
};

ClipWarpController::ClipWarpController(Session session, Root root, CanEdit can_edit, Report report,
                                       Commit commit, Waveform waveform, Playback playback,
                                       TempoEstimate tempo, QObject* parent,
                                       OnsetAnalyzer onset_analyzer)
    : QObject(parent), session_(std::move(session)), root_(std::move(root)),
      can_edit_(std::move(can_edit)), report_(std::move(report)), commit_(std::move(commit)),
      waveform_(std::move(waveform)), playback_source_(std::move(playback)),
      onset_analyzer_(std::move(onset_analyzer)), tempo_(std::move(tempo)) {
  if (!onset_analyzer_)
    onset_analyzer_ = application::analyse_source_onsets;
  suggestion_poll_timer_.setInterval(20);
  connect(&suggestion_poll_timer_, &QTimer::timeout, this, [this] {
    if (!onset_worker_)
      return;
    auto ready = onset_worker_->collect();
    if (!ready)
      return;
    suggestion_poll_timer_.stop();
    deliver_suggestions(ready->serial, std::move(ready->result));
  });
  rebuild();
}

ClipWarpController::~ClipWarpController() {
  cancel_suggestions();
  onset_worker_.reset();
}

std::optional<ClipWarpController::Selected> ClipWarpController::selected() const {
  const auto* project = session_ ? session_() : nullptr;
  if (!project || clip_id_.isEmpty())
    return std::nullopt;
  const auto* clip = find_clip(project->snapshot(), clip_id_);
  if (!clip)
    return std::nullopt;
  return Selected{project->snapshot(), *clip};
}

void ClipWarpController::select_clip(const QString& clip_id) {
  if (clip_id == clip_id_)
    return refresh_snapshot();
  cancel_suggestions();
  clip_id_ = clip_id;
  pending_.reset();
  proposal_.reset();
  suggestion_state_ = QStringLiteral("idle");
  suggestion_message_.clear();
  rebuild();
  refresh_playback();
}

void ClipWarpController::refresh_snapshot() {
  rebuild();
  refresh_playback();
}

void ClipWarpController::rebuild() {
  QVariantMap state;
  const auto current = selected();
  state.insert(QStringLiteral("available"), current.has_value());
  state.insert(QStringLiteral("clipId"), clip_id_);
  state.insert(QStringLiteral("editable"), current.has_value() && can_edit_ && can_edit_());
  state.insert(QStringLiteral("suggestionState"), suggestion_state_);
  state.insert(QStringLiteral("suggestionMessage"), suggestion_message_);
  if (!current) {
    if (state != state_) {
      state_ = state;
      emit changed();
    }
    return;
  }
  const auto& snapshot = current->snapshot;
  const auto& clip = current->clip;
  const auto& processing = clip.processing;
  const auto& warp = processing.warp;
  const auto parts = application::split_transpose(processing.pitch_semitones);
  state.insert(QStringLiteral("modeIndex"), mode_index(warp.mode));
  state.insert(QStringLiteral("modeName"), mode_name(warp.mode));
  state.insert(QStringLiteral("overridden"), warp.mode_overridden);
  state.insert(QStringLiteral("semitones"), parts.semitones);
  state.insert(QStringLiteral("cents"), parts.cents);
  state.insert(QStringLiteral("transposeActive"), warp.mode != project::WarpMode::repitch);
  state.insert(QStringLiteral("preserveFormants"), processing.preserve_formants);
  state.insert(QStringLiteral("formantsActive"), warp.mode == project::WarpMode::complex_pro);
  state.insert(QStringLiteral("formantAmount"), warp.complex_pro.formants);
  state.insert(QStringLiteral("complexProEnvelope"), warp.complex_pro.envelope);
  state.insert(QStringLiteral("beatsLoop"), loop_index(warp.beats.loop));
  state.insert(QStringLiteral("beatsEnvelope"), warp.beats.envelope);
  state.insert(QStringLiteral("tonesGrain"), warp.tones.grain_ms);
  state.insert(QStringLiteral("textureGrain"), warp.texture.grain_ms);
  state.insert(QStringLiteral("textureFlux"), warp.texture.flux);

  const auto request = processing::processed_clip_request(snapshot, clip.id);
  state.insert(QStringLiteral("processed"), request.has_value() && request.value().has_value());
  state.insert(QStringLiteral("renderMessage"),
               !request ? QString::fromStdString(request.error().message)
               : request.value()
                   ? QStringLiteral("Renders through the %1 warp engine").arg(mode_name(warp.mode))
                   : QStringLiteral("Plays its source unprocessed (no pitch, "
                                    "markers or stretch)"));

  const auto layout = application::clip_warp_layout(snapshot, clip.id);
  QVariantList markers;
  QVariantList anchors;
  QVariantList beats;
  QVariantList suggested;
  if (layout) {
    const auto& value = layout.value();
    const auto output = static_cast<double>(std::max<std::uint64_t>(value.output_frames, 1));
    const auto source = static_cast<double>(std::max<std::uint64_t>(value.source_frames, 1));
    state.insert(QStringLiteral("outputFrames"), static_cast<qulonglong>(value.output_frames));
    state.insert(QStringLiteral("musical"), value.musical);
    anchors.append(QVariantMap{{"output", 0.0}, {"source", 0.0}});
    for (std::size_t index = 0; index < value.markers.size(); ++index) {
      const auto& marker = value.markers[index];
      const auto output_fraction = static_cast<double>(marker.output_frame) / output;
      const auto source_fraction =
          static_cast<double>(marker.source_frame - value.source_first) / source;
      markers.append(QVariantMap{{"index", static_cast<int>(index)},
                                 {"fraction", output_fraction},
                                 {"sourceFraction", source_fraction},
                                 {"outputFrame", static_cast<qulonglong>(marker.output_frame)},
                                 {"sourceFrame", static_cast<qulonglong>(marker.source_frame)}});
      anchors.append(QVariantMap{{"output", output_fraction}, {"source", source_fraction}});
    }
    anchors.append(QVariantMap{{"output", 1.0}, {"source", 1.0}});
    // Quarter-note grid across the clip, from the shared tempo map.
    const auto start_tick = snapshot.tempo.sample_to_tick(
        {static_cast<std::int64_t>(value.project_start)}, snapshot.sample_rate);
    if (start_tick) {
      const auto quarter = timeline::ticks_per_quarter;
      auto tick = (start_tick.value().value + quarter - 1) / quarter * quarter;
      for (int count = 0; count < 512; ++count, tick += quarter) {
        const auto sample = snapshot.tempo.tick_to_sample({tick}, snapshot.sample_rate);
        if (!sample || sample.value().value < 0)
          break;
        const auto local =
            static_cast<double>(sample.value().value) - static_cast<double>(value.project_start);
        if (local >= output)
          break;
        beats.append(
            QVariantMap{{"fraction", local / output}, {"downbeat", tick % (4 * quarter) == 0}});
      }
    }
    // Durable onset suggestions inside the clip, where they play now.
    auto plan = processing::prepare_clip_frame_plan(
        clip, *std::ranges::find(snapshot.sources, clip.source, &project::SourceRecord::id),
        snapshot.tempo, snapshot.sample_rate);
    if (plan) {
      for (const auto frame : visible_suggestions(*current)) {
        const auto local = plan.value().warp.output_at_source(frame - value.source_first);
        if (local)
          suggested.append(QVariantMap{{"fraction", static_cast<double>(*local) / output},
                                       {"sourceFrame", static_cast<qulonglong>(frame)}});
      }
    }
    // A proposal belongs to the clip and revision it was made for.
    if (proposal_ && (proposal_->clip != clip_id_ || proposal_->revision != snapshot.revision)) {
      proposal_.reset();
      state.insert(QStringLiteral("proposalMessage"),
                   QStringLiteral("The project changed; propose again"));
    }
    if (proposal_) {
      const auto& placement = std::get<project::MusicalInterval>(proposal_->value.placement);
      const auto start = snapshot.tempo.tick_to_sample(placement.start, snapshot.sample_rate);
      const auto end = snapshot.tempo.tick_to_sample(placement.end, snapshot.sample_rate);
      QVariantList proposed;
      if (start && end && end.value().value > start.value().value) {
        const auto length = static_cast<double>(end.value().value - start.value().value);
        for (const auto& marker : proposal_->value.processing.reviewed_warp_markers) {
          const auto tick = std::get<timeline::TickPosition>(marker.target_offset).value;
          const auto at =
              snapshot.tempo.tick_to_sample({placement.start.value + tick}, snapshot.sample_rate);
          if (at)
            proposed.append(QVariantMap{
                {"fraction", static_cast<double>(at.value().value - start.value().value) / length},
                {"sourceFrame", static_cast<qulonglong>(marker.source_frame.value)}});
        }
        const auto change = length / output;
        QString summary =
            proposal_->kind == QStringLiteral("auto-warp")
                ? QStringLiteral("Auto-warp: %1 reviewed beats onto the grid at %2 BPM source "
                                 "tempo")
                      .arg(proposal_->value.beat_markers)
                      .arg(proposal_->value.source_bpm, 0, 'f', 1)
                : QStringLiteral("Conform: %1 BPM source to the project tempo")
                      .arg(proposal_->value.source_bpm, 0, 'f', 1);
        if (proposal_->value.skipped_beats != 0)
          summary += QStringLiteral(" · %1 beats skipped to keep your markers")
                         .arg(proposal_->value.skipped_beats);
        summary += QStringLiteral(" · clip length ×%1").arg(change, 0, 'f', 3);
        state.insert(QStringLiteral("proposal"), QVariantMap{{"kind", proposal_->kind},
                                                             {"summary", summary},
                                                             {"markers", proposed},
                                                             {"lengthRatio", change}});
      }
    }
  } else {
    state.insert(QStringLiteral("layoutMessage"), QString::fromStdString(layout.error().message));
  }
  state.insert(QStringLiteral("markers"), markers);
  state.insert(QStringLiteral("anchors"), anchors);
  state.insert(QStringLiteral("beats"), beats);
  state.insert(QStringLiteral("suggestions"), suggested);
  state.insert(QStringLiteral("waveform"), waveform_ ? waveform_(clip, 192) : QVariantList{});
  if (state == state_)
    return;
  state_ = state;
  emit changed();
}

void ClipWarpController::refresh_playback() {
  QVariantMap view;
  const auto* project = session_ ? session_() : nullptr;
  const auto saved = project ? project->snapshot().revision : 0;
  const auto observed = playback_source_ ? playback_source_() : WarpPlaybackObservation{};
  const bool processed = state_.value(QStringLiteral("processed")).toBool();
  view.insert(QStringLiteral("savedRevision"), static_cast<qulonglong>(saved));
  view.insert(QStringLiteral("activeRevision"), static_cast<qulonglong>(observed.active_revision));
  QString state;
  QString text;
  QString backing;
  QString tier;
  qulonglong step_downs = 0;
  switch (observed.path) {
  case WarpPlaybackObservation::Path::stopped:
    state = QStringLiteral("stopped");
    text =
        QStringLiteral("Not playing. Saved revision %1 is what Play and Export render.").arg(saved);
    break;
  case WarpPlaybackObservation::Path::direct:
    state = processed ? QStringLiteral("failed") : QStringLiteral("active");
    text = processed ? QStringLiteral("This playback path cannot render warp or pitch. Stop and "
                                      "Play to hear saved revision %1.")
                           .arg(saved)
                     : QStringLiteral("Playing; this clip plays its source unprocessed.");
    break;
  case WarpPlaybackObservation::Path::routed: {
    const auto clip = project::ClipId::parse(clip_id_.toStdString());
    const auto found = clip ? std::ranges::find(observed.processed, clip.value(),
                                                &application::RoutedProcessedClipStatus::clip)
                            : observed.processed.end();
    if (found != observed.processed.end()) {
      backing = found->live.backing == processing::WarpLiveBacking::cache ? QStringLiteral("cache")
                                                                          : QStringLiteral("live");
      tier = tier_name(found->live.tier);
      step_downs = found->live.step_downs;
    }
    const auto heard =
        backing.isEmpty()
            ? QStringLiteral("this clip's source unprocessed")
            : QStringLiteral("a %1 render at %2%3")
                  .arg(backing == QStringLiteral("cache") ? QStringLiteral("cached")
                                                          : QStringLiteral("live"))
                  .arg(tier)
                  .arg(step_downs > 0
                           ? QStringLiteral(" (stepped down %1× to keep up)").arg(step_downs)
                           : QString{});
    if (observed.failure) {
      state = QStringLiteral("failed");
      text = QStringLiteral("Saved revision %1 is not audible: %2. You still hear revision %3.")
                 .arg(saved)
                 .arg(*observed.failure)
                 .arg(observed.active_revision);
    } else if (observed.edit_pending || observed.active_revision != saved) {
      state = QStringLiteral("pending");
      text = QStringLiteral("Saved revision %1 is pending; you still hear revision %2 (%3).")
                 .arg(saved)
                 .arg(observed.active_revision)
                 .arg(heard);
    } else {
      state = QStringLiteral("active");
      text = QStringLiteral("Audible: revision %1, %2.").arg(saved).arg(heard);
    }
    break;
  }
  }
  view.insert(QStringLiteral("state"), state);
  view.insert(QStringLiteral("text"), text);
  view.insert(QStringLiteral("backing"), backing);
  view.insert(QStringLiteral("tier"), tier);
  view.insert(QStringLiteral("stepDowns"), step_downs);
  if (view != playback_) {
    playback_ = view;
    emit playbackChanged();
  }
}

application::ProjectSession* ClipWarpController::editable_session() {
  auto* project = session_ ? session_() : nullptr;
  if (project && can_edit_ && can_edit_())
    return project;
  report_(QStringLiteral("Finish the current project operation before editing warp"));
  return nullptr;
}

void ClipWarpController::edit(const QString& action, project::ClipProcessing processing) {
  auto* project = editable_session();
  const auto current = selected();
  if (!project || !current)
    return;
  if (current->clip.processing == processing)
    return;
  const auto applied =
      project->audio_clips().set_audio_clip_processing(current->clip.id, std::move(processing));
  if (!applied) {
    report_(QStringLiteral("Warp: %1").arg(QString::fromStdString(applied.error().message)));
    return;
  }
  const auto revision = project->snapshot().revision;
  commit_();
  report_(QStringLiteral("%1 · revision %2").arg(action).arg(revision));
}

void ClipWarpController::edit(const QString& action,
                              foundation::Result<project::ClipProcessing> processing) {
  if (!processing) {
    report_(QStringLiteral("Warp: %1").arg(QString::fromStdString(processing.error().message)));
    return;
  }
  edit(action, std::move(processing).value());
}

void ClipWarpController::set_mode(int index) {
  const auto current = selected();
  if (!current || index < 0 || index >= static_cast<int>(mode_order.size())) {
    report_(QStringLiteral("Choose one of the six warp modes"));
    return;
  }
  auto processing = current->clip.processing;
  processing.warp.mode = mode_order[static_cast<std::size_t>(index)];
  processing.warp.mode_overridden = true;
  edit(QStringLiteral("Warp mode %1").arg(mode_name(processing.warp.mode)), processing);
}

void ClipWarpController::set_mode_overridden(bool overridden) {
  const auto current = selected();
  if (!current)
    return;
  auto processing = current->clip.processing;
  processing.warp.mode_overridden = overridden;
  edit(overridden ? QStringLiteral("Warp mode kept as your choice")
                  : QStringLiteral("Warp mode released to analysis suggestions"),
       processing);
}

void ClipWarpController::set_transpose(int semitones, int cents) {
  const auto current = selected();
  const auto total = semitones * 100 + cents;
  if (!current || semitones < -48 || semitones > 48 || cents < -50 || cents > 50 ||
      total < -4'800 || total > 4'800) {
    report_(QStringLiteral("Transpose is -48..48 semitones and -50..50 cents, within ±48 "
                           "semitones in total"));
    return;
  }
  auto processing = current->clip.processing;
  processing.pitch_semitones = application::join_transpose(semitones, cents);
  edit(QStringLiteral("Transpose %1 st %2 ct").arg(semitones).arg(cents), processing);
}

void ClipWarpController::set_preserve_formants(bool preserve) {
  const auto current = selected();
  if (!current)
    return;
  auto processing = current->clip.processing;
  processing.preserve_formants = preserve;
  edit(preserve ? QStringLiteral("Formant preservation on")
                : QStringLiteral("Formant preservation off"),
       processing);
}

void ClipWarpController::set_beats_loop(int index) {
  const auto current = selected();
  if (!current || index < 0 || index > 2) {
    report_(QStringLiteral("Beats gap fill is Off, Forward or Back-and-forth"));
    return;
  }
  constexpr std::array loops{project::WarpBeatsLoop::off, project::WarpBeatsLoop::forward,
                             project::WarpBeatsLoop::back_and_forth};
  auto processing = current->clip.processing;
  processing.warp.beats.loop = loops[static_cast<std::size_t>(index)];
  edit(QStringLiteral("Beats gap fill changed"), processing);
}

void ClipWarpController::set_setting(const QString& key, double value) {
  const auto current = selected();
  const auto range = setting_range(key);
  if (!current || !range || !std::isfinite(value) || value < range->minimum ||
      value > range->maximum) {
    report_(range ? QStringLiteral("%1 must be between %2 and %3")
                        .arg(key)
                        .arg(range->minimum)
                        .arg(range->maximum)
                  : QStringLiteral("Unknown warp setting %1").arg(key));
    return;
  }
  auto processing = current->clip.processing;
  auto& warp = processing.warp;
  if (key == QStringLiteral("beatsEnvelope"))
    warp.beats.envelope = value;
  else if (key == QStringLiteral("tonesGrain"))
    warp.tones.grain_ms = value;
  else if (key == QStringLiteral("textureGrain"))
    warp.texture.grain_ms = value;
  else if (key == QStringLiteral("textureFlux"))
    warp.texture.flux = value;
  else if (key == QStringLiteral("formantAmount"))
    warp.complex_pro.formants = value;
  else
    warp.complex_pro.envelope = value;
  edit(QStringLiteral("Warp setting %1 = %2").arg(key).arg(value), processing);
}

void ClipWarpController::add_marker(double fraction) {
  const auto current = selected();
  const auto frames = state_.value(QStringLiteral("outputFrames")).toULongLong();
  if (!current || !std::isfinite(fraction) || fraction <= 0.0 || fraction >= 1.0 || frames == 0) {
    report_(QStringLiteral("Place a warp marker strictly inside the clip"));
    return;
  }
  const auto output =
      static_cast<std::uint64_t>(std::llround(fraction * static_cast<double>(frames)));
  edit(QStringLiteral("Warp marker added"),
       application::add_warp_marker(current->snapshot, current->clip.id, output));
}

void ClipWarpController::move_marker(int index, double fraction) {
  const auto current = selected();
  const auto frames = state_.value(QStringLiteral("outputFrames")).toULongLong();
  if (!current || index < 0 || !std::isfinite(fraction) || fraction <= 0.0 || fraction >= 1.0 ||
      frames == 0) {
    report_(QStringLiteral("Move the warp marker within the clip"));
    return;
  }
  const auto output =
      static_cast<std::uint64_t>(std::llround(fraction * static_cast<double>(frames)));
  edit(QStringLiteral("Warp marker moved"),
       application::move_warp_marker(current->snapshot, current->clip.id,
                                     static_cast<std::size_t>(index), output));
}

void ClipWarpController::remove_marker(int index) {
  const auto current = selected();
  if (!current || index < 0) {
    report_(QStringLiteral("Choose a warp marker to delete"));
    return;
  }
  edit(QStringLiteral("Warp marker deleted"),
       application::remove_warp_marker(current->snapshot, current->clip.id,
                                       static_cast<std::size_t>(index)));
}

void ClipWarpController::cancel_suggestions() {
  ++suggestion_serial_;
  suggestion_poll_timer_.stop();
  if (onset_worker_)
    onset_worker_->cancel();
}

namespace {
// Suggested markers stay at least 30 ms from each other and from markers.
[[nodiscard]] std::uint64_t suggestion_gap(const project::Snapshot& snapshot,
                                           const project::Clip& clip) {
  const auto source = std::ranges::find(snapshot.sources, clip.source, &project::SourceRecord::id);
  return source == snapshot.sources.end()
             ? std::uint64_t{1}
             : std::max<std::uint64_t>(1, source->sample_rate * 3 / 100);
}
} // namespace

std::vector<std::uint64_t> ClipWarpController::visible_suggestions(const Selected& current) const {
  return application::clip_onset_suggestions(current.snapshot, current.clip.id,
                                             suggestion_gap(current.snapshot, current.clip));
}

void ClipWarpController::request_suggestions() {
  const auto current = selected();
  if (!current || !root_) {
    report_(QStringLiteral("Select an audio clip to analyse"));
    return;
  }
  const auto source = std::ranges::find(current->snapshot.sources, current->clip.source,
                                        &project::SourceRecord::id);
  if (source == current->snapshot.sources.end()) {
    report_(QStringLiteral("The clip's source is missing"));
    return;
  }
  cancel_suggestions();
  const auto serial = suggestion_serial_;
  suggestion_state_ = QStringLiteral("running");
  suggestion_message_ = QStringLiteral("Analysing transients across the source…");
  pending_ = PendingAnalysis{*source, current->snapshot.revision};
  try {
    if (!onset_worker_)
      onset_worker_ = std::make_unique<WarpOnsetWorker>(onset_analyzer_);
    onset_worker_->submit(root_(), current->snapshot, source->id, serial);
    suggestion_poll_timer_.start();
  } catch (const std::exception& error) {
    suggestion_state_ = QStringLiteral("failed");
    suggestion_message_ = QStringLiteral("Could not start analysis: %1").arg(error.what());
  }
  rebuild();
}

void ClipWarpController::deliver_suggestions(std::uint64_t serial,
                                             foundation::Result<application::SourceOnsets> found) {
  if (serial != suggestion_serial_)
    return; // a newer request or a selection change superseded this result
  const auto requested = std::exchange(pending_, std::nullopt);
  auto* project = session_ ? session_() : nullptr;
  if (!requested || !project) {
    suggestion_state_ = QStringLiteral("stale");
    suggestion_message_ = QStringLiteral("The project closed during analysis");
  } else if (!found) {
    suggestion_state_ = QStringLiteral("failed");
    suggestion_message_ = QString::fromStdString(found.error().message);
  } else if (project->snapshot().revision != requested->revision || !can_edit_ || !can_edit_()) {
    suggestion_state_ = QStringLiteral("stale");
    suggestion_message_ = QStringLiteral("The project changed during analysis; analyse again");
  } else {
    auto stored = project->source_analysis().accept_source_onset_analysis(
        requested->source, requested->revision, std::move(found.value().frames),
        found.value().truncated);
    if (!stored) {
      suggestion_state_ = QStringLiteral("failed");
      suggestion_message_ = QString::fromStdString(stored.error().message);
    } else {
      suggestion_state_ = QStringLiteral("ready");
      suggestion_message_ = QStringLiteral("Transient suggestions stored in the project. Review "
                                           "them, then Apply (adds markers without moving "
                                           "audio) or Dismiss.");
      commit_();
    }
  }
  rebuild();
}

void ClipWarpController::apply_suggestions() {
  const auto current = selected();
  if (!current)
    return;
  const auto frames = visible_suggestions(*current);
  if (frames.empty()) {
    report_(QStringLiteral("No transient suggestions to apply; analyse the source first"));
    return;
  }
  auto edited =
      application::apply_warp_suggestions(current->snapshot, current->clip.id, frames,
                                          suggestion_gap(current->snapshot, current->clip));
  edit(QStringLiteral("Applied %1 reviewed warp suggestions").arg(frames.size()),
       std::move(edited));
  rebuild();
}

void ClipWarpController::dismiss_suggestion(int index) {
  const auto current = selected();
  if (!current || index < 0)
    return;
  const auto frames = visible_suggestions(*current);
  if (static_cast<std::size_t>(index) >= frames.size())
    return;
  auto* project = editable_session();
  if (!project)
    return;
  const auto review = std::ranges::find(current->snapshot.source_tempo_reviews,
                                        current->clip.source, &project::SourceTempoReview::source);
  std::vector<std::uint64_t> dismissed;
  if (review != current->snapshot.source_tempo_reviews.end())
    dismissed = review->dismissed_onset_frames;
  dismissed.insert(std::ranges::upper_bound(dismissed, frames[static_cast<std::size_t>(index)]),
                   frames[static_cast<std::size_t>(index)]);
  const auto applied =
      project->source_analysis().set_source_dismissed_onsets(current->clip.source, dismissed);
  if (!applied) {
    report_(QStringLiteral("Warp: %1").arg(QString::fromStdString(applied.error().message)));
    return;
  }
  commit_();
  report_(QStringLiteral("Transient suggestion dismissed · revision %1")
              .arg(project->snapshot().revision));
  rebuild();
}

void ClipWarpController::dismiss_suggestions() {
  const auto current = selected();
  if (!current)
    return;
  const auto frames = visible_suggestions(*current);
  if (frames.empty())
    return;
  auto* project = editable_session();
  if (!project)
    return;
  const auto review = std::ranges::find(current->snapshot.source_tempo_reviews,
                                        current->clip.source, &project::SourceTempoReview::source);
  std::vector<std::uint64_t> dismissed;
  if (review != current->snapshot.source_tempo_reviews.end())
    dismissed = review->dismissed_onset_frames;
  dismissed.insert(dismissed.end(), frames.begin(), frames.end());
  std::ranges::sort(dismissed);
  dismissed.erase(std::ranges::unique(dismissed).begin(), dismissed.end());
  const auto applied =
      project->source_analysis().set_source_dismissed_onsets(current->clip.source, dismissed);
  if (!applied) {
    report_(QStringLiteral("Warp: %1").arg(QString::fromStdString(applied.error().message)));
    return;
  }
  commit_();
  report_(QStringLiteral("%1 transient suggestions dismissed · revision %2")
              .arg(frames.size())
              .arg(project->snapshot().revision));
  rebuild();
}

// Stores the tempo panel's verified analysis for the clip's source when the
// project has no usable review yet, as its own undoable revision.
bool ClipWarpController::ensure_tempo_review(const Selected& current, bool need_beats) {
  const auto review = std::ranges::find(current.snapshot.source_tempo_reviews, current.clip.source,
                                        &project::SourceTempoReview::source);
  if (review != current.snapshot.source_tempo_reviews.end() &&
      (need_beats ? !review->generated_beat_frames.empty() || !review->user_beat_frames.empty()
                  : review->corrected_bpm.value_or(review->detected_bpm) > 0.0))
    return true;
  const auto source =
      std::ranges::find(current.snapshot.sources, current.clip.source, &project::SourceRecord::id);
  const auto estimate =
      tempo_ && source != current.snapshot.sources.end() ? tempo_(*source) : std::nullopt;
  if (!estimate) {
    report_(QStringLiteral("No tempo analysis for this source yet; wait for the tempo panel"));
    return false;
  }
  auto* project = editable_session();
  if (!project)
    return false;
  const auto stored = project->source_analysis().accept_source_tempo_analysis(
      source->id, current.snapshot.revision, *estimate);
  if (!stored) {
    report_(QStringLiteral("Warp: %1").arg(QString::fromStdString(stored.error().message)));
    return false;
  }
  commit_();
  return true;
}

void ClipWarpController::propose(const QString& kind) {
  auto current = selected();
  if (!current)
    return;
  const bool auto_warp = kind == QStringLiteral("auto-warp");
  // Refuse before storing anything: both follow the beat grid.
  if (!std::holds_alternative<project::MusicalInterval>(current->clip.placement)) {
    proposal_.reset();
    report_(QStringLiteral("Warp: switch the clip to musical time to %1")
                .arg(auto_warp ? QStringLiteral("auto-warp it")
                               : QStringLiteral("conform it to the project tempo")));
    rebuild();
    return;
  }
  if (!ensure_tempo_review(*current, auto_warp))
    return;
  current = selected();
  if (!current)
    return;
  auto proposal = auto_warp
                      ? application::propose_auto_warp(current->snapshot, current->clip.id)
                      : application::propose_tempo_conform(current->snapshot, current->clip.id);
  if (!proposal) {
    proposal_.reset();
    report_(QStringLiteral("Warp: %1").arg(QString::fromStdString(proposal.error().message)));
    rebuild();
    return;
  }
  proposal_ = Proposal{kind, clip_id_, current->snapshot.revision, std::move(proposal).value()};
  report_(auto_warp ? QStringLiteral("Auto-warp proposal ready: review it, then Apply")
                    : QStringLiteral("Tempo conform proposal ready: review it, then Apply"));
  rebuild();
}

void ClipWarpController::propose_auto_warp() { propose(QStringLiteral("auto-warp")); }

void ClipWarpController::propose_tempo_conform() { propose(QStringLiteral("conform")); }

void ClipWarpController::apply_proposal() {
  const auto current = selected();
  if (!proposal_ || !current || proposal_->clip != clip_id_ ||
      proposal_->revision != current->snapshot.revision) {
    proposal_.reset();
    report_(QStringLiteral("The warp proposal is out of date; propose again"));
    rebuild();
    return;
  }
  auto* project = editable_session();
  if (!project)
    return;
  auto proposal = std::exchange(proposal_, std::nullopt);
  const auto applied =
      project->warp().apply_audio_clip_warp(current->clip.id, proposal->value.settings_generation,
                                            proposal->value.placement, proposal->value.processing);
  if (!applied) {
    report_(QStringLiteral("Warp: %1").arg(QString::fromStdString(applied.error().message)));
    rebuild();
    return;
  }
  commit_();
  report_(QStringLiteral("%1 applied · revision %2")
              .arg(proposal->kind == QStringLiteral("auto-warp") ? QStringLiteral("Auto-warp")
                                                                 : QStringLiteral("Tempo conform"))
              .arg(project->snapshot().revision));
  rebuild();
}

void ClipWarpController::discard_proposal() {
  proposal_.reset();
  rebuild();
}

} // namespace oscillate::quick
