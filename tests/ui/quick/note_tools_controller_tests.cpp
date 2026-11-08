// The piano roll's tools controller (ADR-0059): scale lock, chord stamps,
// grooves from the pool and from a user file, each one undo step.

#include "../../../apps/daw_quick/src/note_tools_controller.hpp"
#include "oscillate/test/check.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>

namespace {
namespace application = oscillate::application;
namespace project = oscillate::project;
namespace quick = oscillate::quick;
namespace fs = std::filesystem;
using oscillate::timeline::ticks_per_quarter;

using oscillate::test::require;
// How long groove file work may take in the background.
constexpr int groove_wait_ms = 5000;

class Directory {
public:
  Directory() {
    std::string pattern = (fs::temp_directory_path() / "oscillate-note-tools-ui-XXXXXX").string();
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

void tools_edit_in_one_step() {
  Directory directory;
  // User grooves live under the fixture, never the user's own data.
  ::setenv("XDG_DATA_HOME", (directory.path() / "data").c_str(), 1);
  auto opened = application::ProjectSession::create(directory.path() / "song");
  require(opened.has_value(), "project creation failed");
  auto session = std::move(opened.value());
  const auto track = session->instruments().add_synth_track();
  require(track && session->midi_clips().add_midi_clip(track.value(), {0}).has_value(),
          "MIDI clip failed");
  const auto clip = [&]() -> const project::MidiClip& {
    return session->snapshot().tracks.front().midi_clips.front();
  };
  const auto clip_id = QString::fromStdString(clip().id.to_string());
  QString status;
  int commits = 0;
  quick::NoteToolsController tools([&] { return session.get(); }, [] { return true; },
                                   [&](QString message) { status = std::move(message); },
                                   [&] { ++commits; });

  // Scale lock only snaps with a key set and the lock on.
  require(tools.lock_key(61) == 61, "no lock, no snap");
  tools.set_scale_lock(true);
  require(tools.lock_key(61) == 61, "no key, no snap");
  require(
      session->song().set_project_key(project::MusicalKey{0, project::KeyMode::major}).has_value(),
      "key failed");
  require(tools.lock_key(61) == 62, "C# snaps to D in C major");

  // A D triad in C major is D minor, drawn on step 4 of the 16th grid.
  const auto before = session->snapshot().revision;
  const auto drawn = tools.stamp_chord(clip_id, 4, 62, 16, QStringLiteral("Triad"), 0, 4);
  require(drawn.size() == 3 && commits == 1 && session->snapshot().revision == before + 1,
          "a chord is one edit: " + status.toStdString());
  std::vector<int> keys;
  for (const auto& note : clip().notes) {
    require(note.start.value == ticks_per_quarter && note.end.value == 2 * ticks_per_quarter,
            "the chord sits on beat 2 for a beat");
    keys.push_back(note.key);
  }
  std::ranges::sort(keys);
  require(keys == std::vector<int>{62, 65, 69}, "D minor");
  require(tools.chord_preview(62, QStringLiteral("Triad"), 1).size() == 3, "the ghost chord");

  // Strum up a 128th apart, one step: the chord's tones spread.
  tools.strum(clip_id, {}, 1, true);
  require(commits == 2, "strum is one edit: " + status.toStdString());
  const auto starts = [&] {
    std::vector<std::int64_t> values;
    for (const auto& note : clip().notes)
      values.push_back(note.start.value);
    std::ranges::sort(values);
    return values;
  };
  const auto step = ticks_per_quarter / 32;
  require(starts() == std::vector<std::int64_t>{ticks_per_quarter, ticks_per_quarter + step,
                                                ticks_per_quarter + 2 * step},
          "spread a 128th apart");

  // Quantise at full strength puts them back.
  tools.quantize(clip_id, {}, 16, 1.0, 0.5, false);
  require(commits == 3 && starts() == std::vector<std::int64_t>(3, ticks_per_quarter),
          "quantise back to the beat: " + status.toStdString());
  require(session->history().undo().has_value() && starts().back() == ticks_per_quarter + 2 * step,
          "one undo per tool");

  // A user groove file joins the pool and applies by name.
  const auto groove_dir = tools.groove_directory();
  require(QDir().mkpath(groove_dir), "groove directory failed");
  QFile file(groove_dir + QStringLiteral("/Drag.json"));
  require(file.open(QIODevice::WriteOnly), "groove file failed");
  file.write(QByteArray::fromStdString(
      oscillate::persistence::encode_groove({"ignored", 1, {{0.25, 1.0}}})));
  file.close();
  tools.rescan_grooves();
  require(tools.wait_for_groove_files(groove_wait_ms), "the groove folder was not read");
  const auto pool = tools.grooves();
  require(std::ranges::any_of(pool,
                              [](const QVariant& entry) {
                                const auto map = entry.toMap();
                                return map.value(QStringLiteral("name")).toString() ==
                                           QStringLiteral("Drag") &&
                                       map.value(QStringLiteral("user")).toBool();
                              }),
          "the user groove is listed under its file name");
  tools.quantize(clip_id, {}, 16, 1.0, 0.5, false);
  tools.apply_groove(clip_id, {}, QStringLiteral("Drag"), 1.0, 1.0, 0.0);
  require(starts() == std::vector<std::int64_t>(3, ticks_per_quarter + ticks_per_quarter / 4),
          "a quarter of a beat late: " + status.toStdString());
  require(tools.delete_groove(QStringLiteral("Drag")) &&
              tools.wait_for_groove_files(groove_wait_ms) &&
              tools.grooves().size() == pool.size() - 1,
          "a user groove can be deleted");

  // A groove kept from a record joins the pool under a file-safe name and
  // becomes the current groove.
  require(tools.keep_groove({"Ref / Hats", 4, {{0.0, 1.0}, {0.2, 0.8}}}) &&
              tools.property("currentGroove").toString() == QStringLiteral("Ref - Hats") &&
              tools.wait_for_groove_files(groove_wait_ms) &&
              tools.find_groove(QStringLiteral("Ref - Hats")).has_value() &&
              QFile::exists(groove_dir + QStringLiteral("/Ref - Hats.json")),
          "a kept groove is saved and chosen");
  require(!tools.keep_groove({"   ", 4, {{0.0, 1.0}}}), "a groove needs a name");
}

} // namespace

// NOLINTNEXTLINE(bugprone-exception-escape): an uncaught exception is the test's failure report.
int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  tools_edit_in_one_step();
}
