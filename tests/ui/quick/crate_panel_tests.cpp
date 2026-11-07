#include "in_memory_native_synth_adapter.hpp"
#include "native_synth_device_controller.hpp"

#include "oscillate/native_dsp/synth/crate.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/native_dsp/synth/parameters.hpp"
#include "oscillate/project/model.hpp"
#include "oscillate/test/check.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>
#include <QWheelEvent>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The Crate panel (ADR-0063, spec 09 §5): it loads on a cut sample with a
// marker per chop; sixteen pads a bank, bottom-left first, and the bank
// switch; a pad selects its chop and auditions it through the host; the
// selected chop's knobs write that chop's slot; an era writes its settings;
// a chop mode re-chops the document; a marker double-click removes its cut;
// telemetry lights the pads; and with no sample the drop zone shows.
namespace {
using oscillate::quick::InMemoryNativeSynthAdapter;
using oscillate::quick::NativeSynthDeviceController;
using oscillate::quick::NativeSynthGesture;
using oscillate::quick::NativeSynthMedia;
namespace synth = oscillate::native_dsp::synth;
namespace crate = oscillate::native_dsp::synth::crate;
namespace project = oscillate::project;

using oscillate::test::require;

const synth::SynthEngine& engine() {
  const auto* found = synth::find_engine(crate::engine_id);
  require(found != nullptr && found->descriptor != nullptr, "Crate is registered");
  return *found;
}
const synth::EngineDescriptor& table() { return *engine().descriptor; }

// The built-in pluck, cut into four chops whose slots are not in time order,
// so the panel has to map chops to slots.
constexpr std::string_view sample_name = "pluck";
struct TestChop {
  std::uint32_t slot;
  std::uint64_t start_frame;
  std::string_view tag;
  std::string_view name;
};
constexpr TestChop test_chops[] = {{0, 0, "other", "Pluck"},
                                   {3, 8'400, "kick", ""},
                                   {1, 16'800, "snare", ""},
                                   {2, 25'200, "hat", ""}};
constexpr std::size_t chop_count = std::size(test_chops);

std::unique_ptr<InMemoryNativeSynthAdapter> init_adapter() {
  auto values = synth::preset_values(table(), table().presets[0]).value();
  return std::make_unique<InMemoryNativeSynthAdapter>(std::move(values));
}

std::unique_ptr<InMemoryNativeSynthAdapter> adapter_with_sample() {
  auto adapter = init_adapter();
  project::NativeSynthDocument document;
  document.media.push_back(
      {std::string(crate::sample_slot), project::NativeBuiltinMedia{std::string(sample_name)}, 0});
  for (const auto& chop : test_chops)
    document.regions.push_back({chop.slot, std::string(crate::sample_slot), chop.start_frame,
                                std::string(chop.tag), std::string(chop.name)});
  adapter->submit_document(std::move(document), NativeSynthGesture::single);
  // Built-in media: the engine's own audio, named by the built-in.
  adapter->set_media(
      std::string(crate::sample_slot),
      NativeSynthMedia{nullptr, std::string(sample_name), true, false, std::nullopt, {}});
  return adapter;
}

std::size_t index_of(std::string_view id) {
  const auto& parameters = table().parameters;
  for (std::size_t i = 0; i < parameters.size(); ++i)
    if (parameters[i].id == id)
      return i;
  throw std::runtime_error("no parameter " + std::string(id));
}
double value_of(const InMemoryNativeSynthAdapter& adapter, std::string_view id) {
  return adapter.values().at(index_of(id));
}

QObject* crate_of(NativeSynthDeviceController& controller) {
  auto* object = controller.property("crate").value<QObject*>();
  require(object != nullptr, "the controller has a Crate model for engine crate");
  return object;
}

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
      import "qrc:/crate"
      CratePanel {}
    )",
                    QUrl(QStringLiteral("qrc:/CratePanelTest.qml")));
  if (!component.isReady()) {
    throw std::runtime_error("Crate panel failed to load: " +
                             component.errorString().toStdString());
  }
  QObject* object = component.createWithInitialProperties(
      {{QStringLiteral("controller"), QVariant::fromValue<QObject*>(&controller)}});
  require(object != nullptr,
          "Crate panel was not created: " + component.errorString().toStdString());
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

QString numbered(const char* prefix, std::size_t index) {
  return QString::fromLatin1(prefix) + QString::number(index);
}

QPoint centre(QQuickItem* target) {
  return target->mapToScene(QPointF(target->width() / 2, target->height() / 2)).toPoint();
}

void settle() {
  QCoreApplication::processEvents();
  QTest::qWait(20);
}

void click(Panel& panel, QQuickItem* target) {
  QTest::mouseClick(panel.window.get(), Qt::LeftButton, Qt::NoModifier, centre(target));
  settle();
}

void wheel(Panel& panel, QPoint point, int delta) {
  QWheelEvent event(point, panel.window->mapToGlobal(point), QPoint(), QPoint(0, delta),
                    Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
  QCoreApplication::sendEvent(panel.window.get(), &event);
  settle();
}

QString text_of(Panel& panel, const QString& name) {
  return item(panel, name)->property("text").toString();
}

// ---- Contract -------------------------------------------------------------------

void loads_on_a_cut_sample() {
  auto adapter = adapter_with_sample();
  NativeSynthDeviceController controller(table(), *adapter);
  auto panel = load_panel(controller);
  require(panel->root->objectName() == QStringLiteral("cratePanel"),
          "the panel is named cratePanel");
  require(panel->root->implicitWidth() >= 1400 && panel->root->implicitHeight() >= 660,
          "the panel has the mockup's size");
  auto* model = crate_of(controller);
  require(model->property("has_sample").toBool(), "the built-in sample decodes");
  require(model->property("chops").toList().size() == static_cast<qsizetype>(chop_count),
          "every region is a chop");
  require(item(*panel, QStringLiteral("crateWaveform"))->isVisible(), "the waveform shows");
  require(!item(*panel, QStringLiteral("crateDropZone"))->isVisible(),
          "no drop zone with a sample");
  for (std::size_t i = 0; i < chop_count; ++i)
    require(find_item(panel->root, numbered("crateMarker:", i)) != nullptr, "a marker per chop");
  require(find_item(panel->root, numbered("crateMarker:", chop_count)) == nullptr,
          "no marker past the last chop");
  require(text_of(*panel, QStringLiteral("crateChopCount")) == QStringLiteral("4 CHOPS"),
          "the mode bar counts the chops");
  require(!text_of(*panel, QStringLiteral("crateSampleName")).isEmpty(),
          "the header names the sample");
}

void pads_bank_by_sixteen() {
  auto adapter = adapter_with_sample();
  NativeSynthDeviceController controller(table(), *adapter);
  auto panel = load_panel(controller);
  for (std::size_t i = 0; i < crate::bank_size; ++i)
    require(find_item(panel->root, numbered("cratePad:", i)) != nullptr, "bank A has pads 1-16");
  require(find_item(panel->root, numbered("cratePad:", crate::bank_size)) == nullptr,
          "bank A shows no bank B pad");
  // Bottom-left first, as on hardware.
  auto* first = item(*panel, QStringLiteral("cratePad:0"));
  auto* second = item(*panel, QStringLiteral("cratePad:1"));
  auto* top_left = item(*panel, QStringLiteral("cratePad:12"));
  require(centre(first).y() > centre(top_left).y() && centre(first).x() < centre(second).x(),
          "pad 1 sits bottom left, pad 2 beside it, pad 13 above");
  require(!item(*panel, QStringLiteral("cratePad:0"))->property("empty").toBool() &&
              item(*panel, numbered("cratePad:", chop_count))->property("empty").toBool(),
          "pads past the last chop are empty");
  require(text_of(*panel, QStringLiteral("cratePadsTitle")) == QStringLiteral("PADS · C1–D#2"),
          "bank A names its keys");

  click(*panel, item(*panel, QStringLiteral("crateBank:1")));
  require(crate_of(controller)->property("bank").toInt() == 1, "the bank switch picks bank B");
  for (std::size_t i = crate::bank_size; i < 2 * crate::bank_size; ++i)
    require(find_item(panel->root, numbered("cratePad:", i)) != nullptr, "bank B has pads 17-32");
  require(find_item(panel->root, QStringLiteral("cratePad:0")) == nullptr, "bank A is put away");
  require(text_of(*panel, QStringLiteral("cratePadsTitle")) == QStringLiteral("PADS · E2–G3"),
          "bank B continues from key 52");
}

void a_pad_selects_and_auditions_its_chop() {
  auto adapter = adapter_with_sample();
  NativeSynthDeviceController controller(table(), *adapter);
  auto panel = load_panel(controller);
  QSignalSpy pressed(panel->root, SIGNAL(padPressed(int, int)));
  QSignalSpy released(panel->root, SIGNAL(padReleased(int)));
  click(*panel, item(*panel, QStringLiteral("cratePad:2")));
  require(crate_of(controller)->property("selected").toInt() == 2, "the pad selects its chop");
  require(pressed.count() == 1 && released.count() == 1, "a click presses and releases the pad");
  const int key = crate::first_pad_key + 2;
  require(pressed.at(0).at(0).toInt() == key && pressed.at(0).at(1).toInt() > 0,
          "the pad plays its key at a velocity");
  require(released.at(0).at(0).toInt() == key, "the release lets go of the same key");
  require(text_of(*panel, QStringLiteral("crateChopTitle")) == QStringLiteral("CHOP 03"),
          "the chop section shows the selected chop");
  require(item(*panel, QStringLiteral("cratePad:2"))->property("selected").toBool(),
          "the pad shows it is selected");
}

void a_chop_knob_writes_its_slot() {
  auto adapter = adapter_with_sample();
  NativeSynthDeviceController controller(table(), *adapter);
  auto panel = load_panel(controller);
  // Chop 2 in time order lives in slot 3.
  crate_of(controller)->setProperty("selected", 1);
  settle();
  const auto pitch = crate::chop_parameter_id(test_chops[1].slot, "pitch");
  auto* knob = item(*panel, QStringLiteral("crateKnob:") + QString::fromStdString(pitch));
  wheel(*panel, centre(knob), 120);
  require(value_of(*adapter, pitch) > 0.0, "scrolling the pitch knob raises the chop's pitch");
  for (const auto& other : test_chops)
    if (other.slot != test_chops[1].slot)
      require(value_of(*adapter, crate::chop_parameter_id(other.slot, "pitch")) == 0.0,
              "the other chops keep their pitch");
  click(*panel, item(*panel, QStringLiteral("crateFilterMode:1")));
  require(value_of(*adapter, crate::chop_parameter_id(test_chops[1].slot, "filter")) == 1.0,
          "the filter mode segment writes the chop's filter");
  click(*panel, item(*panel, QStringLiteral("cratePlayMode:2")));
  require(value_of(*adapter, crate::chop_parameter_id(test_chops[1].slot, "mode")) == 2.0,
          "the play mode segment writes the chop's mode");
  click(*panel, item(*panel, QStringLiteral("crateReverse")));
  require(value_of(*adapter, crate::chop_parameter_id(test_chops[1].slot, "reverse")) == 1.0,
          "REVERSE turns the chop around");
}

void an_era_writes_its_settings() {
  auto adapter = adapter_with_sample();
  NativeSynthDeviceController controller(table(), *adapter);
  auto panel = load_panel(controller);
  constexpr std::size_t sp12 = static_cast<std::size_t>(crate::Era::sp12);
  click(*panel, item(*panel, numbered("crateEra:", sp12)));
  const auto& settings = crate::era_settings[sp12];
  require(value_of(*adapter, "crate.era") == static_cast<double>(sp12), "the era is chosen");
  require(value_of(*adapter, "crate.bits") == settings.bits, "the era sets its bits");
  require(std::abs(value_of(*adapter, "crate.rate") - settings.rate) < 0.5,
          "the era sets its rate");
  require(std::abs(value_of(*adapter, "crate.drive") - settings.drive) < 1e-9,
          "the era sets its drive");
  require(text_of(*panel, QStringLiteral("crateEraName")).contains(QStringLiteral("SP-12")),
          "the character section names the era");
}

void a_chop_mode_rechops_the_sample() {
  auto adapter = adapter_with_sample();
  NativeSynthDeviceController controller(table(), *adapter);
  auto panel = load_panel(controller);
  auto* model = crate_of(controller);
  const auto before = adapter->document().regions;
  click(*panel, item(*panel, QStringLiteral("crateModeEqual")));
  require(model->property("chop_mode").toString() == QStringLiteral("equal"), "EQUAL is the mode");
  const auto count = static_cast<std::size_t>(model->property("equal_count").toInt());
  require(count > 1, "the default count leaves room for one fewer");
  const auto& after = adapter->document().regions;
  require(after != before, "choosing a mode re-chops the document");
  require(after.size() == count, "equal slices, as many as the count");
  require(item(*panel, QStringLiteral("crateEqualCount"))->isVisible(), "the count shows in EQUAL");
  click(*panel, item(*panel, QStringLiteral("crateEqualLess")));
  require(adapter->document().regions.size() == count - 1, "one fewer slice re-chops again");
  require(text_of(*panel, QStringLiteral("crateModeHint")).contains(QString::number(count - 1)),
          "the hint names the count");
}

void a_marker_double_click_removes_its_cut() {
  auto adapter = adapter_with_sample();
  NativeSynthDeviceController controller(table(), *adapter);
  auto panel = load_panel(controller);
  QTest::mouseDClick(panel->window.get(), Qt::LeftButton, Qt::NoModifier,
                     centre(item(*panel, QStringLiteral("crateMarker:2"))));
  settle();
  require(adapter->document().regions.size() == chop_count - 1, "the cut is removed");
  QTest::mouseDClick(panel->window.get(), Qt::LeftButton, Qt::NoModifier,
                     centre(item(*panel, QStringLiteral("crateMarker:0"))));
  settle();
  require(adapter->document().regions.size() == chop_count - 1, "the first cut stays");
}

void telemetry_lights_the_pads() {
  auto adapter = adapter_with_sample();
  NativeSynthDeviceController controller(table(), *adapter);
  auto panel = load_panel(controller);
  std::vector<float> frame(crate::telemetry::size, -1.0F);
  frame[crate::telemetry::playheads + test_chops[1].slot] = 0.5F;
  frame[crate::telemetry::last_slot] = static_cast<float>(test_chops[1].slot);
  frame[crate::telemetry::last_velocity] = 100.0F;
  frame[crate::telemetry::output_peak_db] = -6.0F;
  adapter->set_telemetry(frame);
  QMetaObject::invokeMethod(panel->root, "poll");
  settle();
  require(item(*panel, QStringLiteral("cratePadLive:1"))->opacity() > 0.0,
          "the sounding chop's pad lights");
  require(item(*panel, QStringLiteral("cratePadLive:0"))->opacity() == 0.0,
          "an idle pad stays dark");
  require(item(*panel, QStringLiteral("cratePadLast:1"))->isVisible(),
          "the last pad struck is marked");
}

void without_a_sample_the_drop_zone_shows() {
  auto adapter = init_adapter();
  NativeSynthDeviceController controller(table(), *adapter);
  auto panel = load_panel(controller);
  require(!crate_of(controller)->property("has_sample").toBool(), "Init has no sample");
  auto* zone = item(*panel, QStringLiteral("crateDropZone"));
  require(zone->isVisible(), "the drop zone shows");
  require(zone->width() > panel->root->width() / 2 && zone->height() > panel->root->height() / 2,
          "the drop zone is big");
  require(!item(*panel, QStringLiteral("crateWaveform"))->isVisible(),
          "no waveform without a sample");
  require(item(*panel, QStringLiteral("crateEra:0"))->isVisible(), "the header still shows");
}

// With CRATE_PANEL_SCREENSHOT_DIR set: the panel on a cut sample, with a
// chop selected and one sounding, and the empty panel.
void screenshots_if_requested() {
  const char* directory = std::getenv("CRATE_PANEL_SCREENSHOT_DIR");
  if (directory == nullptr || *directory == '\0')
    return;
  QDir().mkpath(QString::fromLocal8Bit(directory));
  const QString base = QString::fromLocal8Bit(directory);
  {
    auto adapter = adapter_with_sample();
    NativeSynthDeviceController controller(table(), *adapter);
    controller.set_value(
        QString::fromStdString(crate::chop_parameter_id(test_chops[1].slot, "pitch")), 3.0);
    controller.set_choice(
        QString::fromStdString(crate::chop_parameter_id(test_chops[1].slot, "filter")), 1);
    controller.set_value(
        QString::fromStdString(crate::chop_parameter_id(test_chops[1].slot, "cutoff")), 3'200.0);
    auto panel = load_panel(controller);
    crate_of(controller)->setProperty("selected", 1);
    std::vector<float> frame(crate::telemetry::size, -1.0F);
    frame[crate::telemetry::playheads + test_chops[2].slot] = 0.4F;
    frame[crate::telemetry::last_slot] = static_cast<float>(test_chops[2].slot);
    adapter->set_telemetry(frame);
    QMetaObject::invokeMethod(panel->root, "poll");
    QTest::qWait(150);
    require(panel->window->grabWindow().save(base + QStringLiteral("/crate_panel.png"), "PNG"),
            "screenshot saved");
  }
  {
    auto adapter = init_adapter();
    NativeSynthDeviceController controller(table(), *adapter);
    auto panel = load_panel(controller);
    QTest::qWait(150);
    require(panel->window->grabWindow().save(base + QStringLiteral("/crate_empty.png"), "PNG"),
            "screenshot saved");
  }
  std::cout << "wrote Crate screenshots to " << base.toStdString() << '\n';
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  try {
    loads_on_a_cut_sample();
    pads_bank_by_sixteen();
    a_pad_selects_and_auditions_its_chop();
    a_chop_knob_writes_its_slot();
    an_era_writes_its_settings();
    a_chop_mode_rechops_the_sample();
    a_marker_double_click_removes_its_cut();
    telemetry_lights_the_pads();
    without_a_sample_the_drop_zone_shows();
    screenshots_if_requested();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "crate panel contract passed\n";
  return 0;
}
