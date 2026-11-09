#include "in_memory_native_synth_adapter.hpp"
#include "native_synth_device_controller.hpp"

#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/effects/temper.hpp"
#include "oscillate/native_dsp/synth/parameters.hpp"
#include "oscillate/test/check.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// The Temper panel (ADR-0064): every parameter on a control (the band
// inspector follows the selected band), the sensitivity diamonds dragging
// frequency and sensitivity as one gesture, dynamic bands added by a
// double-click on the screen, switched off by a double-click on their node
// and removed back to their defaults, scrolled for Q, the header's mode,
// phase and Delta, and the telemetry decode into the display, the
// inspector and the heatmap.
namespace {
using oscillate::quick::InMemoryNativeSynthAdapter;
using oscillate::quick::NativeSynthDeviceController;
using oscillate::quick::NativeSynthGesture;
namespace synth = oscillate::native_dsp::synth;
namespace temper = oscillate::native_dsp::effects::temper;

using oscillate::test::require;

const synth::EngineDescriptor& table() { return *temper::engine().descriptor; }

QString qs(std::string_view text) {
  return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

QString band_id(const char* name, int band) {
  return QStringLiteral("temper.band_%1_%2").arg(QString::fromLatin1(name)).arg(band);
}

QString node_id(const char* name, int node) {
  return QStringLiteral("temper.sens_%1_%2").arg(QString::fromLatin1(name)).arg(node);
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
      import "qrc:/temper"
      TemperPanel {}
    )",
                    QUrl(QStringLiteral("qrc:/TemperPanelTest.qml")));
  if (!component.isReady()) {
    throw std::runtime_error("Temper panel failed to load: " +
                             component.errorString().toStdString());
  }
  QObject* object = component.createWithInitialProperties(
      {{QStringLiteral("controller"), QVariant::fromValue<QObject*>(&controller)}});
  require(object != nullptr,
          "Temper panel was not created: " + component.errorString().toStdString());
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

void drag(Panel& panel, QPoint from, QPoint by) {
  QTest::mousePress(panel.window.get(), Qt::LeftButton, Qt::NoModifier, from);
  QTest::mouseMove(panel.window.get(), from + by / 2);
  QTest::mouseMove(panel.window.get(), from + by);
  QTest::mouseRelease(panel.window.get(), Qt::LeftButton, Qt::NoModifier, from + by);
}

void wheel(Panel& panel, QPoint point, int delta) {
  QWheelEvent event(point, panel.window->mapToGlobal(point), QPoint(), QPoint(0, delta),
                    Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
  QCoreApplication::sendEvent(panel.window.get(), &event);
}

QString text_of(Panel& panel, const QString& name) {
  return item(panel, name)->property("text").toString();
}

bool wait_for_text(Panel& panel, const QString& name, const QString& text) {
  return QTest::qWaitFor([&] { return text_of(panel, name) == text; }, 1000);
}

std::vector<double> defaults() { return synth::default_values(table()); }

// ---- Contract -------------------------------------------------------------------

void panel_covers_every_parameter() {
  InMemoryNativeSynthAdapter adapter(defaults());
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(panel->root->implicitWidth() >= 1300 && panel->root->implicitHeight() >= 760,
          "panel has its designed size");
  const int bands = panel->root->property("bandCount").toInt();
  const int nodes = panel->root->property("nodeCount").toInt();
  require(bands == static_cast<int>(temper::bands), "the panel counts the table's bands");
  require(nodes == 6, "and its sensitivity nodes: " + std::to_string(nodes));

  // The suppressor's controls, by kind of widget.
  for (const char* id :
       {"temper.depth", "temper.detail", "temper.max_cut", "temper.tilt", "temper.attack",
        "temper.release", "temper.attacks", "temper.sustains", "temper.separate", "temper.link",
        "temper.unmask_amount", "temper.unmask_focus", "temper.mix", "temper.trim"}) {
    require(find_item(panel->root, QStringLiteral("temperKnob:") + QString::fromLatin1(id)) !=
                nullptr,
            std::string("knob ") + id);
  }
  for (const char* id : {"temper.mode", "temper.phase", "temper.stereo"}) {
    require(find_item(panel->root, QStringLiteral("temperSegment:") + QString::fromLatin1(id)) !=
                nullptr,
            std::string("segment ") + id);
  }
  item(*panel, QStringLiteral("temperToggle:temper.unmask"));
  item(*panel, QStringLiteral("temperDelta"));
  for (int node = 1; node <= nodes; ++node) {
    item(*panel, QStringLiteral("temperNode:%1").arg(node));
  }
  // The inspector follows the selected band.
  for (int band = 1; band <= bands; ++band) {
    click(*panel, centre(item(*panel, QStringLiteral("temperBand:%1").arg(band))));
    require(panel->root->property("selected").toInt() == band - 1, "a chip selects its band");
    item(*panel, QStringLiteral("temperToggle:") + band_id("on", band));
    item(*panel, QStringLiteral("temperSegment:") + band_id("kind", band));
    for (const char* name : {"freq", "q", "range", "threshold", "ratio"}) {
      item(*panel, QStringLiteral("temperKnob:") + band_id(name, band));
    }
  }
  // Nothing in the table is left without a home.
  const std::size_t globals = 19;
  require(table().parameters.size() ==
              globals + static_cast<std::size_t>(nodes) * 2 + static_cast<std::size_t>(bands) * 7,
          "the table is the globals, the nodes and the bands");
  for (const auto& p : table().parameters) {
    const QString id = qs(p.id);
    require(id.startsWith(QStringLiteral("temper.")), "every id is Temper's: " + id.toStdString());
  }
}

void header_and_cards_write_their_parameters() {
  InMemoryNativeSynthAdapter adapter(defaults());
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  auto* mode = item(*panel, QStringLiteral("temperSegment:temper.mode"));
  click(*panel, scene(mode, mode->width() * 0.75, mode->height() / 2));
  require(controller.choice(QStringLiteral("temper.mode")) == 1, "HARD picks Hard");
  auto* phase = item(*panel, QStringLiteral("temperSegment:temper.phase"));
  click(*panel, scene(phase, phase->width() * 5.0 / 6.0, phase->height() / 2));
  require(controller.choice(QStringLiteral("temper.phase")) == 2, "LINEAR picks Linear");
  click(*panel, centre(item(*panel, QStringLiteral("temperDelta"))));
  require(controller.choice(QStringLiteral("temper.delta")) == 1, "Delta switches on");
  require(
      text_of(*panel, QStringLiteral("temperDisplayCaption")).startsWith(QStringLiteral("DELTA")),
      "and the display says so");
  click(*panel, centre(item(*panel, QStringLiteral("temperToggle:temper.unmask"))));
  require(controller.choice(QStringLiteral("temper.unmask")) == 1, "Unmask switches on");
  auto* stereo = item(*panel, QStringLiteral("temperSegment:temper.stereo"));
  click(*panel, scene(stereo, stereo->width() * 1.5 / 5.0, stereo->height() / 2));
  require(controller.choice(QStringLiteral("temper.stereo")) == 1, "MID picks Mid");
  require(item(*panel, QStringLiteral("temperKnob:temper.link"))->property("inactive").toBool(),
          "Link rests unless both channels are processed");
}

void sensitivity_nodes_drag_as_one_gesture() {
  InMemoryNativeSynthAdapter adapter(defaults());
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  auto* display = item(*panel, QStringLiteral("temperDisplay"));
  auto* node = item(*panel, QStringLiteral("temperNode:4"));
  const double plot_x = display->property("plotX").toDouble();
  const double plot_w = display->property("plotW").toDouble();
  const double plot_h = display->property("plotH").toDouble();
  const double top = display->property("topDb").toDouble();
  const double hx = node->property("hx").toDouble();
  const double gain = controller.value(node_id("gain", 4));

  const auto start = adapter.submissions().size();
  drag(*panel, centre(node), QPoint(40, -30));
  const auto& s = adapter.submissions();
  require(s.size() > start + 2 && s[start].gesture == NativeSynthGesture::begin &&
              s.back().gesture == NativeSynthGesture::commit,
          "a diamond drag is begin, updates and commit");
  const double hz = controller.frequency_at((hx + 40.0 - plot_x) / plot_w);
  require(std::fabs(std::log2(controller.value(node_id("freq", 4)) / hz)) < 0.02,
          "the node follows the pointer across");
  const double db = gain + 30.0 / plot_h * 2.0 * top;
  require(std::fabs(controller.value(node_id("gain", 4)) - db) < 0.3,
          "and up: " + std::to_string(controller.value(node_id("gain", 4))));
  QTest::mouseDClick(panel->window.get(), Qt::LeftButton, Qt::NoModifier, centre(node));
  require(controller.value(node_id("gain", 4)) == 0.0, "double-clicking a diamond flattens it");
}

void bands_add_toggle_scroll_and_remove() {
  InMemoryNativeSynthAdapter adapter(defaults());
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(!item(*panel, QStringLiteral("temperHandle:2"))->isVisible(),
          "a band that is off and unselected has no node");

  // Double-click the empty screen: band 1 comes on there, as one gesture.
  auto* area = item(*panel, QStringLiteral("temperDisplayArea"));
  auto* display = item(*panel, QStringLiteral("temperDisplay"));
  const double top = display->property("topDb").toDouble();
  const QPoint spot = scene(area, area->width() * 0.5, area->height() * 0.75);
  const auto added = adapter.submissions().size();
  QTest::mouseDClick(panel->window.get(), Qt::LeftButton, Qt::NoModifier, spot);
  require(controller.choice(band_id("on", 1)) == 1, "the first free band switches on");
  require(std::fabs(std::log2(controller.value(band_id("freq", 1)) /
                              controller.frequency_at(0.5))) < 0.02,
          "where the screen was clicked");
  require(std::fabs(controller.value(band_id("range", 1)) + top / 2.0) < 0.5,
          "at the range clicked: " + std::to_string(controller.value(band_id("range", 1))));
  require(adapter.submissions()[added].gesture == NativeSynthGesture::begin &&
              adapter.submissions().back().gesture == NativeSynthGesture::commit,
          "adding a band is one undo step");
  require(panel->root->property("selected").toInt() == 0, "and selects it");
  QCoreApplication::processEvents();
  auto* handle = item(*panel, QStringLiteral("temperHandle:1"));
  require(handle->isVisible(), "the band has its node");

  // Scroll for Q.
  const double q = controller.value(band_id("q", 1));
  wheel(*panel, centre(handle), 120);
  require(controller.value(band_id("q", 1)) > q, "scrolling raises the Q");

  // Drag: frequency and range together.
  const double range = controller.value(band_id("range", 1));
  drag(*panel, centre(handle), QPoint(-50, -40));
  require(controller.value(band_id("range", 1)) > range + 3.0, "dragging up raises the range");

  // Double-click the node: off, and on again (the selected band keeps its node).
  QTest::mouseDClick(panel->window.get(), Qt::LeftButton, Qt::NoModifier, centre(handle));
  require(controller.choice(band_id("on", 1)) == 0, "double-clicking a node switches it off");
  QCoreApplication::processEvents();
  require(handle->isVisible(), "the selected band's node stays to switch it back");
  QTest::mouseDClick(panel->window.get(), Qt::LeftButton, Qt::NoModifier, centre(handle));
  require(controller.choice(band_id("on", 1)) == 1, "and on again");

  // The inspector's kind tiles.
  auto* kind = item(*panel, QStringLiteral("temperSegment:") + band_id("kind", 1));
  click(*panel, scene(kind, kind->width() * 2.5 / 3.0, kind->height() / 2));
  require(controller.choice(band_id("kind", 1)) == 2, "a tile makes band 1 a high shelf");

  // Remove: off and back to its defaults, as one undo step.
  const auto removing = adapter.submissions().size();
  click(*panel, centre(item(*panel, QStringLiteral("temperBandRemove"))));
  const auto info = controller.parameter(band_id("freq", 1));
  require(controller.choice(band_id("on", 1)) == 0 && controller.choice(band_id("kind", 1)) == 0 &&
              controller.value(band_id("freq", 1)) ==
                  info.value(QStringLiteral("default")).toDouble(),
          "Remove switches the band off and resets it");
  require(adapter.submissions()[removing].gesture == NativeSynthGesture::begin &&
              adapter.submissions().back().gesture == NativeSynthGesture::commit,
          "removing a band is one undo step");
}

std::vector<float> frame_of(float cut_db, float band_gain_db) {
  namespace t = temper::telemetry;
  std::vector<float> frame(t::size, 0.0F);
  frame[t::input_peak] = -6.0F;
  frame[t::input_peak + 1] = -7.0F;
  frame[t::output_peak] = -8.0F;
  frame[t::output_peak + 1] = -9.0F;
  frame[t::max_cut] = cut_db;
  frame[t::keyed] = 1.0F;
  frame[t::transient] = 1.0F;
  frame[t::latency_ms] = 10.7F;
  frame[t::band_gain] = band_gain_db;
  frame[t::band_level] = -24.0F;
  for (std::size_t i = 0; i < temper::display_bands; ++i) {
    frame[t::input_spectrum + i] = -40.0F;
    frame[t::output_spectrum + i] = -42.0F;
    frame[t::sidechain_spectrum + i] = -30.0F;
    frame[t::gain + i] = i == temper::display_bands / 2 ? -cut_db : 0.0F;
  }
  return frame;
}

void telemetry_drives_the_display() {
  auto values = defaults();
  values[*synth::find_parameter(table(), "temper.band_on_1")] = 1.0;
  values[*synth::find_parameter(table(), "temper.unmask")] = 1.0;
  InMemoryNativeSynthAdapter adapter(values);
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  QTest::qWait(300);
  require(text_of(*panel, QStringLiteral("temperLatency")) == QStringLiteral("— ms"),
          "a dash with nothing measured");
  require(text_of(*panel, QStringLiteral("temperBandNow")) == QStringLiteral("—"),
          "and no band reading");

  adapter.set_telemetry(frame_of(6.0F, -3.0F));
  const auto live = controller.telemetry();
  require(live.value(QStringLiteral("live")).toBool() &&
              live.value(QStringLiteral("gain")).toList().size() ==
                  static_cast<qsizetype>(temper::display_bands) &&
              live.value(QStringLiteral("band_gain")).toList().size() ==
                  static_cast<qsizetype>(temper::bands) &&
              live.value(QStringLiteral("keyed")).toBool() &&
              live.value(QStringLiteral("max_cut")).toDouble() == 6.0,
          "the controller decodes Temper's frame");
  require(wait_for_text(*panel, QStringLiteral("temperLatency"), QStringLiteral("10.7 ms")),
          "the latency chip reads the telemetry: " +
              text_of(*panel, QStringLiteral("temperLatency")).toStdString());
  require(text_of(*panel, QStringLiteral("temperBandNow")) == QStringLiteral("−3.0 dB"),
          "the inspector reads the band's gain now: " +
              text_of(*panel, QStringLiteral("temperBandNow")).toStdString());
  require(text_of(*panel, QStringLiteral("temperDisplayCaption"))
              .startsWith(QStringLiteral("CUT NOW −6.0")),
          "the caption reads the cut");
  auto* heatmap = item(*panel, QStringLiteral("temperHeatmap"));
  require(QTest::qWaitFor([&] { return heatmap->property("rowCount").toInt() >= 2; }, 1000),
          "the heatmap gathers rows");
  require(item(*panel, QStringLiteral("temperKeyMeterFill"))->width() > 0.0,
          "the key meter shows the sidechain");
  adapter.set_telemetry({});
  require(wait_for_text(*panel, QStringLiteral("temperLatency"), QStringLiteral("— ms")),
          "no telemetry: a dash again");
}

void presets_step() {
  InMemoryNativeSynthAdapter adapter(defaults());
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(table().presets.size() >= 2, "Temper has presets to step through");
  const QString first = controller.preset_name();
  click(*panel, centre(item(*panel, QStringLiteral("temperPresetNext"))));
  require(controller.preset_name() != first, "next steps the preset");
  QCoreApplication::processEvents();
  require(text_of(*panel, QStringLiteral("temperPresetName")) == controller.preset_name(),
          "the display names it");
  auto* menu = item(*panel, QStringLiteral("temperMenu"));
  click(*panel, centre(item(*panel, QStringLiteral("temperPresetDisplay"))));
  require(menu->isVisible(), "the preset menu opens");
  QTest::keyClick(panel->window.get(), Qt::Key_Escape);
  require(!menu->isVisible(), "Escape closes it");
}

// ---- Screenshots ----------------------------------------------------------------

constexpr double rate = 48'000.0;

// A bright, ringing voice: a 220 Hz buzz with a 3.1 kHz ring and a hiss
// burst every second, and a kick on the key every half second.
struct Signal {
  std::vector<oscillate::engine::StereoFrame> voice;
  std::vector<oscillate::engine::StereoFrame> kick;
};

Signal signal(std::size_t frames) {
  Signal out{std::vector<oscillate::engine::StereoFrame>(frames),
             std::vector<oscillate::engine::StereoFrame>(frames)};
  std::uint32_t seed = 12'345;
  for (std::size_t i = 0; i < frames; ++i) {
    const double t = static_cast<double>(i) / rate;
    double x = 0.0;
    for (int h = 1; h <= 40; ++h) {
      x += 0.12 / h * std::sin(2 * std::numbers::pi * 220.0 * h * t);
    }
    x += 0.12 * std::sin(2 * std::numbers::pi * 3'100.0 * t);
    seed = seed * 1'664'525U + 1'013'904'223U;
    const double hiss = (static_cast<double>(seed >> 8U) / 8'388'608.0 - 1.0);
    x += (std::fmod(t, 1.0) > 0.7 ? 0.08 : 0.004) * hiss;
    const double beat = std::fmod(t, 0.5);
    const double kick =
        0.8 * std::exp(-beat / 0.12) *
        std::sin(2 * std::numbers::pi * (55.0 * beat + 40.0 * (1 - std::exp(-beat / 0.02))));
    out.voice[i] = {static_cast<float>(x), static_cast<float>(0.9 * x)};
    out.kick[i] = {static_cast<float>(kick), static_cast<float>(kick)};
  }
  return out;
}

// The real Temper over the signal; a telemetry frame every 1600 frames.
std::vector<std::vector<float>> real_telemetry(const std::vector<double>& values,
                                               const Signal& input) {
  namespace effects = oscillate::native_dsp::effects;
  auto created = effects::NativeEffectRuntime::create(temper::engine(), values, rate);
  require(created.has_value(), "a Temper runtime");
  auto runtime = std::move(created.value());
  std::vector<std::vector<float>> frames;
  std::vector<oscillate::engine::StereoFrame> block;
  std::vector<oscillate::engine::StereoFrame> key;
  constexpr std::size_t block_frames = 160;
  for (std::size_t position = 0; position < input.voice.size(); position += block_frames) {
    const std::size_t size = std::min(block_frames, input.voice.size() - position);
    const auto from = static_cast<std::ptrdiff_t>(position);
    const auto to = static_cast<std::ptrdiff_t>(position + size);
    block.assign(input.voice.begin() + from, input.voice.begin() + to);
    key.assign(input.kick.begin() + from, input.kick.begin() + to);
    const oscillate::native_dsp::Transport transport{0.0, 0.0, 0.0, 4.0};
    require(runtime->process(block, key, position, 1, transport, {}) ==
                effects::EffectProcessStatus::rendered,
            "Temper renders");
    if ((position + size) % 1'600 == 0) {
      std::vector<float> frame;
      require(runtime->read_telemetry(frame), "Temper publishes telemetry");
      frames.push_back(frame);
    }
  }
  return frames;
}

void screenshots_if_requested() {
  const char* directory = std::getenv("TEMPER_PANEL_SCREENSHOT_DIR");
  if (directory == nullptr || *directory == '\0') {
    return;
  }
  QDir().mkpath(QString::fromLocal8Bit(directory));
  const auto input = signal(static_cast<std::size_t>(3.0 * rate));
  struct Shot {
    const char* preset;
    const char* file;
  };
  for (const Shot& shot :
       {Shot{"Tame Harshness", "/temper_tame.png"}, Shot{"De-Ess", "/temper_de_ess.png"},
        Shot{"Unmask Kick", "/temper_unmask.png"}, Shot{"Ring Control", "/temper_ring.png"}}) {
    std::vector<double> values;
    for (const auto& entry : table().presets) {
      if (entry.name == shot.preset) {
        auto resolved = synth::preset_values(table(), entry);
        require(resolved.has_value(), "preset resolves");
        values = resolved.value();
      }
    }
    require(!values.empty(), "preset exists");
    InMemoryNativeSynthAdapter adapter(values);
    NativeSynthDeviceController controller(table(), adapter);
    auto panel = load_panel(controller);
    const auto frames = real_telemetry(values, input);
    for (const auto& frame : frames) {
      adapter.set_telemetry(frame);
      QTest::qWait(16);
    }
    QTest::qWait(150);
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
    header_and_cards_write_their_parameters();
    sensitivity_nodes_drag_as_one_gesture();
    bands_add_toggle_scroll_and_remove();
    telemetry_drives_the_display();
    presets_step();
    screenshots_if_requested();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "temper panel contract passed\n";
  return 0;
}
