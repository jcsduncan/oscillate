#pragma once

#include "source_analysis_worker.hpp"

#include "oscillate/application/key_cache.hpp"
#include "oscillate/application/project_session.hpp"

#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantMap>

#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <optional>

namespace oscillate::quick {

// Smart drops (ADR-0059): a clip dropped into a song with a key is offered
// a move into that key. Its source's key comes from the project's key
// review, or is analysed in the background (through the shared analysis
// cache) and stored as the review would store it. The offer is a quiet
// hint, never a dialog; taking it is one undoable pitch edit.
class KeyMatchController final : public QObject {
  Q_OBJECT
  // Empty, or {clipId, clipName, from, to, semitones, text}: the clip, the
  // key it is heard in, the song's key it would move to, and the shift.
  Q_PROPERTY(QVariantMap hint READ hint NOTIFY changed)

public:
  using Session = std::function<application::ProjectSession*()>;
  using Root = std::function<std::filesystem::path()>;
  using CanEdit = std::function<bool()>;
  using Report = std::function<void(QString)>;
  using Commit = std::function<void()>;
  // Test seam with KeyReviewController's shape; empty uses the shared cache.
  using Analyzer = std::function<foundation::Result<application::KeyCacheResult>(
      const std::filesystem::path&, const project::SourceRecord&, std::uint64_t,
      const std::function<bool()>&)>;

  KeyMatchController(Session session, Root root, CanEdit can_edit, Report report, Commit commit,
                     Analyzer analyzer = {}, QObject* parent = nullptr);
  ~KeyMatchController() override;

  [[nodiscard]] QVariantMap hint() const { return hint_; }
  // A clip just dropped in: offer it the song's key once its key is known.
  void dropped(project::ClipId clip);
  // After any project change: a hint whose clip was edited, moved or
  // removed, or whose song key changed, is withdrawn.
  void refresh();
  Q_INVOKABLE void apply();
  Q_INVOKABLE void dismiss();

signals:
  void changed();

private:
  void poll();
  void start_next();
  void consider(project::ClipId clip);
  void clear_hint();

  struct Offer {
    project::ClipId clip;
    std::uint64_t settings_generation = 0;
    project::MusicalKey song;
    int semitones = 0;
  };

  Session session_;
  Root root_;
  CanEdit can_edit_;
  Report report_;
  Commit commit_;
  SourceAnalysisWorker<application::KeyCacheResult> worker_;
  QTimer timer_;
  std::deque<project::ClipId> waiting_;
  std::optional<project::ClipId> analysing_;
  std::optional<project::SourceRecord> source_;
  std::uint64_t generation_ = 0;
  std::uint64_t origin_revision_ = 0;
  int retries_ = 0;
  std::optional<Offer> offer_;
  QVariantMap hint_;
};

} // namespace oscillate::quick
