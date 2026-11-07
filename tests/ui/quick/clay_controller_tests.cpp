#include "../../../apps/daw_quick/src/clay_controller.hpp"
#include "oscillate/test/check.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QStandardPaths>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <numbers>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <vector>

// ADR-0081 phase 2: the Notes tab's controller detects a clip's notes on a
// worker, shows them, turns each drag into one undo step, harmonises, sends
// the notes to MIDI, warms the playback render and writes auditions.
namespace {
namespace application = oscillate::application;
namespace project = oscillate::project;
namespace quick = oscillate::quick;
namespace fs = std::filesystem;

constexpr std::uint32_t rate = 48'000;
constexpr double two_pi = 2.0 * std::numbers::pi;
constexpr std::array<double, 3> sung_hz{220.0, 261.63, 329.63};
constexpr double note_seconds = 0.4;
constexpr double gap_seconds = 0.15;
constexpr int harmonics = 8;
constexpr int wait_ms = 60'000;
constexpr std::int16_t full_scale = 32'767;
// How long the slow decoder waits to be released before giving up, so a
// controller that waits for it fails the test instead of hanging.
constexpr auto release_timeout = std::chrono::seconds(2);

using oscillate::test::require;

template <typename T> T take(oscillate::foundation::Result<T> result) {
  if (!result)
    throw std::runtime_error(result.error().message);
  return std::move(result).value();
}

class Directory {
public:
  Directory() {
    std::string pattern = (fs::temp_directory_path() / "oscillate-clay-ui-XXXXXX").string();
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

void put16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value));
  bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void put32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
  put16(bytes, static_cast<std::uint16_t>(value));
  put16(bytes, static_cast<std::uint16_t>(value >> 16U));
}

fs::path write_sung(const fs::path& path) {
  const double length =
      static_cast<double>(sung_hz.size()) * (note_seconds + gap_seconds) + gap_seconds;
  const auto frames = static_cast<std::uint32_t>(length * rate);
  std::vector<std::uint8_t> bytes{'R', 'I', 'F', 'F'};
  put32(bytes, 36 + frames * 4);
  bytes.insert(bytes.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  put32(bytes, 16);
  put16(bytes, 1);
  put16(bytes, 2);
  put32(bytes, rate);
  put32(bytes, rate * 4);
  put16(bytes, 4);
  put16(bytes, 16);
  bytes.insert(bytes.end(), {'d', 'a', 't', 'a'});
  put32(bytes, frames * 4);
  double phase = 0.0;
  for (std::uint32_t i = 0; i < frames; ++i) {
    const double t = static_cast<double>(i) / rate;
    const double slot = t - gap_seconds;
    const auto index = static_cast<std::size_t>(std::max(0.0, slot) / (note_seconds + gap_seconds));
    const double within = slot - static_cast<double>(index) * (note_seconds + gap_seconds);
    double x = 0.0;
    if (slot >= 0.0 && index < sung_hz.size() && within < note_seconds) {
      phase += two_pi * sung_hz[index] / rate;
      const double edge = std::min({1.0, within / 0.02, (note_seconds - within) / 0.02});
      for (int h = 1; h <= harmonics; ++h)
        x += std::sin(phase * h) / h;
      x *= 0.25 * edge;
    }
    for (int channel = 0; channel < 2; ++channel)
      put16(bytes, static_cast<std::uint16_t>(static_cast<std::int16_t>(
                       std::lround(std::clamp(x, -1.0, 1.0) * full_scale))));
  }
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  require(output.good(), "WAV fixture write failed");
  return path;
}

QVariantMap note_at(const QVariantList& notes, int index) { return notes.at(index).toMap(); }

void the_notes_tab_edits_a_clip() {
  Directory directory;
  auto session = take(application::ProjectSession::create(directory.path() / "song"));
  const auto wav = write_sung(directory.path() / "vocal.wav");
  const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  require(fd >= 0, "fixture open failed");
  const auto track = take(session->imports().import_wav_audio(fd));
  (void)::close(fd);
  require(static_cast<bool>(session->saving().save()), "the project saves");
  const auto clip = session->snapshot().tracks.front().clips.front().id;
  int commits = 0;
  QString reported;
  quick::ClayController controller(
      [&] { return session.get(); }, [] { return true; },
      [&](QString message) { reported = std::move(message); }, [&] { ++commits; },
      [](const QString&) { return std::optional<oscillate::persistence::GrooveTemplate>{}; },
      [&] { (void)session->history().undo(); }, [&] { (void)session->history().redo(); });

  controller.open_clip(QString::fromStdString(clip.to_string()));
  require(controller.is_open() && !controller.has_take(), "a clip without a take waits");
  require(controller.clip_end() > controller.clip_start(), "the clip has a span");
  const auto steps = session->history().undo_steps().size();
  controller.detect(QStringLiteral("melodic"));
  require(controller.wait_for_detect(wait_ms), "detection finishes");
  require(controller.has_take() && controller.mode() == QStringLiteral("melodic"),
          "detecting saves a take: " + controller.status().toStdString());
  require(session->history().undo_steps().size() == steps + 1, "as one undo step");
  const auto notes = controller.notes();
  require(notes.size() == static_cast<int>(sung_hz.size()),
          "the three sung notes are shown: " + std::to_string(notes.size()));
  require(controller.waveform().size() == 2 * quick::ClayController::waveform_columns(),
          "with the waveform behind them");
  const auto first = note_at(notes, 0);
  require(std::abs(first.value(QStringLiteral("centre")).toDouble() - 5'700.0) < 20.0,
          "the first note is A3");
  require(!first.value(QStringLiteral("curve")).toList().isEmpty(), "with its pitch curve");

  // A pitch drag previews, then commits as one step.
  const auto key = first.value(QStringLiteral("key")).toString();
  controller.select(key, false);
  controller.setProperty("snap", QStringLiteral("chromatic"));
  require(controller.begin_edit(QStringLiteral("pitch")), "a pitch drag starts");
  controller.update_edit(0.0, 180.0, 1.0);
  require(controller.previewing(), "and previews");
  require(session->history().undo_steps().size() == steps + 1, "without an undo step yet");
  controller.commit_edit();
  require(session->history().undo_steps().size() == steps + 2, "releasing is one step");
  const auto moved = note_at(controller.notes(), 0);
  require(std::abs(moved.value(QStringLiteral("centre")).toDouble() - 5'900.0) < 0.5,
          "snapped two semitones up to B3");

  // Harmonise adds a voice per selected note.
  controller.harmonise(2, 0.0, -6.0);
  require(controller.notes().size() == static_cast<int>(sung_hz.size()) + 1,
          "a harmony joins the notes");
  require(std::ranges::any_of(controller.notes(),
                              [](const QVariant& n) {
                                return n.toMap().value(QStringLiteral("harmony")).toBool();
                              }),
          "marked as a harmony");

  // Undo through the controller goes back a step (the bridge then reports
  // the new revision).
  controller.undo();
  controller.project_changed();
  require(controller.notes().size() == static_cast<int>(sung_hz.size()),
          "undo takes the harmony back");
  controller.redo();
  controller.project_changed();
  require(controller.notes().size() == static_cast<int>(sung_hz.size()) + 1, "redo restores it");

  // To MIDI on a new Nova track.
  const auto tracks = session->snapshot().tracks.size();
  controller.select_all();
  controller.to_midi(QString{});
  require(session->snapshot().tracks.size() == tracks + 1,
          "To MIDI adds a Nova track: " + controller.status().toStdString());
  require(!session->snapshot().tracks.back().midi_clips.empty(), "with the notes");

  // The playback render warms up, and an audition is written.
  require(controller.wait_for_render(wait_ms) &&
              controller.render_state() == QStringLiteral("ready"),
          "the render is warm: " + controller.render_state().toStdString());
  int auditioned = 0;
  QObject::connect(&controller, &quick::ClayController::auditionReady,
                   [&](const QString&) { ++auditioned; });
  controller.audition_selection();
  require(controller.wait_for_render(wait_ms) && auditioned == 1, "an audition is ready");
  require(QFileInfo::exists(controller.audition_path()), "its file exists");
  require(commits > 0, "edits tell the bridge to refresh playback");
  (void)track;
}

// A detection that finishes after its session was replaced, here by a copy
// of the same project with the same clip and revision, is dropped: the copy
// gains no take.
void a_detection_for_a_replaced_session_is_dropped() {
  Directory directory;
  const auto root = directory.path() / "song";
  const auto copy_root = directory.path() / "copy";
  auto session = take(application::ProjectSession::create(root));
  const auto wav = write_sung(directory.path() / "vocal.wav");
  const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  require(fd >= 0, "fixture open failed");
  (void)take(session->imports().import_wav_audio(fd));
  (void)::close(fd);
  require(static_cast<bool>(session->saving().save()), "the project saves");
  fs::copy(root, copy_root, fs::copy_options::recursive);
  auto copy = take(application::ProjectSession::open(copy_root));
  const auto clip = session->snapshot().tracks.front().clips.front().id;
  application::ProjectSession* current = session.get();
  int commits = 0;
  quick::ClayController controller(
      [&] { return current; }, [] { return true; }, [](const QString&) {}, [&] { ++commits; },
      [](const QString&) { return std::optional<oscillate::persistence::GrooveTemplate>{}; }, [] {},
      [] {});
  controller.open_clip(QString::fromStdString(clip.to_string()));
  controller.detect(QStringLiteral("melodic"));
  require(controller.detecting(), "detection did not start");
  current = copy.get();
  const auto revision = copy->snapshot().revision;
  require(controller.wait_for_detect(wait_ms), "detection did not finish");
  require(commits == 0 && copy->snapshot().revision == revision && !controller.has_take() &&
              controller.status().contains(QStringLiteral("dropped")),
          "a detection was saved in the session that replaced its own: " +
              controller.status().toStdString());
}

// Closing the editor while detection is still reading the audio (a slow
// disk) neither waits for the read nor keeps its result: the worker is told
// to stop, left to finish on its own and joined once it has.
void cancelling_a_detection_never_waits_for_the_disk() {
  Directory directory;
  auto session = take(application::ProjectSession::create(directory.path() / "song"));
  const auto wav = write_sung(directory.path() / "vocal.wav");
  const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  require(fd >= 0, "fixture open failed");
  (void)take(session->imports().import_wav_audio(fd));
  (void)::close(fd);
  require(static_cast<bool>(session->saving().save()), "the project saves");
  const auto clip = session->snapshot().tracks.front().clips.front().id;
  quick::ClayController controller(
      [&] { return session.get(); }, [] { return true; }, [](const QString&) {}, [] {},
      [](const QString&) { return std::optional<oscillate::persistence::GrooveTemplate>{}; }, [] {},
      [] {});
  std::binary_semaphore entered{0};
  std::binary_semaphore release{0};
  std::atomic<bool> told_to_stop{false};
  controller.set_decoder([&](const auto&, const auto&, const std::stop_token& stop) {
    entered.release();
    (void)release.try_acquire_for(release_timeout);
    told_to_stop.store(stop.stop_requested());
    return oscillate::foundation::Result<oscillate::processing::clay::DecodedSource>::failure(
        {oscillate::foundation::ErrorCode::cancelled, "slow disk"});
  });
  controller.open_clip(QString::fromStdString(clip.to_string()));
  controller.detect(QStringLiteral("melodic"));
  require(entered.try_acquire_for(release_timeout), "detection did not read the audio");
  controller.close();
  require(!controller.detecting() && controller.abandoned_detections() == 1,
          "closing waited for the detection's read");
  release.release();
  QElapsedTimer waited;
  waited.start();
  while (controller.abandoned_detections() != 0 && waited.elapsed() < wait_ms)
    QCoreApplication::processEvents();
  require(controller.abandoned_detections() == 0 && told_to_stop.load(),
          "the cancelled detection was not told to stop or never finished");
  require(!controller.has_take() && !controller.is_open(), "a cancelled detection left a take");
}

} // namespace

int main(int argc, char** argv) {
  QStandardPaths::setTestModeEnabled(true);
  QCoreApplication app(argc, argv);
  try {
    the_notes_tab_edits_a_clip();
    a_detection_for_a_replaced_session_is_dropped();
    cancelling_a_detection_never_waits_for_the_disk();
  } catch (const std::exception& error) {
    std::cerr << "clay controller: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "clay controller contract passed\n";
  return EXIT_SUCCESS;
}
