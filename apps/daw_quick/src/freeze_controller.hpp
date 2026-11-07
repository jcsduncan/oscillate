#pragma once

#include "oscillate/application/project_session.hpp"

#include <QObject>
#include <QString>
#include <QTimer>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <utility>

namespace oscillate::quick {

// Freeze, Unfreeze, Bounce, Flatten, Bounce selection and Consolidate for
// the track menu (ADR-0059). A render runs on a worker with progress and
// Cancel; staging its WAV runs on a second worker; each result commits on
// the UI thread as one undo step. One job at a time.
class FreezeController final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool busy READ busy NOTIFY changed)
  Q_PROPERTY(double progress READ progress NOTIFY changed)
  Q_PROPERTY(QString trackId READ track_id NOTIFY changed)
  // "Freezing", "Bouncing" or "Flattening" while busy.
  Q_PROPERTY(QString action READ action NOTIFY changed)

public:
  using Session = std::function<application::ProjectSession*()>;
  using CanEdit = std::function<bool()>;
  using Report = std::function<void(QString)>;
  using Commit = std::function<void()>;

  FreezeController(Session session, CanEdit can_edit, Report report, Commit commit,
                   QObject* parent = nullptr);
  FreezeController(const FreezeController&) = delete;
  FreezeController& operator=(const FreezeController&) = delete;
  ~FreezeController() override;

  Q_INVOKABLE void freeze(const QString& track_id);
  Q_INVOKABLE void unfreeze(const QString& track_id);
  Q_INVOKABLE void bounce(const QString& track_id);
  Q_INVOKABLE void flatten(const QString& track_id);
  // Bounce exactly [start, end) of the song, in ticks, to a new audio track.
  void bounce_range(const QString& track_id, double start_tick, double end_tick);
  // A MIDI clip's span, through its track's instrument and devices, to a
  // new audio track (its track muted, kept for later).
  Q_INVOKABLE void bounce_clip(const QString& clip_id);
  // The track's MIDI clips overlapping [start, end) ticks become one clip.
  Q_INVOKABLE void consolidate(const QString& track_id, double start_tick, double end_tick);
  // The same over whole bars (one-based, end exclusive): the loop brace.
  Q_INVOKABLE void bounce_bars(const QString& track_id, int start_bar, int end_bar);
  Q_INVOKABLE void consolidate_bars(const QString& track_id, int start_bar, int end_bar);
  Q_INVOKABLE void cancel();

  [[nodiscard]] bool busy() const noexcept { return job_ != nullptr; }
  [[nodiscard]] double progress() const noexcept;
  [[nodiscard]] QString track_id() const;
  [[nodiscard]] QString action() const;
  // Tests: drive the job to its end on this thread.
  bool wait(int timeout_ms);

signals:
  void changed();

private:
  enum class Kind : std::uint8_t { freeze, bounce, flatten };
  enum class Phase : std::uint8_t { render, stage };
  struct Job {
    Job(Kind job_kind, project::TrackId job_track, std::uint64_t job_session)
        : kind(job_kind), track(job_track), session(job_session) {}
    Kind kind;
    project::TrackId track;
    // The session the job started in; a result for another is dropped.
    std::uint64_t session;
    Phase phase = Phase::render;
    std::atomic<bool> cancel{false};
    std::atomic<bool> done{false};
    std::atomic<double> progress{0.0};
    std::optional<foundation::Result<application::TrackRender>> rendered;
    std::optional<application::TrackRender> render;
    std::optional<foundation::Result<persistence::StagedWav>> staged;
  };

  void start(Kind kind, const QString& track_id,
             std::optional<std::pair<std::uint64_t, std::uint64_t>> range);
  void poll();
  void finish();
  void fail(const QString& context, const foundation::Error& error);
  [[nodiscard]] application::ProjectSession* editable_session();
  // The session the job started in, or null when it has been replaced.
  [[nodiscard]] application::ProjectSession* job_session(const Job& job);
  [[nodiscard]] std::optional<std::pair<double, double>> bar_ticks(int start_bar, int end_bar);

  Session session_;
  CanEdit can_edit_;
  Report report_;
  Commit commit_;
  QTimer timer_;
  std::shared_ptr<Job> job_;
  std::jthread worker_;
};

} // namespace oscillate::quick
