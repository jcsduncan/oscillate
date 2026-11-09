#pragma once

#include "oscillate/application/clip_warp_edit.hpp"
#include "oscillate/application/project_session.hpp"
#include "oscillate/media/cache_store.hpp"
#include "oscillate/processing/warp_render_cache.hpp"
#include "tempo_worker.hpp"

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>

#include <atomic>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace oscillate::quick {

// The clip strip on every audio clip (ADR-0053): its source tempo, Follow,
// stretch mode and downbeat. Every audio source is analysed once, in the
// background and through the shared analysis cache. Opening a project never
// changes it: a result lives here until something needs it in the project.
// A clip dropped in this session is set up ready to play in time (the result
// stored, Follow on, its downbeat on the nearest bar; the first track in a
// project also sets the project's tempo to its own), and an edit of a
// clip's tempo stores its source's result first. Each edit is one
// ProjectSession command; the session stays authoritative.
class ClipTempoController final : public QObject {
  Q_OBJECT
  // Clip id -> {bpm, detected, corrected, confidence, analysing, follow,
  // modeIndex, mode, downbeat, beat, stretch, seconds, sourceKey, cacheHit,
  // rendering, renderProgress}. A stretched clip's source is analysed once
  // in the background and kept in the project's cache (`rendering` until it
  // is, `renderProgress` 0..1); the clip then stretches in real time, so a
  // tempo change or a move never prepares anything again. Positions are
  // fractions of the clip's length: `downbeat` is the first bar line in the
  // clip, `beat` one beat; `stretch` is the percentage the project tempo
  // plays the clip faster (positive) or slower than its own.
  Q_PROPERTY(QVariantMap clips READ clips NOTIFY changed)
  // The stretch modes in the strip's order, with one-line hints.
  Q_PROPERTY(QVariantList modes READ modes CONSTANT)

public:
  using Session = std::function<application::ProjectSession*()>;
  using Root = std::function<std::filesystem::path()>;
  using CanEdit = std::function<bool()>;
  using Report = std::function<void(QString)>;
  using Commit = std::function<void()>;
  // Min/max columns of a source's waveform across a source range.
  using Waveform =
      std::function<QVariantList(project::SourceId, project::SourceRange, std::size_t columns)>;

  ClipTempoController(Session session, Root root, CanEdit can_edit, Report report, Commit commit,
                      TempoWorker::Analyzer analyzer = {}, QObject* parent = nullptr,
                      Waveform waveform = {});

  [[nodiscard]] QVariantMap clips() const { return clips_; }
  [[nodiscard]] QVariantList modes() const;
  // Rebuilds the projection and queues sources that still need analysis.
  void refresh();
  // A clip just dropped in: set up (Follow on, downbeat on the nearest
  // bar) as soon as its source's tempo is known.
  void dropped(project::ClipId clip);
  // The finished analysis of this exact source version, if any.
  [[nodiscard]] std::optional<analysis::TempoEstimate>
  estimate_for(const project::SourceRecord& source) const;

  Q_INVOKABLE void set_follow(const QString& clip_id, bool follow);
  Q_INVOKABLE void set_bpm(const QString& clip_id, double bpm);
  // Back to the detected tempo.
  Q_INVOKABLE void reset_bpm(const QString& clip_id);
  // The track tempo becomes the tempo of the segment from warp marker
  // `index` (ADR-0112).
  Q_INVOKABLE void set_bpm_from_segment(const QString& clip_id, int index);
  // x2 and /2: half- and double-time reads.
  Q_INVOKABLE void scale_bpm(const QString& clip_id, double factor);
  // One tap; from the fourth, the tempo of the taps (a gap over two seconds
  // starts again). Returns the tapped BPM so far, 0 before there is one.
  Q_INVOKABLE double tap(const QString& clip_id);
  Q_INVOKABLE void set_mode(const QString& clip_id, int index);
  // The downbeat at `fraction` of the clip, on the nearest beat of the
  // source's grid unless `free`.
  Q_INVOKABLE void set_downbeat(const QString& clip_id, double fraction, bool free);
  Q_INVOKABLE void reanalyse(const QString& clip_id);

  // --- The docked clip editor ---
  // The clip's waveform as it plays (warp markers included) across
  // [from, to] of its length, in `columns` {min, max} columns.
  Q_INVOKABLE QVariantList detail(const QString& clip_id, double from, double to,
                                  int columns) const;
  // The grid check: how the source's transients sit on its beat grid.
  // {pending} while transients are being found; else {fit (share of beats
  // within 10 ms), steady, driftBar (clip bar from which kicks drift, or -1),
  // driftMs, transients: [{fraction, offsetMs, late}]}.
  Q_INVOKABLE QVariantMap inspect(const QString& clip_id);
  // Straightens the grid from clip bar `bar` on: the tempo of the kicks
  // there, pinned by two warp markers so they land on the grid. Follow only.
  Q_INVOKABLE void fix_drift(const QString& clip_id, int bar);
  Q_INVOKABLE void clear_markers(const QString& clip_id);
  // The source's tempo as each stretch between warp markers plays it.
  Q_INVOKABLE QVariantList segments(const QString& clip_id) const;

signals:
  void changed();

private:
  [[nodiscard]] application::ProjectSession* editable_session();
  void start_next();
  void poll();
  void accept(const project::SourceRecord& source, const analysis::TempoEstimate& estimate);
  // Sets up the dropped clips of `source` whose tempo is known.
  void set_up(project::SourceId source);
  // Stores the source's analysis as its tempo review if it has none yet.
  [[nodiscard]] bool store(application::ProjectSession& session, project::SourceId source);
  // The source's grid: its review's, else the analysis waiting here.
  [[nodiscard]] std::optional<application::SourceBeatGrid>
  grid_of(const project::Snapshot& snapshot, project::SourceId source) const;
  void rebuild();
  void failure(const foundation::Error& error);
  // Queues the analysis of every stretched clip's source not yet in the
  // cache, runs one at a time on a worker, and polls it.
  void render_clips();
  [[nodiscard]] std::optional<std::string> analysis_key(const project::Snapshot& snapshot,
                                                        project::ClipId clip) const;

  Session session_;
  Root root_;
  CanEdit can_edit_;
  Report report_;
  Commit commit_;
  TempoWorker worker_;
  QTimer poll_timer_;
  QVariantMap clips_;
  const application::ProjectSession* seen_session_ = nullptr;
  std::set<project::SourceId> attempted_;
  std::deque<project::SourceId> pending_;
  std::optional<project::SourceRecord> in_flight_;
  // Finished analyses not (yet) stored in the project, and which of them
  // came from the shared analysis cache.
  std::map<project::SourceId, analysis::TempoEstimate> estimates_;
  std::set<project::SourceId> cache_hits_;
  // Sources whose transients the editor wants, though they are reviewed.
  std::set<project::SourceId> wanted_;
  Waveform waveform_;
  // Background warp analysis: keys already in the cache, the queue, and the
  // one running.
  std::optional<media::CacheStore> analysis_cache_;
  std::set<std::string> analysed_keys_;
  std::deque<std::pair<std::string, processing::WarpRenderRequest>> analysis_queue_;
  struct AnalysisJob {
    std::string key;
    std::shared_ptr<std::atomic<double>> progress;
    std::shared_ptr<std::atomic<bool>> done;
    std::jthread thread;
  };
  std::optional<AnalysisJob> analysis_job_;
  QTimer render_timer_;
  // A finished analysis waiting for the session to be idle.
  std::optional<foundation::Result<application::TempoCacheResult>> held_;
  std::uint64_t generation_ = 0;
  // Clips dropped in this session, waiting for their source's tempo.
  std::set<project::ClipId> fresh_;
  struct Taps {
    QElapsedTimer since;
    std::vector<qint64> times;
  };
  std::map<QString, Taps> taps_;
};

} // namespace oscillate::quick
