#include "in_memory_native_synth_adapter.hpp"
#include "native_synth_device_controller.hpp"
#include "panel_kit_checks.hpp"

#include "../../native_dsp/stick_test_library.hpp"
#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/native_dsp/synth/prism.hpp"
#include "oscillate/native_dsp/synth/stick.hpp"
#include "oscillate/test/check.hpp"

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
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// The Stick panel (ADR-0114): the controller's kit library (loading,
// missing with the kit's own install command, ready), its layout of mics
// and pieces and its telemetry decode; and the QML panel, without its kit
// (the shared notice in place of the mixer, the install command, Look
// again) and with it (a strip per mic group, the chip and its popup with
// Show in folder, never a raw path), a piece picked showing its own
// controls, a mute writing its parameter, and the meters and the kit
// lighting from telemetry.
namespace {
using oscillate::quick::InMemoryNativeSynthAdapter;
using oscillate::quick::NativeSynthDeviceController;
namespace synth = oscillate::native_dsp::synth;
namespace stick = synth::stick;
namespace panel_kit = oscillate::test::panel_kit;

using oscillate::test::require;

const oscillate::native_dsp::EngineDescriptor& table() { return *stick::engine().descriptor; }

QString qs(std::string_view text) {
  return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

constexpr int wait_ms = 4'000;
constexpr float frame_hits = 7.0F;
constexpr float frame_voices = 5.0F;
constexpr float frame_peak_db = -9.0F;
constexpr float frame_pedal = 0.3F;
constexpr double pedal_tolerance = 1e-6;
constexpr float loud_db = -6.0F;
constexpr float quiet_db = -40.0F;
constexpr float silent_db = -120.0F;
constexpr float sounding = 0.8F;
constexpr std::size_t snare = static_cast<std::size_t>(stick::Piece::snare);
constexpr std::size_t kick = static_cast<std::size_t>(stick::Piece::kick);
constexpr std::size_t snare_top = static_cast<std::size_t>(stick::MicGroup::snare_top);
constexpr std::size_t overheads = static_cast<std::size_t>(stick::MicGroup::overheads);
// The synthetic kit's snare: sticks, a second snare, a side-stick and brushes.
constexpr int snare_hits = 40;

// A frame with the snare struck: its piece lit, its mics loud.
std::vector<float> snare_frame() {
  namespace t = stick::telemetry;
  std::vector<float> frame(t::size, 0.0F);
  frame[t::library] = static_cast<float>(static_cast<int>(stick::library::State::ready));
  frame[t::kit] = static_cast<float>(stick::Kit::naked);
  frame[t::hits] = frame_hits;
  frame[t::voices] = frame_voices;
  frame[t::output_peak_db] = frame_peak_db;
  frame[t::pedal] = frame_pedal;
  frame[t::last_piece] = static_cast<float>(snare);
  frame[t::piece_level + snare] = sounding;
  for (std::size_t g = 0; g < stick::mic_group_count; ++g) {
    frame[t::mic_db + g] = g == snare_top ? loud_db : g == overheads ? quiet_db : silent_db;
  }
  return frame;
}

// The folder the kits are looked for in, set before anything loads.
std::filesystem::path kits_folder() {
  static const auto folder = [] {
    auto path = wren_test::scratch_directory("stick-panel-kits");
    std::filesystem::remove_all(path);
    ::setenv(std::string(stick::library::directory_variable).c_str(), path.c_str(), 1);
    return path;
  }();
  return folder;
}

// ---- Controller ---------------------------------------------------------------------

// Run before any kit loads in this process.
void controller_reports_loading_then_missing() {
  const auto nowhere = kits_folder();
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  require(controller.stick_library().value(QStringLiteral("state")).toString() ==
              QStringLiteral("loading"),
          "the first look reads in the background");
  stick::library::wait();
  const auto library = controller.stick_library();
  require(library.value(QStringLiteral("state")).toString() == QStringLiteral("missing") &&
              !library.value(QStringLiteral("message")).toString().isEmpty(),
          "a missing kit is reported with a reason");
  require(library.value(QStringLiteral("install_command")).toString() ==
              qs(stick::library::install_command) + QStringLiteral(" --kit naked"),
          "the panel learns how to install the chosen kit");
  require(library.value(QStringLiteral("required"))
              .toString()
              .contains(qs(stick::library::facts(stick::Kit::naked).attribution)),
          "and which kit, with its credit");
  const auto kits = library.value(QStringLiteral("kits")).toList();
  require(kits.size() == static_cast<qsizetype>(stick::kit_count) &&
              !kits.front().toMap().value(QStringLiteral("installed")).toBool(),
          "each kit says whether it is installed");
  require(controller.library_folder(QString::fromStdString(
              (nowhere / stick::library::facts(stick::Kit::naked).folder).string())) ==
              QString::fromStdString(nowhere.parent_path().string()),
          "a kit not yet installed shows its nearest folder");
  const auto& prism = *synth::prism::engine().descriptor;
  InMemoryNativeSynthAdapter other(oscillate::native_dsp::default_values(prism));
  NativeSynthDeviceController prism_controller(prism, other);
  require(prism_controller.stick_library().isEmpty() && prism_controller.stick_layout().isEmpty(),
          "only Stick has its kits");
}

void controller_lays_out_the_kit() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto layout = controller.stick_layout();
  auto pieces = layout.value(QStringLiteral("pieces")).toList();
  require(!layout.value(QStringLiteral("loaded")).toBool() &&
              layout.value(QStringLiteral("toms")).toInt() == static_cast<int>(stick::max_toms),
          "before it loads, the kit's facts: Naked Drums has five toms");
  const auto present = [&](stick::Piece piece) {
    return pieces[static_cast<qsizetype>(piece)].toMap().value(QStringLiteral("present")).toBool();
  };
  require(pieces.size() == static_cast<qsizetype>(stick::piece_count) &&
              present(stick::Piece::splash) && present(stick::Piece::china),
          "a splash and a china");
  require(pieces[static_cast<qsizetype>(kick)].toMap().value(QStringLiteral("keys")).toString() ==
              QStringLiteral("B1, C2"),
          "the kick plays B1 and C2 (35 and 36)");
  controller.set_choice(QStringLiteral("stick.kit"), static_cast<int>(stick::Kit::drs));
  layout = controller.stick_layout();
  pieces = layout.value(QStringLiteral("pieces")).toList();
  constexpr int drs_toms = 3;
  require(layout.value(QStringLiteral("toms")).toInt() == drs_toms && !present(stick::Piece::china),
          "the layout follows the kit: DRS Studio has three toms and no china");
}

void controller_decodes_stick_telemetry() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  adapter.set_telemetry(snare_frame());
  const auto live = controller.telemetry();
  require(live.value(QStringLiteral("live")).toBool() &&
              live.value(QStringLiteral("library")).toString() == QStringLiteral("ready") &&
              live.value(QStringLiteral("hits")).toInt() == static_cast<int>(frame_hits) &&
              live.value(QStringLiteral("voices")).toInt() == static_cast<int>(frame_voices),
          "the library, hits and voices decode");
  require(live.value(QStringLiteral("last_piece")).toInt() == static_cast<int>(snare) &&
              std::fabs(live.value(QStringLiteral("pedal")).toDouble() - frame_pedal) <
                  pedal_tolerance,
          "the last piece and the pedal decode");
  const auto levels = live.value(QStringLiteral("piece_levels")).toList();
  const auto meters = live.value(QStringLiteral("mic_db")).toList();
  require(levels.size() == static_cast<qsizetype>(stick::piece_count) &&
              levels[static_cast<qsizetype>(snare)].toDouble() > 0.0,
          "each piece's level decodes");
  require(meters.size() == static_cast<qsizetype>(stick::mic_group_count) &&
              meters[static_cast<qsizetype>(snare_top)].toDouble() > meters[0].toDouble(),
          "each mic group's meter decodes");
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
      import "qrc:/stick"
      StickPanel {}
    )",
                    QUrl(QStringLiteral("qrc:/StickPanelTest.qml")));
  if (!component.isReady()) {
    throw std::runtime_error("Stick panel failed to load: " +
                             component.errorString().toStdString());
  }
  QObject* object = component.createWithInitialProperties(
      {{QStringLiteral("controller"), QVariant::fromValue<QObject*>(&controller)}});
  require(object != nullptr,
          "Stick panel was not created: " + component.errorString().toStdString());
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

// With STICK_PANEL_SCREENSHOT_DIR set, saves the panel there as `name`.
constexpr const char* screenshot_variable = "STICK_PANEL_SCREENSHOT_DIR";
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

// Without its kit the mixer gives way to the shared notice; once installed,
// Look again finds it, the mixer shows a strip per mic group and the chip's
// popup credits the kit with Show in folder, never a raw path.
void the_notice_guides_the_install() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(text_of(*panel, QStringLiteral("stickLibraryState")) == QStringLiteral("NOT INSTALLED"),
          "the notice says it is not installed");
  require(text_of(*panel, QStringLiteral("stickInstallCommand")) ==
              qs(stick::library::install_command) + QStringLiteral(" --kit naked"),
          "the install command is shown exactly");
  require(text_of(*panel, QStringLiteral("stickLibraryChipText")) ==
              QStringLiteral("Naked Drums not installed"),
          "the chip says the kit is not installed");
  require(find_item(panel->root, QStringLiteral("stickStrip:overheads")) == nullptr,
          "no mixer without its kit");
  click(*panel, centre(item(*panel, QStringLiteral("stickCopyInstall"))));
  require(controller.clipboard_text() ==
              qs(stick::library::install_command) + QStringLiteral(" --kit naked"),
          "Copy puts the command on the clipboard");
  shoot_if_requested(*panel, QStringLiteral("stick_panel_missing_kit.png"));

  stick_test::write_kits(kits_folder());
  click(*panel, centre(item(*panel, QStringLiteral("stickLibraryReload"))));
  require(
      QTest::qWaitFor(
          [&] { return find_item(panel->root, QStringLiteral("stickStrip:overheads")); }, wait_ms),
      "Look again finds the installed kit and its mics");
  require(find_item(panel->root, QStringLiteral("stickStrip:kick_out")) == nullptr,
          "a group the kit has no mic in has no strip");
  require(text_of(*panel, QStringLiteral("stickLibraryChipText")).startsWith("Library ready"),
          "the chip says it is ready, with its size");
  click(*panel, centre(item(*panel, QStringLiteral("stickLibraryChip"))));
  require(
      QTest::qWaitFor(
          [&] { return item(*panel, QStringLiteral("stickLibraryPopup"))->isVisible(); }, wait_ms),
      "the chip opens the library");
  require(!panel_kit::shows_text(panel->root, QString::fromStdString(kits_folder().string())),
          "no raw library path is printed");
  auto* show = find_item(item(*panel, QStringLiteral("stickLibraryPopup")),
                         QStringLiteral("stickShowLibrary"));
  require(show != nullptr && show->isVisible(), "Show in folder stands in for the path");
}

// Runs after the notice's test installed the kits. A piece picked shows its
// own controls; a mute writes its parameter; telemetry lights the piece and
// the meters.
void pieces_mics_and_meters() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(find_item(panel->root, QStringLiteral("stickKnob:stick.snare.tune")) != nullptr,
          "the snare's controls show first");
  require(text_of(*panel, QStringLiteral("stickPieceFacts"))
              .contains(QString::number(snare_hits) + QStringLiteral(" hits")),
          "the piece says how many hits it has");
  require(text_of(*panel, QStringLiteral("stickPieceFacts")).contains(QStringLiteral("side-stick")),
          "and how it was played");
  click(*panel, centre(item(*panel, QStringLiteral("stickPiece") + QString::number(kick))));
  require(QTest::qWaitFor(
              [&] {
                return find_item(panel->root, QStringLiteral("stickKnob:stick.kick.tune")) !=
                       nullptr;
              },
              wait_ms),
          "clicking the kick shows its controls");
  require(text_of(*panel, QStringLiteral("stickPieceTitle")).startsWith(QStringLiteral("KICK")),
          "under its name");
  click(*panel, centre(item(*panel, QStringLiteral("stickMute:overheads"))));
  require(controller.choice(QStringLiteral("stick.mic.overheads.mute")) == 1,
          "M mutes the overheads");
  adapter.set_telemetry(snare_frame());
  auto* meter = item(*panel, QStringLiteral("stickMeter:snare_top"));
  require(QTest::qWaitFor([&] { return meter->height() > 0.0; }, wait_ms),
          "the snare's mic meters");
  require(item(*panel, QStringLiteral("stickMeter:toms"))->height() == 0.0,
          "a silent mic does not");
  require(text_of(*panel, QStringLiteral("stickVoices")) ==
              QString::number(static_cast<int>(frame_voices)) + QStringLiteral(" voices"),
          "the kit row counts the voices");
}

// A groove's frame for the screenshot: kick, snare and hat sounding, every
// mic metering.
std::vector<float> groove_frame() {
  namespace t = stick::telemetry;
  constexpr std::array<float, stick::mic_group_count> mic_db{-10.0F, -14.0F, -12.0F, -6.0F, -12.0F,
                                                             -30.0F, -24.0F, -36.0F, -9.0F, -15.0F,
                                                             -18.0F, -16.0F, -30.0F};
  constexpr float kick_level = 0.6F;
  constexpr float hat_level = 0.45F;
  auto frame = snare_frame();
  frame[t::piece_level + kick] = kick_level;
  frame[t::piece_level + static_cast<std::size_t>(stick::Piece::hihat)] = hat_level;
  for (std::size_t g = 0; g < stick::mic_group_count; ++g) {
    frame[t::mic_db + g] = mic_db[g];
  }
  return frame;
}

// With STICK_PANEL_SCREENSHOT_DIR set: the panel as the mockup shows it, a
// groove sounding and the hat a little open. With STICK_PANEL_SCREENSHOT_LIBRARY
// set, the chip reads that installed kit.
void screenshots_if_requested() {
  if (const char* directory = std::getenv(screenshot_variable);
      directory == nullptr || *directory == '\0') {
    return;
  }
  if (const char* real = std::getenv("STICK_PANEL_SCREENSHOT_LIBRARY");
      real != nullptr && *real != '\0') {
    const auto status = stick::library::load(stick::Kit::naked, real);
    require(status.state == stick::library::State::ready,
            "the screenshot kit loads: " + status.message);
  }
  auto values = oscillate::native_dsp::default_values(table());
  InMemoryNativeSynthAdapter adapter(values);
  NativeSynthDeviceController controller(table(), adapter);
  constexpr double half_open = 0.3;
  controller.set_value(QStringLiteral("stick.perf.pedal"), half_open);
  auto panel = load_panel(controller);
  adapter.set_telemetry(groove_frame());
  constexpr int settle_ms = 300;
  QTest::qWait(settle_ms);
  shoot_if_requested(*panel, QStringLiteral("stick.png"));
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  try {
    controller_reports_loading_then_missing();
    controller_lays_out_the_kit();
    controller_decodes_stick_telemetry();
    the_notice_guides_the_install();
    pieces_mics_and_meters();
    screenshots_if_requested();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "stick panel contract passed\n";
  return 0;
}
