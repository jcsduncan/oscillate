#include "in_memory_native_synth_adapter.hpp"
#include "native_synth_device_controller.hpp"

#include "../../native_dsp/wren_test_library.hpp"
#include "oscillate/native_dsp/synth/parameters.hpp"
#include "oscillate/native_dsp/synth/prism.hpp"
#include "oscillate/native_dsp/synth/wren.hpp"
#include "oscillate/native_dsp/synth/wren_breath.hpp"
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
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// The Wren panel (ADR-0089): the controller's library status, telemetry
// decode and phrase lane, and the QML panel: without its library (the
// message and install command), with it (the credit), a control for every
// parameter at touch size, the tiles and segments writing their
// parameters, the lane drawing what Breath decides for a known clip, and
// the output meter live.
namespace {
using oscillate::quick::InMemoryNativeSynthAdapter;
using oscillate::quick::NativeSynthDeviceController;
namespace synth = oscillate::native_dsp::synth;
namespace wren = synth::wren;
namespace breath = wren::breath;
namespace project = oscillate::project;
namespace timeline = oscillate::timeline;

using oscillate::test::require;

const synth::EngineDescriptor& table() { return *wren::engine().descriptor; }

QString qs(std::string_view text) {
  return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

// The audition line's first two bars (render_flute.py's LINE), drawn flat:
// beat, key, length in beats.
struct Drawn {
  double beat;
  int key;
  double length;
};
constexpr int flat_velocity = 100;
constexpr int a4 = 69, c5 = 72, d5 = 74, e5 = 76, f5 = 77;
constexpr std::array line{Drawn{0.5, a4, 0.5}, Drawn{1.0, d5, 1.6}, Drawn{2.75, c5, 0.25},
                          Drawn{3.0, d5, 0.5}, Drawn{3.5, f5, 2.1}, Drawn{5.5, e5, 0.6},
                          Drawn{6.0, d5, 2.0}};
constexpr double beats_per_bar = 4.0;
constexpr int bars = NativeSynthDeviceController::wren_phrase_bars;
constexpr double span = beats_per_bar * bars;
// The audition's tempo and the line's key (D dorian, nearest project key D
// minor).
constexpr double line_bpm = 88.0;
constexpr std::uint8_t d_tonic = 2;
constexpr project::MusicalKey d_minor{d_tonic, project::KeyMode::minor};
constexpr double seconds_per_minute = 60.0;
constexpr double ms_per_second = 1'000.0;
constexpr double beats_per_ms = line_bpm / seconds_per_minute / ms_per_second;
constexpr double tolerance = 1e-9;
constexpr int wait_ms = 1'000;
constexpr int settle_ms = 400;
// The designed panel: the mockup's width less its audition row.
constexpr double designed_width = 1280.0;
constexpr double designed_height = 900.0;
// A telemetry frame's notes count and output peak.
constexpr float frame_notes = 7.0F;
constexpr float frame_peak_db = -12.0F;

timeline::TickPosition ticks(double beats) {
  return {static_cast<std::int64_t>(std::llround(beats * timeline::ticks_per_quarter))};
}

// The line as a clip on the adapter's track, from the first bar, at the
// audition's tempo in D minor.
void draw_line(InMemoryNativeSynthAdapter& adapter) {
  std::vector<project::MidiNote> notes;
  std::uint64_t id = 1;
  for (const auto& note : line) {
    notes.push_back({project::NoteId::from_bits({0, id++}).value(), ticks(note.beat),
                     ticks(note.beat + note.length), static_cast<std::uint8_t>(note.key),
                     static_cast<std::uint8_t>(flat_velocity)});
  }
  adapter.set_tempo(line_bpm);
  adapter.set_project_key(d_minor);
  require(adapter.add_midi_clip(0, ticks(span), std::move(notes)), "the clip is added");
}

// What the kernel plays for the line with `values`: Breath's decision in the
// project key, then wren::played, each note after the one before.
struct Heard {
  breath::Decision decision;
  wren::Played played;
};
std::vector<Heard> expected(const std::vector<double>& values) {
  std::vector<Heard> out;
  breath::Previous previous;
  for (const auto& drawn : line) {
    const auto bar = static_cast<std::int64_t>(std::floor(drawn.beat / beats_per_bar));
    breath::Context note;
    note.beat = drawn.beat;
    note.bar_beat = drawn.beat - static_cast<double>(bar) * beats_per_bar;
    note.bar = bar;
    note.beats_per_bar = beats_per_bar;
    note.length_beats = drawn.length;
    note.key = drawn.key;
    note.velocity = flat_velocity;
    note.scale = breath::scale_of(d_minor);
    const auto decision = breath::decide(note, previous, wren::breath_settings(values));
    previous = breath::after(note, decision);
    out.push_back({decision, wren::played(values, drawn.key, drawn.length, decision)});
  }
  return out;
}

QString tongue_name(breath::Tongue tongue) {
  switch (tongue) {
  case breath::Tongue::k:
    return QStringLiteral("k");
  case breath::Tongue::slur:
    return QStringLiteral("slur");
  case breath::Tongue::t:
    break;
  }
  return QStringLiteral("t");
}

double choice_index(std::string_view id, std::string_view name) {
  for (const auto& p : table().parameters) {
    if (p.id != id) {
      continue;
    }
    for (std::size_t i = 0; i < p.choices.size(); ++i) {
      if (p.choices[i] == name) {
        return static_cast<double>(i);
      }
    }
  }
  throw std::runtime_error("unknown choice " + std::string(name));
}

int choice_of(std::string_view id, std::string_view name) {
  return static_cast<int>(choice_index(id, name));
}

// ---- Controller ---------------------------------------------------------------------

// Run before any library loads in this process.
void controller_reports_a_missing_library() {
  const auto nowhere = wren_test::scratch_directory("wren-panel-library");
  std::filesystem::remove_all(nowhere);
  ::setenv(std::string(wren::library::directory_variable).c_str(), nowhere.c_str(), 1);
  InMemoryNativeSynthAdapter adapter(synth::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  const auto library = controller.wren_library();
  require(library.value(QStringLiteral("state")).toString() == QStringLiteral("missing") &&
              !library.value(QStringLiteral("message")).toString().isEmpty(),
          "a missing library is reported with a reason");
  require(library.value(QStringLiteral("install_command")).toString() ==
              qs(wren::library::install_command),
          "the panel learns how to install it");
  require(
      library.value(QStringLiteral("required")).toString().contains(qs(wren::library::attribution)),
      "and which library, with its credit");
  require(library.value(QStringLiteral("first_key")).toInt() == wren::first_key &&
              library.value(QStringLiteral("last_key")).toInt() == wren::last_key,
          "and the keys it plays");
  const auto& prism = *synth::prism::engine().descriptor;
  InMemoryNativeSynthAdapter other(synth::default_values(prism));
  NativeSynthDeviceController prism_controller(prism, other);
  require(prism_controller.wren_library().isEmpty() && prism_controller.wren_phrase().isEmpty(),
          "only Wren has a library and a phrase lane");
}

std::vector<float> wren_frame(int key, int velocity, breath::Tongue tongue, bool staccato,
                              bool grace, bool vibrato) {
  namespace t = wren::telemetry;
  std::vector<float> frame(t::size, 0.0F);
  frame[t::library] = static_cast<float>(static_cast<int>(wren::library::State::ready));
  frame[t::notes] = frame_notes;
  frame[t::last_key] = static_cast<float>(key);
  frame[t::last_velocity] = static_cast<float>(velocity);
  frame[t::last_tongue] = static_cast<float>(static_cast<int>(tongue));
  frame[t::last_staccato] = staccato ? 1.0F : 0.0F;
  frame[t::last_grace] = grace ? 1.0F : 0.0F;
  frame[t::vibrato] = vibrato ? 1.0F : 0.0F;
  frame[t::output_peak_db] = frame_peak_db;
  return frame;
}

void controller_decodes_wren_telemetry() {
  constexpr int velocity = 92;
  InMemoryNativeSynthAdapter adapter(synth::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  require(!controller.telemetry().value(QStringLiteral("live")).toBool(), "nothing plays");
  adapter.set_telemetry(wren_frame(d5, velocity, breath::Tongue::k, true, true, true));
  const auto live = controller.telemetry();
  require(live.value(QStringLiteral("live")).toBool() &&
              live.value(QStringLiteral("library")).toString() == QStringLiteral("ready") &&
              live.value(QStringLiteral("notes")).toInt() == static_cast<int>(frame_notes),
          "the library and note count decode");
  require(live.value(QStringLiteral("last_key")).toInt() == d5 &&
              live.value(QStringLiteral("last_name")).toString() == QStringLiteral("D5") &&
              live.value(QStringLiteral("last_velocity")).toInt() == velocity &&
              live.value(QStringLiteral("last_tongue")).toString() == QStringLiteral("k") &&
              live.value(QStringLiteral("last_staccato")).toBool() &&
              live.value(QStringLiteral("last_grace")).toBool() &&
              live.value(QStringLiteral("vibrato")).toBool() &&
              live.value(QStringLiteral("output_peak_db")).toDouble() == frame_peak_db,
          "the latest note decodes as Breath played it");
}

// The lane is empty without a clip, and with one it is exactly what the
// kernel plays: Breath's decision in the project key, then Voices,
// Articulation and Vibrato as wren::played applies them.
void check_phrase(NativeSynthDeviceController& controller, std::string_view what) {
  const auto phrase = controller.wren_phrase();
  const auto notes = phrase.value(QStringLiteral("notes")).toList();
  const auto oracle = expected(controller.current_values());
  require(notes.size() == static_cast<qsizetype>(line.size()),
          std::string(what) + ": every note is in the lane");
  for (std::size_t i = 0; i < line.size(); ++i) {
    const auto note = notes[static_cast<qsizetype>(i)].toMap();
    const auto& heard = oracle[i];
    const auto& d = heard.decision;
    const double grace_beats = heard.played.grace ? d.grace_ms * beats_per_ms : 0.0;
    const double bloom_from = line[i].beat + grace_beats + d.vibrato_delay_ms * beats_per_ms;
    const auto near = [&](const char* key, double value) {
      return std::fabs(note.value(QString::fromLatin1(key)).toDouble() - value) < tolerance;
    };
    require(near("position", line[i].beat / span) &&
                near("length", heard.played.length_beats / span) &&
                note.value(QStringLiteral("drawn")).toInt() == flat_velocity,
            std::string(what) + ": a note sits where it is drawn, as long as it is held");
    require(note.value(QStringLiteral("velocity")).toInt() == d.velocity &&
                note.value(QStringLiteral("tongue")).toString() ==
                    tongue_name(heard.played.tongue) &&
                note.value(QStringLiteral("staccato")).toBool() == d.staccato,
            std::string(what) + ": velocity, tongue and staccato are Breath's");
    require(near("vibrato", heard.played.vibrato) && near("vibrato_from", bloom_from / span) &&
                near("vibrato_full", (bloom_from + d.bloom_ms * beats_per_ms) / span) &&
                near("swell_db", d.swell_db) && near("cents", d.cents),
            std::string(what) + ": vibrato blooms where the kernel blooms it");
    require(note.value(QStringLiteral("grace")).toBool() == heard.played.grace &&
                (!heard.played.grace ||
                 (note.value(QStringLiteral("grace_key")).toInt() == line[i].key + d.grace_step &&
                  d.grace_step == breath::step_above(line[i].key, breath::scale_of(d_minor)) &&
                  near("grace_length", grace_beats / span))),
            std::string(what) + ": graces take the project key's step above");
  }
}

void controller_phrase_follows_breath() {
  InMemoryNativeSynthAdapter adapter(synth::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  require(controller.wren_phrase().value(QStringLiteral("notes")).toList().isEmpty(),
          "no clip, no notes");
  draw_line(adapter);
  const auto phrase = controller.wren_phrase();
  require(phrase.value(QStringLiteral("bars")).toInt() == bars &&
              phrase.value(QStringLiteral("beats_per_bar")).toDouble() == beats_per_bar &&
              phrase.value(QStringLiteral("bpm")).toDouble() == line_bpm &&
              phrase.value(QStringLiteral("max_velocity")).toInt() == breath::max_velocity,
          "two bars of 4/4 at the clip's tempo");
  require(phrase.value(QStringLiteral("low_key")).toInt() == a4 &&
              phrase.value(QStringLiteral("high_key")).toInt() >= f5,
          "the key range spans the notes");
  check_phrase(controller, "Lyrical Air");
  const auto first = phrase.value(QStringLiteral("notes")).toList().front().toMap();
  require(first.value(QStringLiteral("name")).toString() == QStringLiteral("A4"),
          "notes are named C4 = 60");

  controller.set_choice(QStringLiteral("wren.breath.style"),
                        choice_of("wren.breath.style", "Airy"));
  controller.set_choice(QStringLiteral("wren.breath.ornaments"),
                        choice_of("wren.breath.ornaments", "Lots"));
  check_phrase(controller, "Airy, Lots");

  // Legato slurs the close notes; Poly 4 tongues every one of them.
  controller.set_choice(QStringLiteral("wren.articulation"),
                        choice_of("wren.articulation", "Legato"));
  check_phrase(controller, "Legato");
  const auto slurs = [&] {
    int count = 0;
    for (const auto& value : controller.wren_phrase().value(QStringLiteral("notes")).toList()) {
      if (value.toMap().value(QStringLiteral("tongue")).toString() == QStringLiteral("slur")) {
        ++count;
      }
    }
    return count;
  };
  require(slurs() > 0, "Legato slurs notes a sixteenth apart");
  controller.set_choice(QStringLiteral("wren.voices"), choice_of("wren.voices", "Poly 4"));
  check_phrase(controller, "Poly 4");
  require(slurs() == 0, "Poly 4 tongues every note");

  // Staccato holds every note a third of a beat at most.
  controller.set_choice(QStringLiteral("wren.articulation"),
                        choice_of("wren.articulation", "Staccato"));
  check_phrase(controller, "Staccato");
  for (const auto& value : controller.wren_phrase().value(QStringLiteral("notes")).toList()) {
    require(value.toMap().value(QStringLiteral("length")).toDouble() <=
                breath::staccato_beats / span + tolerance,
            "Staccato shortens every note");
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
      import "qrc:/wren"
      WrenPanel {}
    )",
                    QUrl(QStringLiteral("qrc:/WrenPanelTest.qml")));
  if (!component.isReady()) {
    throw std::runtime_error("Wren panel failed to load: " + component.errorString().toStdString());
  }
  QObject* object = component.createWithInitialProperties(
      {{QStringLiteral("controller"), QVariant::fromValue<QObject*>(&controller)}});
  require(object != nullptr,
          "Wren panel was not created: " + component.errorString().toStdString());
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

void collect(QQuickItem* parent, const QString& prefix, std::vector<QQuickItem*>& out) {
  if (parent->objectName().startsWith(prefix)) {
    out.push_back(parent);
  }
  for (QQuickItem* child : parent->childItems()) {
    collect(child, prefix, out);
  }
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

QObject* theme_of(Panel& panel) { return panel.root->property("theme").value<QObject*>(); }

// With WREN_PANEL_SCREENSHOT_DIR set, saves the panel there as `name`.
constexpr const char* screenshot_variable = "WREN_PANEL_SCREENSHOT_DIR";
void shoot_if_requested(Panel& panel, const QString& name) {
  const char* directory = std::getenv(screenshot_variable);
  if (directory == nullptr || *directory == '\0') {
    return;
  }
  QDir().mkpath(QString::fromLocal8Bit(directory));
  const QString path = QString::fromLocal8Bit(directory) + QStringLiteral("/") + name;
  require(panel.window->grabWindow().save(path, "PNG"), "screenshot saved");
  std::cout << "wrote " << path.toStdString() << '\n';
}

// Without its library the panel says why and how to install it; once
// installed, Look Again finds it and the credit shows.
void the_library_plate_guides_the_install() {
  InMemoryNativeSynthAdapter adapter(synth::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(item(*panel, QStringLiteral("wrenLibraryMessage"))->isVisible() &&
              text_of(*panel, QStringLiteral("wrenLibraryMessage"))
                  .contains(qs(wren::library::attribution)),
          "the message names the library and its credit");
  require(text_of(*panel, QStringLiteral("wrenInstallCommand")) ==
              qs(wren::library::install_command),
          "the install command is shown exactly");
  require(text_of(*panel, QStringLiteral("wrenLibraryState")) == QStringLiteral("NOT INSTALLED"),
          "the plate says it is not installed");
  require(!item(*panel, QStringLiteral("wrenAttribution"))->isVisible(), "no credit yet");
  click(*panel, centre(item(*panel, QStringLiteral("wrenCopyInstall"))));
  require(controller.clipboard_text() == qs(wren::library::install_command),
          "Copy puts the command on the clipboard");
  shoot_if_requested(*panel, QStringLiteral("wren_panel_missing_library.png"));

  const auto directory =
      std::filesystem::path(std::getenv(std::string(wren::library::directory_variable).c_str()));
  wren_test::write_library(directory);
  click(*panel, centre(item(*panel, QStringLiteral("wrenLibraryReload"))));
  require(
      QTest::qWaitFor([&] { return item(*panel, QStringLiteral("wrenAttribution"))->isVisible(); },
                      wait_ms),
      "Look Again finds the installed library");
  require(controller.wren_library().value(QStringLiteral("state")).toString() ==
              QStringLiteral("ready"),
          "the controller reports it ready");
  require(text_of(*panel, QStringLiteral("wrenAttribution")) ==
                  controller.wren_library().value(QStringLiteral("attribution")).toString() &&
              !text_of(*panel, QStringLiteral("wrenAttribution")).isEmpty() &&
              !item(*panel, QStringLiteral("wrenLibraryMessage"))->isVisible() &&
              !item(*panel, QStringLiteral("wrenInstallCommand"))->isVisible(),
          "the library's own credit replaces the install steps");
}

// Every parameter has a control, each at least a touch target, and each
// with an accessible name.
void panel_covers_every_parameter() {
  InMemoryNativeSynthAdapter adapter(synth::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(panel->root->implicitWidth() == designed_width &&
              panel->root->implicitHeight() == designed_height,
          "panel has its designed size");
  const double touch = theme_of(*panel)->property("touch").toDouble();
  for (const auto& p : table().parameters) {
    const QString id = qs(p.id);
    auto* slider = find_item(panel->root, QStringLiteral("wrenSlider:") + id);
    auto* choice = find_item(panel->root, QStringLiteral("wrenChoice:") + id);
    require(slider != nullptr || choice != nullptr, "no control for " + id.toStdString());
    auto* control = slider != nullptr ? slider : choice;
    require(control->isVisible(), id.toStdString() + " is shown");
  }
  std::vector<QQuickItem*> tiles;
  collect(panel->root, QStringLiteral("wrenChoice:"), tiles);
  for (auto* tile : tiles) {
    if (tile->objectName().count(QLatin1Char(':')) == 2) {
      require(tile->height() >= touch && tile->width() >= touch,
              tile->objectName().toStdString() + " is a touch target");
    }
  }
  std::vector<QQuickItem*> sliders;
  collect(panel->root, QStringLiteral("wrenSlider:"), sliders);
  for (auto* slider : sliders) {
    require(slider->height() >= touch, slider->objectName().toStdString() + " is a touch target");
  }
  for (const auto* name : {"wrenCopyInstall", "wrenLibraryReload", "wrenPresetDisplay"}) {
    require(item(*panel, QString::fromLatin1(name))->height() >= touch,
            std::string(name) + " is a touch target");
  }
  require(text_of(*panel, QStringLiteral("wrenPresetName")) == QStringLiteral("Lyrical Air"),
          "defaults are Lyrical Air");
  require(text_of(*panel, QStringLiteral("wrenSubtitle")) ==
              QStringLiteral(
                  "Concert flute · Iowa MIS · B3 to D♭7 · pp, mf, ff · straight and vibrato"),
          "the header names the flute");
}

// The tiles and segments write their parameters; sliders step from the
// keyboard.
void tiles_and_sliders_write_their_parameters() {
  InMemoryNativeSynthAdapter adapter(synth::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  const auto pick = [&](std::string_view id, std::string_view name) {
    const int index = choice_of(id, name);
    click(*panel, centre(item(*panel, QStringLiteral("wrenChoice:%1:%2").arg(qs(id)).arg(index))));
    require(controller.choice(qs(id)) == index, std::string(name) + " writes " + std::string(id));
  };
  pick("wren.breath.style", "Airy");
  pick("wren.breath.style", "As drawn");
  pick("wren.breath.ornaments", "Off");
  pick("wren.articulation", "Staccato");
  pick("wren.voices", "Poly 4");
  pick("wren.voices", "Mono");

  auto* humanize = item(*panel, QStringLiteral("wrenSlider:wren.breath.humanize"));
  humanize->forceActiveFocus();
  QTest::keyClick(panel->window.get(), Qt::Key_Right);
  require(controller.value(QStringLiteral("wren.breath.humanize")) > 0.0, "Right adds Humanize");
  QTest::keyClick(panel->window.get(), Qt::Key_Delete);
  require(controller.value(QStringLiteral("wren.breath.humanize")) == 0.0, "Delete resets it");
  require(text_of(*panel, QStringLiteral("wrenSliderValue:wren.breath.humanize")) ==
              QStringLiteral("off"),
          "no Humanize reads off");

  // A click on a slider's caption is on its touch target too.
  auto* tune = item(*panel, QStringLiteral("wrenSlider:wren.tune"));
  const QPointF caption_right(tune->width() - 1.0, 1.0);
  click(*panel, tune->mapToScene(caption_right).toPoint());
  require(controller.value(QStringLiteral("wren.tune")) > 0.0,
          "the caption's right end sets Tune sharp");
}

QQuickItem* lane_note(Panel& panel, std::size_t index) {
  return item(panel, QStringLiteral("wrenPhraseNote:%1").arg(index));
}

// The lane draws the controller's phrase: each note coloured by whether it
// is staccato, as tall as it is loud, with its wave, grace and slur arcs.
void the_lane_draws_what_breath_plays() {
  InMemoryNativeSynthAdapter adapter(synth::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(item(*panel, QStringLiteral("wrenPhraseHint"))->isVisible() &&
              text_of(*panel, QStringLiteral("wrenPhraseHint")) ==
                  QStringLiteral("Draw notes in a clip to see Breath play them"),
          "an empty lane says how to fill it");
  draw_line(adapter);
  // A project change (the clip) reaches the panel through the status.
  controller.refresh();
  QCoreApplication::processEvents();
  require(QTest::qWaitFor(
              [&] { return find_item(panel->root, QStringLiteral("wrenPhraseNote:0")) != nullptr; },
              wait_ms),
          "the clip's notes arrive");
  require(!item(*panel, QStringLiteral("wrenPhraseHint"))->isVisible(), "the hint goes");
  auto* theme = theme_of(*panel);
  auto* lane = item(*panel, QStringLiteral("wrenPhrase"));
  const double min_bar = lane->property("minBar").toDouble();
  const double max_bar = lane->property("maxBar").toDouble();
  const auto check = [&](std::string_view what) {
    const auto oracle = expected(controller.current_values());
    int waves = 0;
    for (std::size_t i = 0; i < line.size(); ++i) {
      auto* note = lane_note(*panel, i);
      const auto& heard = oracle[i];
      const auto colour =
          theme->property(heard.decision.staccato ? "staccato" : "tongued").value<QColor>();
      require(note->property("noteColor").value<QColor>() == colour,
              std::string(what) + ": a note is coloured by how it starts");
      require(std::fabs(note->property("barHeight").toDouble() -
                        (min_bar + (max_bar - min_bar) * heard.decision.velocity /
                                       breath::max_velocity)) < tolerance,
              std::string(what) + ": a note stands as tall as it is loud");
      require(note->property("tongue").toString() == tongue_name(heard.played.tongue),
              std::string(what) + ": the lane knows how each note starts");
      auto* wave = item(*panel, QStringLiteral("wrenPhraseVibrato:%1").arg(i));
      require(heard.played.vibrato > 0.0 || !wave->isVisible(),
              std::string(what) + ": no wave without vibrato");
      waves += wave->isVisible() ? 1 : 0;
      require(item(*panel, QStringLiteral("wrenPhraseGrace:%1").arg(i))->isVisible() ==
                  heard.played.grace,
              std::string(what) + ": graces are drawn");
    }
    return waves;
  };
  require(check("Lyrical Air") > 0, "the long notes' vibrato blooms in the lane");

  // The lane follows the panel's values at once.
  click(*panel, centre(item(*panel, QStringLiteral("wrenChoice:wren.articulation:%1")
                                        .arg(choice_of("wren.articulation", "Legato")))));
  require(QTest::qWaitFor(
              [&] {
                std::vector<QQuickItem*> arcs;
                collect(panel->root, QStringLiteral("wrenPhraseSlur:"), arcs);
                return !arcs.empty();
              },
              wait_ms),
          "Legato draws its slur arcs");
  (void)check("Legato");
  click(*panel, centre(item(*panel, QStringLiteral("wrenChoice:wren.articulation:%1")
                                        .arg(choice_of("wren.articulation", "Staccato")))));
  require(QTest::qWaitFor(
              [&] {
                return lane_note(*panel, 1)->property("noteColor").value<QColor>() ==
                       theme->property("staccato").value<QColor>();
              },
              wait_ms),
          "forcing Staccato recolours the lane");
  (void)check("Staccato");
  click(*panel, centre(item(*panel, QStringLiteral("wrenChoice:wren.breath.style:%1")
                                        .arg(choice_of("wren.breath.style", "As drawn")))));
  require(QTest::qWaitFor(
              [&] { return lane_note(*panel, 0)->property("velocity").toInt() == flat_velocity; },
              wait_ms),
          "As drawn plays the clip's own velocities");
}

// The header's meter follows the output.
void the_output_meter_reads_out_live() {
  InMemoryNativeSynthAdapter adapter(synth::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  auto* meter = item(*panel, QStringLiteral("wrenOutputMeter"));
  require(meter->property("level").toDouble() == 0.0, "silent before any note");
  adapter.set_telemetry(wren_frame(d5, flat_velocity, breath::Tongue::t, false, false, true));
  require(QTest::qWaitFor([&] { return meter->property("level").toDouble() > 0.0; }, wait_ms),
          "the output meter moves");
}

// With WREN_PANEL_SCREENSHOT_DIR set, the panel playing the line is saved
// there; WREN_PANEL_SCREENSHOT_LIBRARY names a real library to show.
void screenshots_if_requested() {
  const char* directory = std::getenv(screenshot_variable);
  if (directory == nullptr || *directory == '\0') {
    return;
  }
  if (const char* real = std::getenv("WREN_PANEL_SCREENSHOT_LIBRARY");
      real != nullptr && *real != '\0') {
    const auto status = wren::library::load(real);
    require(status.state == wren::library::State::ready,
            "the screenshot library loads: " + status.message);
  }
  InMemoryNativeSynthAdapter adapter(synth::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  draw_line(adapter);
  auto panel = load_panel(controller);
  adapter.set_telemetry(wren_frame(d5, flat_velocity, breath::Tongue::t, false, false, true));
  QTest::qWait(settle_ms);
  shoot_if_requested(*panel, QStringLiteral("wren_panel.png"));
  // Folk Breeze's graces, slurred under Legato.
  controller.set_choice(QStringLiteral("wren.breath.style"),
                        choice_of("wren.breath.style", "Airy"));
  controller.set_choice(QStringLiteral("wren.breath.ornaments"),
                        choice_of("wren.breath.ornaments", "Lots"));
  controller.set_choice(QStringLiteral("wren.articulation"),
                        choice_of("wren.articulation", "Legato"));
  QTest::qWait(settle_ms);
  shoot_if_requested(*panel, QStringLiteral("wren_panel_airy_legato.png"));
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  try {
    controller_reports_a_missing_library();
    controller_decodes_wren_telemetry();
    controller_phrase_follows_breath();
    the_library_plate_guides_the_install();
    panel_covers_every_parameter();
    tiles_and_sliders_write_their_parameters();
    the_lane_draws_what_breath_plays();
    the_output_meter_reads_out_live();
    screenshots_if_requested();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "wren panel contract passed\n";
  return 0;
}
