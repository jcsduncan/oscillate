#include "in_memory_native_synth_adapter.hpp"
#include "native_synth_device_controller.hpp"

#include "../../native_dsp/concert_test_library.hpp"
#include "oscillate/native_dsp/synth/concert.hpp"
#include "oscillate/native_dsp/synth/parameters.hpp"
#include "oscillate/native_dsp/synth/prism.hpp"
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
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// The Concert panel ("Concert black", ADR-0071): the controller's library
// status and telemetry decode, and the QML panel driven by the engine
// table: without its library (the message and install command), with it
// (the attribution), and its strings view live and as a preview.
namespace {
using oscillate::quick::InMemoryNativeSynthAdapter;
using oscillate::quick::NativeSynthDeviceController;
namespace synth = oscillate::native_dsp::synth;
namespace concert = synth::concert;

using oscillate::test::require;

const synth::EngineDescriptor& table() { return *concert::engine().descriptor; }

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

// ---- Controller ---------------------------------------------------------------------

// Run before any library loads in this process.
void controller_reports_a_missing_library() {
  // Where the library will be installed mid-test; removed at exit.
  const auto nowhere = concert_test::scratch_directory("concert-panel-library");
  std::filesystem::remove_all(nowhere);
  ::setenv(std::string(concert::library::directory_variable).c_str(), nowhere.c_str(), 1);
  InMemoryNativeSynthAdapter adapter(synth::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  const auto library = controller.concert_library();
  require(library.value(QStringLiteral("state")).toString() == QStringLiteral("missing") &&
              !library.value(QStringLiteral("message")).toString().isEmpty(),
          "a missing library is reported with a reason");
  require(library.value(QStringLiteral("install_command")).toString() ==
              qs(concert::library::install_command),
          "the panel learns how to install it");
  require(library.value(QStringLiteral("required"))
              .toString()
              .contains(qs(concert::library::attribution)),
          "and which library, with its attribution");
  require(library.value(QStringLiteral("key_count")).toInt() ==
                  static_cast<int>(concert::key_count) &&
              library.value(QStringLiteral("first_key")).toInt() == concert::first_key,
          "the keyboard's range reaches QML");
  const auto& prism = *synth::prism::engine().descriptor;
  InMemoryNativeSynthAdapter other(synth::default_values(prism));
  NativeSynthDeviceController prism_controller(prism, other);
  require(prism_controller.concert_library().isEmpty(), "only Concert has a library");
}

std::vector<float> concert_frame(double sustain, int ringing_key, float energy) {
  namespace t = concert::telemetry;
  std::vector<float> frame(t::size, 0.0F);
  frame[t::sustain] = static_cast<float>(sustain);
  for (std::size_t i = 0; i < concert::key_count; ++i) {
    frame[t::first_energy + i] = t::energy_floor;
    frame[t::first_damper + i] = sustain >= 0.5 ? 0.0F : 1.0F;
  }
  const auto key = static_cast<std::size_t>(ringing_key - concert::first_key);
  frame[t::first_energy + key] = energy;
  frame[t::first_damper + key] = 0.0F;
  frame[t::library] = static_cast<float>(static_cast<int>(concert::library::State::ready));
  return frame;
}

void controller_decodes_concert_telemetry() {
  InMemoryNativeSynthAdapter adapter(synth::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  require(!controller.telemetry().value(QStringLiteral("live")).toBool(), "nothing plays");
  auto frame = concert_frame(1.0, 60, -0.5F);
  frame[concert::telemetry::underruns] = 3.0F;
  adapter.set_telemetry(frame);
  const auto live = controller.telemetry();
  require(live.value(QStringLiteral("live")).toBool() &&
              live.value(QStringLiteral("sustain")).toDouble() == 1.0,
          "the pedals decode");
  const auto energies = live.value(QStringLiteral("energies")).toList();
  const auto dampers = live.value(QStringLiteral("dampers")).toList();
  const auto c4 = static_cast<qsizetype>(60 - concert::first_key);
  require(energies.size() == static_cast<qsizetype>(concert::key_count) &&
              dampers.size() == static_cast<qsizetype>(concert::key_count) &&
              std::fabs(energies[c4].toDouble() + 0.5) < 1e-6 && dampers[c4].toDouble() == 0.0,
          "every key's energy and damper decode");
  require(live.value(QStringLiteral("library")).toString() == QStringLiteral("ready") &&
              live.value(QStringLiteral("underruns")).toDouble() == 3.0,
          "the library's state and late reads decode");
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
      import "qrc:/concert"
      ConcertPanel {}
    )",
                    QUrl(QStringLiteral("qrc:/ConcertPanelTest.qml")));
  if (!component.isReady()) {
    throw std::runtime_error("Concert panel failed to load: " +
                             component.errorString().toStdString());
  }
  QObject* object = component.createWithInitialProperties(
      {{QStringLiteral("controller"), QVariant::fromValue<QObject*>(&controller)}});
  require(object != nullptr,
          "Concert panel was not created: " + component.errorString().toStdString());
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

// Without its library the panel says why, what to install and how; once
// installed, Look Again finds it and the attribution shows.
void the_library_plate_guides_the_install() {
  InMemoryNativeSynthAdapter adapter(synth::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(item(*panel, QStringLiteral("concertLibraryMessage"))->isVisible() &&
              text_of(*panel, QStringLiteral("concertLibraryMessage"))
                  .contains(qs(concert::library::attribution)),
          "the message names the library and its attribution");
  require(text_of(*panel, QStringLiteral("concertInstallCommand")) ==
              qs(concert::library::install_command),
          "the install command is shown");
  require(!item(*panel, QStringLiteral("concertAttribution"))->isVisible(),
          "no installed attribution yet");
  click(*panel, centre(item(*panel, QStringLiteral("concertCopyInstall"))));
  require(controller.clipboard_text() == qs(concert::library::install_command),
          "Copy puts the command on the clipboard");

  // Install the test library where Concert looks, then look again.
  const auto directory =
      std::filesystem::path(std::getenv(std::string(concert::library::directory_variable).c_str()));
  concert_test::write_library(directory, concert_test::resident_note_seconds);
  click(*panel, centre(item(*panel, QStringLiteral("concertLibraryReload"))));
  require(
      QTest::qWaitFor(
          [&] { return item(*panel, QStringLiteral("concertAttribution"))->isVisible(); }, 1'000),
      "Look Again finds the installed library");
  require(controller.concert_library().value(QStringLiteral("state")).toString() ==
              QStringLiteral("ready"),
          "the controller reports it ready");
  require(!text_of(*panel, QStringLiteral("concertAttribution")).isEmpty() &&
              !item(*panel, QStringLiteral("concertLibraryMessage"))->isVisible(),
          "the attribution replaces the message");
  require(text_of(*panel, QStringLiteral("concertAttribution")) ==
              controller.concert_library().value(QStringLiteral("attribution")).toString(),
          "the attribution is the library's own");
}

void panel_covers_every_parameter() {
  InMemoryNativeSynthAdapter adapter(synth::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(panel->root->implicitWidth() >= 1240 && panel->root->implicitHeight() >= 640,
          "panel has its designed size");
  for (const auto& p : table().parameters) {
    const QString id = qs(p.id);
    const bool shown = find_item(panel->root, QStringLiteral("concertKnob:") + id) != nullptr ||
                       find_item(panel->root, QStringLiteral("concertSegment:") + id) != nullptr;
    require(shown, "no control for " + id.toStdString());
  }
  require(text_of(*panel, QStringLiteral("concertPresetName")) == QStringLiteral("Concert Grand"),
          "defaults are the Concert Grand");
  require(text_of(*panel, QStringLiteral("concertAboutText"))
              .contains(qs(concert::library::attribution)),
          "the about plate credits the library");
}

void presets_and_controls() {
  InMemoryNativeSynthAdapter adapter(synth::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  auto* perspective = item(*panel, QStringLiteral("concertSegment:concert.perspective"));
  const QPoint audience =
      perspective->mapToScene(QPointF(perspective->width() / 2, perspective->height() * 0.75))
          .toPoint();
  click(*panel, audience);
  require(controller.choice(QStringLiteral("concert.perspective")) == 1,
          "a nameplate picks the perspective");
  QTest::keyClick(panel->window.get(), Qt::Key_Delete);
  require(controller.choice(QStringLiteral("concert.perspective")) == 0,
          "Delete resets the perspective");

  click(*panel, centre(item(*panel, QStringLiteral("concertPresetNext"))));
  require(controller.preset_name() == qs(table().presets[1].name), "next steps the preset");
  click(*panel, centre(item(*panel, QStringLiteral("concertPresetPrevious"))));
  require(controller.preset_name() == QStringLiteral("Concert Grand"), "previous steps back");
  auto* menu = item(*panel, QStringLiteral("concertMenu"));
  click(*panel, centre(item(*panel, QStringLiteral("concertPresetDisplay"))));
  require(menu->isVisible(), "the preset menu opens");
  QTest::keyClick(panel->window.get(), Qt::Key_Escape);
  require(!menu->isVisible(), "Escape closes it");

  auto* knob = item(*panel, QStringLiteral("concertKnob:concert.tone"));
  knob->forceActiveFocus();
  const double before = controller.value(QStringLiteral("concert.tone"));
  QTest::keyClick(panel->window.get(), Qt::Key_Up);
  require(controller.value(QStringLiteral("concert.tone")) > before, "Up brightens the tone");
  require(text_of(*panel, QStringLiteral("concertReadout")) == QStringLiteral("VOICING · TONE"),
          "the display reads back the control");

  adapter.set_status({.saved_revision = 9, .audible_revision = 9, .deferred_changes = true});
  controller.refresh();
  QCoreApplication::processEvents();
  require(item(*panel, QStringLiteral("concertStatus"))->property("tone").value<QColor>() ==
              QColor(QStringLiteral("#e0901f")),
          "deferred status is highlighted");
}

// The strings view previews the library's ring times until the synth plays,
// then shows the strings ringing, the dampers lifted and the pedals down
// (ADR-0037).
void strings_view_goes_live() {
  InMemoryNativeSynthAdapter adapter(preset("Concert Grand"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  auto* view = item(*panel, QStringLiteral("concertStringsView"));
  QTest::qWait(300);
  require(!view->property("live").toBool() &&
              text_of(*panel, QStringLiteral("concertStringsCaption"))
                  .contains(QStringLiteral("NOT LIVE")) &&
              view->property("sustain").toList().size() ==
                  static_cast<qsizetype>(concert::key_count),
          "no telemetry: the ring preview");
  require(item(*panel, QStringLiteral("concertPedal:sustain"))->property("depth").toDouble() == 0.0,
          "pedals are up in the preview");
  adapter.set_telemetry(concert_frame(1.0, 60, -1.0F));
  require(QTest::qWaitFor([&] { return view->property("live").toBool(); }, 1000),
          "live strings arrive");
  require(view->property("ringing").toInt() == 1 &&
              text_of(*panel, QStringLiteral("concertStringsCaption")) ==
                  QStringLiteral("LIVE · 1 KEY RINGING"),
          "C4 rings");
  const int damped = concert_test::last_damped_key - concert::first_key + 1;
  require(view->property("lifted").toInt() == damped,
          "the sustain pedal lifts every damper the library has");
  require(item(*panel, QStringLiteral("concertPedal:sustain"))->property("depth").toDouble() == 1.0,
          "the sustain pedal shows down");
  adapter.set_telemetry(concert_frame(0.0, 48, -2.0F));
  require(QTest::qWaitFor([&] { return view->property("lifted").toInt() == 1; }, 1000),
          "without the pedal only the held key's damper is up");
  adapter.set_telemetry({});
  require(QTest::qWaitFor([&] { return !view->property("live").toBool(); }, 1000),
          "telemetry gone: back to the preview");
}

void screenshots_if_requested() {
  const char* directory = std::getenv("CONCERT_PANEL_SCREENSHOT_DIR");
  if (directory == nullptr || *directory == '\0') {
    return;
  }
  QDir().mkpath(QString::fromLocal8Bit(directory));
  struct Shot {
    const char* preset;
    const char* touched;
    const char* file;
  };
  for (const Shot shot : {Shot{"Concert Grand", "concert.tone", "/concert_grand.png"},
                          Shot{"Felt", "concert.dynamics", "/concert_felt.png"}}) {
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
  InMemoryNativeSynthAdapter adapter(preset("Concert Grand"));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  namespace t = concert::telemetry;
  std::vector<float> frame(t::size, 0.0F);
  frame[t::sustain] = 1.0F;
  for (std::size_t i = 0; i < concert::key_count; ++i) {
    // A pedalled chord with its sympathetic halo.
    const auto key = static_cast<int>(i) + concert::first_key;
    const bool struck = key == 48 || key == 55 || key == 60 || key == 64 || key == 67;
    frame[t::first_energy + i] =
        struck ? -0.4F : static_cast<float>(-4.5 - 0.04 * std::fabs(key - 58.0));
    frame[t::first_damper + i] = 0.0F;
  }
  adapter.set_telemetry(frame);
  QTest::qWait(400);
  const QString path = QString::fromLocal8Bit(directory) + QStringLiteral("/concert_live.png");
  require(panel->window->grabWindow().save(path, "PNG"), "screenshot saved");
  std::cout << "wrote " << path.toStdString() << '\n';
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  try {
    controller_reports_a_missing_library();
    controller_decodes_concert_telemetry();
    the_library_plate_guides_the_install();
    panel_covers_every_parameter();
    presets_and_controls();
    strings_view_goes_live();
    screenshots_if_requested();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "concert panel contract passed\n";
  return 0;
}
