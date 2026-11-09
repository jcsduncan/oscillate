#include "project_bridge.hpp"

#include "oscillate/analysis/tempo.hpp"
#include "oscillate/application/project_session.hpp"
#include "oscillate/application/tempo_cache.hpp"
#include "oscillate/test/check.hpp"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTest>

#include <cmath>
#include <cstdint>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

// The arrangement's warp markers, the docked clip editor's grid check and the
// tempo lane (ADR-0112), through the real shell: every gesture and command
// is one undo step, and the views show what the commands did.
namespace {
namespace analysis = oscillate::analysis;
namespace application = oscillate::application;
namespace project = oscillate::project;
namespace quick = oscillate::quick;

using oscillate::test::require;

constexpr int window_width = 1600;
constexpr int window_height = 1000;
// Four bars in view: a beat of the sixteen-bar loop is about 70 px wide.
constexpr double visible_bars = 4.0;
constexpr std::uint32_t sample_rate = 48'000;
constexpr double loop_bpm = 120.0;
constexpr int beats_per_bar = 4;
constexpr int loop_bars = 16;
constexpr int loop_beats = loop_bars * beats_per_bar;
constexpr std::uint32_t beat_frames = 24'000;
constexpr std::uint32_t loop_frames = loop_beats * beat_frames;
// The kicks drift: on the grid for two bars, then 15 ms late.
constexpr int drift_bar = 3;
constexpr std::uint32_t late_frames = 720;
constexpr int late_ms = 15;
constexpr double analysis_confidence = 0.9;
constexpr std::uint64_t analysis_generation = 1;
// The beats the gestures use.
constexpr int pinned_beat = 4;
constexpr int stretched_beat = 5;
constexpr int second_beat = 12;
// A click this far past a beat still snaps onto its transient.
constexpr double near_px = 5.0;
constexpr double slide_px = 12.0;
constexpr int drag_steps = 6;
constexpr int settle_ms = 15'000;
// A project tempo this far from the loop's stretches it past 6%.
constexpr double far_bpm = 128.0;
constexpr double fraction_tolerance = 1e-4;
constexpr double bpm_tolerance = 0.5;
constexpr int chain_tab = 0;
constexpr int dock_clip_tab = 1;
// The shell's index for the master path.
constexpr int master_track = -2;

QQuickItem* find_item(QQuickItem* parent, const QString& name) {
  if (parent->objectName() == name && parent->isVisible())
    return parent;
  for (auto* child : parent->childItems())
    if (auto* found = find_item(child, name))
      return found;
  return nullptr;
}

int count_items(QQuickItem* parent, const QString& name) {
  if (!parent->isVisible())
    return 0;
  int count = parent->objectName() == name ? 1 : 0;
  for (auto* child : parent->childItems())
    count += count_items(child, name);
  return count;
}

QQuickItem* find_text(QQuickItem* parent, const QString& part) {
  if (!parent->isVisible())
    return nullptr;
  if (parent->property("text").toString().contains(part))
    return parent;
  for (auto* child : parent->childItems())
    if (auto* found = find_text(child, part))
      return found;
  return nullptr;
}

void put16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value));
  bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
}
void put32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
  put16(bytes, static_cast<std::uint16_t>(value));
  put16(bytes, static_cast<std::uint16_t>(value >> 16U));
}

// The kick of `beat` in source frames: late from the drift bar on.
std::uint64_t kick(int beat) {
  const auto late = beat >= (drift_bar - 1) * beats_per_bar ? late_frames : 0;
  return static_cast<std::uint64_t>(beat) * beat_frames + late;
}

std::filesystem::path write_loop(const std::filesystem::path& folder) {
  constexpr std::uint16_t click = 20'000;
  constexpr std::uint16_t quiet = 100;
  constexpr std::uint16_t pcm_format = 1;
  constexpr std::uint16_t mono = 1;
  constexpr std::uint16_t bytes_per_sample = 2;
  constexpr std::uint16_t bits_per_sample = 16;
  constexpr std::uint32_t format_chunk_bytes = 16;
  constexpr std::uint32_t header_bytes = 36;
  std::vector<std::uint8_t> wav{'R', 'I', 'F', 'F'};
  put32(wav, header_bytes + loop_frames * bytes_per_sample);
  wav.insert(wav.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  put32(wav, format_chunk_bytes);
  put16(wav, pcm_format);
  put16(wav, mono);
  put32(wav, sample_rate);
  put32(wav, sample_rate * bytes_per_sample);
  put16(wav, bytes_per_sample);
  put16(wav, bits_per_sample);
  wav.insert(wav.end(), {'d', 'a', 't', 'a'});
  put32(wav, loop_frames * bytes_per_sample);
  std::vector<std::uint16_t> samples(loop_frames, quiet);
  for (int beat = 0; beat < loop_beats; ++beat)
    samples[kick(beat)] = click;
  for (const auto sample : samples)
    put16(wav, sample);
  const auto path = folder / "loop.wav";
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char*>(wav.data()), static_cast<std::streamsize>(wav.size()));
  require(output.good(), "the loop fixture did not write");
  return path;
}

// A sixteen-bar loop at 120 BPM, reviewed with its beats and drifting kicks,
// following the project from bar 1.
struct Song {
  QTemporaryDir directory;
  std::filesystem::path root;
  application::ProjectSession* session = nullptr;
  std::unique_ptr<quick::ProjectBridge> bridge;
  QString clip_id;

  Song() {
    require(directory.isValid(), "fixture directory unavailable");
    qputenv("XDG_CONFIG_HOME", (directory.path() + QStringLiteral("/config")).toUtf8());
    const std::filesystem::path folder(directory.path().toStdString());
    root = folder / "song";
    auto created = application::ProjectSession::create(root);
    require(created.has_value(), "project creation failed");
    session = created.value().get();
    const auto wav = write_loop(folder);
    const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
    require(fd >= 0, "the loop did not open");
    require(session->imports().import_wav_audio(fd).has_value(), "the loop did not import");
    (void)::close(fd);
    const auto& source = session->snapshot().sources.front();
    analysis::TempoEstimate estimate;
    estimate.identity = {application::tempo_cache_key(source).value().hex(), analysis_generation};
    estimate.sample_rate = source.sample_rate;
    estimate.frames = source.frames;
    estimate.bpm = loop_bpm;
    estimate.confidence = analysis_confidence;
    std::vector<std::uint64_t> kicks;
    for (int beat = 0; beat < loop_beats; ++beat) {
      estimate.suggested_beat_frames.push_back(static_cast<std::uint64_t>(beat) * beat_frames);
      kicks.push_back(kick(beat));
    }
    require(session->source_analysis()
                .accept_source_tempo_analysis(source.id, session->snapshot().revision, estimate)
                .has_value(),
            "the loop's tempo review was refused");
    require(session->source_analysis()
                .accept_source_onset_analysis(session->snapshot().sources.front(),
                                              session->snapshot().revision, kicks, false)
                .has_value(),
            "the loop's transients were refused");
    const auto clip = session->snapshot().tracks.front().clips.front().id;
    require(session->warp().set_audio_clip_follow(clip, true).has_value(),
            "the loop does not follow the project");
    clip_id = QString::fromStdString(clip.to_string());
    // A group bus and a return: tracks that hold no clips (#331).
    for (const auto kind : {project::TrackKind::group_bus, project::TrackKind::return_bus})
      require(session->tracks().add_bus_track(kind).has_value(), "a bus was not added");
    bridge = std::make_unique<quick::ProjectBridge>(folder.string());
    bridge->set_project(std::move(created).value(), root);
    bridge->select_audio_clip(clip_id);
  }
  [[nodiscard]] const project::Clip& saved_clip() const {
    return session->snapshot().tracks.front().clips.front();
  }
  [[nodiscard]] QVariantList markers() const {
    return bridge->clip_warp()->property("state").toMap().value(QStringLiteral("markers")).toList();
  }
  [[nodiscard]] QVariantMap marker(int index) const {
    const auto all = markers();
    require(index < all.size(), "the clip has no marker " + std::to_string(index));
    return all.at(index).toMap();
  }
};

class Shell {
public:
  explicit Shell(Song& song) : song_(song) {
    qml_.rootContext()->setContextProperty("projectBridge", song.bridge.get());
    qml_.load(QUrl(QStringLiteral("qrc:/DawShell.qml")));
    require(!qml_.rootObjects().isEmpty(), "DAW shell failed to load");
    window_ = qobject_cast<QQuickWindow*>(qml_.rootObjects().front());
    require(window_ != nullptr, "DAW shell is not a Quick window");
    window_->resize(window_width, window_height);
    window_->setProperty("visibleBars", visible_bars);
    window_->setProperty("viewStartBar", 0.0);
    window_->setProperty("selectedClipId", song.clip_id);
    window_->requestActivate();
    require(QTest::qWaitForWindowActive(window_), "DAW shell window never became active");
    QCoreApplication::processEvents();
  }

  [[nodiscard]] QQuickWindow* window() const { return window_; }
  [[nodiscard]] QQuickItem* wait_for(const QString& name) const {
    QQuickItem* found = nullptr;
    const bool shown = QTest::qWaitFor(
        [&] {
          QCoreApplication::processEvents();
          found = find_item(window_->contentItem(), name);
          return found != nullptr;
        },
        settle_ms);
    require(shown && found != nullptr, "the shell does not show " + name.toStdString());
    return found;
  }
  [[nodiscard]] QQuickItem* item(const QString& name) const {
    return find_item(window_->contentItem(), name);
  }
  [[nodiscard]] int count(const QString& name) const {
    return count_items(window_->contentItem(), name);
  }
  // The window point at clip `fraction` on the overlay, `y` px down it.
  [[nodiscard]] QPoint at(double fraction, double y) const {
    auto* overlay = wait_for(QStringLiteral("warpMarkerOverlay"));
    const auto from = overlay->property("viewFrom").toDouble();
    const auto to = overlay->property("viewTo").toDouble();
    return overlay->mapToScene({(fraction - from) / (to - from) * overlay->width(), y}).toPoint();
  }
  [[nodiscard]] QPoint beat(int beat_index, double y, double past = 0.0) const {
    return at(static_cast<double>(beat_index) / loop_beats, y) + QPoint(static_cast<int>(past), 0);
  }
  void drag(QPoint from, QPoint to, Qt::KeyboardModifiers modifiers = {}) const {
    QTest::mousePress(window_, Qt::LeftButton, modifiers, from);
    for (int step = 1; step <= drag_steps; ++step)
      QTest::mouseMove(window_, from + (to - from) * step / drag_steps);
    QTest::mouseRelease(window_, Qt::LeftButton, modifiers, to);
    QCoreApplication::processEvents();
  }
  void click(const QString& name) const {
    auto* target = wait_for(name);
    QTest::mouseClick(window_, Qt::LeftButton, {},
                      target->mapToScene({target->width() / 2, target->height() / 2}).toPoint());
    QCoreApplication::processEvents();
  }
  // Right-clicks marker `index` and chooses `entry` from its menu.
  void command(int index, const QString& entry) const {
    const auto fraction = song_.marker(index).value(QStringLiteral("fraction")).toDouble();
    QTest::mouseClick(window_, Qt::RightButton, {}, at(fraction, band_y()));
    QCoreApplication::processEvents();
    require(item(QStringLiteral("warpMarkerMenu")) != nullptr, "the marker menu did not open");
    click(QStringLiteral("warpMenu_") + entry);
  }
  // Inside the pin band at the waveform's foot.
  [[nodiscard]] double band_y() const {
    auto* band = wait_for(QStringLiteral("warpPinBand"));
    return band->y() + band->height() / 2;
  }

private:
  Song& song_;
  QQmlApplicationEngine qml_;
  QQuickWindow* window_ = nullptr;
};

double fraction_of(int beat_index, bool late) {
  return (static_cast<double>(beat_index) * beat_frames + (late ? late_frames : 0)) / loop_frames;
}

// Pin, stretch, slide and remove, each one undoable edit.
void markers_pin_stretch_slide_and_remove(Song& song, const Shell& shell) {
  auto& bridge = *song.bridge;
  (void)shell.wait_for(QStringLiteral("warpPinBand"));
  // Hovering the band shows the ghost; a click near a beat pins its kick.
  QTest::mouseMove(shell.window(), shell.beat(pinned_beat, shell.band_y(), near_px));
  require(shell.item(QStringLiteral("warpMarkerGhost")) != nullptr, "no ghost under the pointer");
  auto revision = bridge.revision();
  QTest::mouseClick(shell.window(), Qt::LeftButton, {},
                    shell.beat(pinned_beat, shell.band_y(), near_px));
  QCoreApplication::processEvents();
  require(bridge.revision() == revision + 1 && song.markers().size() == 1,
          "a click on the band did not pin one marker");
  require(song.marker(0).value(QStringLiteral("sourceFrame")).toULongLong() == kick(pinned_beat),
          "the marker did not snap onto the kick");

  // Drag: both sides' tempo shows live, nothing saves until release.
  const auto pinned = shell.beat(pinned_beat, shell.band_y());
  const auto stretched = shell.beat(stretched_beat, shell.band_y(), near_px);
  revision = bridge.revision();
  QTest::mousePress(shell.window(), Qt::LeftButton, {}, pinned);
  for (int step = 1; step <= drag_steps; ++step)
    QTest::mouseMove(shell.window(), pinned + (stretched - pinned) * step / drag_steps);
  // The arrangement's overlay (the docked editor shows the marker too).
  require(count_items(shell.wait_for(QStringLiteral("warpMarkerOverlay")),
                      QStringLiteral("warpSegmentTag")) == 2,
          "the segment tempi do not show while stretching");
  require(bridge.revision() == revision, "a stretch saved before release");
  QTest::mouseRelease(shell.window(), Qt::LeftButton, {}, stretched);
  QCoreApplication::processEvents();
  require(bridge.revision() == revision + 1, "a stretch was not one edit");
  require(std::abs(song.marker(0).value(QStringLiteral("fraction")).toDouble() -
                   fraction_of(stretched_beat, false)) < fraction_tolerance &&
              song.marker(0).value(QStringLiteral("sourceFrame")).toULongLong() ==
                  kick(pinned_beat),
          "the stretch did not move the marker onto the next beat with its audio");
  bridge.undo();

  // Shift-drag: the marker stays, earlier audio slides under it.
  revision = bridge.revision();
  const auto before = song.marker(0);
  shell.drag(shell.beat(pinned_beat, shell.band_y()),
             shell.beat(pinned_beat, shell.band_y(), slide_px), Qt::ShiftModifier);
  require(bridge.revision() == revision + 1, "a slide was not one edit");
  require(std::abs(song.marker(0).value(QStringLiteral("fraction")).toDouble() -
                   before.value(QStringLiteral("fraction")).toDouble()) < fraction_tolerance &&
              song.marker(0).value(QStringLiteral("sourceFrame")).toULongLong() < kick(pinned_beat),
          "the slide moved the marker or did not bring earlier audio under it");
  bridge.undo();
  require(song.marker(0).value(QStringLiteral("sourceFrame")).toULongLong() == kick(pinned_beat),
          "undo did not take the slide back");

  // Double-click removes.
  revision = bridge.revision();
  QTest::mouseDClick(shell.window(), Qt::LeftButton, {}, shell.beat(pinned_beat, shell.band_y()));
  QCoreApplication::processEvents();
  require(bridge.revision() == revision + 1 && song.markers().isEmpty(),
          "a double-click did not remove the marker");
  bridge.undo();
}

// The right-click commands, each one undoable edit.
void marker_menu_commands(Song& song, const Shell& shell) {
  auto& bridge = *song.bridge;
  QTest::mouseClick(shell.window(), Qt::LeftButton, {},
                    shell.beat(second_beat, shell.band_y(), near_px));
  QCoreApplication::processEvents();
  require(song.markers().size() == 2, "the second marker was not pinned");

  // Every entry fits the menu.
  QTest::mouseClick(shell.window(), Qt::RightButton, {}, shell.beat(pinned_beat, shell.band_y()));
  QCoreApplication::processEvents();
  auto* menu = shell.wait_for(QStringLiteral("warpMarkerMenu"));
  for (const auto* entry : {"downbeat", "straight", "tempo", "drift", "remove", "after"}) {
    auto* row = shell.wait_for(QStringLiteral("warpMenu_") + QString::fromLatin1(entry));
    for (auto* text : row->childItems())
      require(text->x() + text->property("implicitWidth").toDouble() <= menu->width(),
              std::string("the marker menu cuts ") + entry + " short");
  }

  auto revision = bridge.revision();
  shell.command(0, QStringLiteral("after"));
  require(bridge.revision() == revision + 1 && song.markers().size() == 1,
          "Remove all after was not one edit");
  bridge.undo();

  revision = bridge.revision();
  const auto length = std::get<project::MusicalInterval>(song.saved_clip().placement);
  shell.command(0, QStringLiteral("straight"));
  require(bridge.revision() == revision + 1 && song.markers().size() == 1,
          "Straight from here was not one edit");
  bridge.undo();
  require(std::get<project::MusicalInterval>(song.saved_clip().placement) == length,
          "undo did not restore the clip after Straight from here");

  revision = bridge.revision();
  shell.command(0, QStringLiteral("tempo"));
  const auto& review = song.session->snapshot().source_tempo_reviews.front();
  require(bridge.revision() == revision + 1 && review.corrected_bpm.has_value() &&
              std::abs(*review.corrected_bpm - loop_bpm) < bpm_tolerance,
          "Set tempo from this segment did not set the track tempo");
  bridge.undo();

  revision = bridge.revision();
  shell.command(0, QStringLiteral("downbeat"));
  require(bridge.revision() == revision + 1 &&
              song.session->snapshot().source_tempo_reviews.front().downbeat_frame ==
                  kick(pinned_beat),
          "Set downbeat here did not mark the marker's audio");
  bridge.undo();

  revision = bridge.revision();
  shell.command(0, QStringLiteral("drift"));
  require(bridge.revision() == revision + 1,
          "Fix drift from here was not one edit: " + bridge.status().toStdString());
  bridge.undo();

  revision = bridge.revision();
  shell.command(1, QStringLiteral("remove"));
  require(bridge.revision() == revision + 1 && song.markers().size() == 1,
          "Remove marker was not one edit");
  // Back to no markers: the remove, the second pin and the first.
  for (int step = 0; step < 3; ++step)
    bridge.undo();
  require(song.markers().isEmpty(), "the markers did not undo away");
}

// The docked clip editor finds where the kicks drift and fixes it.
void grid_check_flags_the_drift_and_fixes_it(Song& song, const Shell& shell) {
  auto* dock = shell.window()->findChild<QObject*>(QStringLiteral("deviceChainDock"));
  require(dock != nullptr, "the shell has no device dock");
  dock->setProperty("activeTab", dock_clip_tab);
  auto* fix = shell.wait_for(QStringLiteral("clipEditorFixDrift"));
  const auto badge =
      QStringLiteral("Kick %1 ms late of the grid from bar %2").arg(late_ms).arg(drift_bar);
  require(QTest::qWaitFor(
              [&] {
                QCoreApplication::processEvents();
                return find_text(shell.window()->contentItem(), badge) != nullptr;
              },
              settle_ms),
          "the grid check does not flag the drift");
  require(find_text(fix, QStringLiteral("Fix drift from bar %1").arg(drift_bar)) != nullptr,
          "Fix drift does not name the drifting bar");
  const auto revision = song.bridge->revision();
  shell.click(QStringLiteral("clipEditorFixDrift"));
  // The first late kick stays on its own beat, pinned where the grid has it.
  require(song.bridge->revision() == revision + 1 && !song.markers().isEmpty(),
          "Fix drift did not pin the kicks in one edit: " + song.bridge->status().toStdString());
  const auto drifting_beat = (drift_bar - 1) * beats_per_bar;
  require(song.marker(0).value(QStringLiteral("sourceFrame")).toULongLong() ==
                  kick(drifting_beat) &&
              std::abs(song.marker(0).value(QStringLiteral("fraction")).toDouble() -
                       fraction_of(drifting_beat, false)) < fraction_tolerance,
          "Fix drift pinned the wrong kick to the drifting bar");
  song.bridge->undo();
  require(song.markers().isEmpty(), "undo did not take the fix back");
}

// The tempo lane: collapsed under the ruler, expanded by a click; a clip's
// stretch shows on hover, amber past 6%; "Ramp to this track's tempo" on a
// clip with no transition explains itself and changes nothing.
void tempo_lane_and_stretch(Song& song, const Shell& shell) {
  auto* workspace = shell.window()->findChild<QObject*>(QStringLiteral("arrangementWorkspace"));
  require(workspace != nullptr, "the shell has no arrangement workspace");
  auto* lane = shell.wait_for(QStringLiteral("tempoLane"));
  require(!workspace->property("tempoLaneExpanded").toBool() &&
              !lane->property("expanded").toBool(),
          "the tempo lane does not start collapsed");
  const auto collapsed = lane->height();
  QTest::mouseClick(shell.window(), Qt::LeftButton, {},
                    lane->mapToScene({lane->width() / 2, lane->height() / 2}).toPoint());
  QCoreApplication::processEvents();
  require(lane->property("expanded").toBool() && lane->height() > collapsed,
          "a click on the collapsed lane did not expand it");
  shell.click(QStringLiteral("tempoLaneHeader"));
  require(!lane->property("expanded").toBool(), "the lane header did not collapse the lane");

  song.bridge->set_initial_tempo_bpm(far_bpm);
  QTest::mouseMove(shell.window(), shell.beat(second_beat, lane->height() * 2));
  auto* stretch = shell.wait_for(QStringLiteral("clipStretchChip"));
  require(stretch->property("lit").toBool(), "a stretch past 6% is not amber");
  song.bridge->undo();

  QTest::mouseClick(shell.window(), Qt::RightButton, {},
                    shell.beat(second_beat, lane->height() * 2));
  QCoreApplication::processEvents();
  const auto revision = song.bridge->revision();
  shell.click(QStringLiteral("rampToClipTempoAction"));
  require(song.bridge->revision() == revision &&
              song.bridge->status().contains(QStringLiteral("no transition")),
          "Ramp to this track's tempo without a transition changed the project: " +
              song.bridge->status().toStdString());
}

// The dock's Clip and Notes tabs are off, with the reason on hover, for the
// master and for tracks that hold no clips, and the dock shows the Chain.
void dock_tabs_follow_whether_the_track_holds_clips(Song& song, const Shell& shell) {
  auto* dock = shell.window()->findChild<QObject*>(QStringLiteral("deviceChainDock"));
  require(dock != nullptr, "the shell has no device dock");
  int group = -1;
  int bus_return = -1;
  const auto tracks = song.bridge->tracks();
  for (int index = 0; index < tracks.size(); ++index) {
    const auto kind = tracks[index].toMap().value(QStringLiteral("kind")).toString();
    group = kind == QStringLiteral("group") ? index : group;
    bus_return = kind == QStringLiteral("return") ? index : bus_return;
  }
  require(group >= 0 && bus_return >= 0, "the fixture's buses are missing");
  for (const int track : {master_track, group, bus_return}) {
    // From the loop's track with its Clip tab open.
    shell.window()->setProperty("selectedTrack", 0);
    dock->setProperty("activeTab", dock_clip_tab);
    QCoreApplication::processEvents();
    require(shell.wait_for(QStringLiteral("dockTabClip"))->property("available").toBool(),
            "the loop's track does not offer its clip");
    shell.window()->setProperty("selectedTrack", track);
    QCoreApplication::processEvents();
    require(dock->property("activeTab").toInt() == chain_tab,
            "a track with no clips did not open the Chain");
    for (const auto* name : {"dockTabClip", "dockTabNotes"}) {
      auto* tab = shell.wait_for(QString::fromLatin1(name));
      require(!tab->property("available").toBool() &&
                  tab->property("reason").toString().contains(QStringLiteral("holds no clips")),
              std::string(name) + " is offered on a track with no clips");
      shell.click(QString::fromLatin1(name));
      require(dock->property("activeTab").toInt() == chain_tab,
              std::string(name) + " opened on a track with no clips");
    }
  }
}

void arrangement_warp_and_tempo() {
  Song song;
  const Shell shell(song);
  markers_pin_stretch_slide_and_remove(song, shell);
  marker_menu_commands(song, shell);
  grid_check_flags_the_drift_and_fixes_it(song, shell);
  tempo_lane_and_stretch(song, shell);
  dock_tabs_follow_whether_the_track_holds_clips(song, shell);
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication application(argc, argv);
  try {
    arrangement_warp_and_tempo();
  } catch (const std::exception& error) {
    std::cerr << "Arrangement warp and tempo test: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
