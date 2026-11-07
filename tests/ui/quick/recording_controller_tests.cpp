#include "../../../apps/daw_quick/src/recording_controller.hpp"

#include "oscillate/application/project_session.hpp"
#include "oscillate/test/check.hpp"

#include <QCoreApplication>
#include <QStandardPaths>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>

// ADR-0086: Record arms a take and starts the song (with a count-in when
// on); keys played while it plays are written where they were heard into
// the track they play, each as it is let go and the take one undo step; a
// key held at Stop ends there; nothing is taken while counting in or
// stopped.
namespace {
namespace application = oscillate::application;
namespace project = oscillate::project;
namespace quick = oscillate::quick;
namespace fs = std::filesystem;

constexpr std::int64_t quarter = oscillate::timeline::ticks_per_quarter;
constexpr std::int64_t bar = 4 * quarter;
// The default song: 120 BPM, 48 kHz, so a quarter is 24000 frames.
constexpr std::uint64_t quarter_frames = 24'000;
constexpr std::uint64_t bar_frames = 4 * quarter_frames;

using oscillate::test::require;

class Directory {
public:
  Directory() {
    std::string pattern = (fs::temp_directory_path() / "oscillate-recording-ui-XXXXXX").string();
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

// A transport the test moves by hand.
struct FakeTransport {
  bool playing = false;
  bool counting_in = false;
  std::uint64_t heard = 0;
  int plays = 0;
  bool asked_count_in = false;
  int commits = 0;
  bool clicking = false;
};

void a_take_records_where_it_was_heard() {
  Directory directory;
  auto session = std::move(application::ProjectSession::create(directory.path() / "song")).value();
  const auto track = session->instruments().add_synth_track().value();
  const auto track_id = QString::fromStdString(track.to_string());
  FakeTransport transport;
  quick::RecordingController recorder(
      {.session = [&] { return session.get(); },
       .playing = [&] { return transport.playing; },
       .heard_sample = [&] { return std::optional{transport.heard}; },
       .loop = [] { return std::optional<application::LoopSamples>{}; },
       .play =
           [&](bool count_in) {
             ++transport.plays;
             transport.asked_count_in = count_in;
           },
       .device_counting_in = [&] { return transport.counting_in; },
       .click_changed = [&](bool on) { transport.clicking = on; },
       .bar_at = [](std::uint64_t sample) { return static_cast<double>(sample) / bar_frames; },
       .commit = [&] { ++transport.commits; },
       .status = [](const QString&) {}});
  recorder.set_count_in(true);
  recorder.set_click(true);
  require(!transport.clicking, "plain playback never clicks");

  // Keys played while stopped are not taken.
  recorder.note_on(track_id, 60, 100);
  recorder.note_off(60);
  require(session->snapshot().tracks.front().midi_clips.empty(), "nothing records while stopped");

  recorder.toggle_record();
  require(recorder.armed() && transport.plays == 1 && transport.asked_count_in,
          "Record from stop starts the song after a count-in");
  require(transport.clicking, "the click sounds while recording");
  // Counting in: the device holds the song; keys are not taken yet.
  transport.playing = true;
  transport.counting_in = true;
  recorder.transport_changed();
  require(recorder.counting_in() && !recorder.recording(), "counting in is not recording");
  recorder.note_on(track_id, 62, 100);
  recorder.note_off(62);
  require(session->snapshot().tracks.front().midi_clips.empty(), "the count-in takes no notes");

  transport.counting_in = false;
  transport.heard = bar_frames + quarter_frames; // bar 2, beat 2
  recorder.transport_changed();
  require(recorder.recording(), "recording once the count-in ends");
  recorder.note_on(track_id, 64, 90);
  require(recorder.held_notes().size() == 1, "a held note is drawn");
  transport.heard += quarter_frames;
  recorder.transport_changed();
  const auto drawn = recorder.held_notes().front().toMap();
  require(drawn.value(QStringLiteral("endBar")).toDouble() >
              drawn.value(QStringLiteral("startBar")).toDouble(),
          "it grows with the song");
  recorder.note_off(64);
  {
    const auto& clips = session->snapshot().tracks.front().midi_clips;
    require(clips.size() == 1 && clips.front().notes.size() == 1, "the note is written as it ends");
    const auto& note = clips.front().notes.front();
    const auto& span = std::get<project::MusicalInterval>(clips.front().placement);
    require(span.start.value == bar && note.start.value == quarter &&
                note.end.value == 2 * quarter && note.key == 64 && note.velocity == 90,
            "where it was heard, in a clip from its bar");
  }
  require(transport.commits == 1, "the views follow");
  // Another note, held when the song stops: it ends where the song stopped.
  recorder.note_on(track_id, 67, 80);
  transport.heard += quarter_frames;
  recorder.stopped();
  require(!recorder.armed() && !transport.clicking, "Stop disarms and the click stops");
  const auto& notes = session->snapshot().tracks.front().midi_clips.front().notes;
  require(notes.size() == 2, "the held note is kept");
  // One undo removes the take.
  require(session->history().undo().has_value() &&
              session->snapshot().tracks.front().midi_clips.empty(),
          "the whole take is one undo step");
}

} // namespace

int main(int argc, char** argv) {
  QStandardPaths::setTestModeEnabled(true);
  QCoreApplication app(argc, argv);
  try {
    a_take_records_where_it_was_heard();
  } catch (const std::exception& error) {
    std::cerr << "recording controller: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
