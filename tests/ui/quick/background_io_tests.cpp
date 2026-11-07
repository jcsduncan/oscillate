#include "../../../apps/daw_quick/src/performance_trace.hpp"
#include "oscillate/foundation/trace.hpp"
#include "oscillate/test/check.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QString>
#include <QTest>

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <semaphore>
#include <string>
#include <system_error>

// Blocking file I/O runs on a worker, never on the GUI thread: each
// controller here is given a slow fake (a writer or reader that waits to be
// released) and must return at once, report that it is busy, and report the
// result or the failure once the worker finishes.
namespace {
namespace foundation = oscillate::foundation;
namespace quick = oscillate::quick;
namespace fs = std::filesystem;

using oscillate::test::require;

// How long a fake waits to be released before giving up, so a controller
// that does the I/O on the calling thread fails the test instead of hanging.
constexpr auto release_timeout = std::chrono::seconds(2);
// How long a test waits for a worker to report back.
constexpr int worker_wait_ms = 5000;
// The step a test waits in while a worker runs.
constexpr int wait_step_ms = 1;
// Events the fake trace writer reports.
constexpr std::size_t written_events = 42;

class Directory {
public:
  Directory() {
    std::string pattern = (fs::temp_directory_path() / "oscillate-background-io-XXXXXX").string();
    require(::mkdtemp(pattern.data()) != nullptr, "fixture directory creation failed");
    path_ = pattern;
  }
  ~Directory() {
    std::error_code ignored;
    fs::remove_all(path_, ignored);
  }
  [[nodiscard]] const fs::path& path() const { return path_; }

private:
  fs::path path_;
};

// A slow disk: the I/O starts, then waits until the test releases it.
struct SlowIo {
  std::binary_semaphore entered{0};
  std::binary_semaphore release{0};
  void wait() {
    entered.release();
    (void)release.try_acquire_for(release_timeout);
  }
};

template <typename Done> void wait_until(Done done, const char* what) {
  QElapsedTimer waited;
  waited.start();
  while (!done() && waited.elapsed() < worker_wait_ms)
    QTest::qWait(wait_step_ms);
  require(done(), what);
}

void a_trace_saves_in_the_background() {
  Directory directory;
  qputenv("XDG_DATA_HOME", QByteArray::fromStdString(directory.path().string()));
  SlowIo io;
  fs::path written_to;
  quick::PerformanceTrace tracer(nullptr, [&](const fs::path& path) {
    io.wait();
    written_to = path;
    return foundation::Result<std::size_t>::success(written_events);
  });
  tracer.start();
  require(tracer.recording(), "the trace did not start");
  tracer.stop_and_save();
  require(io.entered.try_acquire_for(release_timeout), "the trace was not written");
  require(tracer.saving() && !tracer.recording() &&
              tracer.status().startsWith(QStringLiteral("Saving")),
          "stop_and_save waited for the disk: " + tracer.status().toStdString());
  tracer.start();
  require(!tracer.recording() && tracer.status().contains(QStringLiteral("Still saving")),
          "a new trace started while the last was saving");
  io.release.release();
  wait_until([&] { return !tracer.saving(); }, "the trace save did not finish");
  require(tracer.status().startsWith(QStringLiteral("Trace saved")) &&
              tracer.status().endsWith(QStringLiteral("(%1 events)").arg(written_events)) &&
              written_to.parent_path() == directory.path() / "oscillate" / "traces",
          "the saved trace was not reported: " + tracer.status().toStdString());
  // The fake writer did not end the real recording.
  foundation::trace::stop();
}

void a_failed_trace_save_is_reported() {
  Directory directory;
  qputenv("XDG_DATA_HOME", QByteArray::fromStdString(directory.path().string()));
  SlowIo io;
  quick::PerformanceTrace tracer(nullptr, [&](const fs::path&) {
    io.wait();
    return foundation::Result<std::size_t>::failure(
        {foundation::ErrorCode::io_failure, "the disk is full"});
  });
  tracer.start();
  tracer.stop_and_save();
  require(io.entered.try_acquire_for(release_timeout), "the trace was not written");
  require(tracer.saving(), "stop_and_save waited for the disk");
  io.release.release();
  wait_until([&] { return !tracer.saving(); }, "the failed trace save did not finish");
  require(tracer.status().contains(QStringLiteral("the disk is full")),
          "the failed save was not reported: " + tracer.status().toStdString());
  foundation::trace::stop();
}

} // namespace

int main(int argc, char** argv) {
  qunsetenv("OSCILLATE_TRACE");
  QCoreApplication app(argc, argv);
  try {
    a_trace_saves_in_the_background();
    a_failed_trace_save_is_reported();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
}
