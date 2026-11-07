#pragma once

#include "oscillate/analysis/tempo.hpp"
#include "oscillate/application/clip_warp_edit.hpp"
#include "oscillate/application/project_session.hpp"
#include "oscillate/application/routed_audio_source.hpp"

#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace oscillate::quick {

class WarpOnsetWorker;

// Display name of a saved warp mode ("Complex Pro", "Re-Pitch", ...).
[[nodiscard]] QString warp_mode_label(project::WarpMode mode);

// What the active playback run is doing, as observed on the control
// sequencer. The warp editor shows it beside the saved settings so a pending
// edit is never presented as audible.
struct WarpPlaybackObservation {
  enum class Path : std::uint8_t { stopped, routed, direct };
  Path path = Path::stopped;
  std::uint64_t active_revision = 0;
  bool edit_pending = false;
  std::optional<QString> failure;
  std::vector<application::RoutedProcessedClipStatus> processed;
};

// UI-thread controller for the selected audio clip's saved warp and pitch
// intent and its reviewed warp markers. Every edit is one ProjectSession
// command (one undo step), then the owner publishes it to playback. Marker
// positions resolve through the authoritative clip frame plan.
//
// Onset suggestions are the source's durable onset review: a
// bounded, cancellable worker analyses the whole verified source and its
// result is stored only if the project revision it captured is still current.
// Applying suggestions adds clip markers; dismissing them is a saved user
// correction that reanalysis keeps. Auto-warp and tempo conform are
// proposals from the source's tempo review, shown for review and applied as
// one edit only if the clip has not changed since.
class ClipWarpController final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QVariantMap state READ state NOTIFY changed)
  Q_PROPERTY(QVariantMap playback READ playback NOTIFY playbackChanged)

public:
  using Session = std::function<application::ProjectSession*()>;
  using Root = std::function<std::filesystem::path()>;
  using CanEdit = std::function<bool()>;
  using Report = std::function<void(QString)>;
  using Commit = std::function<void()>;
  using Waveform = std::function<QVariantList(const project::Clip&, std::size_t columns)>;
  using Playback = std::function<WarpPlaybackObservation()>;
  // The verified tempo analysis already computed for a source, if any (the
  // tempo panel's worker result); stored into the review on demand.
  using TempoEstimate =
      std::function<std::optional<analysis::TempoEstimate>(const project::SourceRecord&)>;
  using OnsetAnalyzer = std::function<foundation::Result<application::SourceOnsets>(
      const std::filesystem::path&, const project::Snapshot&, project::SourceId,
      const std::atomic<bool>*)>;

  ClipWarpController(Session session, Root root, CanEdit can_edit, Report report, Commit commit,
                     Waveform waveform, Playback playback, TempoEstimate tempo = {},
                     QObject* parent = nullptr, OnsetAnalyzer onset_analyzer = {});
  ClipWarpController(const ClipWarpController&) = delete;
  ClipWarpController& operator=(const ClipWarpController&) = delete;
  ~ClipWarpController() override;

  [[nodiscard]] QVariantMap state() const { return state_; }
  [[nodiscard]] QVariantMap playback() const { return playback_; }

  // Owner hooks on the control sequencer.
  void select_clip(const QString& clip_id);
  void refresh_snapshot();
  void refresh_playback();

  // Mode index follows the editor order: Beats, Tones, Texture, Re-Pitch,
  // Complex, Complex Pro. Choosing a mode marks it as your override.
  Q_INVOKABLE void set_mode(int index);
  Q_INVOKABLE void set_mode_overridden(bool overridden);
  Q_INVOKABLE void set_transpose(int semitones, int cents);
  Q_INVOKABLE void set_preserve_formants(bool preserve);
  Q_INVOKABLE void set_beats_loop(int index);
  // key: beatsEnvelope, tonesGrain, textureGrain, textureFlux, formantAmount,
  // complexProEnvelope.
  Q_INVOKABLE void set_setting(const QString& key, double value);
  // Marker positions are fractions of the clip's rendered length (0..1).
  Q_INVOKABLE void add_marker(double fraction);
  Q_INVOKABLE void move_marker(int index, double fraction);
  Q_INVOKABLE void remove_marker(int index);
  // Analyses the whole source and stores the result as its onset review
  // (replacing earlier generated onsets, keeping dismissals).
  Q_INVOKABLE void request_suggestions();
  Q_INVOKABLE void apply_suggestions();
  // Dismissal is a saved correction: reanalysis does not bring these back.
  Q_INVOKABLE void dismiss_suggestions();
  Q_INVOKABLE void dismiss_suggestion(int index);
  Q_INVOKABLE void propose_auto_warp();
  Q_INVOKABLE void propose_tempo_conform();
  Q_INVOKABLE void apply_proposal();
  Q_INVOKABLE void discard_proposal();

signals:
  void changed();
  void playbackChanged();

private:
  struct Selected;
  [[nodiscard]] std::optional<Selected> selected() const;
  [[nodiscard]] application::ProjectSession* editable_session();
  void edit(const QString& action, project::ClipProcessing processing);
  void edit(const QString& action, foundation::Result<project::ClipProcessing> processing);
  void cancel_suggestions();
  void deliver_suggestions(std::uint64_t serial,
                           foundation::Result<application::SourceOnsets> found);
  [[nodiscard]] std::vector<std::uint64_t> visible_suggestions(const Selected& current) const;
  [[nodiscard]] bool ensure_tempo_review(const Selected& current, bool need_beats);
  void propose(const QString& kind);
  void rebuild();
  Session session_;
  Root root_;
  CanEdit can_edit_;
  Report report_;
  Commit commit_;
  Waveform waveform_;
  Playback playback_source_;
  OnsetAnalyzer onset_analyzer_;
  QString clip_id_;
  QVariantMap state_;
  QVariantMap playback_;
  TempoEstimate tempo_;
  // What the running analysis captured; it is stored only if the project is
  // still at this revision and the source unchanged.
  struct PendingAnalysis {
    project::SourceRecord source;
    std::uint64_t revision = 0;
  };
  std::optional<PendingAnalysis> pending_;
  // A reviewable auto-warp or conform proposal for one clip and revision.
  struct Proposal {
    QString kind;
    QString clip;
    std::uint64_t revision = 0;
    application::ClipWarpProposal value;
  };
  std::optional<Proposal> proposal_;
  QString suggestion_state_ = QStringLiteral("idle");
  QString suggestion_message_;
  std::uint64_t suggestion_serial_ = 0;
  QTimer suggestion_poll_timer_;
  std::unique_ptr<WarpOnsetWorker> onset_worker_;
};

} // namespace oscillate::quick
