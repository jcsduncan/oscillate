#include "in_memory_native_synth_adapter.hpp"
#include "native_synth_device_controller.hpp"
#include "panel_fit_checks.hpp"

#include "oscillate/engine/graph.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/effects/vista.hpp"
#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/test/check.hpp"

#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// The Vista panel ("glacier", ADR-0040): every parameter on a table-driven
// control, band columns and crossovers following Bands, crossover drags that
// keep their order, Haas time only in Haas mode, the telemetry decode into
// the vectorscope, correlation meter and band chips, presets and status, and
// screenshots on request (live ones from the real imager).
namespace {
using oscillate::quick::InMemoryNativeSynthAdapter;
using oscillate::quick::NativeSynthDeviceController;
namespace effects = oscillate::native_dsp::effects;
namespace vista = oscillate::native_dsp::effects::vista;

using oscillate::test::require;

const oscillate::native_dsp::EngineDescriptor& table() { return *vista::engine().descriptor; }
std::size_t index(std::string_view id) {
  return oscillate::test::take(oscillate::native_dsp::find_parameter(table(), id));
}

int preset_index(std::string_view name) {
  for (std::size_t i = 0; i < table().presets.size(); ++i) {
    if (table().presets[i].name == name) {
      return static_cast<int>(i);
    }
  }
  throw std::runtime_error("missing preset");
}

std::vector<double> preset(std::string_view name) {
  return oscillate::native_dsp::preset_values(
             table(), table().presets[static_cast<std::size_t>(preset_index(name))])
      .value();
}

QString qs(std::string_view text) {
  return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
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
      import "qrc:/vista"
      VistaPanel {}
    )",
                    QUrl(QStringLiteral("qrc:/VistaPanelTest.qml")));
  if (!component.isReady()) {
    throw std::runtime_error("Vista panel failed to load: " +
                             component.errorString().toStdString());
  }
  QObject* object = component.createWithInitialProperties(
      {{QStringLiteral("controller"), QVariant::fromValue<QObject*>(&controller)}});
  require(object != nullptr,
          "Vista panel was not created: " + component.errorString().toStdString());
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

void click(Panel& panel, QPoint point) {
  QTest::mouseClick(panel.window.get(), Qt::LeftButton, Qt::NoModifier, point);
}

void drag(Panel& panel, QPoint from, QPoint to) {
  QTest::mousePress(panel.window.get(), Qt::LeftButton, Qt::NoModifier, from);
  for (int step = 1; step <= 8; ++step) {
    const QPoint at = from + (to - from) * step / 8;
    QTest::mouseMove(panel.window.get(), at);
  }
  QTest::mouseRelease(panel.window.get(), Qt::LeftButton, Qt::NoModifier, to);
  QCoreApplication::processEvents();
}

QString text_of(Panel& panel, const QString& name) {
  return item(panel, name)->property("text").toString();
}

// ---- Contract -------------------------------------------------------------------

void panel_covers_every_parameter() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  // The lower cards' readouts end inside their cards.
  oscillate::test::panel_fit::require_inside(
      *item(*panel, QStringLiteral("vistaKnobValue:vista.mono_freq")),
      *item(*panel, QStringLiteral("vistaBassCard")), "the Mono Bass card");
  require(panel->root->implicitWidth() >= 1200 && panel->root->implicitHeight() >= 580,
          "panel has its designed size");
  for (const auto& p : table().parameters) {
    const QString id = qs(p.id);
    bool shown = false;
    for (const char* kind :
         {"vistaKnob:", "vistaSegment:", "vistaToggle:", "vistaBandBar:", "vistaCrossover:"}) {
      shown = shown || find_item(panel->root, QString::fromLatin1(kind) + id) != nullptr;
    }
    require(shown, "no control for " + id.toStdString());
  }
  require(text_of(*panel, QStringLiteral("vistaPresetName")) == QStringLiteral("Natural"),
          "defaults are the Natural preset");
}

void bands_follow_the_band_count() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  for (int choice = 0; choice < 4; ++choice) {
    controller.set_choice(QStringLiteral("vista.bands"), choice);
    QCoreApplication::processEvents();
    const int count = choice + 1;
    for (int band = 1; band <= 4; ++band) {
      const bool visible =
          item(*panel, QStringLiteral("vistaBandBar:vista.width_%1").arg(band))->isVisible();
      require(visible == (band <= count),
              "band " + std::to_string(band) + " shows with " + std::to_string(count) + " bands");
    }
    for (int k = 1; k <= 3; ++k) {
      const bool visible =
          item(*panel, QStringLiteral("vistaCrossover:vista.xover_%1").arg(k))->isVisible();
      require(visible == (k < count),
              "crossover " + std::to_string(k) + " shows with " + std::to_string(count) + " bands");
    }
  }
  // The band segment picks the count by click.
  auto* segment = item(*panel, QStringLiteral("vistaSegment:vista.bands"));
  click(*panel, scene(segment, segment->width() * (1.5 / 4.0), segment->height() / 2));
  require(controller.choice(QStringLiteral("vista.bands")) == 1, "a tile picks two bands");
  require(!item(*panel, QStringLiteral("vistaBandBar:vista.width_3"))->isVisible(),
          "and the third column goes");
}

void dragging_keeps_the_crossovers_in_order() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  controller.set_choice(QStringLiteral("vista.bands"), 3);
  auto panel = load_panel(controller);
  auto* strip = item(*panel, QStringLiteral("vistaBands"));
  const auto grip_of = [&](int k) {
    auto* handle = item(*panel, QStringLiteral("vistaCrossover:vista.xover_%1").arg(k));
    // The grip sits at the top of the handle, centred on its line.
    return scene(handle, handle->width() / 2, strip->property("py").toDouble() + 12);
  };
  const auto value = [&](int k) {
    return controller.value(QStringLiteral("vista.xover_%1").arg(k));
  };
  const double spacing = 1.26;

  // The low crossover dragged far right stops a third of an octave under
  // the middle one.
  drag(*panel, grip_of(1), scene(strip, strip->width() - 20, 30));
  require(value(1) <= value(2) / spacing + 1e-6,
          "the low crossover cannot pass the middle one: " + std::to_string(value(1)) + " vs " +
              std::to_string(value(2)));
  require(value(1) > 1'500.0, "but it moves as far as it may");
  // The high one dragged far left stops above the middle one.
  drag(*panel, grip_of(3), scene(strip, 50, 30));
  require(value(3) >= value(2) * spacing - 1e-6, "the high crossover cannot pass the middle one");
  // The middle one is held between its neighbours both ways.
  drag(*panel, grip_of(2), scene(strip, 45, 30));
  require(value(2) >= value(1) * spacing - 1e-6, "the middle stays above the low");
  drag(*panel, grip_of(2), scene(strip, strip->width() - 15, 30));
  require(value(2) <= value(3) / spacing + 1e-6, "and below the high");
  require(value(1) < value(2) && value(2) < value(3), "the crossovers stay in order");

  // A band column follows the pointer: three quarters down is 50%.
  controller.set_choice(QStringLiteral("vista.bands"), 2);
  QCoreApplication::processEvents();
  auto* bar = item(*panel, QStringLiteral("vistaBandBar:vista.width_1"));
  const double ph = strip->property("ph").toDouble();
  click(*panel, scene(bar, bar->width() / 2, ph * 0.75));
  require(std::fabs(controller.value(QStringLiteral("vista.width_1")) - 0.5) < 0.02,
          "a click three quarters down sets 50%: " +
              std::to_string(controller.value(QStringLiteral("vista.width_1"))));
  QTest::keyClick(panel->window.get(), Qt::Key_Up);
  require(controller.value(QStringLiteral("vista.width_1")) > 0.5, "Up widens the band");
}

void haas_time_shows_only_for_haas() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  auto* time = item(*panel, QStringLiteral("vistaKnob:vista.haas_time"));
  require(!time->isVisible(), "Diffuse hides the Haas time");
  auto* mode = item(*panel, QStringLiteral("vistaSegment:vista.stereoize_mode"));
  click(*panel, scene(mode, mode->width() * 0.75, mode->height() / 2));
  require(controller.choice(QStringLiteral("vista.stereoize_mode")) == 1, "a tile picks Haas");
  require(time->isVisible(), "Haas shows its time");
  click(*panel, scene(mode, mode->width() * 0.25, mode->height() / 2));
  require(!time->isVisible(), "and Diffuse hides it again");
}

std::vector<float> test_frame() {
  namespace t = vista::telemetry;
  std::vector<float> frame(t::size, 0.0F);
  frame[t::correlation_in] = -0.3F;
  frame[t::correlation_out] = 0.42F;
  frame[t::width_in_db] = -6.0F;
  frame[t::width_out_db] = -2.5F;
  frame[t::safe_db] = -1.5F;
  const float band[4] = {0.9F, 0.5F, 0.1F, -0.2F};
  for (std::size_t b = 0; b < vista::bands; ++b) {
    frame[t::band_correlation + b] = band[b];
    frame[t::band_width_db + b] = -3.0F * static_cast<float>(b);
  }
  for (std::size_t i = 0; i < vista::scope_points; ++i) {
    const float m = static_cast<float>(i) / static_cast<float>(vista::scope_points) - 0.5F;
    frame[t::scope + 2 * i] = m;
    frame[t::scope + 2 * i + 1] = -0.5F * m;
  }
  return frame;
}

void telemetry_reaches_the_scope_and_meters() {
  auto values = oscillate::native_dsp::default_values(table());
  values[index("vista.bands")] = 3.0;
  values[index("vista.safe")] = 1.0;
  values[index("vista.safe_floor")] = 0.2;
  InMemoryNativeSynthAdapter adapter(values);
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  auto* scope = item(*panel, QStringLiteral("vistaScope"));
  auto* meter = item(*panel, QStringLiteral("vistaCorrelation"));
  QTest::qWait(300);
  require(!scope->property("live").toBool() && scope->property("pointCount").toInt() == 0,
          "no telemetry: the scope previews the settings");
  require(item(*panel, QStringLiteral("vistaCorrelationFloor"))->isVisible(),
          "safe width marks its floor");
  adapter.set_telemetry(test_frame());
  require(QTest::qWaitFor([&] { return scope->property("live").toBool(); }, 1000),
          "live telemetry arrives");
  require(QTest::qWaitFor([&] { return scope->property("pointCount").toInt() == 256; }, 1000),
          "every scope point is drawn: " + std::to_string(scope->property("pointCount").toInt()));
  require(std::fabs(meter->property("inValue").toDouble() + 0.3) < 1e-6 &&
              std::fabs(meter->property("outValue").toDouble() - 0.42) < 1e-6,
          "the meter reads the input and output correlation");
  require(text_of(*panel, QStringLiteral("vistaCorrelationReadout")) ==
              QStringLiteral("IN −0.30   OUT +0.42"),
          "the correlation readout: " +
              text_of(*panel, QStringLiteral("vistaCorrelationReadout")).toStdString());
  require(text_of(*panel, QStringLiteral("vistaWidthIn")) == QStringLiteral("−6.0 dB") &&
              text_of(*panel, QStringLiteral("vistaWidthOut")) == QStringLiteral("−2.5 dB"),
          "the width readouts");
  require(text_of(*panel, QStringLiteral("vistaSafeReadout")) == QStringLiteral("−1.5 dB"),
          "the safe reduction reads out");
  const double band[4] = {0.9, 0.5, 0.1, -0.2};
  for (int b = 0; b < 4; ++b) {
    auto* chip = item(*panel, QStringLiteral("vistaBandCorrelation:%1").arg(b));
    require(chip->isVisible() && std::fabs(chip->property("value").toDouble() - band[b]) < 1e-6,
            "band " + std::to_string(b) + " shows its correlation");
  }
  adapter.set_telemetry({});
  require(QTest::qWaitFor([&] { return !scope->property("live").toBool(); }, 1000),
          "telemetry gone: back to the preview");
}

void presets_status_and_mono_bass() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  click(*panel, centre(item(*panel, QStringLiteral("vistaPresetNext"))));
  require(controller.preset_name() == QStringLiteral("Wide Master"), "next steps the preset");
  require(item(*panel, QStringLiteral("vistaMonoShade"))->isVisible(),
          "Wide Master's mono bass is shaded on the axis");
  click(*panel, centre(item(*panel, QStringLiteral("vistaPresetPrevious"))));
  require(controller.preset_name() == QStringLiteral("Natural"), "previous steps back");
  require(!item(*panel, QStringLiteral("vistaMonoShade"))->isVisible(), "Natural has no mono bass");
  auto* menu = item(*panel, QStringLiteral("vistaMenu"));
  click(*panel, centre(item(*panel, QStringLiteral("vistaPresetDisplay"))));
  require(menu->isVisible(), "the preset menu opens");
  QTest::keyClick(panel->window.get(), Qt::Key_Escape);
  require(!menu->isVisible(), "Escape closes it");
  controller.apply_preset(preset_index("Mono Check"));
  QCoreApplication::processEvents();
  require(text_of(*panel, QStringLiteral("vistaPresetName")) == QStringLiteral("Mono Check"),
          "a preset shows by name");
  require(
      std::fabs(item(*panel, QStringLiteral("vistaScope"))->property("previewRatio").toDouble()) <
          1e-9,
      "Mono Check previews a vertical line");

  auto* toggle = item(*panel, QStringLiteral("vistaToggle:vista.mono_bass"));
  click(*panel, centre(toggle));
  require(controller.choice(QStringLiteral("vista.mono_bass")) == 1, "a click turns mono bass on");
  require(text_of(*panel, QStringLiteral("vistaReadout")) == QStringLiteral("BASS · MONO BASS"),
          "the display reads back the switch: " +
              text_of(*panel, QStringLiteral("vistaReadout")).toStdString());
  QTest::keyClick(panel->window.get(), Qt::Key_Space);
  require(controller.choice(QStringLiteral("vista.mono_bass")) == 0, "Space turns it off");

  auto* knob = item(*panel, QStringLiteral("vistaKnob:vista.width"));
  knob->forceActiveFocus();
  QTest::keyClick(panel->window.get(), Qt::Key_Up);
  require(controller.value(QStringLiteral("vista.width")) > 0.0, "Up widens from mono");

  adapter.set_status({.saved_revision = 9, .audible_revision = 9, .deferred_changes = true});
  controller.refresh();
  QCoreApplication::processEvents();
  require(item(*panel, QStringLiteral("vistaStatus"))->property("tone").value<QColor>() ==
              QColor(QStringLiteral("#f2b25c")),
          "deferred status is highlighted");
}

// ---- Screenshots ----------------------------------------------------------------

// A real Vista over correlated stereo noise: its telemetry after a second.
std::vector<float> real_telemetry(const std::vector<double>& values, double rho) {
  auto created = effects::NativeEffectRuntime::create(vista::engine(), values, 48'000.0);
  require(created.has_value(), "a Vista runtime");
  auto runtime = std::move(created.value());
  std::uint32_t a = 11;
  std::uint32_t b = 29;
  const auto next = [](std::uint32_t& state) {
    state = state * 1'664'525U + 1'013'904'223U;
    return static_cast<double>(state >> 8U) / 8'388'608.0 - 1.0;
  };
  std::vector<oscillate::engine::StereoFrame> block(256);
  // A little low-passing makes the cloud read like music, not white noise.
  double la = 0.0;
  double lb = 0.0;
  for (std::uint64_t position = 0; position < 48'000; position += block.size()) {
    for (auto& frame : block) {
      la += 0.35 * (next(a) - la);
      lb += 0.35 * (next(b) - lb);
      const double left = 0.5 * la;
      const double right = 0.5 * (rho * la + std::sqrt(1.0 - rho * rho) * lb);
      frame = {static_cast<float>(left), static_cast<float>(right)};
    }
    const oscillate::native_dsp::Transport transport{0.0, 0.0, 0.0, 4.0};
    require(runtime->process(block, {}, position, 1, transport, {}) ==
                effects::EffectProcessStatus::rendered,
            "Vista renders");
  }
  std::vector<float> frame;
  require(runtime->read_telemetry(frame), "Vista publishes telemetry");
  return frame;
}

void screenshots_if_requested() {
  const char* directory = std::getenv("VISTA_PANEL_SCREENSHOT_DIR");
  if (directory == nullptr || *directory == '\0') {
    return;
  }
  QDir().mkpath(QString::fromLocal8Bit(directory));
  struct Shot {
    const char* preset;
    const char* touched;
    const char* file;
  };
  for (const Shot shot : {Shot{"Natural", "vista.width", "/vista_natural.png"},
                          Shot{"Wide Master", "vista.width_4", "/vista_wide_master.png"},
                          Shot{"Stereoize Vocal", "vista.stereoize", "/vista_stereoize_vocal.png"},
                          Shot{"Mono Check", "vista.width", "/vista_mono_check.png"}}) {
    InMemoryNativeSynthAdapter adapter(preset(shot.preset));
    NativeSynthDeviceController controller(table(), adapter);
    auto panel = load_panel(controller);
    panel->root->property("theme").value<QObject*>()->setProperty(
        "lastTouched", QString::fromLatin1(shot.touched));
    QTest::qWait(150);
    const QString path = QString::fromLocal8Bit(directory) + QString::fromLatin1(shot.file);
    require(panel->window->grabWindow().save(path, "PNG"), "screenshot saved");
    std::cout << "wrote " << path.toStdString() << '\n';
  }
  struct Live {
    const char* preset;
    double rho;
    const char* file;
  };
  for (const Live shot : {Live{"Wide Master", 0.6, "/vista_live.png"},
                          Live{"Huge Pads", 0.4, "/vista_live_safe.png"}}) {
    const auto values = preset(shot.preset);
    InMemoryNativeSynthAdapter adapter(values);
    NativeSynthDeviceController controller(table(), adapter);
    auto panel = load_panel(controller);
    adapter.set_telemetry(real_telemetry(values, shot.rho));
    QTest::qWait(500);
    const QString path = QString::fromLocal8Bit(directory) + QString::fromLatin1(shot.file);
    require(panel->window->grabWindow().save(path, "PNG"), "screenshot saved");
    std::cout << "wrote " << path.toStdString() << '\n';
  }
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  try {
    panel_covers_every_parameter();
    bands_follow_the_band_count();
    dragging_keeps_the_crossovers_in_order();
    haas_time_shows_only_for_haas();
    telemetry_reaches_the_scope_and_meters();
    presets_status_and_mono_bass();
    screenshots_if_requested();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "vista panel contract passed\n";
  return 0;
}
