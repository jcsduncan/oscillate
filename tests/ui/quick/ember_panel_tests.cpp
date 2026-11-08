#include "in_memory_native_synth_adapter.hpp"
#include "native_synth_device_controller.hpp"
#include "native_synth_visuals.hpp"

#include "oscillate/native_dsp/synth/ember.hpp"
#include "oscillate/native_dsp/synth/parameters.hpp"
#include "oscillate/native_dsp/synth/vintage_filters.hpp"
#include "oscillate/test/check.hpp"

#include <QDir>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRectF>
#include <QTest>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using oscillate::quick::InMemoryNativeSynthAdapter;
using oscillate::quick::NativeSynthDeviceController;
using oscillate::quick::NativeSynthGesture;
namespace synth = oscillate::native_dsp::synth;

using oscillate::test::require;

const synth::EngineDescriptor& table() { return *synth::ember::engine().descriptor; }

std::size_t index(std::string_view id) { return *synth::find_parameter(table(), id); }

int preset_index(std::string_view name) {
  for (std::size_t i = 0; i < table().presets.size(); ++i) {
    if (table().presets[i].name == name) {
      return static_cast<int>(i);
    }
  }
  throw std::runtime_error("missing preset");
}

// ---- Visuals ------------------------------------------------------------------

double response_db(std::vector<double> values, double hz) {
  const auto* visuals = oscillate::quick::visuals_for("ember");
  require(visuals != nullptr, "Ember registers visuals");
  return visuals->filter_response_db(table(), values, hz);
}

void filter_visuals_match_the_prototypes() {
  auto values = synth::default_values(table());
  values[index("ember.filter.cutoff")] = 1'000.0;
  values[index("ember.filter.resonance")] = 0.0;
  // OTA at zero resonance: four one-pole stages, -12 dB at cutoff.
  require(std::fabs(response_db(values, 1'000.0) + 20.0 * std::log10(2.0) * 2.0) < 1e-6,
          "OTA 24 is four one-poles at zero resonance");
  require(std::fabs(response_db(values, 10.0)) < 0.01, "OTA passes lows at zero resonance");
  // Resonance costs bass: DC gain (1 + 0.25k) / (1 + k).
  values[index("ember.filter.resonance")] = 0.8;
  const double k = 0.8 * synth::max_ota_feedback;
  require(std::fabs(response_db(values, 1.0) - 20.0 * std::log10((1.0 + 0.25 * k) / (1.0 + k))) <
              0.01,
          "OTA resonance thins the bass like the DSP");
  require(response_db(values, 1'000.0) > response_db(values, 300.0), "OTA resonance peaks");

  values[index("ember.filter.type")] = 1;
  values[index("ember.filter.resonance")] = 0.0;
  values[index("ember.filter.morph")] = 0.0;
  require(std::fabs(response_db(values, 1'000.0) + 3.0103) < 0.01, "SEM LP is -3 dB at cutoff");
  values[index("ember.filter.morph")] = 1.0;
  require(std::fabs(response_db(values, 1'000.0) + 3.0103) < 0.01 &&
              response_db(values, 100.0) < -35.0,
          "SEM HP at morph 1");
  values[index("ember.filter.morph")] = 0.5;
  require(response_db(values, 1'000.0) < -60.0 && std::fabs(response_db(values, 20.0)) < 0.1,
          "SEM notch at morph 0.5");
}

// ---- QML ----------------------------------------------------------------------

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
      import "qrc:/ember"
      EmberPanel {}
    )",
                    QUrl(QStringLiteral("qrc:/EmberPanelTest.qml")));
  if (!component.isReady()) {
    throw std::runtime_error("Ember panel failed to load: " +
                             component.errorString().toStdString());
  }
  QObject* object = component.createWithInitialProperties(
      {{QStringLiteral("controller"), QVariant::fromValue<QObject*>(&controller)}});
  require(object != nullptr,
          "Ember panel was not created: " + component.errorString().toStdString());
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

void panel_covers_every_parameter_and_edits() {
  InMemoryNativeSynthAdapter adapter(synth::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(panel->root->implicitWidth() >= 1200 && panel->root->implicitHeight() >= 680,
          "panel has its designed size");
  // The Circuit card's note sits whole inside the card, below its well.
  auto* note = item(*panel, QStringLiteral("emberCircuitNote"));
  auto* well = item(*panel, QStringLiteral("emberCircuitView"));
  const auto card = note->parentItem()->mapRectToScene(note->parentItem()->boundingRect());
  const auto note_area = note->mapRectToScene(
      QRectF(0.0, 0.0, note->width(), note->property("implicitHeight").toDouble()));
  require(card.contains(note_area), "the Circuit note runs past its card");
  require(well->mapRectToScene(well->boundingRect()).bottom() <= note_area.top(),
          "the Circuit well draws over its note");

  for (const auto& p : table().parameters) {
    const QString id = QString::fromUtf8(p.id.data(), static_cast<qsizetype>(p.id.size()));
    bool shown = false;
    for (const char* kind : {"emberFader:", "emberKnob:", "emberButtons:", "emberToggle:",
                             "emberDropdown:", "emberWheel:", "emberStepper:"}) {
      shown = shown || find_item(panel->root, QString::fromLatin1(kind) + id) != nullptr;
    }
    require(shown, "no control for " + id.toStdString());
  }

  // Keyboard on a fader: arrows step, Delete resets, Tab reaches it.
  auto* cutoff = item(*panel, QStringLiteral("emberFader:ember.filter.cutoff"));
  require(cutoff->activeFocusOnTab(), "faders are keyboard reachable");
  cutoff->forceActiveFocus();
  const double before = controller.value(QStringLiteral("ember.filter.cutoff"));
  QTest::keyClick(panel->window.get(), Qt::Key_Up);
  require(controller.value(QStringLiteral("ember.filter.cutoff")) > before, "arrow raises");
  QTest::keyClick(panel->window.get(), Qt::Key_Delete);
  require(controller.is_default(QStringLiteral("ember.filter.cutoff")), "Delete resets");

  // A drag on the fader cap is one gesture and moves the value up.
  const auto start = adapter.submissions().size();
  const double cap_y = 2 +
                       (cutoff->height() - 32 - 12) *
                           (1 - controller.normalised(QStringLiteral("ember.filter.cutoff"))) +
                       6;
  const QPoint grab = cutoff->mapToScene(QPointF(cutoff->width() / 2, cap_y)).toPoint();
  QTest::mousePress(panel->window.get(), Qt::LeftButton, Qt::NoModifier, grab);
  QTest::mouseMove(panel->window.get(), grab - QPoint(0, 20));
  QTest::mouseMove(panel->window.get(), grab - QPoint(0, 40));
  QTest::mouseRelease(panel->window.get(), Qt::LeftButton, Qt::NoModifier, grab - QPoint(0, 40));
  const auto& s = adapter.submissions();
  require(s.size() > start + 2 && s[start].gesture == NativeSynthGesture::begin &&
              s.back().gesture == NativeSynthGesture::commit,
          "a fader drag is begin, updates and commit");
  require(controller.value(QStringLiteral("ember.filter.cutoff")) > before,
          "dragging up raises the cutoff");

  // Knobs drag the same way.
  auto* hiss = item(*panel, QStringLiteral("emberKnob:ember.fx.hiss"));
  const auto knob_start = adapter.submissions().size();
  const QPoint knob = hiss->mapToScene(QPointF(hiss->width() / 2, 22)).toPoint();
  QTest::mousePress(panel->window.get(), Qt::LeftButton, Qt::NoModifier, knob);
  QTest::mouseMove(panel->window.get(), knob - QPoint(0, 30));
  QTest::mouseRelease(panel->window.get(), Qt::LeftButton, Qt::NoModifier, knob - QPoint(0, 30));
  require(adapter.submissions().size() > knob_start + 1 &&
              adapter.submissions().back().gesture == NativeSynthGesture::commit,
          "a knob drag is a gesture");

  // Panel buttons choose by click; toggles flip.
  auto* type = item(*panel, QStringLiteral("emberButtons:ember.filter.type"));
  QTest::mouseClick(panel->window.get(), Qt::LeftButton, Qt::NoModifier,
                    type->mapToScene(QPointF(type->width() * 3 / 4, type->height() / 2)).toPoint());
  require(controller.choice(QStringLiteral("ember.filter.type")) == 1, "click picks SEM");
  auto* sync = item(*panel, QStringLiteral("emberToggle:ember.vco.sync"));
  QTest::mouseClick(panel->window.get(), Qt::LeftButton, Qt::NoModifier,
                    sync->mapToScene(QPointF(sync->width() / 2, sync->height() / 2)).toPoint());
  require(controller.choice(QStringLiteral("ember.vco.sync")) == 1, "toggle turns sync on");

  // Preset menu: open, then choose with the keyboard.
  auto* box = item(*panel, QStringLiteral("emberPresetBox"));
  auto* menu = item(*panel, QStringLiteral("emberMenu"));
  QTest::mouseClick(panel->window.get(), Qt::LeftButton, Qt::NoModifier,
                    box->mapToScene(QPointF(box->width() / 2, box->height() / 2)).toPoint());
  require(menu->isVisible(), "preset menu opens");
  QTest::keyClick(panel->window.get(), Qt::Key_Down);
  QTest::keyClick(panel->window.get(), Qt::Key_Return);
  require(!menu->isVisible() && controller.preset_index() >= 0, "keyboard chooses a preset");
  require(item(*panel, QStringLiteral("emberPresetName"))->property("text").toString() ==
              controller.preset_name(),
          "the header shows the preset");

  // Deferred status reaches the header.
  adapter.set_status({.saved_revision = 9, .audible_revision = 9, .deferred_changes = true});
  controller.refresh();
  QCoreApplication::processEvents();
  require(item(*panel, QStringLiteral("emberStatus"))->property("tone").value<QColor>() ==
              QColor(QStringLiteral("#f0b45a")),
          "deferred status is highlighted");
}

void controls_follow_their_modes() {
  InMemoryNativeSynthAdapter adapter(synth::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  const auto visible = [&](const char* name) {
    return item(*panel, QString::fromLatin1(name))->isVisible();
  };
  const auto opacity = [&](const char* name) {
    return item(*panel, QString::fromLatin1(name))->opacity();
  };

  controller.set_choice(QStringLiteral("ember.filter.type"), 0);
  require(!visible("emberFader:ember.filter.morph"), "morph is hidden for the OTA filter");
  controller.set_choice(QStringLiteral("ember.filter.type"), 1);
  require(visible("emberFader:ember.filter.morph"), "morph shows for the SEM filter");

  controller.set_choice(QStringLiteral("ember.lfo.sync"), 0);
  require(visible("emberKnob:ember.lfo.rate") && !visible("emberDropdown:ember.lfo.division"),
          "free LFO shows its rate");
  controller.set_choice(QStringLiteral("ember.lfo.sync"), 1);
  require(!visible("emberKnob:ember.lfo.rate") && visible("emberDropdown:ember.lfo.division"),
          "synced LFO shows its division");

  controller.set_choice(QStringLiteral("ember.fx.delay_sync"), 1);
  require(!visible("emberKnob:ember.fx.delay_time") &&
              visible("emberDropdown:ember.fx.delay_division"),
          "synced delay shows its division");
  controller.set_choice(QStringLiteral("ember.fx.delay_sync"), 0);
  require(visible("emberKnob:ember.fx.delay_time"), "free delay shows its time");

  controller.set_choice(QStringLiteral("ember.amp.mode"), 0);
  require(opacity("emberFader:ember.amp.attack") < 0.6 &&
              opacity("emberFader:ember.amp.velocity") > 0.9,
          "amp ADSR dims unless the amp has its own envelope");
  controller.set_choice(QStringLiteral("ember.amp.mode"), 2);
  require(opacity("emberFader:ember.amp.attack") > 0.9, "Own mode lights the amp ADSR");

  controller.set_choice(QStringLiteral("ember.voice.mode"), 0);
  require(!visible("emberDropdown:ember.voice.chord") &&
              opacity("emberKnob:ember.voice.detune") < 0.6,
          "chord and detune stay out of the way in poly");
  controller.set_choice(QStringLiteral("ember.voice.mode"), 2);
  require(visible("emberDropdown:ember.voice.chord"), "chord shape shows in Chord mode");
  controller.set_choice(QStringLiteral("ember.voice.mode"), 1);
  require(opacity("emberKnob:ember.voice.detune") > 0.9, "unison lights the detune");

  // A dropdown opens the shared menu and sets the choice.
  controller.set_choice(QStringLiteral("ember.voice.mode"), 2);
  auto* chord = item(*panel, QStringLiteral("emberDropdown:ember.voice.chord"));
  QTest::mouseClick(panel->window.get(), Qt::LeftButton, Qt::NoModifier,
                    chord->mapToScene(QPointF(chord->width() / 2, chord->height() / 2)).toPoint());
  require(item(*panel, QStringLiteral("emberMenu"))->isVisible(), "chord menu opens");
  QTest::keyClick(panel->window.get(), Qt::Key_Down);
  QTest::keyClick(panel->window.get(), Qt::Key_Return);
  require(controller.choice(QStringLiteral("ember.voice.chord")) == 1, "keyboard picks Minor");
}

// A frame in ember.hpp's layout: four cards sounding, each VCO B a few cents
// off and drifting apart, the crystal DCOs nearly exact.
std::vector<float> circuit_frame() {
  namespace t = synth::ember::telemetry;
  std::vector<float> frame(t::size, 0.0F);
  frame[t::version] = 1.0F;
  for (std::size_t c = 0; c < synth::ember_circuit::cards; ++c) {
    const auto spread = static_cast<float>(c) - 3.5F;
    frame[t::card_active + c] = c < 4 ? 1.0F : 0.0F;
    frame[t::card_vco_cents + c] = 2.5F * spread;
    frame[t::card_dco_cents + c] = 0.1F * spread;
    frame[t::card_cutoff + c] = 0.3F * spread;
  }
  frame[t::hum_gate] = 1.0F;
  frame[t::load] = 0.4F;
  return frame;
}

void circuit_view_shows_the_cards() {
  InMemoryNativeSynthAdapter adapter(synth::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  adapter.set_telemetry(circuit_frame());
  auto* view = item(*panel, QStringLiteral("emberCircuitView"));
  require(QTest::qWaitFor(
              [&] {
                return view->property("cards").toList().size() ==
                       static_cast<qsizetype>(synth::ember_circuit::cards);
              },
              2'000),
          "the circuit view shows every voice card");
  const auto first = view->property("cards").toList().front().toMap();
  require(first.value(QStringLiteral("active")).toBool() &&
              std::fabs(first.value(QStringLiteral("vco_cents")).toDouble() + 8.75) < 1e-3,
          "with each card's state and VCO error");
}

void screenshots_if_requested() {
  const char* directory = std::getenv("EMBER_PANEL_SCREENSHOT_DIR");
  if (directory == nullptr || *directory == '\0') {
    return;
  }
  QDir().mkpath(QString::fromLocal8Bit(directory));
  struct Shot {
    const char* preset;
    const char* file;
  };
  for (const Shot shot :
       {Shot{"Ember Default", "/ember_default.png"}, Shot{"Sweep Pad", "/ember_sweep_pad.png"},
        Shot{"Chord Stab", "/ember_chord_stab.png"}}) {
    auto values = synth::preset_values(
                      table(), table().presets[static_cast<std::size_t>(preset_index(shot.preset))])
                      .value();
    InMemoryNativeSynthAdapter adapter(values);
    NativeSynthDeviceController controller(table(), adapter);
    auto panel = load_panel(controller);
    adapter.set_telemetry(circuit_frame());
    QTest::qWait(400);
    const QString path = QString::fromLocal8Bit(directory) + QString::fromLatin1(shot.file);
    require(panel->window->grabWindow().save(path, "PNG"), "screenshot saved");
    std::cout << "wrote " << path.toStdString() << '\n';
  }
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  try {
    filter_visuals_match_the_prototypes();
    panel_covers_every_parameter_and_edits();
    controls_follow_their_modes();
    circuit_view_shows_the_cards();
    screenshots_if_requested();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "ember panel contract passed\n";
  return 0;
}
