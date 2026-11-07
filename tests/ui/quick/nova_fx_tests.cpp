#include "in_memory_native_synth_adapter.hpp"
#include "native_synth_device_controller.hpp"

#include "oscillate/native_dsp/synth/nova.hpp"
#include "oscillate/native_dsp/synth/parameters.hpp"
#include "oscillate/project/model.hpp"
#include "oscillate/test/check.hpp"

#include <QCoreApplication>
#include <QFont>
#include <QGuiApplication>
#include <QJSValue>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRectF>
#include <QStyleHints>
#include <QTest>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Nova's effect strips (08a §5): every parameter a module uses has a
// control on its strip (its knobs, the mode menu, the delay's sync, the
// EQ's band types) and none it does not use; a strip collapses to a
// summary; presets step and say Custom once a value moves; the delay's
// feedback filter reads in Hz and the shifter in signed Hz; the filter
// module says CUTOFF as the filter card does; a full bus says why it cannot
// take another module; the split crossover shows only with SPLIT on; a
// strip's text does not overlap.
namespace {
using oscillate::quick::InMemoryNativeSynthAdapter;
using oscillate::quick::NativeSynthDeviceController;
using oscillate::quick::NativeSynthGesture;
namespace synth = oscillate::native_dsp::synth;
namespace project = oscillate::project;

using oscillate::test::require;

const synth::EngineDescriptor& table() { return *synth::nova::engine().descriptor; }

// The contract's modules (08a §5), written out here as the oracle: the
// values each uses as knobs (with mix), whether p7 is its mode, whether p5
// is the delay's sync, and how many EQ bands carry a type menu on p(4b).
struct ModuleUse {
  const char* id;
  std::vector<std::string> knobs;
  bool mode;
  bool sync;
  int bands;
};

constexpr int slot_params = 16;

const std::vector<ModuleUse>& modules() {
  static const std::vector<ModuleUse> all{
      {"distortion", {"p0", "p1", "p2", "mix"}, true, false, 0},
      {"filter", {"p0", "p1", "p2", "mix"}, true, false, 0},
      {"chorus", {"p0", "p1", "p2", "p3", "mix"}, true, false, 0},
      {"flanger", {"p0", "p1", "p2", "mix"}, true, false, 0},
      {"phaser", {"p0", "p1", "p2", "p3", "mix"}, true, false, 0},
      {"delay", {"p0", "p1", "p2", "mix"}, true, true, 0},
      {"reverb", {"p0", "p1", "p2", "p3", "mix"}, true, false, 0},
      {"compressor", {"p0", "p1", "p2", "p3", "p4", "mix"}, true, false, 0},
      {"multiband", {"p0", "p1", "p2", "p3", "p4", "p5", "mix"}, false, false, 0},
      {"eq",
       {"p1", "p2", "p3", "p5", "p6", "p7", "p9", "p10", "p11", "p13", "p14", "p15", "mix"},
       false,
       false,
       4},
      {"dimension", {"p0", "p1", "p2", "mix"}, false, false, 0},
      {"hyper", {"p0", "p1", "p2", "mix"}, true, false, 0},
      {"utility", {"p0", "p1", "p2", "p3", "mix"}, false, false, 0},
      {"convolve", {"p0", "p1", "p2", "mix"}, false, false, 0},
      {"shifter", {"p0", "p1", "p2", "mix"}, true, false, 0},
      {"flyby",
       {"p1", "p2", "p3", "p4", "p5", "p6", "p7", "p8", "p9", "p10", "mix"},
       true,
       false,
       0},
  };
  return all;
}

// ---- Harness ---------------------------------------------------------------------------------

struct Panel {
  QQmlEngine engine;
  std::unique_ptr<QQuickWindow> window = std::make_unique<QQuickWindow>();
  QQuickItem* root = nullptr;
};

void settle() {
  QCoreApplication::processEvents();
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  QCoreApplication::processEvents();
}

std::unique_ptr<Panel> load_panel(NativeSynthDeviceController& controller) {
  auto panel = std::make_unique<Panel>();
  QQmlComponent component(&panel->engine);
  component.setData(R"(
      import QtQuick
      import "qrc:/nova"
      NovaPanel {}
    )",
                    QUrl(QStringLiteral("qrc:/NovaFxTest.qml")));
  if (!component.isReady()) {
    throw std::runtime_error("Nova panel failed to load: " + component.errorString().toStdString());
  }
  QObject* object = component.createWithInitialProperties(
      {{QStringLiteral("controller"), QVariant::fromValue<QObject*>(&controller)}});
  require(object != nullptr,
          "Nova panel was not created: " + component.errorString().toStdString());
  panel->root = qobject_cast<QQuickItem*>(object);
  require(panel->root != nullptr, "panel is a Quick item");
  panel->root->setWidth(panel->root->implicitWidth());
  panel->root->setHeight(panel->root->implicitHeight());
  panel->window->resize(static_cast<int>(panel->root->width()),
                        static_cast<int>(panel->root->height()));
  panel->root->setParentItem(panel->window->contentItem());
  panel->window->show();
  require(QTest::qWaitForWindowExposed(panel->window.get()), "panel window is exposed");
  settle();
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
  settle();
}

void show_fx(Panel& panel) {
  click(panel, centre(item(panel, QStringLiteral("novaSpine:fx"))));
  require(panel.root->property("page").toInt() == 1, "the strip's FX item shows its page");
}

QString text_of(Panel& panel, const QString& name) {
  return item(panel, name)->property("text").toString();
}

// A panel whose main bus holds these modules at uids 1, 2, ...
struct Rig {
  InMemoryNativeSynthAdapter adapter{synth::default_values(table())};
  std::unique_ptr<NativeSynthDeviceController> controller;
  std::unique_ptr<Panel> panel;

  // The main chain and the bus are named at every call.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  explicit Rig(const std::vector<std::string>& main, const std::vector<std::string>& bus = {}) {
    project::NativeSynthDocument document;
    project::NativeChain chain{"fx.main", {}};
    std::uint32_t uid = 1;
    for (const auto& module : main) {
      chain.modules.push_back({uid++, module});
    }
    document.chains.push_back(chain);
    if (!bus.empty()) {
      project::NativeChain second{"fx.bus.1", {}};
      for (const auto& module : bus) {
        second.modules.push_back({uid++, module});
      }
      document.chains.push_back(second);
    }
    adapter.submit_document(document, NativeSynthGesture::single);
    controller = std::make_unique<NativeSynthDeviceController>(table(), adapter);
    panel = load_panel(*controller);
    // The panel gives a slot its module's defaults when it adds one; a
    // document alone leaves the table's zeros, so do the same here.
    for (const auto& chain : document.chains) {
      for (const auto& slot : chain.modules) {
        QVariant values;
        QMetaObject::invokeMethod(panel->root, "moduleDefaults", Q_RETURN_ARG(QVariant, values),
                                  Q_ARG(QVariant, static_cast<int>(slot.uid)),
                                  Q_ARG(QVariant, QString::fromStdString(slot.module)),
                                  Q_ARG(QVariant, QVariant()));
        controller->set_values(values.metaType() == QMetaType::fromType<QJSValue>()
                                   ? values.value<QJSValue>().toVariant().toMap()
                                   : values.toMap());
      }
    }
    show_fx(*panel);
  }
};

QString fx_id(int uid, const std::string& p) {
  return QStringLiteral("nova.fx.%1.%2").arg(uid).arg(QString::fromStdString(p));
}

bool within(QQuickItem* inner, QQuickItem* outer) {
  const QRectF a = inner->mapRectToScene(QRectF(0, 0, inner->width(), inner->height()));
  const QRectF b = outer->mapRectToScene(QRectF(0, 0, outer->width(), outer->height()));
  return b.adjusted(-0.5, -0.5, 0.5, 0.5).contains(a);
}

// ---- Contracts -------------------------------------------------------------------------------

// Every module's every parameter is reachable on its strip, and no knob
// names a value the module does not use.
void every_module_reaches_every_parameter() {
  for (const auto& use : modules()) {
    Rig rig({use.id});
    auto& panel = *rig.panel;
    auto* strip = item(panel, QStringLiteral("novaFxStrip:1"));
    const std::string label = std::string(use.id) + ": ";
    for (const auto& p : use.knobs) {
      auto* knob = find_item(strip, QStringLiteral("novaKnob:") + fx_id(1, p));
      // A failure message in a test loop.
      // NOLINTNEXTLINE(performance-inefficient-string-concatenation)
      require(knob != nullptr, label + "no control for " + p);
      require(knob->isVisible() && within(knob, strip), label + p + " is shown on the strip");
    }
    for (int k = 0; k < slot_params; ++k) {
      const std::string p = "p" + std::to_string(k);
      const bool used = std::ranges::find(use.knobs, p) != use.knobs.end();
      auto* knob = find_item(strip, QStringLiteral("novaKnob:") + fx_id(1, p));
      require(used == (knob != nullptr && knob->isVisible()),
              // A failure message in a test loop.
              // NOLINTNEXTLINE(performance-inefficient-string-concatenation)
              label + "a knob for " + p + " exactly when the module uses it");
    }
    auto* mode = find_item(strip, QStringLiteral("novaFxMode:1"));
    require(use.mode == (mode != nullptr && mode->isVisible()),
            label + "the mode menu shows exactly when the module has one");
    auto* sync = find_item(strip, QStringLiteral("novaFxSync:1"));
    require(use.sync == (sync != nullptr && sync->isVisible()),
            label + "sync shows exactly for the delay");
    for (int b = 0; b < 4; ++b) {
      auto* type = find_item(strip, QStringLiteral("novaFxBandType:1:%1").arg(b));
      require((b < use.bands) == (type != nullptr && type->isVisible()),
              label + "a band type menu exactly for each EQ band");
    }
    require(item(panel, QStringLiteral("novaFxDisplay:1"))->isVisible(), label + "a live display");
  }
}

// A band's type menu writes its p(4b) as the contract's choice value.
void eq_band_types_write_their_choice() {
  Rig rig({"eq"});
  auto& controller = *rig.controller;
  auto& panel = *rig.panel;
  click(panel, centre(item(panel, QStringLiteral("novaFxBandType:1:2"))));
  require(item(panel, QStringLiteral("novaMenu"))->isVisible(), "the type menu opens");
  // From Bell (index 1), Down twice: Low Shelf, then High Shelf.
  QTest::keyClick(panel.window.get(), Qt::Key_Down);
  QTest::keyClick(panel.window.get(), Qt::Key_Down);
  QTest::keyClick(panel.window.get(), Qt::Key_Return);
  settle();
  require(std::fabs(controller.value(QStringLiteral("nova.fx.1.p8")) - 3.0 / 9.0) < 1e-9,
          "band 3 is a high shelf");
}

// Collapse shows a one-line summary; the preset arrows step, and a moved
// value makes the preset Custom.
void collapse_and_presets() {
  Rig rig({"reverb", "delay"});
  auto& controller = *rig.controller;
  auto& panel = *rig.panel;
  auto* strip = item(panel, QStringLiteral("novaFxStrip:1"));
  const double open = strip->height();
  click(panel, centre(item(panel, QStringLiteral("novaFxCollapse:1"))));
  require(strip->height() < open, "collapsed, the strip is its header");
  require(item(panel, QStringLiteral("novaFxSummary:1"))->isVisible(), "with a summary");
  require(!item(panel, QStringLiteral("novaFxDisplay:1"))->isVisible(), "and no display");
  click(panel, centre(item(panel, QStringLiteral("novaFxCollapse:1"))));
  require(strip->height() == open, "expanded again");

  const QString name = QStringLiteral("novaFxPreset:1");
  const auto shown = [&] {
    for (auto* child : item(panel, name)->childItems()) {
      const QString text = child->property("text").toString();
      if (!text.isEmpty()) {
        return text;
      }
    }
    return QString();
  };
  require(shown() == QStringLiteral("Default"), "a new slot plays its defaults");
  // A click that soon after the last two counts as part of a double click.
  QTest::qWait(QGuiApplication::styleHints()->mouseDoubleClickInterval());
  click(panel, centre(item(panel, QStringLiteral("novaFxPresetNext:1"))));
  require(shown() == QStringLiteral("Big Hall"), "▶ steps to the first factory preset");
  require(std::fabs(controller.value(QStringLiteral("nova.fx.1.p0")) - 0.8) < 1e-9,
          "and loads its values");
  controller.set_value(QStringLiteral("nova.fx.1.p2"), 0.9);
  settle();
  require(shown() == QStringLiteral("Custom"), "a moved value makes it Custom");
  click(panel, centre(item(panel, QStringLiteral("novaFxPresetPrevious:1"))));
  // The last preset: Infinite, or one of the user's own when they have some.
  require(shown() != QStringLiteral("Custom") && shown() != QStringLiteral("Default"),
          "◀ from Custom loads the last preset");
}

// The delay's feedback filter reads as the frequency it sets, not "Off".
void delay_filter_reads_in_hertz() {
  Rig rig({"delay"});
  auto& controller = *rig.controller;
  auto& panel = *rig.panel;
  const QString value = QStringLiteral("novaKnobValue:nova.fx.1.p2");
  const auto reads = [&](double p) {
    controller.set_value(QStringLiteral("nova.fx.1.p2"), p);
    settle();
    return text_of(panel, value);
  };
  require(reads(0.5) == QStringLiteral("Full"), "the centre leaves the repeats full range");
  require(reads(0.0) == QStringLiteral("LP 1.00 kHz"), "fully left low-passes at 1 kHz");
  require(reads(1.0) == QStringLiteral("HP 2.00 kHz"), "fully right high-passes at 2 kHz");
  require(reads(0.25) == QStringLiteral("LP 4.47 kHz"),
          "between, the frequency it sets: " + reads(0.25).toStdString());
}

// The shifter's shift reads in signed Hz: 5000 · (2p − 1)³.
void shifter_reads_in_signed_hertz() {
  Rig rig({"shifter"});
  auto& controller = *rig.controller;
  auto& panel = *rig.panel;
  const QString value = QStringLiteral("novaKnobValue:nova.fx.1.p0");
  require(text_of(panel, value) == QStringLiteral("+40.0 Hz"), "the default shifts up 40 Hz");
  controller.set_value(QStringLiteral("nova.fx.1.p0"), 0.0);
  settle();
  require(text_of(panel, value) == QStringLiteral("−5.00 kHz"), "fully left shifts 5 kHz down");
  require(item(panel, QStringLiteral("novaFxMode:1"))->property("text").toString() ==
              QStringLiteral("Shift"),
          "the mode reads Shift");
}

// Flyby reads in metres, km/h and the names of its choices; its path is
// the mode menu, on p0.
void flyby_reads_in_its_units() {
  Rig rig({"flyby"});
  auto& controller = *rig.controller;
  auto& panel = *rig.panel;
  const auto value = [&](int p) {
    return text_of(panel, QStringLiteral("novaKnobValue:nova.fx.1.p%1").arg(p));
  };
  require(value(1) == QStringLiteral("14.0 m"),
          "the default passes 14 m away: " + value(1).toStdString());
  require(value(2) == QStringLiteral("90.0 km/h"), "at 90 km/h: " + value(2).toStdString());
  require(value(3) == QStringLiteral("Loop"), "looping");
  require(value(7) == QStringLiteral("Headphones"), "for headphones");
  controller.set_value(QStringLiteral("nova.fx.1.p3"), 0.5);
  settle();
  require(value(3) == QStringLiteral("Sync"), "the middle of RUN is Sync");
  require(item(panel, QStringLiteral("novaFxMode:1"))->property("text").toString() ==
              QStringLiteral("Left → Right"),
          "the path menu reads Left → Right");
  controller.set_value(QStringLiteral("nova.fx.1.p0"), 0.75);
  settle();
  require(item(panel, QStringLiteral("novaFxMode:1"))->property("text").toString() ==
              QStringLiteral("Orbit"),
          "three quarters of the path is Orbit");
}

// The filter module's cutoff is CUTOFF, as on the filters' cards on MAIN.
void filter_cutoff_is_named_cutoff() {
  Rig rig({"filter"});
  auto& panel = *rig.panel;
  require(item(panel, QStringLiteral("novaKnob:nova.fx.1.p0"))->property("label").toString() ==
              QStringLiteral("CUTOFF"),
          "the filter module says CUTOFF");
  panel.root->setProperty("page", 0);
  settle();
  require(
      item(panel, QStringLiteral("novaKnob:nova.filter.1.cutoff"))->property("label").toString() ==
          QStringLiteral("CUTOFF"),
      "as the filter's controls do");
}

// A full bus says why there is no "+ Add effect"; one with room offers it.
void a_full_bus_says_why() {
  Rig rig({"distortion", "filter", "chorus", "flanger", "phaser", "delay", "reverb", "eq"},
          {"utility"});
  auto& panel = *rig.panel;
  require(!item(panel, QStringLiteral("novaAddEffect"))->isVisible(), "no add on a full bus");
  auto* full = item(panel, QStringLiteral("novaFxFull"));
  require(full->isVisible() && full->property("text").toString() ==
                                   QStringLiteral("8 OF 8 SLOTS · REMOVE ONE TO ADD"),
          "the full bus says so");
  require(within(full, item(panel, QStringLiteral("novaFxRack"))), "in the rack");
  panel.root->setProperty("fxChain", QStringLiteral("fx.bus.1"));
  settle();
  require(item(panel, QStringLiteral("novaAddEffect"))->isVisible() &&
              !item(panel, QStringLiteral("novaFxFull"))->isVisible(),
          "a bus with room offers to add");
}

// The crossover shows only while SPLIT is on, named for what it does.
void crossover_shows_with_split() {
  Rig rig({"distortion"});
  auto& controller = *rig.controller;
  auto& panel = *rig.panel;
  const auto shown = [&] {
    auto* knob = find_item(panel.root, QStringLiteral("novaFxCrossover"));
    return knob != nullptr && knob->isVisible();
  };
  require(!shown(), "no crossover while SPLIT is off");
  controller.set_choice(QStringLiteral("nova.fx.split"), 1);
  settle();
  require(shown(), "SPLIT shows the crossover");
  auto* knob = item(panel, QStringLiteral("novaFxCrossover"));
  require(knob->property("label").toString() == QStringLiteral("CROSSOVER") &&
              knob->property("parameterId").toString() == QStringLiteral("nova.fx.split_frequency"),
          "named for what it sets");
  require(within(knob, item(panel, QStringLiteral("novaFxRack"))), "inside the rack");
  panel.root->setProperty("fxChain", QStringLiteral("fx.bus.1"));
  settle();
  require(!shown(), "only on Main");
}

// The painted box of a Text, from its alignment.
QRectF painted(QQuickItem* text) {
  const double width = text->property("contentWidth").toDouble();
  const double height = text->property("contentHeight").toDouble();
  const int align = text->property("horizontalAlignment").toInt();
  double x = 0;
  if (align & Qt::AlignHCenter) {
    x = (text->width() - width) / 2;
  } else if (align & Qt::AlignRight) {
    x = text->width() - width;
  }
  double y = 0;
  const int valign = text->property("verticalAlignment").toInt();
  if (valign & Qt::AlignVCenter) {
    y = (text->height() - height) / 2;
  }
  return text->mapRectToScene(QRectF(x, y, width, height));
}

// The knob a text belongs to, or null.
QQuickItem* knob_of(QQuickItem* at) {
  for (auto* up = at; up != nullptr; up = up->parentItem()) {
    if (up->objectName().startsWith(QStringLiteral("novaKnob:"))) {
      return up;
    }
  }
  return nullptr;
}

void visible_texts(QQuickItem* at, std::vector<QQuickItem*>& out) {
  if (!at->isVisible() || at->opacity() == 0.0) {
    return;
  }
  if (QString::fromLatin1(at->metaObject()->className()).startsWith(QStringLiteral("QQuickText")) &&
      !at->property("text").toString().isEmpty()) {
    out.push_back(at);
  }
  for (auto* child : at->childItems()) {
    visible_texts(child, out);
  }
}

// A strip's text stays on the strip, apart, and at least 10 px (the knobs'
// own captions are the knob's to size).
void strips_do_not_overlap() {
  const std::vector<std::string> kinds{"compressor", "multiband", "eq",       "chorus",
                                       "delay",      "shifter",   "convolve", "flyby"};
  Rig rig(kinds);
  auto& panel = *rig.panel;
  for (int uid = 1; uid <= static_cast<int>(kinds.size()); ++uid) {
    auto* strip = item(panel, QStringLiteral("novaFxStrip:%1").arg(uid));
    std::vector<QQuickItem*> texts;
    visible_texts(strip, texts);
    const QRectF bounds = strip->mapRectToScene(QRectF(0, 0, strip->width(), strip->height()));
    for (std::size_t i = 0; i < texts.size(); ++i) {
      const QRectF a = painted(texts[i]);
      const std::string what = texts[i]->property("text").toString().toStdString();
      require(bounds.adjusted(-0.5, -0.5, 0.5, 0.5).contains(a), "text leaves the strip: " + what);
      auto* owner = knob_of(texts[i]);
      if (owner == nullptr) {
        const auto font = texts[i]->property("font").value<QFont>();
        require(font.pixelSize() >= 10 || font.pixelSize() < 0, "text under 10 px: " + what);
      }
      for (std::size_t j = i + 1; j < texts.size(); ++j) {
        if (owner != nullptr && knob_of(texts[j]) == owner) {
          continue; // A knob lays out its own caption and value.
        }
        const QRectF b = painted(texts[j]);
        require(!a.adjusted(0.5, 0.5, -0.5, -0.5).intersects(b.adjusted(0.5, 0.5, -0.5, -0.5)),
                "text overlaps on strip " + std::to_string(uid) + ": " + what + " / " +
                    texts[j]->property("text").toString().toStdString());
      }
    }
  }
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  try {
    every_module_reaches_every_parameter();
    eq_band_types_write_their_choice();
    collapse_and_presets();
    delay_filter_reads_in_hertz();
    shifter_reads_in_signed_hertz();
    flyby_reads_in_its_units();
    filter_cutoff_is_named_cutoff();
    a_full_bus_says_why();
    crossover_shows_with_split();
    strips_do_not_overlap();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "nova fx contract passed\n";
  return 0;
}
