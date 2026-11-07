#include "clip_tempo_controller.hpp"

#include "oscillate/application/clip_warp_edit.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/processing/warp_render_cache.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <span>
#include <utility>
#include <variant>

namespace oscillate::quick {
namespace {

struct ModeEntry {
  project::WarpMode mode;
  const char* name;
  const char* hint;
};
// The order the strip shows the modes in, not the engine's.
constexpr std::array<ModeEntry, 6> mode_entries{{
    {project::WarpMode::complex, "Complex", "Full mixes and finished tracks"},
    {project::WarpMode::complex_pro, "Complex Pro", "Vocals up front; keeps formants"},
    {project::WarpMode::beats, "Beats", "Drum loops and tools; slices at transients"},
    {project::WarpMode::repitch, "Repitch", "Like a turntable: tempo and pitch together"},
    {project::WarpMode::tones, "Tones", "Single melodic lines"},
    {project::WarpMode::texture, "Texture", "Pads, noise and ambience"},
}};

constexpr qint64 tap_reset_ms = 2'000;
constexpr int poll_interval_ms = 100;
constexpr int render_poll_ms = 250;

[[nodiscard]] const project::Clip* find_clip(const project::Snapshot& snapshot, const QString& id) {
  const auto parsed = project::ClipId::parse(id.toStdString());
  if (!parsed)
    return nullptr;
  for (const auto& track : snapshot.tracks) {
    const auto found = std::ranges::find(track.clips, parsed.value(), &project::Clip::id);
    if (found != track.clips.end())
      return &*found;
  }
  return nullptr;
}

[[nodiscard]] const project::SourceTempoReview* review_of(const project::Snapshot& snapshot,
                                                          project::SourceId source) {
  const auto found =
      std::ranges::find(snapshot.source_tempo_reviews, source, &project::SourceTempoReview::source);
  return found == snapshot.source_tempo_reviews.end() ? nullptr : &*found;
}

[[nodiscard]] bool analysed(const project::Snapshot& snapshot, project::SourceId source) {
  const auto* review = review_of(snapshot, source);
  return review != nullptr && (review->detected_bpm > 0.0 || review->corrected_bpm);
}

} // namespace

ClipTempoController::ClipTempoController(Session session, Root root, CanEdit can_edit,
                                         Report report, Commit commit,
                                         TempoWorker::Analyzer analyzer, QObject* parent,
                                         Waveform waveform)
    : QObject(parent), session_(std::move(session)), root_(std::move(root)),
      can_edit_(std::move(can_edit)), report_(std::move(report)), commit_(std::move(commit)),
      worker_(std::move(analyzer)), waveform_(std::move(waveform)) {
  poll_timer_.setInterval(poll_interval_ms);
  connect(&poll_timer_, &QTimer::timeout, this, &ClipTempoController::poll);
  render_timer_.setInterval(render_poll_ms);
  connect(&render_timer_, &QTimer::timeout, this, [this] {
    render_clips();
    rebuild();
  });
}

QVariantList ClipTempoController::modes() const {
  QVariantList modes;
  for (const auto& entry : mode_entries)
    modes.push_back(QVariantMap{{QStringLiteral("name"), QString::fromUtf8(entry.name)},
                                {QStringLiteral("hint"), QString::fromUtf8(entry.hint)}});
  return modes;
}

application::ProjectSession* ClipTempoController::editable_session() {
  auto* session = session_ ? session_() : nullptr;
  if (!session)
    return nullptr;
  if (can_edit_ && !can_edit_()) {
    report_(QStringLiteral("Wait for the current operation to finish"));
    return nullptr;
  }
  return session;
}

void ClipTempoController::failure(const foundation::Error& error) {
  report_(QString::fromStdString(error.message));
}

std::optional<analysis::TempoEstimate>
ClipTempoController::estimate_for(const project::SourceRecord& source) const {
  const auto found = estimates_.find(source.id);
  if (found == estimates_.end() || found->second.frames != source.frames ||
      found->second.sample_rate != source.sample_rate)
    return std::nullopt;
  return found->second;
}

void ClipTempoController::refresh() {
  const auto* session = session_ ? session_() : nullptr;
  if (session != seen_session_) {
    // Another project: nothing it holds was dropped in this session.
    seen_session_ = session;
    attempted_.clear();
    pending_.clear();
    estimates_.clear();
    cache_hits_.clear();
    fresh_.clear();
    taps_.clear();
    in_flight_.reset();
    held_.reset();
    ++generation_;
    worker_.submit({}, std::nullopt, generation_);
    poll_timer_.stop();
    render_timer_.stop();
    if (analysis_job_)
      analysis_job_->thread.request_stop();
    analysis_job_.reset();
    analysis_queue_.clear();
    analysed_keys_.clear();
    analysis_cache_.reset();
    if (session) {
      if (auto store = persistence::ProjectStore::open_reader(root_()))
        if (auto directory = store.value().open_cache_directory())
          if (auto cache = media::CacheStore::adopt_directory_fd(directory.value()))
            analysis_cache_.emplace(std::move(cache).value());
    }
  }
  if (session) {
    const auto& snapshot = session->snapshot();
    for (const auto& source : snapshot.sources) {
      // Only audio clips' sources: a synth's media has no clip strip.
      const bool on_a_clip = std::ranges::any_of(snapshot.tracks, [&](const auto& track) {
        return std::ranges::any_of(track.clips,
                                   [&](const auto& clip) { return clip.source == source.id; });
      });
      if (on_a_clip && !analysed(snapshot, source.id) && !attempted_.contains(source.id) &&
          !estimates_.contains(source.id) &&
          std::ranges::find(pending_, source.id) == pending_.end() &&
          !(in_flight_ && in_flight_->id == source.id))
        pending_.push_back(source.id);
    }
  }
  start_next();
  render_clips();
  rebuild();
}

std::optional<std::string> ClipTempoController::analysis_key(const project::Snapshot& snapshot,
                                                             project::ClipId clip) const {
  auto request = processing::processed_clip_request(snapshot, clip);
  if (!request || !request.value())
    return std::nullopt;
  auto key = processing::warp_analysis_cache_key(*request.value());
  if (!key || !key.value())
    return std::nullopt;
  return key.value()->hex();
}

void ClipTempoController::render_clips() {
  const auto* session = session_ ? session_() : nullptr;
  if (!session || !analysis_cache_)
    return;
  const auto& snapshot = session->snapshot();
  // A finished job's key is in the cache now.
  if (analysis_job_ && analysis_job_->done->load(std::memory_order_acquire)) {
    analysed_keys_.insert(analysis_job_->key);
    analysis_job_.reset();
  }
  for (const auto& track : snapshot.tracks) {
    for (const auto& clip : track.clips) {
      auto request = processing::processed_clip_request(snapshot, clip.id);
      if (!request || !request.value())
        continue;
      auto key = processing::warp_analysis_cache_key(*request.value());
      if (!key || !key.value())
        continue;
      const auto hex = key.value()->hex();
      if (analysed_keys_.contains(hex) || (analysis_job_ && analysis_job_->key == hex) ||
          std::ranges::any_of(analysis_queue_, [&](const auto& item) { return item.first == hex; }))
        continue;
      // Analysed in an earlier session: the cache already holds it.
      if (processing::warp_analysis_ready(*analysis_cache_, *request.value())) {
        analysed_keys_.insert(hex);
        continue;
      }
      analysis_queue_.emplace_back(hex, *request.value());
    }
  }
  if (!analysis_job_ && !analysis_queue_.empty()) {
    auto [key, request] = std::move(analysis_queue_.front());
    analysis_queue_.pop_front();
    auto progress = std::make_shared<std::atomic<double>>(0.0);
    auto done = std::make_shared<std::atomic<bool>>(false);
    analysis_job_.emplace(AnalysisJob{key, progress, done,
                                      std::jthread([root = root_(), request = std::move(request),
                                                    progress, done](const std::stop_token& stop) {
                                        (void)processing::prepare_warp_analysis(
                                            root, request, progress.get(), stop);
                                        done->store(true, std::memory_order_release);
                                      })});
  }
  if (analysis_job_ || !analysis_queue_.empty())
    render_timer_.start();
  else
    render_timer_.stop();
}

void ClipTempoController::start_next() {
  if (in_flight_)
    return;
  const auto* session = session_ ? session_() : nullptr;
  while (session && !pending_.empty()) {
    const auto id = pending_.front();
    pending_.pop_front();
    const auto& sources = session->snapshot().sources;
    const auto source = std::ranges::find(sources, id, &project::SourceRecord::id);
    if (source == sources.end() || (analysed(session->snapshot(), id) && !wanted_.contains(id)) ||
        estimates_.contains(id))
      continue;
    in_flight_ = *source;
    worker_.submit(root_(), *source, ++generation_);
    poll_timer_.start();
    return;
  }
}

void ClipTempoController::poll() {
  if (!held_) {
    auto outcome = worker_.collect();
    if (!outcome)
      return;
    if (!in_flight_ || outcome->generation != generation_ || outcome->source != *in_flight_)
      return;
    held_ = std::move(outcome->result);
  }
  // Applying needs an idle session; keep the result until it is.
  if (held_->has_value() && can_edit_ && !can_edit_())
    return;
  const auto source = *in_flight_;
  if (held_->has_value()) {
    if (held_->value().cache_hit)
      cache_hits_.insert(source.id);
    else
      cache_hits_.erase(source.id);
    accept(source, held_->value().estimate);
  }
  held_.reset();
  attempted_.insert(source.id);
  in_flight_.reset();
  poll_timer_.stop();
  start_next();
  rebuild();
}

void ClipTempoController::accept(const project::SourceRecord& source,
                                 const analysis::TempoEstimate& estimate) {
  auto* session = session_ ? session_() : nullptr;
  if (!session)
    return;
  const auto& sources = session->snapshot().sources;
  const auto current = std::ranges::find(sources, source.id, &project::SourceRecord::id);
  // The estimate is for this immutable source version, whatever else has
  // changed since it was queued.
  if (current == sources.end() || *current != source)
    return;
  estimates_.insert_or_assign(source.id, estimate);
  set_up(source.id);
}

void ClipTempoController::dropped(project::ClipId clip) {
  fresh_.insert(clip);
  const auto* session = session_ ? session_() : nullptr;
  if (!session)
    return;
  for (const auto& track : session->snapshot().tracks)
    for (const auto& item : track.clips)
      if (item.id == clip)
        return set_up(item.source);
}

void ClipTempoController::set_up(project::SourceId source) {
  auto* session = session_ ? session_() : nullptr;
  if (!session || !grid_of(session->snapshot(), source))
    return;
  std::vector<project::ClipId> setup;
  for (const auto& track : session->snapshot().tracks)
    for (const auto& clip : track.clips)
      if (clip.source == source && fresh_.erase(clip.id) > 0 &&
          std::holds_alternative<project::ElapsedInterval>(clip.placement))
        setup.push_back(clip.id);
  if (setup.empty() || !store(*session, source))
    return;
  // The first track in a project sets its tempo: with no other audio and no
  // tempo changes, the project plays at this track's own BPM.
  const auto& snapshot = session->snapshot();
  const bool alone = std::ranges::all_of(snapshot.tracks, [&](const auto& track) {
    return std::ranges::all_of(track.clips, [&](const auto& clip) {
      return std::ranges::find(setup, clip.id) != setup.end();
    });
  });
  if (alone && snapshot.tempo.events().size() == 1) {
    const auto grid = grid_of(snapshot, source);
    const auto bpm = std::round(grid->bpm * 100.0) / 100.0;
    if (bpm != snapshot.tempo.events().front().bpm)
      if (auto tempo = session->song().set_tempo_event(
              {0}, bpm, snapshot.tempo.events().front().shape_to_next);
          !tempo)
        failure(tempo.error());
  }
  for (const auto clip : setup)
    if (auto followed = session->warp().set_audio_clip_follow(clip, true); !followed)
      failure(followed.error());
  commit_();
}

bool ClipTempoController::store(application::ProjectSession& session, project::SourceId source) {
  if (analysed(session.snapshot(), source))
    return true;
  const auto estimate = estimates_.find(source);
  if (estimate == estimates_.end())
    return false;
  auto stored = session.source_analysis().accept_source_tempo_analysis(
      source, session.snapshot().revision, estimate->second);
  if (!stored) {
    failure(stored.error());
    return false;
  }
  return true;
}

std::optional<application::SourceBeatGrid>
ClipTempoController::grid_of(const project::Snapshot& snapshot, project::SourceId source) const {
  if (auto grid = application::source_beat_grid(snapshot, source))
    return grid;
  const auto estimate = estimates_.find(source);
  if (estimate == estimates_.end() || estimate->second.bpm <= 0.0)
    return std::nullopt;
  const auto& found = estimate->second;
  return application::SourceBeatGrid{
      found.bpm, found.sample_rate,
      found.suggested_beat_frames.empty() ? 0 : found.suggested_beat_frames.front()};
}

void ClipTempoController::rebuild() {
  QVariantMap clips;
  const auto* session = session_ ? session_() : nullptr;
  if (session) {
    const auto& snapshot = session->snapshot();
    for (const auto& track : snapshot.tracks) {
      for (const auto& clip : track.clips) {
        const auto source =
            std::ranges::find(snapshot.sources, clip.source, &project::SourceRecord::id);
        if (source == snapshot.sources.end() || source->sample_rate == 0)
          continue;
        QVariantMap entry;
        const auto* review = review_of(snapshot, clip.source);
        const auto grid = grid_of(snapshot, clip.source);
        const auto waiting_estimate = estimates_.find(clip.source);
        const auto* estimate =
            !review && waiting_estimate != estimates_.end() ? &waiting_estimate->second : nullptr;
        const bool waiting = !grid && ((in_flight_ && in_flight_->id == clip.source) ||
                                       std::ranges::find(pending_, clip.source) != pending_.end());
        const auto frames =
            static_cast<double>(clip.source_range.end.value - clip.source_range.start.value);
        entry.insert(QStringLiteral("analysing"), waiting);
        entry.insert(QStringLiteral("cacheHit"), cache_hits_.contains(clip.source));
        entry.insert(QStringLiteral("sourceKey"),
                     waiting_estimate != estimates_.end()
                         ? QString::fromStdString(waiting_estimate->second.identity.source_key)
                         : QString());
        entry.insert(QStringLiteral("bpm"), grid ? grid->bpm : 0.0);
        entry.insert(QStringLiteral("detected"), review     ? review->detected_bpm
                                                 : estimate ? estimate->bpm
                                                            : 0.0);
        entry.insert(QStringLiteral("corrected"), review && review->corrected_bpm.has_value());
        entry.insert(QStringLiteral("confidence"), review     ? review->confidence
                                                   : estimate ? estimate->confidence
                                                              : 0.0);
        entry.insert(QStringLiteral("follow"),
                     std::holds_alternative<project::MusicalInterval>(clip.placement));
        entry.insert(QStringLiteral("seconds"), frames / source->sample_rate);
        const auto mode =
            std::ranges::find(mode_entries, clip.processing.warp.mode, &ModeEntry::mode);
        entry.insert(QStringLiteral("modeIndex"),
                     static_cast<int>(std::distance(mode_entries.begin(), mode)));
        entry.insert(QStringLiteral("mode"), QString::fromUtf8(mode->name));
        double downbeat = -1.0;
        double beat = 0.0;
        double stretch = 0.0;
        if (grid && frames > 0.0) {
          // The first bar line at or after the clip's start.
          const auto bar = 4.0 * grid->period();
          const auto start = static_cast<double>(clip.source_range.start.value);
          const auto first = static_cast<double>(grid->downbeat) +
                             std::ceil((start - static_cast<double>(grid->downbeat)) / bar) * bar;
          if (auto at = application::fraction_of_source_frame(
                  snapshot, clip.id, static_cast<std::uint64_t>(std::llround(first))))
            downbeat = at.value();
          beat = grid->period() / frames;
          if (std::holds_alternative<project::MusicalInterval>(clip.placement)) {
            const auto start_tick = std::get<project::MusicalInterval>(clip.placement).start;
            double project_bpm = 0.0;
            if (auto sample = snapshot.tempo.tick_to_sample(start_tick, snapshot.sample_rate);
                sample &&
                snapshot.tempo.bpm_at_sample_rt(sample.value(), snapshot.sample_rate,
                                                project_bpm) &&
                project_bpm > 0.0)
              stretch = (project_bpm / grid->bpm - 1.0) * 100.0;
          }
        }
        // A stretched clip renders in the background before it plays cleanly.
        double render = 1.0;
        bool rendering = false;
        if (const auto key = analysis_key(snapshot, clip.id);
            key && !analysed_keys_.contains(*key)) {
          rendering = true;
          render = analysis_job_ && analysis_job_->key == *key
                       ? analysis_job_->progress->load(std::memory_order_relaxed)
                       : 0.0;
        }
        entry.insert(QStringLiteral("rendering"), rendering);
        entry.insert(QStringLiteral("renderProgress"), render);
        entry.insert(QStringLiteral("downbeat"), downbeat);
        entry.insert(QStringLiteral("beat"), beat);
        entry.insert(QStringLiteral("stretch"), stretch);
        clips.insert(QString::fromStdString(clip.id.to_string()), entry);
      }
    }
  }
  if (clips == clips_)
    return;
  clips_ = std::move(clips);
  emit changed();
}

void ClipTempoController::set_follow(const QString& clip_id, bool follow) {
  auto* session = editable_session();
  if (!session)
    return;
  const auto* clip = find_clip(session->snapshot(), clip_id);
  if (!clip)
    return report_(QStringLiteral("That clip no longer exists"));
  if (!store(*session, clip->source))
    return report_(QStringLiteral("This clip's tempo is still being analysed"));
  if (auto applied = session->warp().set_audio_clip_follow(clip->id, follow); !applied)
    return failure(applied.error());
  report_(follow ? QStringLiteral("Clip follows the project tempo")
                 : QStringLiteral("Clip plays at its own tempo"));
  commit_();
}

void ClipTempoController::set_bpm(const QString& clip_id, double bpm) {
  auto* session = editable_session();
  if (!session)
    return;
  const auto* clip = find_clip(session->snapshot(), clip_id);
  if (!clip)
    return report_(QStringLiteral("That clip no longer exists"));
  (void)store(*session, clip->source);
  if (auto applied = session->warp().set_audio_clip_source_bpm(clip->id, bpm); !applied)
    return failure(applied.error());
  report_(QStringLiteral("Track tempo set to %1 BPM").arg(bpm, 0, 'f', 2));
  commit_();
}

void ClipTempoController::reset_bpm(const QString& clip_id) {
  auto* session = editable_session();
  if (!session)
    return;
  const auto* clip = find_clip(session->snapshot(), clip_id);
  if (!clip)
    return report_(QStringLiteral("That clip no longer exists"));
  (void)store(*session, clip->source);
  if (auto applied = session->warp().set_audio_clip_source_bpm(clip->id, std::nullopt); !applied)
    return failure(applied.error());
  report_(QStringLiteral("Track tempo back to the detected tempo"));
  commit_();
}

void ClipTempoController::scale_bpm(const QString& clip_id, double factor) {
  const auto* session = session_ ? session_() : nullptr;
  if (!session)
    return;
  const auto* clip = find_clip(session->snapshot(), clip_id);
  if (!clip)
    return;
  const auto grid = grid_of(session->snapshot(), clip->source);
  if (!grid)
    return report_(QStringLiteral("This clip has no tempo yet"));
  set_bpm(clip_id, grid->bpm * factor);
}

double ClipTempoController::tap(const QString& clip_id) {
  auto& taps = taps_[clip_id];
  if (!taps.since.isValid())
    taps.since.start();
  const auto now = taps.since.elapsed();
  if (!taps.times.empty() && now - taps.times.back() > tap_reset_ms)
    taps.times.clear();
  taps.times.push_back(now);
  if (taps.times.size() < 4)
    return 0.0;
  const auto span = static_cast<double>(taps.times.back() - taps.times.front());
  const auto bpm = 60'000.0 * static_cast<double>(taps.times.size() - 1) / span;
  // Two decimals, as the strip shows it.
  const auto rounded = std::round(bpm * 100.0) / 100.0;
  set_bpm(clip_id, rounded);
  return rounded;
}

void ClipTempoController::set_mode(const QString& clip_id, int index) {
  if (index < 0 || index >= static_cast<int>(mode_entries.size()))
    return;
  auto* session = editable_session();
  if (!session)
    return;
  const auto* clip = find_clip(session->snapshot(), clip_id);
  if (!clip)
    return report_(QStringLiteral("That clip no longer exists"));
  auto processing = clip->processing;
  processing.warp.mode = mode_entries[static_cast<std::size_t>(index)].mode;
  processing.warp.mode_overridden = true;
  if (processing == clip->processing)
    return;
  if (auto applied = session->audio_clips().set_audio_clip_processing(clip->id, processing);
      !applied)
    return failure(applied.error());
  report_(QStringLiteral("Stretch mode: %1")
              .arg(QString::fromUtf8(mode_entries[static_cast<std::size_t>(index)].name)));
  commit_();
}

void ClipTempoController::set_downbeat(const QString& clip_id, double fraction, bool free) {
  auto* session = editable_session();
  if (!session)
    return;
  const auto* found = find_clip(session->snapshot(), clip_id);
  if (!found)
    return report_(QStringLiteral("That clip no longer exists"));
  (void)store(*session, found->source);
  const auto& snapshot = session->snapshot();
  const auto* clip = find_clip(snapshot, clip_id);
  auto frame = application::source_frame_at_fraction(snapshot, clip->id, fraction);
  if (!frame)
    return failure(frame.error());
  auto chosen = frame.value();
  if (!free) {
    if (const auto grid = application::source_beat_grid(snapshot, clip->source)) {
      const auto period = grid->period();
      const auto beats =
          std::round((static_cast<double>(chosen) - static_cast<double>(grid->downbeat)) / period);
      const auto snapped = static_cast<double>(grid->downbeat) + beats * period;
      if (snapped >= 0.0)
        chosen = static_cast<std::uint64_t>(std::llround(snapped));
    }
  }
  if (auto applied = session->warp().set_audio_clip_downbeat(clip->id, chosen); !applied)
    return failure(applied.error());
  report_(QStringLiteral("Downbeat set"));
  commit_();
}

void ClipTempoController::reanalyse(const QString& clip_id) {
  const auto* session = session_ ? session_() : nullptr;
  if (!session)
    return;
  const auto* clip = find_clip(session->snapshot(), clip_id);
  if (!clip)
    return;
  attempted_.erase(clip->source);
  const auto& sources = session->snapshot().sources;
  const auto source = std::ranges::find(sources, clip->source, &project::SourceRecord::id);
  if (source == sources.end())
    return;
  // A stored review is kept until the new result replaces it.
  if (in_flight_) {
    if (in_flight_->id != source->id && std::ranges::find(pending_, source->id) == pending_.end())
      pending_.push_front(source->id);
  } else {
    in_flight_ = *source;
    worker_.submit(root_(), *source, ++generation_);
    poll_timer_.start();
  }
  report_(QStringLiteral("Analysing the track again"));
  rebuild();
}

namespace {

// The clip's output (as it plays, project-rate frames from its start) and
// its source (absolute source-rate frames), piecewise linear between its
// warp markers.
struct OutputMap {
  std::vector<std::pair<double, double>> anchors; // output, source
  double output_frames = 0.0;

  [[nodiscard]] static std::optional<OutputMap> open(const project::Snapshot& snapshot,
                                                     project::ClipId clip) {
    auto layout = application::clip_warp_layout(snapshot, clip);
    if (!layout || layout.value().output_frames == 0)
      return std::nullopt;
    const auto& value = layout.value();
    OutputMap map;
    map.output_frames = static_cast<double>(value.output_frames);
    map.anchors.emplace_back(0.0, static_cast<double>(value.source_first));
    for (const auto& marker : value.markers)
      map.anchors.emplace_back(static_cast<double>(marker.output_frame),
                               static_cast<double>(marker.source_frame));
    map.anchors.emplace_back(map.output_frames,
                             static_cast<double>(value.source_first + value.source_frames));
    return map;
  }
  [[nodiscard]] static double between(const std::pair<double, double>& a,
                                      const std::pair<double, double>& b, double at, bool forward) {
    const auto [from, to] = forward ? std::pair{a.first, b.first} : std::pair{a.second, b.second};
    const auto [into_from, into_to] =
        forward ? std::pair{a.second, b.second} : std::pair{a.first, b.first};
    return to == from ? into_from : into_from + (at - from) * (into_to - into_from) / (to - from);
  }
  [[nodiscard]] double source_at(double output) const {
    for (std::size_t index = 1; index < anchors.size(); ++index)
      if (output <= anchors[index].first || index + 1 == anchors.size())
        return between(anchors[index - 1], anchors[index], output, true);
    return anchors.back().second;
  }
  [[nodiscard]] double output_at(double source) const {
    for (std::size_t index = 1; index < anchors.size(); ++index)
      if (source <= anchors[index].second || index + 1 == anchors.size())
        return between(anchors[index - 1], anchors[index], source, false);
    return anchors.back().first;
  }
};

constexpr double on_grid_ms = 10.0;

// A transient near one of the grid's beats: which beat, and how far off.
struct Hit {
  std::int64_t beat;
  double offset; // source frames, positive when late
  double frame;
};

[[nodiscard]] std::vector<Hit> hits(const application::SourceBeatGrid& grid,
                                    std::span<const std::uint64_t> onsets, double first,
                                    double last) {
  std::vector<Hit> found;
  const auto period = grid.period();
  const auto anchor = static_cast<double>(grid.downbeat);
  for (const auto onset : onsets) {
    const auto frame = static_cast<double>(onset);
    if (frame < first || frame >= last)
      continue;
    const auto beat = static_cast<std::int64_t>(std::llround((frame - anchor) / period));
    const auto offset = frame - (anchor + static_cast<double>(beat) * period);
    if (std::abs(offset) <= 0.25 * period)
      found.push_back({beat, offset, frame});
  }
  return found;
}

[[nodiscard]] double median(std::vector<double> values) {
  if (values.empty())
    return 0.0;
  const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
  std::ranges::nth_element(values, middle);
  return *middle;
}

} // namespace

QVariantList ClipTempoController::detail(const QString& clip_id, double from, double to,
                                         int columns) const {
  const auto* session = session_ ? session_() : nullptr;
  if (!session || !waveform_ || columns <= 0 || columns > 4'096 || !(to > from))
    return {};
  const auto& snapshot = session->snapshot();
  const auto* clip = find_clip(snapshot, clip_id);
  if (!clip)
    return {};
  const auto map = OutputMap::open(snapshot, clip->id);
  const auto source = std::ranges::find(snapshot.sources, clip->source, &project::SourceRecord::id);
  if (!map || source == snapshot.sources.end())
    return {};
  const auto total = static_cast<double>(source->frames);
  const auto range = [&](double begin, double end) {
    const auto a = std::clamp(map->source_at(begin * map->output_frames), 0.0, total);
    const auto b = std::clamp(map->source_at(end * map->output_frames), 0.0, total);
    return project::SourceRange{{static_cast<std::int64_t>(std::floor(a))},
                                {std::max(static_cast<std::int64_t>(std::floor(a)) + 1,
                                          static_cast<std::int64_t>(std::ceil(b)))}};
  };
  from = std::clamp(from, 0.0, 1.0);
  to = std::clamp(to, from, 1.0);
  // Without markers the clip plays its source linearly: one request.
  if (map->anchors.size() == 2)
    return waveform_(clip->source, range(from, to), static_cast<std::size_t>(columns));
  QVariantList result;
  for (int column = 0; column < columns; ++column) {
    const auto begin = from + (to - from) * column / columns;
    const auto end = from + (to - from) * (column + 1) / columns;
    const auto one = waveform_(clip->source, range(begin, end), 1);
    result.push_back(one.isEmpty() ? QVariant(QVariantMap{{"min", 0.0}, {"max", 0.0}})
                                   : one.front());
  }
  return result;
}

QVariantMap ClipTempoController::inspect(const QString& clip_id) {
  const auto* session = session_ ? session_() : nullptr;
  if (!session)
    return {};
  const auto& snapshot = session->snapshot();
  const auto* clip = find_clip(snapshot, clip_id);
  if (!clip)
    return {};
  const auto grid = grid_of(snapshot, clip->source);
  if (!grid)
    return {{"pending", true}};
  std::span<const std::uint64_t> onsets;
  if (const auto estimate = estimates_.find(clip->source); estimate != estimates_.end()) {
    onsets = estimate->second.suggested_onset_frames;
  } else if (const auto* review = review_of(snapshot, clip->source);
             review && !review->generated_onset_frames.empty()) {
    onsets = review->generated_onset_frames;
  } else {
    // Reviewed earlier: find its transients again (the cache makes this quick).
    if (wanted_.insert(clip->source).second) {
      attempted_.erase(clip->source);
      if (std::ranges::find(pending_, clip->source) == pending_.end())
        pending_.push_back(clip->source);
      start_next();
    }
    return {{"pending", true}};
  }
  const auto map = OutputMap::open(snapshot, clip->id);
  if (!map)
    return {};
  const auto first = static_cast<double>(clip->source_range.start.value);
  const auto last = static_cast<double>(clip->source_range.end.value);
  const auto found = hits(*grid, onsets, first, last);
  const auto period = grid->period();
  const auto ms = [&](double frames) { return frames * 1'000.0 / grid->sample_rate; };
  // Bars counted from the first downbeat in the clip, as the editor's ruler.
  const auto first_beat = static_cast<std::int64_t>(
      std::ceil((first - static_cast<double>(grid->downbeat)) / (4.0 * period)) * 4.0);
  std::map<std::int64_t, std::vector<double>> bars;
  std::size_t fitting = 0;
  QVariantList transients;
  for (const auto& hit : found) {
    const auto offset = ms(hit.offset);
    fitting += static_cast<std::size_t>(std::abs(offset) <= on_grid_ms);
    bars[(hit.beat - first_beat) / 4 + (hit.beat >= first_beat ? 1 : 0)].push_back(offset);
    if (transients.size() < 6'000)
      transients.push_back(QVariantMap{{"fraction", map->output_at(hit.frame) / map->output_frames},
                                       {"offsetMs", offset},
                                       {"late", std::abs(offset) > on_grid_ms}});
  }
  // Drift: from the first bar whose kicks, and the next three bars', sit
  // more than 10 ms off the grid on the same side.
  std::vector<std::pair<std::int64_t, double>> per_bar;
  for (const auto& [bar, offsets] : bars)
    if (offsets.size() >= 2)
      per_bar.emplace_back(bar, median(offsets));
  std::int64_t drift_bar = -1;
  double drift_ms = 0.0;
  for (std::size_t index = 0; index + 3 < per_bar.size(); ++index) {
    const auto sign = per_bar[index].second > 0.0;
    const bool drifting =
        std::all_of(per_bar.begin() + static_cast<std::ptrdiff_t>(index),
                    per_bar.begin() + static_cast<std::ptrdiff_t>(index + 4), [&](const auto& bar) {
                      return std::abs(bar.second) > on_grid_ms && (bar.second > 0.0) == sign;
                    });
    if (drifting) {
      drift_bar = per_bar[index].first;
      std::vector<double> after;
      for (std::size_t later = index; later < per_bar.size(); ++later)
        after.push_back(per_bar[later].second);
      drift_ms = median(after);
      break;
    }
  }
  const auto fit =
      found.empty() ? 0.0 : static_cast<double>(fitting) / static_cast<double>(found.size());
  return {{"pending", false},
          {"fit", fit},
          {"steady", drift_bar < 0 && fit >= 0.8},
          {"driftBar", static_cast<qlonglong>(drift_bar)},
          {"driftMs", drift_ms},
          {"transients", transients}};
}

QVariantList ClipTempoController::segments(const QString& clip_id) const {
  const auto* session = session_ ? session_() : nullptr;
  if (!session)
    return {};
  const auto& snapshot = session->snapshot();
  const auto* clip = find_clip(snapshot, clip_id);
  if (!clip || !std::holds_alternative<project::MusicalInterval>(clip->placement))
    return {};
  const auto map = OutputMap::open(snapshot, clip->id);
  const auto source = std::ranges::find(snapshot.sources, clip->source, &project::SourceRecord::id);
  if (!map || map->anchors.size() <= 2 || source == snapshot.sources.end())
    return {};
  const auto layout = application::clip_warp_layout(snapshot, clip->id);
  QVariantList result;
  for (std::size_t index = 1; index < map->anchors.size(); ++index) {
    const auto& [output_from, source_from] = map->anchors[index - 1];
    const auto& [output_to, source_to] = map->anchors[index];
    double project_bpm = 0.0;
    const auto middle = static_cast<std::int64_t>(layout.value().project_start) +
                        static_cast<std::int64_t>((output_from + output_to) / 2.0);
    if (!snapshot.tempo.bpm_at_sample_rt({middle}, snapshot.sample_rate, project_bpm) ||
        source_to <= source_from)
      continue;
    // Output at the project rate, source at its own: the source's tempo is
    // the project's scaled by how much shorter it plays.
    const auto seconds_out = (output_to - output_from) / snapshot.sample_rate;
    const auto seconds_in = (source_to - source_from) / source->sample_rate;
    result.push_back(QVariantMap{{"from", output_from / map->output_frames},
                                 {"to", output_to / map->output_frames},
                                 {"bpm", project_bpm * seconds_out / seconds_in}});
  }
  return result;
}

void ClipTempoController::fix_drift(const QString& clip_id, int bar) {
  auto* session = editable_session();
  if (!session)
    return;
  const auto& snapshot = session->snapshot();
  const auto* clip = find_clip(snapshot, clip_id);
  if (!clip)
    return report_(QStringLiteral("That clip no longer exists"));
  const auto* musical = std::get_if<project::MusicalInterval>(&clip->placement);
  if (!musical)
    return report_(QStringLiteral("Turn Follow on to fix drift against the grid"));
  const auto grid = grid_of(snapshot, clip->source);
  const auto estimate = estimates_.find(clip->source);
  const auto* review = review_of(snapshot, clip->source);
  std::span<const std::uint64_t> onsets;
  if (estimate != estimates_.end())
    onsets = estimate->second.suggested_onset_frames;
  else if (review)
    onsets = review->generated_onset_frames;
  if (!grid || onsets.empty())
    return report_(QStringLiteral("The clip's transients are still being found"));
  const auto first = static_cast<double>(clip->source_range.start.value);
  const auto last = static_cast<double>(clip->source_range.end.value);
  const auto period = grid->period();
  const auto first_beat = static_cast<std::int64_t>(
      std::ceil((first - static_cast<double>(grid->downbeat)) / (4.0 * period)) * 4.0);
  const auto from_beat = first_beat + 4 * static_cast<std::int64_t>(std::max(1, bar) - 1);
  // The kicks' own tempo from that bar on: a least-squares line through
  // their beats, matched twice so the second pass follows the drift.
  auto fit_grid = *grid;
  double slope = period;
  double intercept = static_cast<double>(grid->downbeat);
  std::vector<Hit> used;
  for (int pass = 0; pass < 2; ++pass) {
    used.clear();
    for (const auto& hit : hits(fit_grid, onsets, first, last))
      if (hit.beat >= from_beat)
        used.push_back(hit);
    if (used.size() < 8)
      return report_(QStringLiteral("Too few kicks after bar %1 to fit").arg(bar));
    double mean_k = 0.0;
    double mean_f = 0.0;
    for (const auto& hit : used) {
      mean_k += static_cast<double>(hit.beat);
      mean_f += hit.frame;
    }
    mean_k /= static_cast<double>(used.size());
    mean_f /= static_cast<double>(used.size());
    double covariance = 0.0;
    double spread = 0.0;
    for (const auto& hit : used) {
      covariance += (static_cast<double>(hit.beat) - mean_k) * (hit.frame - mean_f);
      spread += (static_cast<double>(hit.beat) - mean_k) * (static_cast<double>(hit.beat) - mean_k);
    }
    if (spread <= 0.0)
      return report_(QStringLiteral("Too few kicks after bar %1 to fit").arg(bar));
    slope = covariance / spread;
    intercept = mean_f - slope * mean_k;
    // Re-anchor the grid on the fitted line for the second pass.
    fit_grid.bpm = 60.0 * grid->sample_rate / slope;
    fit_grid.downbeat = static_cast<std::uint64_t>(
        std::max(0.0, intercept + slope * static_cast<double>(from_beat)));
  }
  const auto last_beat = used.back().beat;
  // Each pinned beat plays at its place on the grid: the downbeat's tick
  // in the clip plus whole quarter notes.
  auto downbeat_tick = application::clip_downbeat_tick(snapshot, clip->id);
  if (!downbeat_tick)
    return failure(downbeat_tick.error());
  const auto clip_downbeat = downbeat_tick.value() - musical->start.value;
  const auto tick_of = [&](std::int64_t beat) {
    return clip_downbeat + (beat - first_beat) * timeline::ticks_per_quarter;
  };
  const auto pinned_from =
      static_cast<std::int64_t>(std::llround(intercept + slope * static_cast<double>(from_beat)));
  const auto pinned_to =
      static_cast<std::int64_t>(std::llround(intercept + slope * static_cast<double>(last_beat)));
  auto processing = clip->processing;
  // Markers after the fix point are replaced.
  std::erase_if(processing.reviewed_warp_markers, [&](const project::ClipWarpMarker& marker) {
    return marker.source_frame.value >= pinned_from - static_cast<std::int64_t>(period / 4.0);
  });
  const auto length = musical->end.value - musical->start.value;
  for (const auto& [frame, beat] : {std::pair{pinned_from, from_beat}, {pinned_to, last_beat}}) {
    const auto tick = tick_of(beat);
    if (tick <= 0 || tick >= length)
      continue;
    processing.reviewed_warp_markers.push_back({{frame}, timeline::TickPosition{tick}});
  }
  std::ranges::sort(
      processing.reviewed_warp_markers, {},
      [](const project::ClipWarpMarker& marker) { return marker.source_frame.value; });
  if (auto applied = session->audio_clips().set_audio_clip_processing(clip->id, processing);
      !applied)
    return failure(applied.error());
  report_(QStringLiteral("Drift fixed from bar %1: the kicks there play at %2 BPM")
              .arg(bar)
              .arg(60.0 * grid->sample_rate / slope, 0, 'f', 2));
  commit_();
}

void ClipTempoController::clear_markers(const QString& clip_id) {
  auto* session = editable_session();
  if (!session)
    return;
  const auto* clip = find_clip(session->snapshot(), clip_id);
  if (!clip || clip->processing.reviewed_warp_markers.empty())
    return;
  auto processing = clip->processing;
  processing.reviewed_warp_markers.clear();
  if (auto applied = session->audio_clips().set_audio_clip_processing(clip->id, processing);
      !applied)
    return failure(applied.error());
  report_(QStringLiteral("Warp markers cleared: the clip plays one straight grid"));
  commit_();
}

} // namespace oscillate::quick
