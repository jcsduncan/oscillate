#pragma once

#include "background_job.hpp"

#include "oscillate/foundation/error.hpp"

#include <QObject>
#include <QString>
#include <QTimer>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>

class QQuickWindow;

namespace oscillate::quick {

// Records a performance trace (ADR-0061) from the command palette, or from
// launch when OSCILLATE_TRACE names the file. While recording it adds the
// UI thread's own spans: each busy spell of its event loop, each scene-graph
// render and the interval between presented frames. Traces open in
// ui.perfetto.dev; tools/trace_report summarises them. A trace is written
// on a worker, so a slow disk never stalls the window; the one recorded from
// launch is written on quit, which waits for it.
class PerformanceTrace final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool recording READ recording NOTIFY changed)
  Q_PROPERTY(bool saving READ saving NOTIFY changed)
  Q_PROPERTY(QString status READ status NOTIFY changed)

public:
  // Ends the recording and writes it to the path; returns the events
  // written. Test seam: empty uses foundation::trace::stop_and_write.
  using Writer = std::function<foundation::Result<std::size_t>(const std::filesystem::path&)>;

  explicit PerformanceTrace(QObject* parent = nullptr, Writer writer = {});
  ~PerformanceTrace() override;

  // Hooks the window's frames; a recording started from OSCILLATE_TRACE
  // (at construction) is written on quit.
  void attach(QQuickWindow* window);

  [[nodiscard]] bool recording() const noexcept { return recording_; }
  [[nodiscard]] bool saving() const noexcept { return save_.running(); }
  [[nodiscard]] QString status() const { return status_; }

  Q_INVOKABLE void start();
  // Writes the trace to the traces folder (or OSCILLATE_TRACE's file) in
  // the background; `status` reports where, or why it failed.
  Q_INVOKABLE void stop_and_save();
  // Blocks until a save in progress finishes and reports it (for quit).
  void wait_for_save();

signals:
  void changed();

private:
  void finish(const std::filesystem::path& path);
  void poll();
  void report(const std::filesystem::path& path, const foundation::Result<std::size_t>& written);

  Writer writer_;
  BackgroundJob<foundation::Result<std::size_t>> save_;
  std::filesystem::path saving_path_;
  QTimer timer_;
  bool recording_ = false;
  QString status_;
  std::filesystem::path launch_path_;
  std::uint64_t busy_since_ = 0;
  std::uint64_t last_frame_ = 0;
  std::uint64_t render_since_ = 0; // Render thread only.
};

} // namespace oscillate::quick
