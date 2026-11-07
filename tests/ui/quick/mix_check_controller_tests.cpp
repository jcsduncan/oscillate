#include "../../../apps/daw_quick/src/mix_check_controller.hpp"
#include "oscillate/test/check.hpp"

#include <QCoreApplication>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

// ADR-0074: the Mix Check screen's controller runs a check on a worker,
// fills its findings, applies a fix as one undo step and re-checks, goes
// stale on an edit, refuses while blocked, and Listen solos and restores.
namespace {
namespace application = oscillate::application;
namespace project = oscillate::project;
namespace quick = oscillate::quick;
namespace fs = std::filesystem;

constexpr std::uint32_t rate = 48'000;
constexpr double seconds = 6.0;
constexpr std::int16_t full_scale = 32'767;
constexpr int wait_ms = 120'000;

using oscillate::test::require;

class Directory {
public:
  Directory() {
    std::string pattern = (fs::temp_directory_path() / "oscillate-mix-check-ui-XXXXXX").string();
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

void put16(std::vector<char>& bytes, std::uint16_t value) {
  bytes.push_back(static_cast<char>(value & 0xFFU));
  bytes.push_back(static_cast<char>(value >> 8U));
}
void put32(std::vector<char>& bytes, std::uint32_t value) {
  put16(bytes, static_cast<std::uint16_t>(value));
  put16(bytes, static_cast<std::uint16_t>(value >> 16U));
}

// A 55 Hz bass over a 30 Hz rumble, 16-bit stereo.
fs::path rumbling_bass(const fs::path& path) {
  const auto frames = static_cast<std::uint32_t>(seconds * rate);
  std::vector<char> bytes{'R', 'I', 'F', 'F'};
  put32(bytes, 36 + frames * 4);
  for (const char c : std::string_view("WAVEfmt "))
    bytes.push_back(c);
  put32(bytes, 16);
  put16(bytes, 1);
  put16(bytes, 2);
  put32(bytes, rate);
  put32(bytes, rate * 4);
  put16(bytes, 4);
  put16(bytes, 16);
  for (const char c : std::string_view("data"))
    bytes.push_back(c);
  put32(bytes, frames * 4);
  for (std::uint32_t i = 0; i < frames; ++i) {
    const double t = static_cast<double>(i) / rate;
    const double x = 0.5 * std::sin(2.0 * std::numbers::pi * 55.0 * t) +
                     0.25 * std::sin(2.0 * std::numbers::pi * 30.0 * t);
    const auto sample =
        static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lround(x * full_scale)));
    put16(bytes, sample);
    put16(bytes, sample);
  }
  std::ofstream(path, std::ios::binary)
      .write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  return path;
}

[[nodiscard]] QString finding_id(const quick::MixCheckController& check, const QString& prefix) {
  for (const auto& entry : check.findings()) {
    const auto id = entry.toMap().value(QStringLiteral("id")).toString();
    if (id.startsWith(prefix))
      return id;
  }
  return {};
}

[[nodiscard]] QVariantMap finding(const quick::MixCheckController& check, const QString& id) {
  for (const auto& entry : check.findings())
    if (entry.toMap().value(QStringLiteral("id")).toString() == id)
      return entry.toMap();
  return {};
}

void the_screen_checks_fixes_and_listens() {
  Directory directory;
  auto session = application::ProjectSession::create(directory.path() / "song").value();
  const auto wav = rumbling_bass(directory.path() / "bass.wav");
  const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  require(fd >= 0, "fixture open");
  const auto bass = session->imports().import_wav_audio(fd).value();
  (void)::close(fd);
  require(session->saving().save().has_value(), "save");

  QString blocked;
  int commits = 0;
  std::vector<std::pair<project::TrackId, bool>> solos;
  std::vector<std::uint64_t> plays;
  int stops = 0;
  quick::MixCheckController check(
      {.session = [&] { return session.get(); },
       .blocked = [&] { return blocked; },
       .can_edit = [] { return true; },
       .commit =
           [&] {
             ++commits;
             (void)session->saving().save();
           },
       .project_name = [] { return QStringLiteral("Song"); },
       .set_solo =
           [&](project::TrackId track, bool soloed) {
             solos.emplace_back(track, soloed);
             require(session->tracks().set_track_soloed(track, soloed).has_value(), "solo");
           },
       .play_from = [&](std::uint64_t frame) { plays.push_back(frame); },
       .stop = [&] { ++stops; }});

  blocked = QStringLiteral("Stop playback to run Mix Check");
  check.run();
  require(!check.running() && check.status() == blocked, "a blocked check does not start");
  blocked.clear();

  check.run();
  require(check.running(), "the check runs in the background");
  require(check.wait(wait_ms), "the check finished");
  require(check.has_result(), "a result: " + check.status().toStdString());
  require(check.meters().size() == 5, "five meter tiles");
  require(check.spectrum().value(QStringLiteral("freqs")).toList().size() == 200 &&
              check.spectrum().value(QStringLiteral("after")).toList().isEmpty(),
          "the spectrum, with no after yet");
  require(check.targets().size() == 3 && check.target() == QStringLiteral("club"), "targets");
  require(check.summary().value(QStringLiteral("trackCount")).toInt() == 1, "one track checked");
  const auto sub = finding_id(check, QStringLiteral("sub:"));
  require(!sub.isEmpty(), "the rumble is listed");
  const auto shown = finding(check, sub);
  require(shown.value(QStringLiteral("severity")).toString() == QStringLiteral("bad") &&
              shown.value(QStringLiteral("fix")).toMap().value(QStringLiteral("device")) ==
                  QStringLiteral("Contour") &&
              !shown.value(QStringLiteral("evidence"))
                   .toMap()
                   .value(QStringLiteral("x"))
                   .toList()
                   .isEmpty(),
          "with its fix and evidence");

  // Listen solos the bass and plays; stopping restores the solo states.
  check.listen(sub);
  require(check.listening() == sub && plays.size() == 1 && solos.size() == 1 &&
              solos.front() == std::pair{bass, true},
          "Listen solos the track and plays");
  check.project_changed();
  require(!check.stale(), "a solo does not make the check stale");
  check.stopListening();
  require(check.listening().isEmpty() && stops == 1 && solos.back() == std::pair{bass, false},
          "stopping restores the solo");

  // Apply: one undo step, then a re-check marks it fixed.
  const auto steps = session->history().undo_steps().size();
  check.apply(sub);
  require(commits == 1 && session->history().undo_steps().size() == steps + 1, "one undo step");
  require(check.wait(wait_ms), "the re-check finished");
  const auto fixed = finding(check, sub);
  require(fixed.value(QStringLiteral("applied")).toBool() &&
              !fixed.value(QStringLiteral("result")).toString().isEmpty(),
          "the finding reads fixed: " + check.status().toStdString());
  require(!check.spectrum().value(QStringLiteral("after")).toList().isEmpty(),
          "the spectrum gains an after curve");
  require(!check.since_last().isEmpty(), "since the last check");

  // An edit makes the result stale.
  require(session->tracks().set_track_level_pan(bass, 0.5, 0.0).has_value(), "edit");
  check.project_changed();
  require(check.stale(), "an edit makes the check stale");

  check.set_target(QStringLiteral("stream"));
  require(check.target() == QStringLiteral("stream"), "the target changes");
}

// A run that finishes after its session was replaced, here by a copy of
// the same project with the same tracks, is dropped: its findings name
// tracks the copy shares and must not be offered there.
void a_run_for_a_replaced_session_is_dropped() {
  Directory directory;
  const auto root = directory.path() / "song";
  auto session = application::ProjectSession::create(root).value();
  const auto wav = rumbling_bass(directory.path() / "bass.wav");
  const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  require(fd >= 0, "fixture open");
  (void)session->imports().import_wav_audio(fd).value();
  (void)::close(fd);
  require(session->saving().save().has_value(), "save");
  const auto copy_root = directory.path() / "copy";
  fs::copy(root, copy_root, fs::copy_options::recursive);
  auto copy = application::ProjectSession::open(copy_root).value();
  application::ProjectSession* current = session.get();
  quick::MixCheckController check({.session = [&] { return current; },
                                   .blocked = [] { return QString(); },
                                   .can_edit = [] { return true; },
                                   .commit = [] {},
                                   .project_name = [] { return QStringLiteral("Song"); },
                                   .set_solo = [](project::TrackId, bool) {},
                                   .play_from = [](std::uint64_t) {},
                                   .stop = [] {}});
  check.run();
  require(check.running(), "the check did not start");
  current = copy.get();
  require(check.wait(wait_ms), "the check did not finish");
  require(!check.has_result() && check.findings().isEmpty() &&
              check.status().contains(QStringLiteral("dropped")),
          "a report was shown for the session that replaced its own: " +
              check.status().toStdString());
}

} // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  try {
    the_screen_checks_fixes_and_listens();
    a_run_for_a_replaced_session_is_dropped();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "mix check controller contract passed\n";
  return 0;
}
