#include "performance_trace.hpp"

#include "oscillate/foundation/log.hpp"
#include "oscillate/foundation/trace.hpp"

#include <QAbstractEventDispatcher>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QQuickWindow>
#include <QThread>

#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace oscillate::quick {
namespace trace = foundation::trace;

namespace {

constexpr double bytes_per_mebibyte = 1024.0 * 1024.0;
// How often a save in progress is checked for its result.
constexpr int save_poll_ms = 50;

constexpr std::string_view trace_prefix = "trace-";
constexpr std::string_view trace_extension = ".json";

} // namespace

std::filesystem::path traces_directory() {
  QString data = qEnvironmentVariable("XDG_DATA_HOME");
  if (data.isEmpty())
    data = QDir::homePath() + QStringLiteral("/.local/share");
  return std::filesystem::path((data + QStringLiteral("/oscillate/traces")).toStdString());
}

std::optional<std::filesystem::path> latest_trace() {
  // The names hold a zero-padded local time, so the greatest is the newest.
  std::optional<std::filesystem::path> latest;
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(traces_directory(), error)) {
    const auto name = entry.path().filename().string();
    if (!entry.is_regular_file(error) || !name.starts_with(trace_prefix) ||
        !name.ends_with(trace_extension))
      continue;
    if (!latest || name > latest->filename().string())
      latest = entry.path();
  }
  return latest;
}

PerformanceTrace::PerformanceTrace(QObject* parent, Writer writer)
    : QObject(parent), writer_(writer ? std::move(writer) : Writer(trace::stop_and_write)),
      timer_(this) {
  timer_.setInterval(save_poll_ms);
  connect(&timer_, &QTimer::timeout, this, &PerformanceTrace::poll);
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
  wait_for_save();
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
      wait_for_save();
    });
  }
}

void PerformanceTrace::start() {
  if (recording_)
    return;
  if (saving()) {
    status_ = QStringLiteral("Still saving the last trace");
  } else if (!trace::start()) {
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
  if (!recording_ || saving())
    return;
  if (!launch_path_.empty()) {
    finish(launch_path_);
    return;
  }
  const auto directory = traces_directory();
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  const auto stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
  finish(directory /
         (std::string(trace_prefix) + stamp.toStdString() + std::string(trace_extension)));
}

void PerformanceTrace::finish(const std::filesystem::path& path) {
  recording_ = false;
  const auto failed = save_.start(
      0, [writer = writer_, path] { return writer(path); },
      [](std::string what) {
        return foundation::Result<std::size_t>::failure(
            {foundation::ErrorCode::internal, "Trace not saved: " + what});
      });
  if (failed) {
    // The writer never ran, so the recording is still on: end it, or no
    // later trace could start.
    trace::stop();
    report(path, foundation::Result<std::size_t>::failure(
                     {foundation::ErrorCode::unavailable, "Trace not saved: " + *failed}));
    return;
  }
  saving_path_ = path;
  status_ = QStringLiteral("Saving the trace to %1").arg(QString::fromStdString(path.string()));
  timer_.start();
  emit changed();
}

void PerformanceTrace::poll() {
  if (!save_.finished())
    return;
  timer_.stop();
  auto finished = save_.take();
  report(saving_path_, finished ? std::move(finished->result)
                                : foundation::Result<std::size_t>::failure(
                                      {foundation::ErrorCode::internal, "Trace not saved"}));
}

void PerformanceTrace::wait_for_save() {
  if (!save_.running())
    return;
  save_.wait();
  poll();
}

void PerformanceTrace::report(const std::filesystem::path& path,
                              const foundation::Result<std::size_t>& written) {
  status_ = written ? QStringLiteral("Trace saved: %1 (%2 events)")
                          .arg(QString::fromStdString(path.string()))
                          .arg(written.value())
                    : QString::fromStdString(written.error().message);
  foundation::log::message(written ? foundation::log::Level::info : foundation::log::Level::error,
                           foundation::log::Category::application, "{}", status_.toStdString());
  emit changed();
}

} // namespace oscillate::quick
