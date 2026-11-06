#pragma once

#include "source_analysis_worker.hpp"

#include "oscillate/application/key_cache.hpp"
#include "oscillate/application/project_session.hpp"

#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantMap>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>

namespace oscillate::quick {

// "A minor", or "A min" abbreviated.
[[nodiscard]] QString key_name(project::MusicalKey key, bool abbreviated = false);

// Worker-only: the source's key suggestion through the shared analysis
// cache (SourceAnalysisWorker's analyzer shape). Sets cache_degraded when
// the cache could not be used.
[[nodiscard]] foundation::Result<application::KeyCacheResult>
load_source_key(const std::filesystem::path& root, const project::SourceRecord& source,
                std::uint64_t generation, const std::function<bool()>& cancelled,
                bool& cache_degraded, const std::stop_token& stop);

// UI-thread projection of durable source key review. The worker owns all
// decoding, cache and analysis I/O; only a still-selected exact source and
// captured project revision can publish its result.
class KeyReviewController final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QVariantMap preview READ preview NOTIFY changed)

public:
  using Analyzer = std::function<foundation::Result<application::KeyCacheResult>(
      const std::filesystem::path&, const project::SourceRecord&, std::uint64_t,
      const std::function<bool()>&)>;
  explicit KeyReviewController(Analyzer analyzer = {}, QObject* parent = nullptr);
  ~KeyReviewController() override;

  [[nodiscard]] QVariantMap preview() const { return preview_; }
  void set_project(application::ProjectSession* session, std::filesystem::path root);
  void select_clip(QString clip_id, std::optional<project::SourceRecord> source);
  void refresh_snapshot();
  Q_INVOKABLE void reanalyze();
  Q_INVOKABLE void set_correction(int tonic, int mode);
  Q_INVOKABLE void clear_correction();

signals:
  void changed();
  void projectRevisionChanged();

private:
  void poll();
  void publish();
  [[nodiscard]] bool selection_matches_snapshot() const;
  void cancel_selection();
  SourceAnalysisWorker<application::KeyCacheResult> worker_;
  QTimer timer_;
  application::ProjectSession* session_ = nullptr;
  std::filesystem::path root_;
  std::optional<project::SourceRecord> source_;
  QString clip_id_;
  std::uint64_t generation_ = 0;
  std::uint64_t origin_revision_ = 0;
  QString state_ = QStringLiteral("idle");
  QString message_;
  bool cache_hit_ = false;
  bool cache_degraded_ = false;
  QVariantMap preview_;
};

} // namespace oscillate::quick
