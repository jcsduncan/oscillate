#include "freeze_controller.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <limits>
#include <ranges>
#include <type_traits>
#include <utility>
#include <variant>

namespace oscillate::quick {
namespace {

[[nodiscard]] QString verb(bool busy_freezing, bool flattening) {
  if (busy_freezing)
    return QStringLiteral("Freezing");
  return flattening ? QStringLiteral("Flattening") : QStringLiteral("Bouncing");
}

[[nodiscard]] std::optional<project::TrackId> parse_track(const QString& text) {
  auto parsed = project::TrackId::parse(text.toStdString());
  if (!parsed)
    return std::nullopt;
  return parsed.value();
}

[[nodiscard]] bool whole_tick(double value) {
  return std::isfinite(value) && value >= 0.0 &&
         value <= static_cast<double>(std::numeric_limits<std::int64_t>::max() / 2) &&
         std::floor(value) == value;
}

} // namespace

FreezeController::FreezeController(Session session, CanEdit can_edit, Report report, Commit commit,
                                   QObject* parent)
    : QObject(parent), session_(std::move(session)), can_edit_(std::move(can_edit)),
      report_(std::move(report)), commit_(std::move(commit)) {
  timer_.setInterval(50);
  connect(&timer_, &QTimer::timeout, this, &FreezeController::poll);
}

FreezeController::~FreezeController() {
  if (job_)
    job_->cancel.store(true, std::memory_order_release);
  if (worker_.joinable()) {
    worker_.request_stop();
    worker_.join();
  }
}

double FreezeController::progress() const noexcept {
  if (!job_)
    return 0.0;
  // Staging copies the file: the last few percent.
  const auto rendered = job_->progress.load(std::memory_order_acquire);
  return job_->phase == Phase::render ? rendered * 0.95 : 0.97;
}

QString FreezeController::track_id() const {
  return job_ ? QString::fromStdString(job_->track.to_string()) : QString();
}

QString FreezeController::action() const {
  return job_ ? verb(job_->kind == Kind::freeze, job_->kind == Kind::flatten) : QString();
}

application::ProjectSession* FreezeController::editable_session() {
  auto* session = session_();
  if (session == nullptr) {
    report_(QStringLiteral("Open a project first"));
    return nullptr;
  }
  if (job_) {
    report_(QStringLiteral("%1 another track; wait or cancel it").arg(action()));
    return nullptr;
  }
  if (!can_edit_())
    return nullptr;
  return session;
}

void FreezeController::fail(const QString& context, const foundation::Error& error) {
  report_(QStringLiteral("%1: %2").arg(context, QString::fromStdString(error.message)));
}

void FreezeController::freeze(const QString& track_id) {
  start(Kind::freeze, track_id, std::nullopt);
}

void FreezeController::bounce(const QString& track_id) {
  start(Kind::bounce, track_id, std::nullopt);
}

void FreezeController::flatten(const QString& track_id) {
  start(Kind::flatten, track_id, std::nullopt);
}

void FreezeController::bounce_range(const QString& track_id, double start_tick, double end_tick) {
  auto* session = editable_session();
  if (session == nullptr)
    return;
  if (!whole_tick(start_tick) || !whole_tick(end_tick) || end_tick <= start_tick) {
    report_(QStringLiteral("Select a time range to bounce"));
    return;
  }
  const auto& state = session->snapshot();
  const auto first =
      state.tempo.tick_to_sample({static_cast<std::int64_t>(start_tick)}, state.sample_rate);
  const auto last =
      state.tempo.tick_to_sample({static_cast<std::int64_t>(end_tick)}, state.sample_rate);
  if (!first || !last || first.value().value < 0 || last.value() <= first.value()) {
    report_(QStringLiteral("Select a time range to bounce"));
    return;
  }
  start(Kind::bounce, track_id,
        std::pair{static_cast<std::uint64_t>(first.value().value),
                  static_cast<std::uint64_t>(last.value().value)});
}

void FreezeController::bounce_clip(const QString& clip_id) {
  auto* session = editable_session();
  const auto clip = project::ClipId::parse(clip_id.toStdString());
  if (session == nullptr || !clip) {
    report_(QStringLiteral("Select a MIDI clip to bounce"));
    return;
  }
  const auto& state = session->snapshot();
  for (const auto& track : state.tracks) {
    const auto found = std::ranges::find(track.midi_clips, clip.value(), &project::MidiClip::id);
    if (found == track.midi_clips.end())
      continue;
    // The clip's span in project frames, whichever timebase it is placed on.
    const auto span = std::visit(
        [&](const auto& interval) -> std::optional<std::pair<std::int64_t, std::int64_t>> {
          if constexpr (std::is_same_v<std::decay_t<decltype(interval)>,
                                       project::MusicalInterval>) {
            const auto first = state.tempo.tick_to_sample(interval.start, state.sample_rate);
            const auto last = state.tempo.tick_to_sample(interval.end, state.sample_rate);
            if (!first || !last)
              return std::nullopt;
            return std::pair{first.value().value, last.value().value};
          } else {
            return std::pair{interval.start.value, interval.end.value};
          }
        },
        found->placement);
    if (!span || span->first < 0 || span->second <= span->first) {
      report_(QStringLiteral("Select a MIDI clip to bounce"));
      return;
    }
    start(Kind::bounce, QString::fromStdString(track.id.to_string()),
          std::pair{static_cast<std::uint64_t>(span->first),
                    static_cast<std::uint64_t>(span->second)});
    return;
  }
  report_(QStringLiteral("Select a MIDI clip to bounce"));
}

void FreezeController::unfreeze(const QString& track_id) {
  auto* session = editable_session();
  const auto track = parse_track(track_id);
  if (session == nullptr || !track)
    return;
  if (auto done = session->freeze().unfreeze_track(*track); !done) {
    fail(QStringLiteral("Unfreeze"), done.error());
    return;
  }
  commit_();
  report_(QStringLiteral("Unfrozen: the devices play again · one undo step"));
}

void FreezeController::consolidate(const QString& track_id, double start_tick, double end_tick) {
  auto* session = editable_session();
  const auto track = parse_track(track_id);
  if (session == nullptr || !track)
    return;
  if (!whole_tick(start_tick) || !whole_tick(end_tick)) {
    report_(QStringLiteral("Select a time range to consolidate"));
    return;
  }
  auto clip = session->midi_clips().consolidate_midi_clips(
      *track, {static_cast<std::int64_t>(start_tick)}, {static_cast<std::int64_t>(end_tick)});
  if (!clip) {
    fail(QStringLiteral("Consolidate"), clip.error());
    return;
  }
  commit_();
  report_(QStringLiteral("Consolidated into one clip · one undo step"));
}

std::optional<std::pair<double, double>> FreezeController::bar_ticks(int start_bar, int end_bar) {
  auto* session = session_();
  if (session == nullptr || start_bar < 1 || end_bar <= start_bar) {
    report_(QStringLiteral("Set the loop over the bars first"));
    return std::nullopt;
  }
  const auto& meter = session->snapshot().meter;
  const auto first = meter.tick_at({start_bar, 1, 0});
  const auto last = meter.tick_at({end_bar, 1, 0});
  if (!first || !last) {
    report_(QStringLiteral("The bars are outside the song"));
    return std::nullopt;
  }
  return std::pair{static_cast<double>(first.value().value),
                   static_cast<double>(last.value().value)};
}

void FreezeController::bounce_bars(const QString& track_id, int start_bar, int end_bar) {
  if (const auto ticks = bar_ticks(start_bar, end_bar))
    bounce_range(track_id, ticks->first, ticks->second);
}

void FreezeController::consolidate_bars(const QString& track_id, int start_bar, int end_bar) {
  if (const auto ticks = bar_ticks(start_bar, end_bar))
    consolidate(track_id, ticks->first, ticks->second);
}

void FreezeController::start(Kind kind, const QString& track_id,
                             std::optional<std::pair<std::uint64_t, std::uint64_t>> range) {
  auto* session = editable_session();
  const auto track = parse_track(track_id);
  if (session == nullptr)
    return;
  if (!track) {
    report_(QStringLiteral("Choose a track"));
    return;
  }
  const auto& state = session->snapshot();
  const auto owner = std::ranges::find(state.tracks, *track, &project::Track::id);
  if (kind == Kind::freeze && owner != state.tracks.end() && owner->frozen) {
    report_(QStringLiteral("The track is already frozen"));
    return;
  }
  auto task = session->freeze().begin_track_render(*track, range);
  if (!task) {
    fail(QStringLiteral("Render"), task.error());
    return;
  }
  auto job = std::make_shared<Job>(kind, *track);
  try {
    worker_ = std::jthread([job, task = std::move(task).value()]() mutable {
      try {
        job->rendered.emplace(std::move(task).run([job](std::uint64_t done, std::uint64_t total) {
          if (total != 0)
            job->progress.store(static_cast<double>(done) / static_cast<double>(total),
                                std::memory_order_release);
          return !job->cancel.load(std::memory_order_acquire);
        }));
      } catch (const std::exception& error) {
        job->rendered.emplace(foundation::Result<application::TrackRender>::failure(
            {foundation::ErrorCode::internal, error.what()}));
      }
      job->done.store(true, std::memory_order_release);
    });
  } catch (const std::exception& error) {
    report_(QStringLiteral("Cannot start the render: %1").arg(error.what()));
    return;
  }
  job_ = std::move(job);
  timer_.start();
  report_(QStringLiteral("%1 the track…").arg(action()));
  emit changed();
}

void FreezeController::cancel() {
  if (job_)
    job_->cancel.store(true, std::memory_order_release);
  if (worker_.joinable())
    worker_.request_stop();
}

void FreezeController::poll() {
  if (!job_) {
    timer_.stop();
    return;
  }
  if (!job_->done.load(std::memory_order_acquire)) {
    emit changed();
    return;
  }
  if (worker_.joinable())
    worker_.join();
  finish();
}

void FreezeController::finish() {
  auto job = job_;
  const auto stop = [&](std::optional<QString> message) {
    job_.reset();
    timer_.stop();
    if (message)
      report_(*message);
    emit changed();
  };
  const bool cancelled = job->cancel.load(std::memory_order_acquire);
  if (job->phase == Phase::render) {
    auto& rendered = *job->rendered;
    if (cancelled)
      return stop(QStringLiteral("%1 cancelled").arg(action()));
    if (!rendered)
      return stop(QStringLiteral("%1 failed: %2")
                      .arg(action(), QString::fromStdString(rendered.error().message)));
    auto* session = session_();
    if (session == nullptr)
      return stop(std::nullopt);
    job->render.emplace(std::move(rendered).value());
    auto stage = session->freeze().begin_track_render_stage(*job->render);
    if (!stage)
      return stop(QStringLiteral("%1 failed: %2")
                      .arg(action(), QString::fromStdString(stage.error().message)));
    job->phase = Phase::stage;
    job->done.store(false, std::memory_order_release);
    try {
      worker_ =
          std::jthread([job, task = std::move(stage.value().task)](std::stop_token token) mutable {
            try {
              job->staged.emplace(std::move(task).run(token));
            } catch (const std::exception& error) {
              job->staged.emplace(foundation::Result<persistence::StagedWav>::failure(
                  {foundation::ErrorCode::internal, error.what()}));
            }
            job->done.store(true, std::memory_order_release);
          });
    } catch (const std::exception& error) {
      return stop(QStringLiteral("Cannot stage the render: %1").arg(error.what()));
    }
    emit changed();
    return;
  }
  auto& staged = *job->staged;
  if (cancelled)
    return stop(QStringLiteral("%1 cancelled").arg(action()));
  if (!staged)
    return stop(QStringLiteral("%1 failed: %2")
                    .arg(action(), QString::fromStdString(staged.error().message)));
  auto* session = session_();
  if (session == nullptr)
    return stop(std::nullopt);
  const auto origin = job->render->origin_revision();
  const auto first = job->render->first_frame();
  QString message;
  if (job->kind == Kind::freeze) {
    auto frozen =
        session->freeze().finish_track_freeze(std::move(staged).value(), origin, job->track, first);
    if (!frozen)
      return stop(
          QStringLiteral("Freeze failed: %1").arg(QString::fromStdString(frozen.error().message)));
    message = QStringLiteral("Frozen: the track plays its render · one undo step");
  } else {
    const auto mode = job->kind == Kind::flatten ? application::BounceMode::flatten
                                                 : application::BounceMode::new_track;
    auto bounced = session->freeze().finish_track_bounce(std::move(staged).value(), origin,
                                                         job->track, first, mode);
    if (!bounced)
      return stop(QStringLiteral("%1 failed: %2")
                      .arg(action(), QString::fromStdString(bounced.error().message)));
    message = job->kind == Kind::flatten
                  ? QStringLiteral("Flattened to audio · one undo step")
                  : QStringLiteral("Bounced to a new audio track; the source is muted · one "
                                   "undo step");
  }
  job_.reset();
  timer_.stop();
  commit_();
  report_(message);
  emit changed();
}

bool FreezeController::wait(int timeout_ms) {
  QElapsedTimer elapsed;
  elapsed.start();
  while (job_ && elapsed.elapsed() < timeout_ms) {
    if (job_->done.load(std::memory_order_acquire))
      poll();
    else {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  }
  return !job_;
}

} // namespace oscillate::quick
