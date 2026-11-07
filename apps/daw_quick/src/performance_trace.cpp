#include "performance_trace.hpp"

#include "oscillate/foundation/log.hpp"
#include "oscillate/foundation/trace.hpp"

#include <QAbstractEventDispatcher>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QQuickWindow>
#include <QThread>

#include <system_error>

namespace oscillate::quick {
namespace trace = foundation::trace;

namespace {

constexpr double bytes_per_mebibyte = 1024.0 * 1024.0;

std::filesystem::path traces_directory() {
  QString data = qEnvironmentVariable("XDG_DATA_HOME");
  if (data.isEmpty())
    data = QDir::homePath() + QStringLiteral("/.local/share");
  return std::filesystem::path((data + QStringLiteral("/oscillate/traces")).toStdString());
}

} // namespace

PerformanceTrace::PerformanceTrace(QObject* parent) : QObject(parent) {
  trace::name_thread("ui");
  // From launch, so opening the project is traced too.
  const auto requested = qEnvironmentVariable("OSCILLATE_TRACE");
  if (!requested.isEmpty()) {
    launch_path_ = std::filesystem::path(requested.toStdString());
    start();
  }
}

PerformanceTrace::~PerformanceTrace() {
  if (recording_ && !launch_path_.empty())
    finish(launch_path_);
}

void PerformanceTrace::attach(QQuickWindow* window) {
  // The event loop's busy spells: from waking to blocking again.
  if (auto* dispatcher = QAbstractEventDispatcher::instance(thread())) {
    connect(dispatcher, &QAbstractEventDispatcher::awake, this, [this] {
      if (trace::enabled())
        busy_since_ = trace::now_ns();
    });
    connect(dispatcher, &QAbstractEventDispatcher::aboutToBlock, this, [this] {
      if (busy_since_ != 0)
        trace::span("ui.busy", 0, busy_since_, trace::now_ns());
      busy_since_ = 0;
    });
  }
  // Rendering runs on the scene graph's thread: direct connections.
  connect(
      window, &QQuickWindow::beforeRendering, this,
      [this] {
        // The basic render loop renders on the UI thread: keep its name.
        if (QThread::currentThread() != thread()) {
          thread_local const bool named = (trace::name_thread("render"), true);
          (void)named;
        }
        render_since_ = trace::enabled() ? trace::now_ns() : 0;
      },
      Qt::DirectConnection);
  connect(
      window, &QQuickWindow::afterRendering, this,
      [this] {
        if (render_since_ != 0)
          trace::span("ui.render", 0, render_since_, trace::now_ns());
      },
      Qt::DirectConnection);
  connect(
      window, &QQuickWindow::frameSwapped, this,
      [this] {
        if (!trace::enabled()) {
          last_frame_ = 0;
          return;
        }
        const auto now = trace::now_ns();
        if (last_frame_ != 0)
          trace::counter("ui.frame_interval_ms", static_cast<double>(now - last_frame_) / 1e6);
        last_frame_ = now;
      },
      Qt::DirectConnection);
  if (!launch_path_.empty()) {
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, [this] {
      if (recording_)
        finish(launch_path_);
    });
  }
}

void PerformanceTrace::start() {
  if (recording_)
    return;
  if (!trace::start()) {
    status_ = QStringLiteral("A trace is already recording");
  } else {
    recording_ = true;
    status_ = QStringLiteral("Recording a performance trace (%1 MiB buffer)")
                  .arg(static_cast<double>(trace::capacity_events() * trace::event_bytes) /
                           bytes_per_mebibyte,
                       0, 'f', 0);
  }
  foundation::log::info(foundation::log::Category::application, "{}", status_.toStdString());
  emit changed();
}

void PerformanceTrace::stop_and_save() {
  if (!recording_)
    return;
  if (!launch_path_.empty()) {
    finish(launch_path_);
    return;
  }
  const auto directory = traces_directory();
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  const auto stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
  finish(directory / ("trace-" + stamp.toStdString() + ".json"));
}

void PerformanceTrace::finish(const std::filesystem::path& path) {
  recording_ = false;
  const auto written = trace::stop_and_write(path);
  status_ = written ? QStringLiteral("Trace saved: %1 (%2 events)")
                          .arg(QString::fromStdString(path.string()))
                          .arg(written.value())
                    : QString::fromStdString(written.error().message);
  foundation::log::message(written ? foundation::log::Level::info : foundation::log::Level::error,
                           foundation::log::Category::application, "{}", status_.toStdString());
  emit changed();
}

} // namespace oscillate::quick
