#include "in_memory_native_synth_adapter.hpp"
#include "native_synth_device_controller.hpp"

#include "oscillate/native_dsp/effects/expanse.hpp"
#include "oscillate/native_dsp/synth/parameters.hpp"
#include "oscillate/test/check.hpp"

#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QGuiApplication>
#include <QMetaObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>

#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// The Expanse panel ("night sky", ADR-0040): its decay landscape against an
// independent model of the reverb's absorption design, every parameter on a
// table-driven control, spaces, presets, keys and status, freeze, the live
// sky light from telemetry, and screenshots on request.
namespace {
using oscillate::quick::InMemoryNativeSynthAdapter;
using oscillate::quick::NativeSynthDeviceController;
namespace synth = oscillate::native_dsp::synth;
namespace expanse = oscillate::native_dsp::effects::expanse;

using oscillate::test::require;

const synth::EngineDescriptor& table() { return *expanse::engine().descriptor; }
std::size_t index(std::string_view id) { return *synth::find_parameter(table(), id); }

int preset_index(std::string_view name) {
  for (std::size_t i = 0; i < table().presets.size(); ++i) {
    if (table().presets[i].name == name) {
      return static_cast<int>(i);
    }
  }
  throw std::runtime_error("missing preset");
}

std::vector<double> preset(std::string_view name) {
  return synth::preset_values(table(),
                              table().presets[static_cast<std::size_t>(preset_index(name))])
      .value();
}

QString qs(std::string_view text) {
  return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

// ---- Oracle -----------------------------------------------------------------------

// The RBJ cookbook shelves (S = 1) as written there, evaluated with complex
// arithmetic: |H(e^jw)| in dB for a shelf of amplitude `gain`.
double cookbook_shelf_db(bool high, double gain, double corner, double hz, double rate) {
  const double a = std::pow(10.0, 20.0 * std::log10(gain) / 40.0);
  const double w0 = 2.0 * std::numbers::pi * std::min(corner, 0.45 * rate) / rate;
  const double cw = std::cos(w0);
  const double alpha = std::sin(w0) / 2.0 * std::sqrt((a + 1.0 / a) * (1.0 / 1.0 - 1.0) + 2.0);
  const double root = 2.0 * std::sqrt(a) * alpha;
  std::array<double, 3> b{};
  std::array<double, 3> d{};
  if (!high) {
    b = {a * ((a + 1) - (a - 1) * cw + root), 2 * a * ((a - 1) - (a + 1) * cw),
         a * ((a + 1) - (a - 1) * cw - root)};
    d = {(a + 1) + (a - 1) * cw + root, -2 * ((a - 1) + (a + 1) * cw),
         (a + 1) + (a - 1) * cw - root};
  } else {
    b = {a * ((a + 1) + (a - 1) * cw + root), -2 * a * ((a - 1) + (a + 1) * cw),
         a * ((a + 1) + (a - 1) * cw - root)};
    d = {(a + 1) - (a - 1) * cw + root, 2 * ((a - 1) - (a + 1) * cw),
         (a + 1) - (a - 1) * cw - root};
  }
  const std::complex<double> z1 = std::polar(1.0, -2.0 * std::numbers::pi * hz / rate);
  const auto h = (b[0] + b[1] * z1 + b[2] * z1 * z1) / (d[0] + d[1] * z1 + d[2] * z1 * z1);
  return 20.0 * std::log10(std::abs(h));
}

// The reverb's absorption for a 50 ms loop: mid gain, then the low and high
// shelves at their gain ratios; RT60 is 60 dB over the loss per second.
double designed_rt(const std::vector<double>& v, double hz) {
  const double tau = 0.05;
  const double decay = v[index("expanse.decay")];
  const double mid = std::pow(10.0, -3.0 * tau / decay);
  const double low = std::pow(10.0, -3.0 * tau / (decay * v[index("expanse.low_decay")]));
  const double high = std::pow(10.0, -3.0 * tau / (decay * v[index("expanse.high_decay")]));
  const double db =
      20.0 * std::log10(mid) +
      cookbook_shelf_db(false, low / mid, v[index("expanse.low_freq")], hz, 48'000.0) +
      cookbook_shelf_db(true, high / mid, v[index("expanse.high_freq")], hz, 48'000.0);
  return -60.0 * tau / db;
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
      import "qrc:/expanse"
      ExpansePanel {}
    )",
                    QUrl(QStringLiteral("qrc:/ExpansePanelTest.qml")));
  if (!component.isReady()) {
    throw std::runtime_error("Expanse panel failed to load: " +
                             component.errorString().toStdString());
  }
  QObject* object = component.createWithInitialProperties(
      {{QStringLiteral("controller"), QVariant::fromValue<QObject*>(&controller)}});
  require(object != nullptr,
          "Expanse panel was not created: " + component.errorString().toStdString());
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

QVariant call(QQuickItem* target, const char* function, const QVariant& argument) {
  QVariant result;
  require(QMetaObject::invokeMethod(target, function, Q_RETURN_ARG(QVariant, result),
                                    Q_ARG(QVariant, argument)),
          std::string("calls ") + function);
  return result;
}

double landscape_rt(QQuickItem* view, double hz) { return call(view, "rtAt", hz).toDouble(); }

void landscape_follows_the_absorption_design() {
  struct Case {
    std::string_view preset;
    std::vector<std::pair<std::string_view, double>> edits;
  };
  for (const Case& c :
       {Case{"Concert Hall", {}}, Case{"Dark Hall", {}}, Case{"Cathedral", {}},
        Case{"Init",
             {{"expanse.decay", 2.0},
              {"expanse.low_decay", 1.5},
              {"expanse.low_freq", 200.0},
              {"expanse.high_decay", 0.5},
              {"expanse.high_freq", 8'000.0}}},
        Case{"Bright Plate", {{"expanse.high_freq", 16'000.0}, {"expanse.high_decay", 0.1}}}}) {
    auto values = preset(c.preset);
    for (const auto& [id, value] : c.edits) {
      values[index(id)] = value;
    }
    InMemoryNativeSynthAdapter adapter(values);
    NativeSynthDeviceController controller(table(), adapter);
    auto panel = load_panel(controller);
    auto* view = item(*panel, QStringLiteral("expanseLandscape"));
    for (const double hz : {20.0, 63.0, 250.0, 1'000.0, 4'000.0, 12'000.0, 20'000.0}) {
      const double expected = designed_rt(values, hz);
      const double shown = landscape_rt(view, hz);
      require(std::fabs(shown / expected - 1.0) < 1e-9,
              std::string(c.preset) + ": the landscape's RT60 at " + std::to_string(hz) +
                  " Hz is " + std::to_string(shown) + " s, designed " + std::to_string(expected));
    }
    // The band ends reach the decay multipliers, whatever the loop.
    const double decay = values[index("expanse.decay")];
    require(std::fabs(landscape_rt(view, 1.0) / (decay * values[index("expanse.low_decay")]) -
                      1.0) < 0.02,
            "the lowest band decays at Low x Decay");
    if (values[index("expanse.high_freq")] <= 8'000.0) {
      require(
          std::fabs(landscape_rt(view, 23'000.0) / (decay * values[index("expanse.high_decay")]) -
                    1.0) < 0.05,
          "the highest band decays at High x Decay");
    }
  }
  // Between far-apart crossovers the mid band holds the decay itself, and
  // the readouts show the three bands.
  auto values = preset("Init");
  values[index("expanse.decay")] = 2.0;
  values[index("expanse.low_decay")] = 1.5;
  values[index("expanse.low_freq")] = 100.0;
  values[index("expanse.high_decay")] = 0.5;
  values[index("expanse.high_freq")] = 10'000.0;
  InMemoryNativeSynthAdapter adapter(values);
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  auto* view = item(*panel, QStringLiteral("expanseLandscape"));
  require(std::fabs(landscape_rt(view, 1'000.0) / 2.0 - 1.0) < 0.05, "the mid band holds Decay");
  require(text_of(*panel, QStringLiteral("expanseRt:low")) == QStringLiteral("3.00 s"),
          "the low readout: " + text_of(*panel, QStringLiteral("expanseRt:low")).toStdString());
  require(text_of(*panel, QStringLiteral("expanseRt:mid")).startsWith(QStringLiteral("2.0")),
          "the mid readout");
  require(text_of(*panel, QStringLiteral("expanseRt:pre")) == QStringLiteral("0 ms"),
          "the pre-delay readout");
  // A slower decay widens the view; reflections follow the space.
  const double before = view->property("span").toDouble();
  controller.set_value(QStringLiteral("expanse.decay"), 8.0);
  QCoreApplication::processEvents();
  require(view->property("span").toDouble() > before * 3.0, "a longer decay spans further");
  const std::array<int, 6> taps{12, 16, 0, 20, 14, 6};
  for (int mode = 0; mode < 6; ++mode) {
    controller.set_choice(QStringLiteral("expanse.mode"), mode);
    QCoreApplication::processEvents();
    QVariant reflections;
    require(QMetaObject::invokeMethod(view, "reflections", Q_RETURN_ARG(QVariant, reflections)),
            "calls reflections");
    require(reflections.toList().size() == taps[static_cast<std::size_t>(mode)],
            "each space shows its own early reflections");
  }
}

void panel_covers_every_parameter() {
  InMemoryNativeSynthAdapter adapter(synth::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(panel->root->implicitWidth() >= 1240 && panel->root->implicitHeight() >= 640,
          "panel has its designed size");
  for (const auto& p : table().parameters) {
    const QString id = qs(p.id);
    const bool shown = find_item(panel->root, QStringLiteral("expanseKnob:") + id) != nullptr ||
                       find_item(panel->root, QStringLiteral("expanseSegment:") + id) != nullptr ||
                       find_item(panel->root, QStringLiteral("expanseToggle:") + id) != nullptr;
    require(shown, "no control for " + id.toStdString());
  }
  require(text_of(*panel, QStringLiteral("expansePresetName")) == QStringLiteral("Concert Hall"),
          "defaults are the Concert Hall");
}

void spaces_presets_and_keys() {
  InMemoryNativeSynthAdapter adapter(synth::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  auto* spaces = item(*panel, QStringLiteral("expanseSegment:expanse.mode"));
  // The fifth tile is the cathedral.
  const QPoint cathedral =
      spaces->mapToScene(QPointF(spaces->width() / 2, spaces->height() * (4.5 / 6.0))).toPoint();
  click(*panel, cathedral);
  require(controller.choice(QStringLiteral("expanse.mode")) == 4, "a tile picks the space");
  require(text_of(*panel, QStringLiteral("expanseReadout")) == QStringLiteral("SPACE · MODE"),
          "the display reads back the space: " +
              text_of(*panel, QStringLiteral("expanseReadout")).toStdString());
  QTest::keyClick(panel->window.get(), Qt::Key_Down);
  require(controller.choice(QStringLiteral("expanse.mode")) == 5, "Down steps to Cloud");
  QTest::keyClick(panel->window.get(), Qt::Key_Delete);
  require(controller.choice(QStringLiteral("expanse.mode")) == 1, "Delete resets to Hall");

  auto* interval = item(*panel, QStringLiteral("expanseSegment:expanse.shimmer_pitch"));
  click(*panel,
        interval->mapToScene(QPointF(interval->width() * (2.5 / 6.0), interval->height() / 2))
            .toPoint());
  require(controller.choice(QStringLiteral("expanse.shimmer_pitch")) == 2, "a fifth is picked");

  click(*panel, centre(item(*panel, QStringLiteral("expansePresetNext"))));
  require(controller.preset_name() == QStringLiteral("Small Room"), "next steps the preset");
  click(*panel, centre(item(*panel, QStringLiteral("expansePresetPrevious"))));
  require(controller.preset_name() == QStringLiteral("Concert Hall"), "previous steps back");
  auto* menu = item(*panel, QStringLiteral("expanseMenu"));
  click(*panel, centre(item(*panel, QStringLiteral("expansePresetDisplay"))));
  require(menu->isVisible(), "the preset menu opens");
  QTest::keyClick(panel->window.get(), Qt::Key_Escape);
  require(!menu->isVisible(), "Escape closes it");
  controller.apply_preset(preset_index("Shimmer Heaven"));
  QCoreApplication::processEvents();
  require(text_of(*panel, QStringLiteral("expansePresetName")) == QStringLiteral("Shimmer Heaven"),
          "a preset shows by name");
  require(controller.choice(QStringLiteral("expanse.mode")) == 4, "and sets its space");

  auto* knob = item(*panel, QStringLiteral("expanseKnob:expanse.decay"));
  knob->forceActiveFocus();
  const double before = controller.value(QStringLiteral("expanse.decay"));
  QTest::keyClick(panel->window.get(), Qt::Key_Up);
  require(controller.value(QStringLiteral("expanse.decay")) > before, "Up lengthens the decay");
  require(text_of(*panel, QStringLiteral("expanseReadout")) == QStringLiteral("SPACE · DECAY"),
          "the display reads back the knob");

  adapter.set_status({.saved_revision = 9, .audible_revision = 9, .deferred_changes = true});
  controller.refresh();
  QCoreApplication::processEvents();
  require(item(*panel, QStringLiteral("expanseStatus"))->property("tone").value<QColor>() ==
              QColor(QStringLiteral("#f0a646")),
          "deferred status is highlighted");
}

void freeze_holds_the_landscape() {
  InMemoryNativeSynthAdapter adapter(preset("Concert Hall"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  auto* toggle = item(*panel, QStringLiteral("expanseToggle:expanse.freeze"));
  auto* view = item(*panel, QStringLiteral("expanseLandscape"));
  require(!toggle->property("on").toBool() && !view->property("frozen").toBool(), "not frozen");
  click(*panel, centre(toggle));
  require(controller.choice(QStringLiteral("expanse.freeze")) == 1, "a click freezes");
  require(toggle->property("on").toBool() && view->property("frozen").toBool(),
          "the toggle and the landscape show it");
  require(std::isinf(landscape_rt(view, 1'000.0)), "a frozen space never decays");
  require(text_of(*panel, QStringLiteral("expanseRt:mid")) == QStringLiteral("∞") &&
              text_of(*panel, QStringLiteral("expanseLandscapeCaption"))
                  .startsWith(QStringLiteral("FROZEN")),
          "the readouts say so");
  QTest::keyClick(panel->window.get(), Qt::Key_Space);
  require(controller.choice(QStringLiteral("expanse.freeze")) == 0, "Space thaws it");
  require(std::isfinite(landscape_rt(view, 1'000.0)), "and the decay is back");
}

void the_sky_lights_with_the_reverb() {
  InMemoryNativeSynthAdapter adapter(preset("Concert Hall"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  auto* view = item(*panel, QStringLiteral("expanseLandscape"));
  QTest::qWait(300);
  require(!view->property("live").toBool() &&
              item(*panel, QStringLiteral("expanseGlow"))->property("opacity").toDouble() == 0.0,
          "no telemetry: the settings preview");
  namespace t = expanse::telemetry;
  std::vector<float> frame(t::size, 0.0F);
  frame[t::input_rms] = 0.25F;
  frame[t::early_rms] = 0.05F;
  frame[t::late_rms] = 0.1F;
  adapter.set_telemetry(frame);
  require(QTest::qWaitFor([&] { return view->property("live").toBool(); }, 1000),
          "live levels arrive");
  require(text_of(*panel, QStringLiteral("expanseLandscapeCaption")) ==
              QStringLiteral("LIVE · LATE -20 dB"),
          "the late level reads out: " +
              text_of(*panel, QStringLiteral("expanseLandscapeCaption")).toStdString());
  require(std::fabs(item(*panel, QStringLiteral("expanseMeter:early"))->property("db").toDouble() +
                    26.0206) < 1e-3,
          "the early meter reads -26 dB");
  require(view->property("glow").toDouble() > 0.6, "the sky glows");
  adapter.set_telemetry({});
  require(QTest::qWaitFor([&] { return !view->property("live").toBool(); }, 1000),
          "telemetry gone: back to the preview");
}

void screenshots_if_requested() {
  const char* directory = std::getenv("EXPANSE_PANEL_SCREENSHOT_DIR");
  if (directory == nullptr || *directory == '\0') {
    return;
  }
  QDir().mkpath(QString::fromLocal8Bit(directory));
  struct Shot {
    const char* preset;
    const char* touched;
    const char* file;
  };
  for (const Shot shot :
       {Shot{"Concert Hall", "expanse.decay", "/expanse_concert_hall.png"},
        Shot{"Small Room", "expanse.size", "/expanse_small_room.png"},
        Shot{"Vocal Plate", "expanse.mode", "/expanse_vocal_plate.png"},
        Shot{"Shimmer Heaven", "expanse.shimmer", "/expanse_shimmer_heaven.png"},
        Shot{"Endless Cloud", "expanse.late_diffusion", "/expanse_endless_cloud.png"}}) {
    InMemoryNativeSynthAdapter adapter(preset(shot.preset));
    NativeSynthDeviceController controller(table(), adapter);
    auto panel = load_panel(controller);
    auto* theme = panel->root->property("theme").value<QObject*>();
    theme->setProperty("lastTouched", QString::fromLatin1(shot.touched));
    QTest::qWait(150);
    const QString path = QString::fromLocal8Bit(directory) + QString::fromLatin1(shot.file);
    require(panel->window->grabWindow().save(path, "PNG"), "screenshot saved");
    std::cout << "wrote " << path.toStdString() << '\n';
  }
  {
    auto values = preset("Cathedral");
    values[index("expanse.freeze")] = 1.0;
    InMemoryNativeSynthAdapter adapter(values);
    NativeSynthDeviceController controller(table(), adapter);
    auto panel = load_panel(controller);
    panel->root->property("theme").value<QObject*>()->setProperty("lastTouched",
                                                                  QStringLiteral("expanse.freeze"));
    QTest::qWait(150);
    const QString path = QString::fromLocal8Bit(directory) + QStringLiteral("/expanse_frozen.png");
    require(panel->window->grabWindow().save(path, "PNG"), "screenshot saved");
    std::cout << "wrote " << path.toStdString() << '\n';
  }
  InMemoryNativeSynthAdapter adapter(preset("Concert Hall"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  namespace t = expanse::telemetry;
  std::vector<float> frame(t::size, 0.0F);
  frame[t::input_rms] = 0.3F;
  frame[t::early_rms] = 0.08F;
  frame[t::late_rms] = 0.16F;
  adapter.set_telemetry(frame);
  QTest::qWait(400);
  const QString path = QString::fromLocal8Bit(directory) + QStringLiteral("/expanse_live.png");
  require(panel->window->grabWindow().save(path, "PNG"), "screenshot saved");
  std::cout << "wrote " << path.toStdString() << '\n';
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  try {
    landscape_follows_the_absorption_design();
    panel_covers_every_parameter();
    spaces_presets_and_keys();
    freeze_holds_the_landscape();
    the_sky_lights_with_the_reverb();
    screenshots_if_requested();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "expanse panel contract passed\n";
  return 0;
}
