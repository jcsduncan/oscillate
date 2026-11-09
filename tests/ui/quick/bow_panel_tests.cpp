#include "in_memory_native_synth_adapter.hpp"
#include "native_synth_device_controller.hpp"
#include "panel_kit_checks.hpp"

#include "../../native_dsp/bow_test_library.hpp"
#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/native_dsp/synth/bow.hpp"
#include "oscillate/native_dsp/synth/prism.hpp"
#include "oscillate/test/check.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// The Bow panel (ADR-0111): the controller's library status (loading,
// missing, ready), its keyboard layout and telemetry decode and the library
// folder it shows; and the QML panel, without its library (the shared notice
// in place of the bow display, the install command, Look again) and with it
// (the chip and its popup with Show in folder, never a raw path), the
// articulation list writing its parameter and showing a latched keyswitch,
// and the keyboard strip lighting the held keys.
namespace {
using oscillate::quick::InMemoryNativeSynthAdapter;
using oscillate::quick::NativeSynthDeviceController;
namespace synth = oscillate::native_dsp::synth;
namespace bow = synth::bow;
namespace panel_kit = oscillate::test::panel_kit;

using oscillate::test::require;

const oscillate::native_dsp::EngineDescriptor& table() { return *bow::engine().descriptor; }

QString qs(std::string_view text) {
  return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

constexpr int wait_ms = 2'000;
constexpr int settle_ms = 400;
constexpr float frame_notes = 5.0F;
constexpr float frame_wheel = 0.42F;
constexpr float frame_vibrato = 0.3F;
constexpr float frame_voices = 6.0F;
constexpr float frame_peak_db = -14.0F;
constexpr int c4 = 60;
constexpr int e4 = 64;
constexpr int g4 = 67;
constexpr std::size_t pizzicato = static_cast<std::size_t>(bow::Articulation::pizzicato);

std::vector<float> bow_frame(bool latched, std::vector<int> held,
                             std::size_t articulation = pizzicato, float dynamics = frame_wheel,
                             float vibrato = frame_vibrato) {
  namespace t = bow::telemetry;
  std::vector<float> frame(t::size, 0.0F);
  frame[t::library] = static_cast<float>(static_cast<int>(bow::library::State::ready));
  frame[t::notes] = frame_notes;
  frame[t::articulation] = static_cast<float>(articulation);
  frame[t::latched] = latched ? 1.0F : 0.0F;
  frame[t::dynamics] = dynamics;
  frame[t::vibrato] = vibrato;
  frame[t::voices] = frame_voices;
  frame[t::output_peak_db] = frame_peak_db;
  frame[t::held_count] = static_cast<float>(held.size());
  for (std::size_t slot = 0; slot < t::held_slots; ++slot) {
    frame[t::held + slot] = slot < held.size() ? static_cast<float>(held[slot]) : -1.0F;
  }
  return frame;
}

// ---- Controller ---------------------------------------------------------------------

// Run before any library loads in this process.
void controller_reports_loading_then_missing() {
  const auto nowhere = wren_test::scratch_directory("bow-panel-library");
  std::filesystem::remove_all(nowhere);
  ::setenv(std::string(bow::library::directory_variable).c_str(), nowhere.c_str(), 1);
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  require(controller.bow_library().value(QStringLiteral("state")).toString() ==
              QStringLiteral("loading"),
          "the first look reads in the background");
  bow::library::wait();
  const auto library = controller.bow_library();
  require(library.value(QStringLiteral("state")).toString() == QStringLiteral("missing") &&
              !library.value(QStringLiteral("message")).toString().isEmpty(),
          "a missing library is reported with a reason");
  require(library.value(QStringLiteral("install_command")).toString() ==
              qs(bow::library::install_command),
          "the panel learns how to install it");
  require(
      library.value(QStringLiteral("required")).toString().contains(qs(bow::library::attribution)),
      "and which library, with its credit");
  // Show in folder opens the nearest folder that exists.
  require(controller.library_folder(QString::fromStdString(nowhere.string())) ==
              QString::fromStdString(nowhere.parent_path().string()),
          "a library not yet installed shows its parent folder");
  const auto& prism = *synth::prism::engine().descriptor;
  InMemoryNativeSynthAdapter other(oscillate::native_dsp::default_values(prism));
  NativeSynthDeviceController prism_controller(prism, other);
  require(prism_controller.bow_library().isEmpty() && prism_controller.bow_layout().isEmpty(),
          "only Bow has its library and keyboard");
}

void controller_lays_out_the_keyboard() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto layout = controller.bow_layout();
  const auto violas = bow::playable(bow::Section::violas);
  require(layout.value(QStringLiteral("low")).toInt() == violas.low &&
              layout.value(QStringLiteral("high")).toInt() == violas.high &&
              layout.value(QStringLiteral("low_name")).toString() == QStringLiteral("C3") &&
              layout.value(QStringLiteral("high_name")).toString() == QStringLiteral("E6"),
          "the violas play C3 to E6");
  const auto switches = layout.value(QStringLiteral("keyswitches")).toList();
  require(
      switches.size() == static_cast<qsizetype>(bow::articulation_count) &&
          switches.front().toMap().value(QStringLiteral("key")).toInt() == bow::first_keyswitch &&
          switches.front().toMap().value(QStringLiteral("name")).toString() == QStringLiteral("C0"),
      "a keyswitch per articulation from C0");
  require(layout.value(QStringLiteral("dynamics")).toList().size() ==
              static_cast<qsizetype>(bow::dynamic_names.size()),
          "the wheel's dynamics, ppp to ff");
  controller.set_choice(QStringLiteral("bow.section"), static_cast<int>(bow::Section::basses));
  layout = controller.bow_layout();
  require(layout.value(QStringLiteral("low")).toInt() == bow::playable(bow::Section::basses).low,
          "the range follows the section");
}

void controller_decodes_bow_telemetry() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  adapter.set_telemetry(bow_frame(true, {c4, e4, g4}));
  const auto live = controller.telemetry();
  require(live.value(QStringLiteral("live")).toBool() &&
              live.value(QStringLiteral("library")).toString() == QStringLiteral("ready") &&
              live.value(QStringLiteral("notes")).toInt() == static_cast<int>(frame_notes),
          "the library and note count decode");
  require(live.value(QStringLiteral("articulation")).toInt() == static_cast<int>(pizzicato) &&
              live.value(QStringLiteral("latched")).toBool(),
          "the articulation playing and its latch decode");
  require(live.value(QStringLiteral("dynamic_name")).toString() == QStringLiteral("mp") &&
              live.value(QStringLiteral("voices")).toInt() == static_cast<int>(frame_voices),
          "the wheel reads as a dynamic, and the voices count");
  const auto held = live.value(QStringLiteral("held")).toList();
  require(held.size() == 3 && held[0].toInt() == c4 && held[2].toInt() == g4,
          "the held keys decode");
}

// ---- Panel ------------------------------------------------------------------------------

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
      import "qrc:/bow"
      BowPanel {}
    )",
                    QUrl(QStringLiteral("qrc:/BowPanelTest.qml")));
  if (!component.isReady()) {
    throw std::runtime_error("Bow panel failed to load: " + component.errorString().toStdString());
  }
  QObject* object = component.createWithInitialProperties(
      {{QStringLiteral("controller"), QVariant::fromValue<QObject*>(&controller)}});
  require(object != nullptr, "Bow panel was not created: " + component.errorString().toStdString());
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

// With BOW_PANEL_SCREENSHOT_DIR set, saves the panel there as `name`.
constexpr const char* screenshot_variable = "BOW_PANEL_SCREENSHOT_DIR";
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

// Without its library the bow display gives way to the shared notice; once
// installed, Look again finds it, the chip says so and its popup credits the
// library with Show in folder, never a raw path.
void the_notice_guides_the_install() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(!item(*panel, QStringLiteral("bowDisplay"))->isVisible(), "no bow without strings");
  require(text_of(*panel, QStringLiteral("bowLibraryState")) == QStringLiteral("NOT INSTALLED"),
          "the notice says it is not installed");
  require(text_of(*panel, QStringLiteral("bowInstallCommand")) == qs(bow::library::install_command),
          "the install command is shown exactly");
  require(
      text_of(*panel, QStringLiteral("bowLibraryMessage")).contains(qs(bow::library::attribution)),
      "the message names the library and its credit");
  require(text_of(*panel, QStringLiteral("bowLibraryChipText")) ==
              QStringLiteral("Library missing"),
          "the chip says it is missing");
  click(*panel, centre(item(*panel, QStringLiteral("bowCopyInstall"))));
  require(controller.clipboard_text() == qs(bow::library::install_command),
          "Copy puts the command on the clipboard");
  shoot_if_requested(*panel, QStringLiteral("bow_panel_missing_library.png"));

  const auto directory =
      std::filesystem::path(std::getenv(std::string(bow::library::directory_variable).c_str()));
  bow_test::write_library(directory);
  click(*panel, centre(item(*panel, QStringLiteral("bowLibraryReload"))));
  require(QTest::qWaitFor([&] { return item(*panel, QStringLiteral("bowDisplay"))->isVisible(); },
                          wait_ms),
          "Look again finds the installed library");
  require(text_of(*panel, QStringLiteral("bowLibraryChipText")).startsWith("Library ready"),
          "the chip says it is ready, with its size");
  click(*panel, centre(item(*panel, QStringLiteral("bowLibraryChip"))));
  require(
      QTest::qWaitFor([&] { return item(*panel, QStringLiteral("bowLibraryPopup"))->isVisible(); },
                      wait_ms),
      "the chip opens the library");
  require(!panel_kit::shows_text(panel->root, QString::fromStdString(directory.string())),
          "no raw library path is printed");
  auto* show =
      find_item(item(*panel, QStringLiteral("bowLibraryPopup")), QStringLiteral("bowShowLibrary"));
  require(show != nullptr && show->isVisible(), "Show in folder stands in for the path");
}

// Runs after the notice's test installed the library. With
// BOW_PANEL_SCREENSHOT_LIBRARY set, the panel reads the installed one (the
// chip's size in the screenshot).
void articulations_write_and_show_the_keyswitch() {
  if (const char* real = std::getenv("BOW_PANEL_SCREENSHOT_LIBRARY");
      real != nullptr && *real != '\0') {
    const auto status = bow::library::load(real);
    require(status.state == bow::library::State::ready,
            "the screenshot library loads: " + status.message);
  }
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(text_of(*panel, QStringLiteral("bowKeyswitch0")) == QStringLiteral("C0"),
          "each articulation shows its keyswitch");
  click(*panel,
        centre(item(*panel, QStringLiteral("bowArticulation") +
                                QString::number(static_cast<int>(bow::Articulation::tremolo)))));
  require(controller.choice(QStringLiteral("bow.articulation")) ==
              static_cast<int>(bow::Articulation::tremolo),
          "a row picks its articulation");
  adapter.set_telemetry(bow_frame(true, {c4, e4, g4}));
  require(
      QTest::qWaitFor(
          [&] { return text_of(*panel, QStringLiteral("bowLatched")).startsWith("Latched by"); },
          wait_ms),
      "a latched keyswitch is shown");
  require(item(*panel, QStringLiteral("bowKey") + QString::number(c4))->property("color") ==
              item(*panel, QStringLiteral("bowArticulation") + QString::number(pizzicato))
                  ->property("color"),
          "held keys and the playing articulation share the accent");
  require(text_of(*panel, QStringLiteral("bowVoices")) == QStringLiteral("3 notes · 6 voices"),
          "the strip counts notes and voices");
  require(text_of(*panel, QStringLiteral("bowRange")).contains(QStringLiteral("C3–E6")),
          "the strip names the playable range");
}

// With BOW_PANEL_SCREENSHOT_DIR set: the panel as the mockup shows it, a
// three-note chord held under Legato while the wheel swells from p to mf
// and back, with the vibrato blooming under it.
void screenshots_if_requested() {
  if (const char* directory = std::getenv(screenshot_variable);
      directory == nullptr || *directory == '\0') {
    return;
  }
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  constexpr int frames = 160;
  constexpr int poll_ms = 40;
  constexpr double floor = 0.18;
  constexpr double swell = 0.55;
  constexpr double stretch = 1.15;
  constexpr double shape = 1.6;
  constexpr double vibrato_floor = 0.15;
  constexpr double vibrato_depth = 0.45;
  constexpr double vibrato_start = 0.2;
  constexpr double vibrato_stretch = 1.2;
  constexpr double pi = std::numbers::pi;
  constexpr int a3 = 57;
  for (int i = 0; i < frames; ++i) {
    const double t = static_cast<double>(i) / frames;
    const double dynamics =
        floor + swell * std::pow(std::sin(pi * std::min(1.0, t * stretch)), shape);
    const double vibrato =
        vibrato_floor +
        vibrato_depth * std::max(0.0, std::sin(pi * (t - vibrato_start) * vibrato_stretch));
    adapter.set_telemetry(bow_frame(false, {a3, c4, e4},
                                    static_cast<std::size_t>(bow::Articulation::legato),
                                    static_cast<float>(dynamics), static_cast<float>(vibrato)));
    QTest::qWait(poll_ms);
  }
  shoot_if_requested(*panel, QStringLiteral("bow.png"));
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  try {
    controller_reports_loading_then_missing();
    controller_lays_out_the_keyboard();
    controller_decodes_bow_telemetry();
    the_notice_guides_the_install();
    articulations_write_and_show_the_keyswitch();
    screenshots_if_requested();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "bow panel contract passed\n";
  return 0;
}
