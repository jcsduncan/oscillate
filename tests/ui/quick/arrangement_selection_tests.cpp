#include "project_bridge.hpp"

#include "oscillate/application/project_session.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/wav.hpp"

#include <QGuiApplication>
#include <QMetaObject>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

// Arrangement selection (UQ-14): Ctrl+A selects every clip on every track
// while the arrangement has the keyboard and the piano roll's notes while
// the roll has it; a box drawn across tracks selects their clips; a drag or
// an arrow key moves the whole selection as one undo step, or nothing.
namespace {
namespace application = oscillate::application;
namespace project = oscillate::project;
namespace quick = oscillate::quick;
namespace timeline = oscillate::timeline;

using oscillate::test::require;

constexpr std::int64_t bar = 4 * timeline::ticks_per_quarter;
constexpr std::int64_t beat = timeline::ticks_per_quarter;
// The source audio: one second (half a bar at 120 BPM) of a quiet level.
constexpr std::size_t source_frames = oscillate::test::sample_rate_48k;
constexpr std::int16_t source_level = 4096;
constexpr int window_width = 1600;
constexpr int window_height = 1000;
// Eight bars across the arrangement, so a bar is a wide target.
constexpr double visible_bars = 8.0;
// Clip starts, counted from zero: the audio clip and the upper MIDI clip
// at bar 0 (so neither can move earlier), the lower MIDI clip at bar 2.
constexpr std::int64_t lower_clip_bar = 2;
// Empty lane space right of every clip, and a point inside the first bar's
// clips (the half-second audio clip spans its first half).
constexpr double empty_bar = 6.5;
constexpr double inside_first_bar = 0.25;
constexpr int track_count = 3;
constexpr int clip_count = 3;
constexpr int note_count = 2;
constexpr std::uint8_t first_key = 60;
constexpr std::uint8_t second_key = 64;
// Pointer steps for a drag, so the gesture passes the click slop.
constexpr int drag_steps = 8;

QQuickItem* find_item(QQuickItem* parent, const QString& name) {
  if (parent->objectName() == name && parent->isVisible())
    return parent;
  for (auto* child : parent->childItems()) {
    if (auto* found = find_item(child, name))
      return found;
  }
  return nullptr;
}

void find_items(QQuickItem* parent, const QString& name, QList<QQuickItem*>& found) {
  if (parent->objectName() == name && parent->isVisible())
    found.push_back(parent);
  for (auto* child : parent->childItems())
    find_items(child, name, found);
}

std::filesystem::path write_source(const std::filesystem::path& directory) {
  const auto path = directory / "source.wav";
  const std::vector<std::int16_t> samples(source_frames, source_level);
  oscillate::test::write_wav_pcm16(path, oscillate::test::sample_rate_48k,
                                   oscillate::test::mono_channels, samples);
  return path;
}

std::int64_t start_tick(const project::Snapshot& snapshot, const project::Placement& placement) {
  if (const auto* musical = std::get_if<project::MusicalInterval>(&placement))
    return musical->start.value;
  const auto tick = snapshot.tempo.sample_to_tick(
      std::get<project::ElapsedInterval>(placement).start, snapshot.sample_rate);
  require(tick.has_value(), "an elapsed clip start has no tick");
  return tick.value().value;
}

// Every clip's track and start, by clip id.
std::map<std::string, std::pair<std::string, std::int64_t>>
clip_starts(const application::ProjectSession& session) {
  const auto& snapshot = session.snapshot();
  std::map<std::string, std::pair<std::string, std::int64_t>> starts;
  for (const auto& track : snapshot.tracks) {
    for (const auto& clip : track.clips)
      starts[clip.id.to_string()] = {track.id.to_string(), start_tick(snapshot, clip.placement)};
    for (const auto& clip : track.midi_clips)
      starts[clip.id.to_string()] = {track.id.to_string(), start_tick(snapshot, clip.placement)};
  }
  return starts;
}

void require_shifted(const std::map<std::string, std::pair<std::string, std::int64_t>>& before,
                     const std::map<std::string, std::pair<std::string, std::int64_t>>& after,
                     std::int64_t delta, const std::string& message) {
  require(before.size() == after.size(), message + ": a clip appeared or vanished");
  for (const auto& [id, placed] : before) {
    const auto found = after.find(id);
    require(found != after.end(), message + ": a clip vanished");
    require(found->second.first == placed.first, message + ": a clip changed track");
    require(found->second.second == placed.second + delta, message);
  }
}

class Shell {
public:
  explicit Shell(quick::ProjectBridge& bridge) {
    qml_.rootContext()->setContextProperty("projectBridge", &bridge);
    qml_.load(QUrl(QStringLiteral("qrc:/DawShell.qml")));
    require(!qml_.rootObjects().isEmpty(), "DAW shell failed to load");
    window_ = qobject_cast<QQuickWindow*>(qml_.rootObjects().front());
    require(window_ != nullptr, "DAW shell is not a Quick window");
    window_->resize(window_width, window_height);
    window_->setProperty("visibleBars", visible_bars);
    window_->setProperty("viewStartBar", 0.0);
    window_->requestActivate();
    require(QTest::qWaitForWindowActive(window_), "DAW shell window never became active");
    QCoreApplication::processEvents();
  }

  [[nodiscard]] QQuickWindow& window() const { return *window_; }
  [[nodiscard]] QQuickItem* item(const QString& name) const {
    return find_item(window_->contentItem(), name);
  }

  // The scene point at `at_bar` on arrangement row `row`, halfway down it.
  [[nodiscard]] QPoint lane_point(int row, double at_bar) const {
    auto* arrangement = item(QStringLiteral("arrangementView"));
    require(arrangement != nullptr, "arrangement missing");
    QList<QQuickItem*> rows;
    find_items(window_->contentItem(), QStringLiteral("arrangementRow"), rows);
    require(rows.size() == track_count, "an arrangement row is missing");
    std::ranges::sort(rows, {}, [](QQuickItem* item) { return item->mapToScene({0, 0}).y(); });
    const auto* lane = rows[row];
    const auto x = arrangement->mapToScene({arrangement->width() * at_bar / visible_bars, 0}).x();
    const auto y = lane->mapToScene({0, lane->height() / 2}).y();
    return QPointF(x, y).toPoint();
  }

  [[nodiscard]] double bar_width() const {
    auto* arrangement = item(QStringLiteral("arrangementView"));
    require(arrangement != nullptr, "arrangement missing");
    return arrangement->width() / visible_bars;
  }

  void click(QPoint at, Qt::KeyboardModifiers modifiers = Qt::NoModifier) const {
    QTest::mouseClick(window_, Qt::LeftButton, modifiers, at);
    QCoreApplication::processEvents();
  }
  void drag(QPoint from, QPoint to) const {
    QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, from);
    for (int step = 1; step <= drag_steps; ++step)
      QTest::mouseMove(window_, from + (to - from) * step / drag_steps);
    QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, to);
    QCoreApplication::processEvents();
  }
  void key(Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier) const {
    QTest::keyClick(window_, key, modifiers);
    QCoreApplication::processEvents();
  }

private:
  QQmlApplicationEngine qml_;
  QQuickWindow* window_ = nullptr;
};

void clips_select_and_move_together() {
  QTemporaryDir directory;
  require(directory.isValid(), "selection fixture directory unavailable");
  const auto folder = std::filesystem::path(directory.path().toStdString());
  qputenv("XDG_CONFIG_HOME", (directory.path() + QStringLiteral("/config")).toUtf8());
  const auto root = folder / "song";
  auto created = application::ProjectSession::create(root);
  require(created.has_value(), "project creation failed");
  auto& session = *created.value();

  // An audio track, then two instrument tracks with a MIDI clip each.
  const auto source = write_source(folder);
  const int descriptor = ::open(source.c_str(), O_RDONLY | O_CLOEXEC);
  require(descriptor >= 0, "WAV fixture open failed");
  const auto imported = session.imports().import_wav_audio(descriptor);
  (void)::close(descriptor);
  require(imported.has_value(), "audio import failed");
  const auto upper = session.instruments().add_synth_track();
  const auto lower = session.instruments().add_synth_track();
  require(upper && lower, "instrument tracks were not added");
  require(session.midi_clips().add_midi_clip(upper.value(), {0}).has_value() &&
              session.midi_clips().add_midi_clip(lower.value(), {lower_clip_bar * bar}).has_value(),
          "MIDI clips were not added");
  const auto& tracks = session.snapshot().tracks;
  require(tracks.size() == track_count, "the fixture has three tracks");
  const auto upper_clip = tracks[1].midi_clips.front().id;
  require(session.notes().add_note(upper_clip, {0}, first_key).has_value() &&
              session.notes().add_note(upper_clip, {beat}, second_key).has_value(),
          "notes were not added");
  const auto upper_clip_id = QString::fromStdString(upper_clip.to_string());

  quick::ProjectBridge bridge(directory.path().toStdString());
  bridge.set_project(std::move(created).value(), root);
  const Shell shell(bridge);

  // A box from empty space on the top row to the clips on the bottom row
  // selects the clips on all three tracks; Escape lets go of them.
  shell.drag(shell.lane_point(0, empty_bar), shell.lane_point(2, inside_first_bar));
  require(bridge.selected_clip_ids().size() == clip_count,
          "a box across three tracks did not select their clips");
  shell.key(Qt::Key_Escape);
  require(bridge.selected_clip_ids().isEmpty(), "Escape did not clear the clip selection");

  // Ctrl+A after a click on empty lane space: every clip, audio and MIDI.
  shell.click(shell.lane_point(0, empty_bar));
  shell.key(Qt::Key_A, Qt::ControlModifier);
  require(bridge.selected_clip_ids().size() == clip_count,
          "Ctrl+A did not select every clip in the arrangement");

  // Dragging one selected clip a bar later moves every clip a bar, as one
  // undo step.
  const auto before = clip_starts(session);
  const auto revision = bridge.revision();
  const auto grab = shell.lane_point(1, inside_first_bar);
  shell.drag(grab, grab + QPointF(shell.bar_width(), 0).toPoint());
  require(bridge.revision() == revision + 1, "the group drag was not one edit");
  require_shifted(before, clip_starts(session), bar,
                  "dragging a selected clip did not move every clip one bar");
  bridge.undo();
  QCoreApplication::processEvents();
  require_shifted(before, clip_starts(session), 0, "one undo did not restore every clip");

  // The arrow keys move the whole selection a beat; a move one clip cannot
  // make (before bar 1) moves none.
  shell.click(shell.lane_point(0, empty_bar));
  shell.key(Qt::Key_A, Qt::ControlModifier);
  shell.key(Qt::Key_Right);
  require_shifted(before, clip_starts(session), beat, "Right did not move every clip a beat");
  bridge.undo();
  QCoreApplication::processEvents();
  shell.click(shell.lane_point(0, empty_bar));
  shell.key(Qt::Key_A, Qt::ControlModifier);
  require(bridge.selected_clip_ids().size() == clip_count, "Ctrl+A did not select every clip");
  const auto refused_at = bridge.revision();
  shell.key(Qt::Key_Left);
  require(bridge.revision() == refused_at, "a refused group move made an edit");
  require_shifted(before, clip_starts(session), 0, "a refused group move moved some clips");

  // Opening a clip's notes gives the piano roll the keyboard: Ctrl+A picks
  // its notes and leaves the clip selection alone.
  shell.click(shell.lane_point(0, empty_bar));
  shell.click(shell.lane_point(1, inside_first_bar));
  require(bridge.selected_clip_ids() == QStringList{upper_clip_id},
          "a click on the MIDI clip did not select it alone");
  auto* dock = shell.item(QStringLiteral("deviceChainDock"));
  require(dock != nullptr, "the device dock is missing");
  QMetaObject::invokeMethod(dock, "openNotes");
  QCoreApplication::processEvents();
  auto* roll = shell.item(QStringLiteral("pianoRoll"));
  require(roll != nullptr, "opening the clip's notes did not show the piano roll");
  shell.key(Qt::Key_A, Qt::ControlModifier);
  require(bridge.selected_note_ids().size() == note_count,
          "Ctrl+A in the piano roll did not select its notes");
  require(bridge.selected_clip_ids() == QStringList{upper_clip_id},
          "Ctrl+A in the piano roll changed the clip selection");

  // A click on the clip in the arrangement takes the keyboard back, with the
  // roll still open: Ctrl+A selects every clip again.
  shell.click(shell.lane_point(1, inside_first_bar));
  require(shell.item(QStringLiteral("pianoRoll")) != nullptr,
          "clicking the open clip closed its piano roll");
  shell.key(Qt::Key_A, Qt::ControlModifier);
  require(bridge.selected_clip_ids().size() == clip_count,
          "Ctrl+A in the arrangement did not select every clip with the roll open");
  require(bridge.selected_note_ids().isEmpty(),
          "Ctrl+A in the arrangement kept the roll's notes, so Delete would take them");

  // A Ctrl-click lets the focused clip go from the selection: the arrow
  // keys then move the selected clips, not the focused one.
  shell.click(shell.lane_point(1, inside_first_bar), Qt::ControlModifier);
  require(bridge.selected_clip_ids().size() == clip_count - 1,
          "a Ctrl-click did not take the clip out of the selection");
  const auto unselected = clip_starts(session);
  shell.key(Qt::Key_Right);
  const auto nudged = clip_starts(session);
  for (const auto& [id, placed] : unselected) {
    const auto expected = id == upper_clip.to_string() ? placed.second : placed.second + beat;
    require(nudged.at(id).second == expected,
            "Right moved the focused clip rather than the selection");
  }
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication application(argc, argv);
  try {
    clips_select_and_move_together();
  } catch (const std::exception& error) {
    std::cerr << "Arrangement selection test: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
