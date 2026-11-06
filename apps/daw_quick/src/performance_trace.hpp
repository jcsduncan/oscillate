#pragma once

#include <QObject>
#include <QString>

#include <cstdint>
#include <filesystem>

class QQuickWindow;

namespace oscillate::quick {

// Records a performance trace (ADR-0061) from the command palette, or from
// launch when OSCILLATE_TRACE names the file. While recording it adds the
// UI thread's own spans: each busy spell of its event loop, each scene-graph
// render and the interval between presented frames. Traces open in
// ui.perfetto.dev; tools/trace_report summarises them.
class PerformanceTrace final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool recording READ recording NOTIFY changed)
  Q_PROPERTY(QString status READ status NOTIFY changed)

public:
  explicit PerformanceTrace(QObject* parent = nullptr);
  ~PerformanceTrace() override;

  // Hooks the window's frames; a recording started from OSCILLATE_TRACE
  // (at construction) is written on quit.
  void attach(QQuickWindow* window);

  [[nodiscard]] bool recording() const noexcept { return recording_; }
  [[nodiscard]] QString status() const { return status_; }

  Q_INVOKABLE void start();
  // Writes the trace to the traces folder (or OSCILLATE_TRACE's file).
  Q_INVOKABLE void stop_and_save();

signals:
  void changed();

private:
  void finish(const std::filesystem::path& path);

  bool recording_ = false;
  QString status_;
  std::filesystem::path launch_path_;
  std::uint64_t busy_since_ = 0;
  std::uint64_t last_frame_ = 0;
  std::uint64_t render_since_ = 0; // Render thread only.
};

} // namespace oscillate::quick
