#pragma once

#include "waveform_detail.hpp"
#include "waveform_worker.hpp"

#include "oscillate/application/project_session.hpp"

#include <QObject>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace oscillate::quick {

// Owns the UI-thread waveform selection, worker generation and view state.
// The bridge retains its QML methods and calls this owner through a small host.
class WaveformController final : public QObject {
  Q_OBJECT
public:
  static constexpr std::size_t max_sources = 64;
  static constexpr std::size_t max_clips = 256;

  struct Host {
    std::function<application::ProjectSession*()> session;
    std::function<void()> refresh;
    // Re-projects only the waveform columns, when close-up detail arrives.
    std::function<void()> refresh_waveforms;
    std::function<void(QString)> status;
  };

  struct ClipView {
    double start_bar;
    double end_bar;
    bool processed;
    std::size_t audio_clip_ordinal;
    // False: the clip's waveform state only, without computing its columns.
    bool columns = true;
  };

  explicit WaveformController(Host host, QObject* parent = nullptr);

  void set_project(std::filesystem::path root);
  void invalidate();
  void cancel_work();
  void refresh_sources(const project::Snapshot& snapshot);
  void retry_failures();
  [[nodiscard]] bool set_viewport(double first_bar, double visible_bars, int pixel_width);
  void rebuild_clip(const QString& clip_id);

  [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }
  [[nodiscard]] const std::vector<project::SourceRecord>& sources() const noexcept {
    return sources_;
  }
  [[nodiscard]] const analysis::WaveformSummary* summary(const std::string& id) const noexcept;
  [[nodiscard]] const QString* failure(const std::string& id) const noexcept;
  [[nodiscard]] bool cache_degraded(const std::string& id) const noexcept;
  [[nodiscard]] double first_bar() const noexcept { return first_bar_; }
  [[nodiscard]] double visible_bars() const noexcept { return visible_bars_; }
  [[nodiscard]] int view_width() const noexcept { return view_width_; }
  // A source's waveform across any range of it, for the clip editor.
  [[nodiscard]] QVariantList view_range(project::SourceId source, project::SourceRange range,
                                        std::size_t columns) const;
  [[nodiscard]] QVariantList view_for_warp(const project::Clip& clip, std::size_t columns) const;
  // Zoomed in past the summary's finest level, the columns come from the
  // source's own frames once they are read; until then from the summary.
  [[nodiscard]] QVariantMap project_clip_waveform(const project::Snapshot& snapshot,
                                                  const project::Clip& clip, ClipView view);

private:
  // A source's own frames, read for the stretch in view.
  struct Detail {
    WaveformDetail frames;
    std::uint64_t wanted_begin = 0;
    std::uint64_t wanted_end = 0;
    bool waiting = false;
    bool unreadable = false;
  };
  static constexpr std::size_t max_details = 8;

  void poll();
  void poll_details();
  [[nodiscard]] QVariantList close_up_columns(const project::SourceRecord& source,
                                              const analysis::WaveformSummary& summary,
                                              std::span<const std::uint64_t> edges);

  Host host_;
  QTimer timer_;
  WaveformWorker worker_;
  std::filesystem::path root_;
  double first_bar_ = 0.0;
  double visible_bars_ = 16.0;
  int view_width_ = 960;
  std::uint64_t generation_ = 0;
  std::vector<project::SourceRecord> sources_;
  std::unordered_map<std::string, analysis::WaveformSummary> summaries_;
  std::unordered_map<std::string, QString> failures_;
  std::unordered_map<std::string, bool> cache_degraded_;
  std::string rebuild_source_id_;
  QTimer detail_timer_;
  WaveformDetailWorker detail_worker_;
  std::unordered_map<std::string, Detail> details_;
  std::vector<std::string> detail_order_;
};

} // namespace oscillate::quick
