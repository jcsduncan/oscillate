#include "in_memory_native_synth_adapter.hpp"
#include "knock_panel_model.hpp"
#include "knock_patterns_model.hpp"
#include "native_synth_device_controller.hpp"

#include "oscillate/application/knock_patterns.hpp"
#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/native_dsp/synth/knock.hpp"
#include "oscillate/test/check.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSet>
#include <QSignalSpy>
#include <QTest>
#include <QVariantMap>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The Knock panel (ADR-0068): the model names every pad by its sound and
// designs its ringing modes; the panel has a control for every parameter,
// rebinds the chain to the selected pad, writes the right ids (Decay, Swing,
// Orbit and Width among them), flashes the pad the running track hits and
// shows where it landed, applies kits, and runs the Patterns area (loops,
// Roll, Mutate, locks, bars, Evolve and Write to clip); and screenshots on
// request.
namespace {
using oscillate::quick::InMemoryNativeSynthAdapter;
using oscillate::quick::KnockPanelModel;
using oscillate::quick::KnockPatternsModel;
namespace patterns = oscillate::application::knock_patterns;
using oscillate::quick::NativeSynthDeviceController;
using oscillate::quick::NativeSynthGesture;
namespace synth = oscillate::native_dsp::synth;
namespace knock = oscillate::native_dsp::synth::knock;

using oscillate::test::require;

const oscillate::native_dsp::EngineDescriptor& table() {
  const auto* found = synth::find_engine("knock");
  require(found != nullptr && found->descriptor != nullptr, "Knock is registered");
  return *found->descriptor;
}

std::size_t index(std::string_view id) {
  const auto found = oscillate::native_dsp::find_parameter(table(), id);
  require(found.has_value(), "missing parameter " + std::string(id));
  return *found;
}

QString qs(std::string_view text) {
  return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

QString pad_id(int pad, std::string_view control) {
  return qs(oscillate::quick::knock_pad_parameter(static_cast<std::size_t>(pad), control));
}

constexpr int pads = static_cast<int>(knock::pad_count);

// ---- The model ------------------------------------------------------------------

void the_model_names_and_designs_every_pad() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  KnockPanelModel model(controller);

  require(KnockPanelModel::pad_count() == pads &&
              KnockPanelModel::first_key() == static_cast<int>(knock::first_key),
          "the model follows knock.hpp's pads");
  const auto keys = model.key_names();
  require(keys.size() == pads && keys.front() == QStringLiteral("C1") &&
              keys[1] == QStringLiteral("C♯1"),
          "pads are named by key from C1");
  require(model.pad_parameter(0, QStringLiteral("res.pitch")) ==
                  QStringLiteral("knock.pad1.res.pitch") &&
              model.pad_parameter(pads - 1, QStringLiteral("hand")) ==
                  QStringLiteral("knock.pad%1.hand").arg(pads),
          "pad parameters are knock.padN.<control>");
  require(model.pad_parameter(pads, QStringLiteral("hand")).isEmpty() &&
              model.pad_parameter(-1, QStringLiteral("hand")).isEmpty() &&
              model.pad_parameter(0, QStringLiteral("no.such")).isEmpty(),
          "pads and controls that do not exist have no id");

  const auto labels = model.pad_labels();
  require(labels.size() == pads, "every pad has a label");
  for (int pad = 0; pad < pads; ++pad) {
    require(labels[pad].contains(QStringLiteral(" · ")), "a pad is named by its sound");
    const auto modes = model.modes(pad);
    require(!modes.isEmpty(), "every pad rings");
    for (const auto& value : modes) {
      const auto mode = value.toMap();
      const double hz = mode.value(QStringLiteral("hz")).toDouble();
      const double level = mode.value(QStringLiteral("level")).toDouble();
      const auto part = mode.value(QStringLiteral("part")).toString();
      require(hz > 0.0 && level >= 0.0 && level <= 1.0 &&
                  mode.value(QStringLiteral("t60")).toDouble() > 0.0,
              "a mode has a frequency, a level and a ring");
      require(part == QStringLiteral("resonator") || part == QStringLiteral("coupled") ||
                  part == QStringLiteral("body"),
              "a mode belongs to a part");
    }
  }
  require(model.modes(pads).isEmpty() && model.modes(-1).isEmpty(), "no modes off the pads");
  require(model.view_fraction(KnockPanelModel::view_min_hz()) == 0.0 &&
              model.view_fraction(KnockPanelModel::view_max_hz()) == 1.0,
          "the view spans its range");

  // The label follows the resonator's material.
  const auto material = pad_id(2, "res.material");
  const auto choices =
      controller.parameter(material).value(QStringLiteral("choices")).toStringList();
  require(choices.size() > 1, "materials are a choice");
  const int next = (controller.choice(material) + 1) % static_cast<int>(choices.size());
  const int revision = model.revision();
  controller.set_choice(material, next);
  require(model.revision() > revision, "an edit redesigns the pads");
  require(model.pad_labels()[2].startsWith(choices[next]), "the label names the new material");

  // A higher pitch moves the resonator's lowest mode up.
  const auto lowest = [&](int pad) {
    double low = std::numeric_limits<double>::infinity();
    for (const auto& value : model.modes(pad)) {
      const auto mode = value.toMap();
      if (mode.value(QStringLiteral("part")).toString() == QStringLiteral("resonator"))
        low = std::min(low, mode.value(QStringLiteral("hz")).toDouble());
    }
    return low;
  };
  const auto pitch = pad_id(4, "res.pitch");
  const double before = lowest(4);
  const double hz = controller.value(pitch);
  const bool up =
      hz * 2.0 <= controller.parameter(pitch).value(QStringLiteral("maximum")).toDouble();
  controller.set_value(pitch, up ? hz * 2.0 : hz / 2.0);
  require(up ? lowest(4) > before : lowest(4) < before, "the resonator follows its pitch");

  // The piano roll's names, from any values; others' values name nothing.
  const auto values = oscillate::native_dsp::default_values(table());
  require(!oscillate::quick::knock_pad_label(table(), values, 0).empty() &&
              oscillate::quick::knock_pad_label(table(), {}, 0).empty() &&
              oscillate::quick::knock_pad_label(table(), values, knock::pad_count).empty(),
          "labels need Knock's values and a pad");
}

// ---- The panel ------------------------------------------------------------------

struct Panel {
  QQmlEngine engine;
  std::unique_ptr<QQuickWindow> window = std::make_unique<QQuickWindow>();
  QQuickItem* root = nullptr;
};

std::unique_ptr<Panel> load_panel(NativeSynthDeviceController& controller, KnockPanelModel* model) {
  auto panel = std::make_unique<Panel>();
  QQmlComponent component(&panel->engine);
  component.setData(R"(
      import QtQuick
      import "qrc:/knock"
      KnockPanel {}
    )",
                    QUrl(QStringLiteral("qrc:/KnockPanelTest.qml")));
  if (!component.isReady()) {
    throw std::runtime_error("Knock panel failed to load: " +
                             component.errorString().toStdString());
  }
  QObject* object = component.createWithInitialProperties(
      {{QStringLiteral("controller"), QVariant::fromValue<QObject*>(&controller)},
       {QStringLiteral("knock"), QVariant::fromValue<QObject*>(model)}});
  require(object != nullptr,
          "Knock panel was not created: " + component.errorString().toStdString());
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

QPoint scene(QQuickItem* target, double x, double y) {
  return target->mapToScene(QPointF(x, y)).toPoint();
}
QPoint centre(QQuickItem* target) {
  return scene(target, target->width() / 2, target->height() / 2);
}

void click(Panel& panel, QQuickItem* target) {
  QTest::mouseClick(panel.window.get(), Qt::LeftButton, Qt::NoModifier, centre(target));
}

int selected_pad(const Panel& panel) { return panel.root->property("selectedPad").toInt(); }

void select_pad(Panel& panel, int pad) {
  panel.root->setProperty("selectedPad", pad);
  QCoreApplication::processEvents();
}

QObject* theme(const Panel& panel) { return panel.root->property("theme").value<QObject*>(); }

// A point along a slider's rail, `fraction` of the way (the theme's row:
// label, gap, rail, gap, value).
QPoint on_rail(Panel& panel, QQuickItem* slider, double fraction) {
  const auto* tokens = theme(panel);
  const double label = tokens->property("sliderLabelWidth").toDouble();
  const double value = tokens->property("sliderValueWidth").toDouble();
  const double gap = tokens->property("sliderGap").toDouble();
  const double left = label + gap;
  const double right = slider->width() - value - gap;
  return scene(slider, left + fraction * (right - left), slider->height() / 2);
}

void panel_covers_every_parameter() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  KnockPanelModel model(controller);
  auto panel = load_panel(controller, &model);
  require(panel->root->implicitWidth() >= 1360 && panel->root->implicitHeight() >= 720,
          "panel has its designed size");
  // Each pad selected once: what it shows, with the global strip, covers the table.
  QSet<QString> shown;
  for (int pad = 0; pad < pads; ++pad) {
    select_pad(*panel, pad);
    for (const auto& parameter : table().parameters) {
      const auto id = qs(parameter.id);
      for (const auto* kind : {"knockSlider:", "knockChoice:", "knockKnob:"}) {
        if (!shown.contains(id) &&
            find_item(panel->root, QString::fromLatin1(kind) + id) != nullptr)
          shown.insert(id);
      }
    }
  }
  for (const auto& parameter : table().parameters)
    require(shown.contains(qs(parameter.id)), "no control for " + std::string(parameter.id));
  for (int pad = 0; pad < pads; ++pad)
    require(item(*panel, QStringLiteral("knockPad:%1").arg(pad))->isVisible(), "every pad shows");
}

void pads_select_rebind_and_play() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  KnockPanelModel model(controller);
  auto panel = load_panel(controller, &model);
  QSignalSpy triggered(panel->root, SIGNAL(padTriggered(int)));
  const int first = static_cast<int>(knock::first_key);

  require(selected_pad(*panel) == 0, "the first pad is selected first");
  require(find_item(panel->root, QStringLiteral("knockSlider:") + pad_id(0, "res.pitch")) !=
              nullptr,
          "the chain starts on pad 1");

  auto* pad = item(*panel, QStringLiteral("knockPad:2"));
  click(*panel, pad);
  require(selected_pad(*panel) == 2, "clicking a pad selects it");
  require(triggered.size() == 1 && triggered.front().front().toInt() == first + 2,
          "a pad click asks the host to play its key");
  require(pad->property("hits").toInt() == 1, "a clicked pad flashes");
  require(find_item(panel->root, QStringLiteral("knockSlider:") + pad_id(0, "res.pitch")) ==
                  nullptr &&
              find_item(panel->root, QStringLiteral("knockSlider:") + pad_id(2, "res.pitch")) !=
                  nullptr &&
              find_item(panel->root, QStringLiteral("knockChoice:") + pad_id(2, "striker.type")) !=
                  nullptr,
          "the chain follows the selected pad");
  require(item(*panel, QStringLiteral("knockPadName"))->property("text").toString() ==
              model.pad_labels()[2],
          "the heading names the pad by its sound");
  require(item(*panel, QStringLiteral("knockPadLabel:2"))->property("text").toString() ==
              model.pad_labels()[2],
          "the pad shows its label");
  require(item(*panel, QStringLiteral("knockModes"))->property("modes").toList().size() ==
              model.modes(2).size(),
          "the modes view draws the selected pad");

  // Keyboard on the pads: arrows walk the grid, Space plays.
  pad->forceActiveFocus();
  QTest::keyClick(panel->window.get(), Qt::Key_Right);
  require(selected_pad(*panel) == 3, "Right selects the next pad");
  QTest::keyClick(panel->window.get(), Qt::Key_Down);
  require(selected_pad(*panel) == 7, "Down walks a row");
  QTest::keyClick(panel->window.get(), Qt::Key_Space);
  require(triggered.size() == 2 && triggered.back().front().toInt() == first + 7,
          "Space plays the pad");

  // A slider edits the selected pad's parameter, and only it.
  const auto decay = pad_id(7, "res.decay");
  auto* slider = item(*panel, QStringLiteral("knockSlider:") + decay);
  slider->forceActiveFocus();
  const double before = controller.value(decay);
  QTest::keyClick(panel->window.get(), Qt::Key_Up);
  require(controller.value(decay) > before, "the slider edits the selected pad");
  require(controller.value(pad_id(0, "res.decay")) ==
              table().parameters[index(pad_id(0, "res.decay").toStdString())].default_value,
          "other pads are untouched");
  QTest::keyClick(panel->window.get(), Qt::Key_Delete);
  require(controller.is_default(decay), "Delete resets");

  // A drag along the rail is one gesture.
  const auto hardness = pad_id(7, "striker.hardness");
  auto* hard = item(*panel, QStringLiteral("knockSlider:") + hardness);
  const auto start = adapter.submissions().size();
  QTest::mousePress(panel->window.get(), Qt::LeftButton, Qt::NoModifier,
                    on_rail(*panel, hard, 0.2));
  QTest::mouseMove(panel->window.get(), on_rail(*panel, hard, 0.5));
  QTest::mouseMove(panel->window.get(), on_rail(*panel, hard, 0.8));
  QTest::mouseRelease(panel->window.get(), Qt::LeftButton, Qt::NoModifier,
                      on_rail(*panel, hard, 0.8));
  const auto& s = adapter.submissions();
  require(s.size() > start + 2 && s[start].gesture == NativeSynthGesture::begin &&
              s.back().gesture == NativeSynthGesture::commit,
          "a slider drag is begin, updates and commit");
  require(std::fabs(controller.normalised(hardness) - 0.8) < 0.05, "the drag lands where it ends");

  // A material tile picks the material, renames the pad and plays it.
  const auto material = pad_id(7, "res.material");
  const auto choices =
      controller.parameter(material).value(QStringLiteral("choices")).toStringList();
  const int next = (controller.choice(material) + 1) % static_cast<int>(choices.size());
  click(*panel, item(*panel, QStringLiteral("knockChoice:%1:%2").arg(material).arg(next)));
  require(controller.choice(material) == next, "a tile picks the material");
  require(item(*panel, QStringLiteral("knockPadLabel:7"))
              ->property("text")
              .toString()
              .startsWith(choices[next]),
          "the pad is renamed by its new material");
  require(triggered.size() == 3, "a new material is played");

  // Selecting by property does not play.
  select_pad(*panel, 0);
  require(triggered.size() == 3, "selecting by property does not play");
}

void global_decay_and_strip_write_their_ids() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  KnockPanelModel model(controller);
  auto panel = load_panel(controller, &model);

  auto* decay = item(*panel, QStringLiteral("knockKnob:knock.decay"));
  require(
      decay->property("diameter").toInt() >
          item(*panel, QStringLiteral("knockKnob:knock.room.size"))->property("diameter").toInt(),
      "Decay is the strip's big knob");
  decay->forceActiveFocus();
  const auto start = adapter.submissions().size();
  const double before = controller.value(QStringLiteral("knock.decay"));
  QTest::keyClick(panel->window.get(), Qt::Key_Up);
  require(adapter.submissions().size() == start + 1, "one step is one edit");
  const auto& written = adapter.submissions().back().values;
  require(written[index("knock.decay")] > before, "Decay writes knock.decay");
  for (const auto* other :
       {"knock.room.size", "knock.room.level", "knock.velocity", "knock.out.level"})
    require(written[index(other)] == table().parameters[index(other)].default_value,
            std::string("Decay leaves ") + other + " alone");
  // Decay scales every pad's ring, so the modes are redesigned.
  require(model.revision() > 1, "the modes follow Decay");

  for (const auto* id :
       {"knock.room.size", "knock.room.level", "knock.velocity", "knock.out.level"}) {
    auto* knob = item(*panel, QStringLiteral("knockKnob:") + QString::fromLatin1(id));
    knob->forceActiveFocus();
    const double value = controller.value(QString::fromLatin1(id));
    QTest::keyClick(panel->window.get(), Qt::Key_Down);
    QTest::keyClick(panel->window.get(), Qt::Key_Up);
    QTest::keyClick(panel->window.get(), Qt::Key_Up);
    require(controller.value(QString::fromLatin1(id)) != value ||
                value >= table().parameters[index(id)].maximum,
            std::string("the strip edits ") + id);
  }
}

std::vector<float> knock_frame(int last_pad, int hits, int lit_pad, float level,
                               const std::vector<float>& places = {}) {
  namespace t = knock::telemetry;
  std::vector<float> frame(t::size, 0.0F);
  frame[t::last_pad] = static_cast<float>(last_pad);
  frame[t::hits] = static_cast<float>(hits);
  frame[t::first_level + static_cast<std::size_t>(lit_pad)] = level;
  frame[t::output_peak_db] = -6.0F;
  for (std::size_t pad = 0; pad < places.size() && pad < knock::pad_count; ++pad)
    frame[t::first_place + pad] = places[pad];
  return frame;
}

// The pads flash from the running track's hits (ADR-0037) and the modes
// view rings from the selected pad's.
void telemetry_flashes_the_hit_pad() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  KnockPanelModel model(controller);
  auto panel = load_panel(controller, &model);
  const auto hits = [&](int pad) {
    return item(*panel, QStringLiteral("knockPad:%1").arg(pad))->property("hits").toInt();
  };
  const auto live = [&](int pad) {
    return item(*panel, QStringLiteral("knockPadLive:%1").arg(pad))->opacity();
  };
  auto* modes = item(*panel, QStringLiteral("knockModes"));
  QTest::qWait(120);
  require(hits(5) == 0 && live(5) == 0.0, "no telemetry: pads rest");
  require(modes->property("hitAt").toDouble() < 0.0, "no hit: the modes rest");

  adapter.set_telemetry(knock_frame(5, 1, 5, 0.8F));
  require(QTest::qWaitFor([&] { return hits(5) == 1; }, 1000), "the hit pad flashes");
  require(live(5) > 0.0, "the sounding pad glows");
  for (int pad = 0; pad < pads; ++pad) {
    if (pad != 5)
      require(hits(pad) == 0 && live(pad) == 0.0, "other pads stay dark");
  }
  QTest::qWait(120);
  require(hits(5) == 1, "one hit flashes once");
  require(modes->property("hitAt").toDouble() < 0.0, "another pad's hit leaves the view at rest");

  // A hit on the selected pad rings its modes.
  adapter.set_telemetry(knock_frame(0, 2, 0, 0.9F));
  require(QTest::qWaitFor([&] { return hits(0) == 1; }, 1000), "the next hit flashes its pad");
  require(modes->property("hitAt").toDouble() > 0.0 && modes->property("ringing").toBool(),
          "the selected pad's hit rings the modes view");

  adapter.set_telemetry({});
  require(QTest::qWaitFor([&] { return live(0) == 0.0; }, 1000), "telemetry gone: pads go dark");
}

void kits_apply() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  KnockPanelModel model(controller);
  auto panel = load_panel(controller, &model);
  require(table().presets.size() >= 2, "Knock has kits");
  const auto kit_name = [&] {
    return item(*panel, QStringLiteral("knockKitName"))->property("text").toString();
  };

  controller.apply_preset(0);
  QCoreApplication::processEvents();
  require(kit_name() == qs(table().presets[0].name), "the header names the kit");
  click(*panel, item(*panel, QStringLiteral("knockKitNext")));
  require(controller.preset_name() == qs(table().presets[1].name), "next steps the kit");
  require(kit_name() == qs(table().presets[1].name), "the header follows");
  const auto expected = oscillate::native_dsp::preset_values(table(), table().presets[1]).value();
  require(controller.current_values() == expected, "the kit's values are applied");
  for (int pad = 0; pad < pads; ++pad) {
    require(model.pad_labels()[pad] == qs(oscillate::quick::knock_pad_label(
                                           table(), expected, static_cast<std::size_t>(pad))),
            "the pads are renamed by the kit");
  }
  click(*panel, item(*panel, QStringLiteral("knockKitPrevious")));
  require(controller.preset_name() == qs(table().presets[0].name), "previous steps back");

  auto* menu = item(*panel, QStringLiteral("knockMenu"));
  click(*panel, item(*panel, QStringLiteral("knockKitBox")));
  require(menu->isVisible(), "the kit menu opens");
  QTest::keyClick(panel->window.get(), Qt::Key_Down);
  QTest::keyClick(panel->window.get(), Qt::Key_Return);
  require(!menu->isVisible() && controller.preset_index() > 0 && !controller.preset_modified(),
          "the keyboard chooses another kit");
  require(kit_name() == controller.preset_name(), "the header shows the chosen kit");
}

// Width, Swing and Orbit (ADR-0068) bind to their ids: each pad's Width
// card follows the selected pad, Swing and Orbit sit in the global strip.
void width_swing_and_orbit_bind_their_ids() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  KnockPanelModel model(controller);
  auto panel = load_panel(controller, &model);
  for (const auto* id : {"knock.swing", "knock.orbit"})
    (void)index(id);
  require(find_item(panel->root, QStringLiteral("knockKnob:knock.swing")) != nullptr,
          "Swing is a knob in the global strip");
  require(find_item(panel->root, QStringLiteral("knockChoice:knock.orbit")) != nullptr,
          "Orbit is a choice in the global strip");
  select_pad(*panel, 4);
  for (const auto* control : {"width.spread", "width.depth", "width.haas"}) {
    (void)index(pad_id(4, control).toStdString());
    require(find_item(panel->root, QStringLiteral("knockSlider:") + pad_id(4, control)) != nullptr,
            std::string("the Width card has ") + control);
  }
  for (const auto* control : {"width.motion", "swing.follow"}) {
    (void)index(pad_id(4, control).toStdString());
    require(find_item(panel->root, QStringLiteral("knockChoice:") + pad_id(4, control)) != nullptr,
            std::string("the Width card picks ") + control);
  }
  (void)index(pad_id(4, "key.follow").toStdString());
  require(find_item(panel->root, QStringLiteral("knockChoice:") + pad_id(4, "key.follow")) !=
              nullptr,
          "the pad head picks whether the pad follows the key");
  require(find_item(panel->root, QStringLiteral("knockSlider:") + pad_id(0, "width.spread")) ==
              nullptr,
          "the Width card follows the selected pad");

  // A Motion tile writes the selected pad's motion, and only it.
  const auto motion = pad_id(4, "width.motion");
  const auto choices = controller.parameter(motion).value(QStringLiteral("choices")).toStringList();
  require(choices.size() == 4, "Motion has four ways to move");
  const int next = (controller.choice(motion) + 1) % static_cast<int>(choices.size());
  click(*panel, item(*panel, QStringLiteral("knockChoice:%1:%2").arg(motion).arg(next)));
  require(controller.choice(motion) == next, "a Motion tile picks the motion");
  require(controller.choice(pad_id(0, "width.motion")) ==
              static_cast<int>(
                  table().parameters[index(pad_id(0, "width.motion").toStdString())].default_value),
          "other pads keep theirs");
  const auto orbit = QStringLiteral("knock.orbit");
  const int bars =
      (controller.choice(orbit) + 1) %
      static_cast<int>(
          controller.parameter(orbit).value(QStringLiteral("choices")).toStringList().size());
  click(*panel, item(*panel, QStringLiteral("knockChoice:knock.orbit:%1").arg(bars)));
  require(controller.choice(orbit) == bars, "an Orbit tile writes knock.orbit");
}

// The stereo strip shows where each pad's latest hit landed (telemetry
// first_place), the latest hit and any pad that moved since.
void the_stereo_strip_shows_where_pads_land() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  KnockPanelModel model(controller);
  auto panel = load_panel(controller, &model);
  auto* stereo = item(*panel, QStringLiteral("knockStereo"));
  const auto landed = [&](int pad) {
    const auto times = stereo->property("landedAt").toList();
    return pad < times.size() ? times[pad].toDouble() : -1.0;
  };
  const auto place = [&](int pad) {
    const auto places = stereo->property("places").toList();
    return pad < places.size() ? places[pad].toDouble() : 0.0;
  };
  QTest::qWait(80);
  require(landed(3) < 0.0, "nothing has landed before a hit");
  std::vector<float> places(knock::pad_count, 0.0F);
  places[3] = -0.5F;
  adapter.set_telemetry(knock_frame(3, 1, 3, 0.8F, places));
  require(QTest::qWaitFor([&] { return landed(3) > 0.0; }, 1000), "the hit pad lands");
  require(std::fabs(place(3) + 0.5) < 1e-6, "it lands where the engine says");
  require(landed(5) < 0.0, "other pads have not landed");
  // Two hits between polls: the latest and the one that moved both land.
  places[5] = 0.7F;
  adapter.set_telemetry(knock_frame(3, 3, 3, 0.8F, places));
  require(QTest::qWaitFor([&] { return landed(5) > 0.0; }, 1000), "a pad that moved lands");
  require(std::fabs(place(5) - 0.7) < 1e-6, "at its new place");
  require(model.place_min() == -1.0 && model.place_max() == 1.0, "the strip spans left to right");
}

QVariantList cells_shown(Panel& panel) {
  QVariantList cells;
  for (int lane = 0; lane < KnockPatternsModel::lane_count(); ++lane) {
    for (int step = 0; step < KnockPatternsModel::steps_per_bar(); ++step)
      cells.push_back(
          item(panel, QStringLiteral("knockCell:%1:%2").arg(lane).arg(step))->property("kind"));
  }
  return cells;
}

std::size_t cells_changed(const patterns::Pattern& a, const patterns::Pattern& b) {
  std::size_t changed = 0;
  for (std::size_t bar = 0; bar < std::min(a.bars.size(), b.bars.size()); ++bar)
    for (std::size_t lane = 0; lane < patterns::lane_count; ++lane)
      for (std::size_t step = 0; step < patterns::steps_per_bar; ++step)
        changed += a.bars[bar][lane][step] != b.bars[bar][lane][step] ? 1U : 0U;
  return changed;
}

bool lane_kept(const patterns::Pattern& before, const patterns::Pattern& after, std::size_t lane) {
  for (std::size_t bar = 0; bar < after.bars.size(); ++bar) {
    if (after.bars[bar][lane] != before.bars[bar % before.bars.size()][lane])
      return false;
  }
  return true;
}

// Loops, the generator, locks and the grid: everything stays in the panel
// until Write to clip.
void patterns_load_roll_mutate_and_lock() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  KnockPanelModel model(controller, 48'000, &adapter);
  auto* patterns_model = model.pattern_model();
  require(patterns_model != nullptr && model.patterns() == patterns_model,
          "the panel has patterns");
  require(patterns_model->loops().size() == static_cast<qsizetype>(patterns::library().size()),
          "the library lists every loop");
  require(patterns_model->pattern_bars() > 0 && !patterns_model->loop_name().isEmpty(),
          "the panel starts on a loop");
  auto panel = load_panel(controller, &model);
  const auto saved = adapter.status().saved_revision;

  // A loop from the Top chip fills the grid.
  click(*panel, item(*panel, QStringLiteral("knockRole:style0")));
  click(*panel, item(*panel, QStringLiteral("knockLoop:Tin Roof")));
  require(patterns_model->loop_name() == QStringLiteral("Tin Roof"), "a loop click loads it");
  const auto* entry = patterns::find_loop("Tin Roof");
  require(entry != nullptr, "Tin Roof is in the library");
  const auto loaded =
      patterns::loop_pattern(*entry, patterns::roles_of(controller.current_values()));
  require(patterns_model->pattern() == loaded, "the loop is the library's");
  QVariantList expected;
  for (const auto& lane : loaded.bars.front())
    for (const auto cell : lane)
      expected.push_back(static_cast<int>(cell));
  require(cells_shown(*panel) == expected, "the grid shows the loop's first bar");
  require(item(*panel, QStringLiteral("knockBarTab:%1").arg(loaded.bars.size() - 1)) != nullptr,
          "a bar tab per bar");

  // Roll gives another loop; a locked lane holds through Roll and Mutate.
  const auto before_roll = patterns_model->pattern();
  const int seed = patterns_model->seed();
  click(*panel, item(*panel, QStringLiteral("knockRoll")));
  require(patterns_model->seed() != seed && patterns_model->pattern() != before_roll,
          "Roll changes the loop");
  click(*panel, item(*panel, QStringLiteral("knockLock:2")));
  require(patterns_model->locked()[2].toBool(), "a lock click locks the lane");
  const auto locked = patterns_model->pattern();
  click(*panel, item(*panel, QStringLiteral("knockRoll")));
  require(patterns_model->pattern() != locked && lane_kept(locked, patterns_model->pattern(), 2),
          "Roll keeps the locked lane");
  const auto before_mutate = patterns_model->pattern();
  click(*panel, item(*panel, QStringLiteral("knockMutate")));
  const auto changed = cells_changed(before_mutate, patterns_model->pattern());
  const auto cells = before_mutate.bars.size() * patterns::lane_count * patterns::steps_per_bar;
  require(changed > 0 && changed * 3 < cells, "Mutate changes a little");
  require(lane_kept(before_mutate, patterns_model->pattern(), 2), "Mutate keeps the locked lane");

  // A cell click cycles it.
  const int kind = patterns_model->cell(0, 0, 1);
  click(*panel, item(*panel, QStringLiteral("knockCell:0:1")));
  require(patterns_model->cell(0, 0, 1) == (kind + 1) % static_cast<int>(patterns::cell_kinds),
          "a cell click steps the cell");

  // The generator: bars and a style reshape the loop; bar tabs page it.
  click(*panel, item(*panel, QStringLiteral("knockTabGenerate")));
  click(*panel, item(*panel, QStringLiteral("knockBars:4")));
  require(patterns_model->bars() == 4 && patterns_model->pattern_bars() == 4,
          "Bars sets the length");
  click(*panel, item(*panel, QStringLiteral("knockBarTab:3")));
  require(patterns_model->view_bar() == 3, "a bar tab shows its bar");
  click(*panel, item(*panel, QStringLiteral("knockStyle:1")));
  require(patterns_model->style() == 1 && lane_kept(before_mutate, patterns_model->pattern(), 2),
          "a style reshapes the loop and keeps the lock");
  auto* density = item(*panel, QStringLiteral("knockGen:density"));
  density->forceActiveFocus();
  const double was = patterns_model->density();
  QTest::keyClick(panel->window.get(), Qt::Key_Right);
  require(patterns_model->density() > was, "a setting steps by key");

  require(adapter.status().saved_revision == saved && adapter.clips().empty(),
          "nothing touches the project before Write to clip");

  // Audition asks the host.
  QSignalSpy audition(panel->root, SIGNAL(auditionRequested()));
  click(*panel, item(*panel, QStringLiteral("knockAudition")));
  require(audition.size() == 1, "Audition asks the host to play the pattern");
  require(!patterns_model->audition_notes(120.0, 48'000, 64).empty(), "the audition has notes");
}

// Write to clip: a new clip at the playhead's bar, or the selected clip's
// notes replaced; one undo step each.
void write_to_clip_is_one_step() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  KnockPanelModel model(controller, 48'000, &adapter);
  auto* patterns_model = model.pattern_model();
  auto panel = load_panel(controller, &model);
  panel->root->setProperty("playheadBar", 4.6);
  require(patterns_model->load_loop(QStringLiteral("Night Bus")), "a loop loads by name");
  const auto expected = patterns::to_notes(patterns_model->pattern(),
                                           static_cast<std::uint32_t>(patterns_model->seed()));
  require(expected.has_value() && !expected.value().notes.empty(), "the loop has notes");

  auto revision = adapter.status().saved_revision;
  click(*panel, item(*panel, QStringLiteral("knockWrite")));
  require(adapter.clips().size() == 1 && adapter.status().saved_revision == revision + 1,
          "Write to clip makes one clip in one step");
  const auto& clip = adapter.clips().front();
  require(clip.bar == 4, "the clip starts at the playhead's bar");
  require(clip.notes.size() == expected.value().notes.size() &&
              clip.length == expected.value().length,
          "the clip holds the pattern's notes");
  require(
      !item(*panel, QStringLiteral("knockPatternStatus"))->property("text").toString().isEmpty(),
      "the panel says what it wrote");

  // With the clip selected and Evolve on, the clip is rewritten in place.
  click(*panel, item(*panel, QStringLiteral("knockEvolve:16")));
  require(patterns_model->evolve_bars() == 16 && patterns_model->written_bars() == 16,
          "Evolve writes sixteen bars");
  const int notes = patterns_model->note_count();
  panel->root->setProperty("selectedClipIds", QStringList{QString::fromStdString(clip.id)});
  revision = adapter.status().saved_revision;
  click(*panel, item(*panel, QStringLiteral("knockWrite")));
  require(adapter.clips().size() == 1 && adapter.status().saved_revision == revision + 1,
          "a selected clip is replaced in one step");
  require(adapter.clips().front().length.value == 16 * patterns::bar_ticks.value &&
              static_cast<int>(adapter.clips().front().notes.size()) == notes,
          "the selected clip takes the evolved pattern");

  // A selection elsewhere writes a new clip; without the project, nothing.
  panel->root->setProperty("selectedClipIds", QStringList{QStringLiteral("not-this-track")});
  click(*panel, item(*panel, QStringLiteral("knockWrite")));
  require(adapter.clips().size() == 2, "another track's clip is left alone");
  KnockPanelModel preview(controller);
  require(!preview.pattern_model()->write_to_clip(0, {}) &&
              !preview.pattern_model()->message().isEmpty(),
          "without the project a write is refused");
}

void screenshots_if_requested() {
  const char* directory = std::getenv("KNOCK_PANEL_SCREENSHOT_DIR");
  if (directory == nullptr || *directory == '\0') {
    return;
  }
  QDir().mkpath(QString::fromLocal8Bit(directory));
  for (std::size_t preset = 0; preset < table().presets.size(); ++preset) {
    InMemoryNativeSynthAdapter adapter(
        oscillate::native_dsp::preset_values(table(), table().presets[preset]).value());
    NativeSynthDeviceController controller(table(), adapter);
    KnockPanelModel model(controller);
    auto panel = load_panel(controller, &model);
    // The selected pad just hit, so its modes ring in the shot.
    adapter.set_telemetry(knock_frame(0, 1, 0, 0.7F));
    QTest::qWait(150);
    QString name = qs(table().presets[preset].name).toLower();
    name.replace(QLatin1Char(' '), QLatin1Char('_'));
    const QString path = QString::fromLocal8Bit(directory) + QStringLiteral("/knock_") + name +
                         QStringLiteral(".png");
    require(panel->window->grabWindow().save(path, "PNG"), "screenshot saved");
    std::cout << "wrote " << path.toStdString() << '\n';
  }
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  try {
    the_model_names_and_designs_every_pad();
    panel_covers_every_parameter();
    pads_select_rebind_and_play();
    global_decay_and_strip_write_their_ids();
    telemetry_flashes_the_hit_pad();
    kits_apply();
    width_swing_and_orbit_bind_their_ids();
    the_stereo_strip_shows_where_pads_land();
    patterns_load_roll_mutate_and_lock();
    write_to_clip_is_one_step();
    screenshots_if_requested();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "knock panel contract passed\n";
  return 0;
}
