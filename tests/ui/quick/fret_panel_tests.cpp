#include "in_memory_native_synth_adapter.hpp"
#include "native_synth_device_controller.hpp"

#include "../../native_dsp/fret_test_library.hpp"
#include "oscillate/native_dsp/synth/fret.hpp"
#include "oscillate/native_dsp/synth/fret_feel.hpp"
#include "oscillate/native_dsp/synth/parameters.hpp"
#include "oscillate/native_dsp/synth/prism.hpp"
#include "oscillate/project/model.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/timeline/timing.hpp"

#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// The Fret panel (ADR-0088): the controller's library status, telemetry
// decode and groove lane, and the QML panel: without its library (the
// message and install command), with it (the credit), a control for every
// parameter, the style tiles, the lane drawing what Feel decides for a
// known clip, and the latest note live.
namespace {
using oscillate::quick::InMemoryNativeSynthAdapter;
using oscillate::quick::NativeSynthDeviceController;
namespace synth = oscillate::native_dsp::synth;
namespace fret = synth::fret;
namespace feel = fret::feel;
namespace project = oscillate::project;
namespace timeline = oscillate::timeline;

using oscillate::test::require;

const synth::EngineDescriptor& table() { return *fret::engine().descriptor; }

QString qs(std::string_view text) {
  return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

// The mockup's two bars, drawn flat: beat, key, length in beats.
struct Drawn {
  double beat;
  int key;
  double length;
};
constexpr int flat_velocity = 100;
constexpr std::array line{
    Drawn{0.00, 28, 0.40}, Drawn{0.50, 28, 0.15}, Drawn{0.75, 40, 0.20}, Drawn{1.25, 38, 0.15},
    Drawn{1.50, 35, 0.45}, Drawn{2.50, 28, 0.20}, Drawn{2.75, 28, 0.15}, Drawn{3.00, 31, 0.50},
    Drawn{3.75, 33, 0.20}, Drawn{4.25, 28, 0.30}, Drawn{4.75, 28, 0.15}, Drawn{5.00, 40, 0.15},
    Drawn{5.50, 38, 0.40}, Drawn{6.25, 35, 0.20}, Drawn{6.50, 33, 0.15}, Drawn{6.75, 31, 0.60}};
constexpr double beats_per_bar = 4.0;
constexpr int bars = NativeSynthDeviceController::fret_groove_bars;

timeline::TickPosition ticks(double beats) {
  return {static_cast<std::int64_t>(std::llround(beats * timeline::ticks_per_quarter))};
}

// The line as a clip on the adapter's track, from the first bar.
void draw_line(InMemoryNativeSynthAdapter& adapter) {
  std::vector<project::MidiNote> notes;
  std::uint64_t id = 1;
  for (const auto& note : line) {
    notes.push_back({project::NoteId::from_bits({0, id++}).value(), ticks(note.beat),
                     ticks(note.beat + note.length), static_cast<std::uint8_t>(note.key),
                     static_cast<std::uint8_t>(flat_velocity)});
  }
  require(adapter.add_midi_clip(0, ticks(bars * beats_per_bar), std::move(notes)),
          "the clip is added");
}

// What the kernel plays for the line with `values`: Feel's decision, then
// Articulation, each note after the one before.
struct Played {
  feel::Decision decision;
  feel::Articulation articulation;
};
std::vector<Played> expected(const std::vector<double>& values) {
  std::vector<Played> out;
  feel::Previous previous;
  for (const auto& drawn : line) {
    const auto bar = static_cast<std::int64_t>(std::floor(drawn.beat / beats_per_bar));
    const feel::Note note{drawn.beat, drawn.beat - static_cast<double>(bar) * beats_per_bar,
                          bar,        drawn.length,
                          drawn.key,  flat_velocity};
    const auto decision = feel::decide(note, previous, fret::feel_settings(values));
    previous = feel::after(note, decision);
    out.push_back({decision, fret::played_articulation(values, decision.articulation)});
  }
  return out;
}

QString articulation_name(feel::Articulation articulation) {
  switch (articulation) {
  case feel::Articulation::staccato:
    return QStringLiteral("staccato");
  case feel::Articulation::ghost:
    return QStringLiteral("ghost");
  case feel::Articulation::bridge:
    return QStringLiteral("bridge");
  case feel::Articulation::pluck:
    break;
  }
  return QStringLiteral("pluck");
}

// ---- Controller ---------------------------------------------------------------------

// Run before any library loads in this process.
void controller_reports_a_missing_library() {
  const auto nowhere = fret_test::scratch_directory("fret-panel-library");
  std::filesystem::remove_all(nowhere);
  ::setenv(std::string(fret::library::directory_variable).c_str(), nowhere.c_str(), 1);
  InMemoryNativeSynthAdapter adapter(synth::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  const auto library = controller.fret_library();
  require(library.value(QStringLiteral("state")).toString() == QStringLiteral("missing") &&
              !library.value(QStringLiteral("message")).toString().isEmpty(),
          "a missing library is reported with a reason");
  require(library.value(QStringLiteral("install_command")).toString() ==
              qs(fret::library::install_command),
          "the panel learns how to install it");
  require(
      library.value(QStringLiteral("required")).toString().contains(qs(fret::library::attribution)),
      "and which library, with its credit");
  const auto& prism = *synth::prism::engine().descriptor;
  InMemoryNativeSynthAdapter other(synth::default_values(prism));
  NativeSynthDeviceController prism_controller(prism, other);
  require(prism_controller.fret_library().isEmpty() &&
              prism_controller.fret_groove(false).isEmpty(),
          "only Fret has a library and a groove lane");
}

std::vector<float> fret_frame(int key, int velocity, feel::Articulation articulation, bool pushed) {
  namespace t = fret::telemetry;
  std::vector<float> frame(t::size, 0.0F);
  frame[t::library] = static_cast<float>(static_cast<int>(fret::library::State::ready));
  frame[t::notes] = 7.0F;
  frame[t::last_key] = static_cast<float>(key);
  frame[t::last_velocity] = static_cast<float>(velocity);
  frame[t::last_articulation] = static_cast<float>(static_cast<int>(articulation));
  frame[t::last_pushed] = pushed ? 1.0F : 0.0F;
  frame[t::output_peak_db] = -12.0F;
  return frame;
}

void controller_decodes_fret_telemetry() {
  InMemoryNativeSynthAdapter adapter(synth::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  require(!controller.telemetry().value(QStringLiteral("live")).toBool(), "nothing plays");
  auto frame = fret_frame(28, 96, feel::Articulation::ghost, true);
  frame[fret::telemetry::underruns] = 2.0F;
  adapter.set_telemetry(frame);
  const auto live = controller.telemetry();
  require(live.value(QStringLiteral("live")).toBool() &&
              live.value(QStringLiteral("library")).toString() == QStringLiteral("ready") &&
              live.value(QStringLiteral("underruns")).toDouble() == 2.0 &&
              live.value(QStringLiteral("notes")).toInt() == 7,
          "the library, late reads and note count decode");
  require(live.value(QStringLiteral("last_key")).toInt() == 28 &&
              live.value(QStringLiteral("last_name")).toString() == QStringLiteral("E1") &&
              live.value(QStringLiteral("last_velocity")).toInt() == 96 &&
              live.value(QStringLiteral("last_articulation")).toString() ==
                  QStringLiteral("ghost") &&
              live.value(QStringLiteral("last_pushed")).toBool() &&
              live.value(QStringLiteral("output_peak_db")).toDouble() == -12.0,
          "the latest note decodes as Feel played it");
}

// The lane is empty without a clip, and with one it is exactly what Feel
// decides: velocities, articulations, pushes and Pocket's delays, in order.
void controller_groove_follows_feel() {
  InMemoryNativeSynthAdapter adapter(synth::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  require(controller.fret_groove(false).value(QStringLiteral("notes")).toList().isEmpty(),
          "no clip, no notes");
  draw_line(adapter);
  controller.set_value(QStringLiteral("fret.feel.pocket"), 10.0);
  const auto groove = controller.fret_groove(false);
  require(groove.value(QStringLiteral("bars")).toInt() == bars &&
              groove.value(QStringLiteral("beats_per_bar")).toDouble() == beats_per_bar &&
              groove.value(QStringLiteral("max_velocity")).toInt() == feel::max_velocity,
          "two bars of 4/4");
  const auto notes = groove.value(QStringLiteral("notes")).toList();
  const auto oracle = expected(controller.current_values());
  require(notes.size() == static_cast<qsizetype>(line.size()), "every note is in the lane");
  for (std::size_t i = 0; i < line.size(); ++i) {
    const auto note = notes[static_cast<qsizetype>(i)].toMap();
    const auto& played = oracle[i];
    require(std::fabs(note.value(QStringLiteral("position")).toDouble() -
                      line[i].beat / (bars * beats_per_bar)) < 1e-9,
            "a note sits at its place across the two bars");
    require(note.value(QStringLiteral("velocity")).toInt() == played.decision.velocity &&
                note.value(QStringLiteral("articulation")).toString() ==
                    articulation_name(played.articulation) &&
                note.value(QStringLiteral("pushed")).toBool() == played.decision.pushed &&
                note.value(QStringLiteral("delay_ms")).toDouble() == played.decision.delay_ms &&
                note.value(QStringLiteral("drawn")).toInt() == flat_velocity,
            "the lane is Feel's decision for each note");
  }
  // Hand-checked against ADR-0088's rules: the off-beat repeat of E1 is a
  // ghost at its fixed velocity, the pop to E2 a staccato, the sixteenth
  // held past the next eighth in bar 2 pushed, and only off-beats late.
  require(notes[1].toMap().value(QStringLiteral("articulation")).toString() ==
                  QStringLiteral("ghost") &&
              notes[1].toMap().value(QStringLiteral("velocity")).toInt() == feel::ghost_velocity,
          "the repeated E1 ghosts");
  require(notes[2].toMap().value(QStringLiteral("articulation")).toString() ==
              QStringLiteral("staccato"),
          "the short pop is staccato");
  require(notes[9].toMap().value(QStringLiteral("pushed")).toBool(), "the held sixteenth pushes");
  require(notes[0].toMap().value(QStringLiteral("delay_ms")).toDouble() == 0.0 &&
              notes[1].toMap().value(QStringLiteral("delay_ms")).toDouble() == 10.0,
          "Pocket delays the off-beats only");
  require(notes[0].toMap().value(QStringLiteral("name")).toString() == QStringLiteral("E1") &&
              notes[0].toMap().value(QStringLiteral("pitch")).toString() == QStringLiteral("E"),
          "notes are named B0 = 23");

  // Articulation forces one sound on every note, as the kernel does.
  controller.set_choice(QStringLiteral("fret.articulation"), 4);
  for (const auto& value : controller.fret_groove(false).value(QStringLiteral("notes")).toList()) {
    require(value.toMap().value(QStringLiteral("articulation")).toString() ==
                QStringLiteral("bridge"),
            "Behind the bridge plays every note");
  }
  // As drawn: the clip's own velocities, plucked.
  for (const auto& value : controller.fret_groove(true).value(QStringLiteral("notes")).toList()) {
    const auto note = value.toMap();
    require(note.value(QStringLiteral("velocity")).toInt() == flat_velocity &&
                note.value(QStringLiteral("articulation")).toString() == QStringLiteral("pluck") &&
                !note.value(QStringLiteral("pushed")).toBool(),
            "compare shows the notes as drawn");
  }
}

// ---- QML ------------------------------------------------------------------------

struct Panel {
  QQmlEngine engine;
  std::unique_ptr<QQuickWindow> window = std::make_unique<QQuickWindow>();
  QQuickItem* root = nullptr;
};

std::unique_ptr<Panel> load_panel(NativeSynthDeviceController& controller) {
  auto panel = std::make_unique<Panel>();
  QQmlComponent component(&panel->engine);
  component.setData(R"(
      import QtQuick
      import "qrc:/fret"
      FretPanel {}
    )",
                    QUrl(QStringLiteral("qrc:/FretPanelTest.qml")));
  if (!component.isReady()) {
    throw std::runtime_error("Fret panel failed to load: " + component.errorString().toStdString());
  }
  QObject* object = component.createWithInitialProperties(
      {{QStringLiteral("controller"), QVariant::fromValue<QObject*>(&controller)}});
  require(object != nullptr,
          "Fret panel was not created: " + component.errorString().toStdString());
  panel->root = qobject_cast<QQuickItem*>(object);
  require(panel->root != nullptr, "panel is a Quick item");
  panel->root->setWidth(panel->root->implicitWidth());
  panel->root->setHeight(panel->root->implicitHeight());
  panel->window->resize(static_cast<int>(panel->root->width()),
                        static_cast<int>(panel->root->height()));
  panel->root->setParentItem(panel->window->contentItem());
  panel->window->show();
  require(QTest::qWaitForWindowExposed(panel->window.get()), "panel window is exposed");
  QCoreApplication::processEvents();
  return panel;
}

QQuickItem* find_item(QQuickItem* parent, const QString& name) {
  if (parent->objectName() == name) {
    return parent;
  }
  for (QQuickItem* child : parent->childItems()) {
    if (QQuickItem* found = find_item(child, name)) {
      return found;
    }
  }
  return nullptr;
}

QQuickItem* item(Panel& panel, const QString& name) {
  auto* found = find_item(panel.root, name);
  require(found != nullptr, "missing item " + name.toStdString());
  return found;
}

QPoint centre(QQuickItem* target) {
  return target->mapToScene(QPointF(target->width() / 2, target->height() / 2)).toPoint();
}

void click(Panel& panel, QPoint point) {
  QTest::mouseClick(panel.window.get(), Qt::LeftButton, Qt::NoModifier, point);
}

QString text_of(Panel& panel, const QString& name) {
  return item(panel, name)->property("text").toString();
}

// Without its library the panel says why and how to install it; once
// installed, Look Again finds it and the credit shows.
void the_library_plate_guides_the_install() {
  InMemoryNativeSynthAdapter adapter(synth::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(item(*panel, QStringLiteral("fretLibraryMessage"))->isVisible() &&
              text_of(*panel, QStringLiteral("fretLibraryMessage"))
                  .contains(qs(fret::library::attribution)),
          "the message names the library and its credit");
  require(text_of(*panel, QStringLiteral("fretInstallCommand")) ==
              qs(fret::library::install_command),
          "the install command is shown exactly");
  require(text_of(*panel, QStringLiteral("fretLibraryState")) == QStringLiteral("NOT INSTALLED"),
          "the plate says it is not installed");
  require(!item(*panel, QStringLiteral("fretAttribution"))->isVisible(), "no credit yet");
  click(*panel, centre(item(*panel, QStringLiteral("fretCopyInstall"))));
  require(controller.clipboard_text() == qs(fret::library::install_command),
          "Copy puts the command on the clipboard");

  const auto directory =
      std::filesystem::path(std::getenv(std::string(fret::library::directory_variable).c_str()));
  fret_test::write_library(directory, fret_test::resident_note_seconds);
  click(*panel, centre(item(*panel, QStringLiteral("fretLibraryReload"))));
  require(QTest::qWaitFor(
              [&] { return item(*panel, QStringLiteral("fretAttribution"))->isVisible(); }, 1'000),
          "Look Again finds the installed library");
  require(controller.fret_library().value(QStringLiteral("state")).toString() ==
              QStringLiteral("ready"),
          "the controller reports it ready");
  require(text_of(*panel, QStringLiteral("fretAttribution")) ==
                  controller.fret_library().value(QStringLiteral("attribution")).toString() &&
              !text_of(*panel, QStringLiteral("fretAttribution")).isEmpty() &&
              !item(*panel, QStringLiteral("fretLibraryMessage"))->isVisible() &&
              !item(*panel, QStringLiteral("fretInstallCommand"))->isVisible(),
          "the library's own credit replaces the install steps");
}

void panel_covers_every_parameter() {
  InMemoryNativeSynthAdapter adapter(synth::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(panel->root->implicitWidth() >= 1280 && panel->root->implicitHeight() >= 800,
          "panel has its designed size");
  for (const auto& p : table().parameters) {
    const QString id = qs(p.id);
    const bool shown = find_item(panel->root, QStringLiteral("fretSlider:") + id) != nullptr ||
                       find_item(panel->root, QStringLiteral("fretChoice:") + id) != nullptr;
    require(shown, "no control for " + id.toStdString());
  }
  require(text_of(*panel, QStringLiteral("fretPresetName")) == QStringLiteral("Funk Pocket"),
          "defaults are the Funk Pocket");
  require(text_of(*panel, QStringLiteral("fretSubtitle")) ==
              QStringLiteral("Black and Blue · fingered 5-string · B0 to E4"),
          "the header names the bass");
}

// The tiles write their parameters; sliders step from the keyboard.
void tiles_and_sliders_write_their_parameters() {
  InMemoryNativeSynthAdapter adapter(synth::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  const auto style = static_cast<int>(feel::Style::laid_back);
  click(*panel, centre(item(*panel, QStringLiteral("fretChoice:fret.feel.style:%1").arg(style))));
  require(controller.choice(QStringLiteral("fret.feel.style")) == style,
          "a style tile writes fret.feel.style");
  click(*panel, centre(item(*panel, QStringLiteral("fretChoice:fret.feel.style:0"))));
  require(controller.choice(QStringLiteral("fret.feel.style")) == 0, "As drawn is a tile too");
  click(*panel, centre(item(*panel, QStringLiteral("fretChoice:fret.feel.ghosts:0"))));
  require(controller.choice(QStringLiteral("fret.feel.ghosts")) == 0, "Ghost notes turn off");
  click(*panel, centre(item(*panel, QStringLiteral("fretChoice:fret.articulation:3"))));
  require(controller.choice(QStringLiteral("fret.articulation")) == 3, "Ghost is forced");

  auto* pocket = item(*panel, QStringLiteral("fretSlider:fret.feel.pocket"));
  pocket->forceActiveFocus();
  QTest::keyClick(panel->window.get(), Qt::Key_Right);
  require(controller.value(QStringLiteral("fret.feel.pocket")) > 0.0, "Right moves Pocket later");
  QTest::keyClick(panel->window.get(), Qt::Key_Delete);
  require(controller.value(QStringLiteral("fret.feel.pocket")) == 0.0, "Delete resets it");
  require(text_of(*panel, QStringLiteral("fretSliderValue:fret.feel.pocket")) ==
              QStringLiteral("off"),
          "no Pocket reads off");
}

QQuickItem* lane_note(Panel& panel, std::size_t index) {
  return item(panel, QStringLiteral("fretGrooveNote:%1").arg(index));
}

// The lane draws the controller's groove: each note's colour is its
// articulation's, its bar as tall as its velocity, ringed when pushed.
void the_lane_draws_what_feel_plays() {
  InMemoryNativeSynthAdapter adapter(synth::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(item(*panel, QStringLiteral("fretGrooveHint"))->isVisible() &&
              text_of(*panel, QStringLiteral("fretGrooveHint")) ==
                  QStringLiteral("Draw notes in a clip to see Feel play them"),
          "an empty lane says how to fill it");
  draw_line(adapter);
  // A project change (the clip) reaches the panel through the status.
  controller.refresh();
  QCoreApplication::processEvents();
  require(QTest::qWaitFor(
              [&] { return find_item(panel->root, QStringLiteral("fretGrooveNote:0")) != nullptr; },
              1'000),
          "the clip's notes arrive");
  require(!item(*panel, QStringLiteral("fretGrooveHint"))->isVisible(), "the hint goes");
  auto* theme = panel->root->property("theme").value<QObject*>();
  auto* lane = item(*panel, QStringLiteral("fretGroove"));
  const double room = lane_note(*panel, 0)->height() - lane->property("velocityHeight").toDouble();
  const auto oracle = expected(controller.current_values());
  for (std::size_t i = 0; i < line.size(); ++i) {
    auto* note = lane_note(*panel, i);
    const auto& played = oracle[i];
    const auto colour = theme->property(articulation_name(played.articulation).toUtf8().constData())
                            .value<QColor>();
    require(note->property("noteColor").value<QColor>() == colour,
            "a note is coloured by its articulation");
    require(std::fabs(note->property("barHeight").toDouble() -
                      room * played.decision.velocity / feel::max_velocity) < 1e-6,
            "a note stands as tall as its velocity");
    require(note->property("pushed").toBool() == played.decision.pushed, "pushed notes are ringed");
  }
  require(lane_note(*panel, 1)->property("noteColor").value<QColor>() ==
              theme->property("ghost").value<QColor>(),
          "the ghost is slate");

  // The lane follows the panel's values at once.
  click(*panel, centre(item(*panel, QStringLiteral("fretChoice:fret.articulation:2"))));
  require(QTest::qWaitFor(
              [&] {
                return lane_note(*panel, 0)->property("articulation").toString() ==
                       QStringLiteral("staccato");
              },
              1'000),
          "forcing Staccato recolours the lane");
  click(*panel, centre(item(*panel, QStringLiteral("fretCompare"))));
  require(QTest::qWaitFor(
              [&] { return lane_note(*panel, 1)->property("velocity").toInt() == flat_velocity; },
              1'000),
          "Compare shows the notes as drawn");
}

// The header shows the latest note as Feel played it, and the output.
void the_last_note_reads_out_live() {
  InMemoryNativeSynthAdapter adapter(synth::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(text_of(*panel, QStringLiteral("fretLastNote")) == QStringLiteral("—"),
          "no note before any plays");
  adapter.set_telemetry(fret_frame(40, 108, feel::Articulation::staccato, true));
  require(QTest::qWaitFor(
              [&] {
                return text_of(*panel, QStringLiteral("fretLastNote")) ==
                       QStringLiteral("E2 · 108 · staccato · pushed");
              },
              1'000),
          "the latest note reads out");
  require(item(*panel, QStringLiteral("fretOutputMeter"))->property("level").toDouble() > 0.0,
          "the output meter moves");
}

void screenshots_if_requested() {
  const char* directory = std::getenv("FRET_PANEL_SCREENSHOT_DIR");
  if (directory == nullptr || *directory == '\0') {
    return;
  }
  QDir().mkpath(QString::fromLocal8Bit(directory));
  InMemoryNativeSynthAdapter adapter(synth::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  draw_line(adapter);
  auto panel = load_panel(controller);
  adapter.set_telemetry(fret_frame(28, 112, feel::Articulation::pluck, false));
  QTest::qWait(400);
  const QString path = QString::fromLocal8Bit(directory) + QStringLiteral("/fret_panel.png");
  require(panel->window->grabWindow().save(path, "PNG"), "screenshot saved");
  std::cout << "wrote " << path.toStdString() << '\n';
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  try {
    controller_reports_a_missing_library();
    controller_decodes_fret_telemetry();
    controller_groove_follows_feel();
    the_library_plate_guides_the_install();
    panel_covers_every_parameter();
    tiles_and_sliders_write_their_parameters();
    the_lane_draws_what_feel_plays();
    the_last_note_reads_out_live();
    screenshots_if_requested();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "fret panel contract passed\n";
  return 0;
}
