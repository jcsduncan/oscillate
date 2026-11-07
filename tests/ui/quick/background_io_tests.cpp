#include "../../../apps/daw_quick/src/background_job.hpp"
#include "../../../apps/daw_quick/src/import_controller.hpp"
#include "../../../apps/daw_quick/src/note_tools_controller.hpp"
#include "../../../apps/daw_quick/src/performance_trace.hpp"
#include "oscillate/application/project_session.hpp"
#include "oscillate/foundation/trace.hpp"
#include "oscillate/persistence/groove_file.hpp"
#include "oscillate/test/check.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QString>
#include <QTest>
#include <QUrl>
#include <QVariantMap>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <memory>
#include <semaphore>
#include <string>
#include <sys/stat.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>

// Blocking file I/O runs on a worker, never on the GUI thread: each
// controller here is given a slow fake (a writer or reader that waits to be
// released) and must return at once, report that it is busy, and report the
// result or the failure once the worker finishes.
namespace {
namespace application = oscillate::application;
namespace foundation = oscillate::foundation;
namespace persistence = oscillate::persistence;
namespace project = oscillate::project;
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
// How often a slow file's feeder looks for its reader.
constexpr auto feeder_step = std::chrono::milliseconds(1);
// A FIFO only its owner reads and writes.
constexpr mode_t fifo_mode = 0600;
// A Standard MIDI File, format 0 at 480 ticks a quarter, holding one middle C
// a quarter note long.
constexpr std::array<std::uint8_t, 35> one_note_midi{
    'M',  'T',  'h',  'd',  0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x01, 0x01, 0xE0, // header
    'M',  'T',  'r',  'k',  0x00, 0x00, 0x00, 0x0D,                                     // 13 bytes
    0x00, 0x90, 0x3C, 0x64,                                                             // note on
    0x83, 0x60, 0x80, 0x3C, 0x00, // off at 480
    0x00, 0xFF, 0x2F, 0x00};      // end of track
constexpr std::size_t imported_notes = 1;

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

// A slow file: a FIFO, so whoever opens it to read blocks until the test
// releases its bytes. A feeder thread writes them on release, or after
// release_timeout, so a controller reading on the test's thread fails the
// test instead of hanging it.
class SlowFile {
public:
  SlowFile(fs::path path, QByteArray bytes) : path_(std::move(path)), bytes_(std::move(bytes)) {
    require(::mkfifo(path_.c_str(), fifo_mode) == 0, "fixture FIFO creation failed");
    feeder_ = std::jthread([this] { feed(); });
  }
  [[nodiscard]] const fs::path& path() const { return path_; }
  void release() { released_.release(); }

private:
  void feed() {
    (void)released_.try_acquire_for(release_timeout);
    // Without a reader a non-blocking open fails; wait for one.
    const auto deadline = std::chrono::steady_clock::now() + release_timeout;
    int fd = -1;
    while (fd < 0 && std::chrono::steady_clock::now() < deadline) {
      fd = ::open(path_.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
      if (fd < 0)
        std::this_thread::sleep_for(feeder_step);
    }
    if (fd < 0)
      return;
    [[maybe_unused]] const auto written =
        ::write(fd, bytes_.constData(), static_cast<std::size_t>(bytes_.size()));
    (void)::close(fd);
  }

  fs::path path_;
  QByteArray bytes_;
  std::binary_semaphore released_{0};
  std::jthread feeder_;
};

template <typename Done> void wait_until(Done done, const char* what) {
  QElapsedTimer waited;
  waited.start();
  while (!done() && waited.elapsed() < worker_wait_ms)
    QTest::qWait(wait_step_ms);
  require(done(), what);
}

// The runner itself: work given up, or replaced by new work, is never
// waited for; its thread is joined once it finishes.
void abandoned_work_is_never_waited_for() {
  constexpr int first_result = 1;
  constexpr int second_result = 2;
  const auto failure = [](const std::string&) { return 0; };
  quick::BackgroundJob<int> job;
  SlowIo io;
  require(!job.start(
              0,
              [&] {
                io.wait();
                return first_result;
              },
              failure),
          "the first work did not start");
  require(io.entered.try_acquire_for(release_timeout), "the first work did not run");
  require(!job.start(
              0, [] { return second_result; }, failure) &&
              job.abandoned() == 1,
          "starting new work waited for the running work");
  wait_until([&] { return job.finished(); }, "the second work did not finish");
  const auto taken = job.take();
  require(taken && taken->result == second_result, "the abandoned work's result was taken");
  io.release.release();
  wait_until([&] { return job.abandoned() == 0; }, "the abandoned work was never joined");
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

struct ImportFixture {
  Directory directory;
  std::unique_ptr<application::ProjectSession> session =
      application::ProjectSession::create(directory.path() / "song").value();
  QString status;
  quick::ImportController imports{
      {.session = [this] { return session.get(); },
       .idle = [] { return true; },
       .control_busy = [] { return false; },
       .started = [] {},
       .refresh = [] {},
       .submit_playback_edit = [] {},
       .track_created = [](int) {},
       .revision = [this] { return session->snapshot().revision; },
       .status = [this](QString message) { status = std::move(message); }}};
};

[[nodiscard]] std::size_t notes(const application::ProjectSession& session) {
  std::size_t count = 0;
  for (const auto& track : session.snapshot().tracks)
    for (const auto& clip : track.midi_clips)
      count += clip.notes.size();
  return count;
}

void a_midi_file_is_read_in_the_background() {
  ImportFixture fixture;
  SlowFile file(fixture.directory.path() / "riff.mid",
                QByteArray(reinterpret_cast<const char*>(one_note_midi.data()),
                           static_cast<qsizetype>(one_note_midi.size())));
  fixture.imports.import_midi(QUrl::fromLocalFile(QString::fromStdString(file.path().string())));
  require(fixture.imports.import_busy() && !fixture.imports.import_enabled() &&
              notes(*fixture.session) == 0,
          "the MIDI import waited for the disk: " + fixture.status.toStdString());
  file.release();
  wait_until([&] { return !fixture.imports.import_busy(); }, "the MIDI file was not read");
  require(notes(*fixture.session) == imported_notes &&
              fixture.status.contains(QStringLiteral("riff imported")),
          "the read MIDI file was not imported: " + fixture.status.toStdString());
}

void a_missing_midi_file_is_reported() {
  ImportFixture fixture;
  fixture.imports.import_midi(QUrl::fromLocalFile(
      QString::fromStdString((fixture.directory.path() / "gone.mid").string())));
  wait_until([&] { return !fixture.imports.import_busy(); }, "the failed read did not finish");
  require(fixture.status.contains(QStringLiteral("cannot open")) && notes(*fixture.session) == 0,
          "the missing file was not reported: " + fixture.status.toStdString());
}

[[nodiscard]] bool pool_has(const quick::NoteToolsController& tools, const QString& name) {
  const auto pool = tools.grooves();
  return std::ranges::any_of(pool, [&](const QVariant& entry) {
    return entry.toMap().value(QStringLiteral("name")).toString() == name;
  });
}

void a_groove_is_imported_in_the_background() {
  Directory directory;
  qputenv("XDG_DATA_HOME", QByteArray::fromStdString((directory.path() / "data").string()));
  QString reported;
  quick::NoteToolsController tools([] { return nullptr; }, [] { return true; },
                                   [&](QString message) { reported = std::move(message); }, [] {});
  require(tools.wait_for_groove_files(worker_wait_ms), "the groove folder was not read");
  SlowFile file(
      directory.path() / "Shuffle.json",
      QByteArray::fromStdString(persistence::encode_groove({"Shuffle", 1, {{0.25, 1.0}}})));
  require(tools.import_groove(QString::fromStdString(file.path().string())),
          "the groove import did not start");
  require(tools.groove_files_busy() && !pool_has(tools, QStringLiteral("Shuffle")),
          "the groove import waited for the disk");
  file.release();
  require(tools.wait_for_groove_files(worker_wait_ms) && pool_has(tools, QStringLiteral("Shuffle")),
          "the imported groove did not join the pool: " + reported.toStdString());

  SlowFile broken(directory.path() / "Broken.json", QByteArrayLiteral("not a groove"));
  require(tools.import_groove(QString::fromStdString(broken.path().string())),
          "the second import did not start");
  broken.release();
  require(tools.wait_for_groove_files(worker_wait_ms) &&
              reported == QStringLiteral("Not a groove file") &&
              !pool_has(tools, QStringLiteral("Broken")),
          "a broken groove file was not reported: " + reported.toStdString());
}

} // namespace

int main(int argc, char** argv) {
  qunsetenv("OSCILLATE_TRACE");
  QCoreApplication app(argc, argv);
  try {
    abandoned_work_is_never_waited_for();
    a_trace_saves_in_the_background();
    a_failed_trace_save_is_reported();
    a_midi_file_is_read_in_the_background();
    a_missing_midi_file_is_reported();
    a_groove_is_imported_in_the_background();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
}
