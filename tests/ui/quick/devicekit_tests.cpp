#include "in_memory_native_synth_adapter.hpp"
#include "native_synth_device_controller.hpp"

#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/effects/trim.hpp"
#include "oscillate/native_dsp/synth/parameters.hpp"
#include "oscillate/test/check.hpp"

#include <QAccessible>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QTest>
#include <QUrl>
#include <QVariant>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// The device-panel toolkit (qml/devicekit): every widget that edits a
// parameter makes exactly one undo step per action. A knob drag is one
// begin, its updates and one commit; a click, key or wheel step on any
// widget is one single edit. Each widget also speaks its role, and a theme
// prefixes its widgets' object names. Driven on the Trim table with a bare
// KitTheme.
namespace {
using oscillate::quick::InMemoryNativeSynthAdapter;
using oscillate::quick::NativeSynthDeviceController;
using oscillate::quick::NativeSynthGesture;
namespace synth = oscillate::native_dsp::synth;
namespace trim = oscillate::native_dsp::effects::trim;

using oscillate::test::require;

const synth::EngineDescriptor& table() { return *trim::engine().descriptor; }

struct Harness {
  QQmlEngine engine;
  std::unique_ptr<QQuickWindow> window = std::make_unique<QQuickWindow>();
  QQuickItem* root = nullptr;
};

std::unique_ptr<Harness> load(NativeSynthDeviceController& controller) {
  auto harness = std::make_unique<Harness>();
  QQmlComponent component(&harness->engine);
  component.setData(R"(
      import QtQuick
      import "qrc:/devicekit" as Kit
      Item {
          id: root
          required property var controller
          property Kit.KitTheme theme: Kit.KitTheme { prefix: "kit" }
          width: 640
          height: 520
          function roleOf(item) { return item.Accessible.role }
          Column {
              x: 20
              y: 20
              spacing: 14
              Kit.Knob { controller: root.controller; theme: root.theme; parameterId: "trim.gain" }
              Kit.Segment {
                  width: 360
                  controller: root.controller
                  theme: root.theme
                  parameterId: "trim.law"
              }
              Row {
                  spacing: 12
                  Kit.Toggle { controller: root.controller; theme: root.theme; parameterId: "trim.mono" }
                  Kit.Switch { controller: root.controller; theme: root.theme; parameterId: "trim.swap" }
                  Kit.Dropdown {
                      controller: root.controller
                      theme: root.theme
                      parameterId: "trim.solo"
                      menu: menu
                  }
                  Kit.Stepper { controller: root.controller; theme: root.theme; parameterId: "trim.width" }
              }
              Row {
                  spacing: 12
                  Kit.PresetArrow {
                      objectName: "kitPresetNext"
                      controller: root.controller
                      theme: root.theme
                      direction: 1
                  }
                  Kit.StatusBadge { objectName: "kitStatus"; controller: root.controller; theme: root.theme }
              }
          }
          Kit.Menu { id: menu; anchors.fill: parent; theme: root.theme }
      }
    )",
                    QUrl(QStringLiteral("qrc:/DeviceKitTest.qml")));
  require(component.isReady(), "kit harness loads: " + component.errorString().toStdString());
  QObject* object = component.createWithInitialProperties(
      {{QStringLiteral("controller"), QVariant::fromValue<QObject*>(&controller)}});
  require(object != nullptr, "kit harness is created: " + component.errorString().toStdString());
  harness->root = qobject_cast<QQuickItem*>(object);
  harness->window->resize(640, 520);
  harness->root->setParentItem(harness->window->contentItem());
  harness->window->show();
  require(QTest::qWaitForWindowExposed(harness->window.get()), "window is exposed");
  QCoreApplication::processEvents();
  return harness;
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

QQuickItem* item(Harness& harness, const char* name) {
  auto* found = find_item(harness.root, QString::fromLatin1(name));
  require(found != nullptr, std::string("missing ") + name);
  return found;
}

QPoint at(QQuickItem* target, double x, double y) {
  return target->mapToScene(QPointF(x, y)).toPoint();
}

QPoint centre(QQuickItem* target) { return at(target, target->width() / 2, target->height() / 2); }

int role_of(Harness& harness, QQuickItem* target) {
  QVariant role;
  QMetaObject::invokeMethod(harness.root, "roleOf", Q_RETURN_ARG(QVariant, role),
                            Q_ARG(QVariant, QVariant::fromValue<QObject*>(target)));
  return role.toInt();
}

// The submissions since `from`, which must be exactly the gestures given.
void expect(const InMemoryNativeSynthAdapter& adapter, std::size_t& from,
            const std::vector<NativeSynthGesture>& gestures, std::string_view what) {
  const auto& all = adapter.submissions();
  require(all.size() == from + gestures.size(),
          std::string(what) + ": " + std::to_string(all.size() - from) + " submissions, expected " +
              std::to_string(gestures.size()));
  for (std::size_t i = 0; i < gestures.size(); ++i) {
    require(all[from + i].gesture == gestures[i],
            std::string(what) + ": gesture " + std::to_string(i));
  }
  from = all.size();
}

void each_action_is_one_undo_step() {
  InMemoryNativeSynthAdapter adapter(synth::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto harness = load(controller);
  auto& window = *harness->window;
  std::size_t seen = adapter.submissions().size();

  // A knob drag: begin, live updates, one commit.
  auto* knob = item(*harness, "kitKnob:trim.gain");
  const QPoint grip = at(knob, knob->width() / 2, 20);
  QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, grip);
  for (int step = 1; step <= 4; ++step) {
    QTest::mouseMove(&window, grip - QPoint(0, 8 * step));
  }
  QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, grip - QPoint(0, 32));
  const auto& drag = adapter.submissions();
  require(drag.size() > seen + 2 && drag[seen].gesture == NativeSynthGesture::begin &&
              drag.back().gesture == NativeSynthGesture::commit,
          "a knob drag is begin, updates, commit");
  for (std::size_t i = seen + 1; i + 1 < drag.size(); ++i) {
    require(drag[i].gesture == NativeSynthGesture::update, "a drag only updates in between");
  }
  seen = drag.size();
  // The keyboard and Delete: one step each.
  knob->forceActiveFocus();
  QTest::keyClick(&window, Qt::Key_Up);
  expect(adapter, seen, {NativeSynthGesture::single}, "knob arrow key");
  QTest::keyClick(&window, Qt::Key_Delete);
  expect(adapter, seen, {NativeSynthGesture::single}, "knob reset");

  // A tile other than the chosen one, and not the first, so Left can step.
  auto* segment = item(*harness, "kitSegment:trim.law");
  const int count = static_cast<int>(segment->property("choices").toList().size());
  const int current = segment->property("current").toInt();
  const int target = current == count - 1 ? count - 2 : count - 1;
  require(count >= 3 && target >= 1, "the pan law has choices to pick");
  QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier,
                    at(segment, segment->width() * (target + 0.5) / count, segment->height() / 2));
  expect(adapter, seen, {NativeSynthGesture::single}, "segment click");
  QTest::keyClick(&window, Qt::Key_Left);
  expect(adapter, seen, {NativeSynthGesture::single}, "segment key");

  auto* toggle = item(*harness, "kitToggle:trim.mono");
  QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, centre(toggle));
  expect(adapter, seen, {NativeSynthGesture::single}, "toggle click");
  QTest::keyClick(&window, Qt::Key_Space);
  expect(adapter, seen, {NativeSynthGesture::single}, "toggle key");

  auto* swap = item(*harness, "kitToggle:trim.swap");
  QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, centre(swap));
  expect(adapter, seen, {NativeSynthGesture::single}, "switch click");

  auto* dropdown = item(*harness, "kitDropdown:trim.solo");
  QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, centre(dropdown));
  require(item(*harness, "kitMenu")->isVisible(), "a dropdown opens the menu");
  expect(adapter, seen, {}, "opening the menu");
  QTest::keyClick(&window, Qt::Key_Down);
  QTest::keyClick(&window, Qt::Key_Return);
  expect(adapter, seen, {NativeSynthGesture::single}, "menu pick");
  require(!item(*harness, "kitMenu")->isVisible(), "the menu closes on a pick");

  auto* up = item(*harness, "kitStepperUp");
  QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, centre(up));
  expect(adapter, seen, {NativeSynthGesture::single}, "stepper arrow");

  QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier,
                    centre(item(*harness, "kitPresetNext")));
  expect(adapter, seen, {NativeSynthGesture::single}, "preset arrow");

  // Roles, as assistive technology hears them.
  require(role_of(*harness, knob) == QAccessible::Dial, "a knob is a dial");
  require(role_of(*harness, segment) == QAccessible::ComboBox, "a segment is a combo box");
  require(role_of(*harness, toggle) == QAccessible::CheckBox, "a toggle is a check box");
  require(role_of(*harness, swap) == QAccessible::CheckBox, "a switch is a check box");
  require(role_of(*harness, dropdown) == QAccessible::ComboBox, "a dropdown is a combo box");
  require(role_of(*harness, item(*harness, "kitStepper:trim.width")) == QAccessible::SpinBox,
          "a stepper is a spin box");
  require(role_of(*harness, item(*harness, "kitPresetNext")) == QAccessible::Button,
          "a preset arrow is a button");
  require(role_of(*harness, item(*harness, "kitStatus")) == QAccessible::StaticText,
          "the status is static text");
}

void read_only_panels_edit_nothing() {
  InMemoryNativeSynthAdapter adapter(synth::default_values(table()));
  oscillate::quick::NativeSynthPanelStatus status;
  status.editable = false;
  status.message = "Locked";
  adapter.set_status(status);
  NativeSynthDeviceController controller(table(), adapter);
  auto harness = load(controller);
  auto& window = *harness->window;
  const std::size_t before = adapter.submissions().size();
  for (const char* name :
       {"kitKnob:trim.gain", "kitSegment:trim.law", "kitToggle:trim.mono", "kitToggle:trim.swap",
        "kitDropdown:trim.solo", "kitStepperUp", "kitPresetNext"}) {
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, centre(item(*harness, name)));
  }
  require(adapter.submissions().size() == before, "nothing edits a read-only panel");
  require(!item(*harness, "kitMenu")->isVisible(), "no menu opens on a read-only panel");
}

// Every theme's text reads against the surface it is set on: ink, inkDim
// and inkFaint each reach WCAG's 4.5:1 contrast for body text against the
// theme's inkGround. Every *Theme.qml under the QML tree is checked, so a
// new panel theme is covered as soon as it exists.
constexpr double minimum_text_contrast = 4.5;
// QColor's 8-bit alpha of a fully opaque colour.
constexpr int opaque_alpha = 255;
// WCAG 2 relative luminance: the sRGB transfer function's linear segment,
// its curve, the channel weights and the flare term in the ratio.
constexpr double srgb_linear_limit = 0.03928;
constexpr double srgb_linear_slope = 12.92;
constexpr double srgb_curve_offset = 0.055;
constexpr double srgb_curve_scale = 1.055;
constexpr double srgb_curve_exponent = 2.4;
constexpr double luminance_red = 0.2126;
constexpr double luminance_green = 0.7152;
constexpr double luminance_blue = 0.0722;
constexpr double contrast_flare = 0.05;

double linear_channel(double value) {
  return value <= srgb_linear_limit
             ? value / srgb_linear_slope
             : std::pow((value + srgb_curve_offset) / srgb_curve_scale, srgb_curve_exponent);
}

double relative_luminance(const QColor& colour) {
  return luminance_red * linear_channel(colour.redF()) +
         luminance_green * linear_channel(colour.greenF()) +
         luminance_blue * linear_channel(colour.blueF());
}

double contrast(const QColor& first, const QColor& second) {
  const double a = relative_luminance(first);
  const double b = relative_luminance(second);
  return (std::max(a, b) + contrast_flare) / (std::min(a, b) + contrast_flare);
}

bool is_singleton(const QString& path) {
  QFile file(path);
  require(file.open(QIODevice::ReadOnly), "theme opens: " + path.toStdString());
  // The pragma at the start of a line, not a mention in a comment.
  static const QRegularExpression pragma(QStringLiteral("^\\s*pragma\\s+Singleton\\b"),
                                         QRegularExpression::MultilineOption);
  return pragma.match(QString::fromUtf8(file.readAll())).hasMatch();
}

void every_theme_reads_at_body_text_contrast() {
  const QString root = QStringLiteral(OSCILLATE_QUICK_QML_DIR);
  QQmlEngine engine;
  bool saw_kit = false;
  QDirIterator themes(root, {QStringLiteral("*Theme.qml")}, QDir::Files,
                      QDirIterator::Subdirectories);
  while (themes.hasNext()) {
    const QString path = themes.next();
    // The shell's singleton is one shared ShellTheme, checked as itself.
    if (is_singleton(path)) {
      continue;
    }
    const std::string name = QDir(root).relativeFilePath(path).toStdString();
    QQmlComponent component(&engine, QUrl::fromLocalFile(path));
    std::unique_ptr<QObject> theme(component.create());
    require(theme != nullptr, name + " loads: " + component.errorString().toStdString());
    const auto ground = theme->property("inkGround").value<QColor>();
    require(ground.isValid() && ground.alpha() == opaque_alpha,
            name + ": inkGround is an opaque colour");
    for (const char* token : {"ink", "inkDim", "inkFaint"}) {
      const auto ink = theme->property(token).value<QColor>();
      require(ink.isValid() && ink.alpha() == opaque_alpha,
              name + ": " + token + " is an opaque colour");
      const double ratio = contrast(ink, ground);
      require(ratio >= minimum_text_contrast, name + ": " + token + " " + ink.name().toStdString() +
                                                  " on " + ground.name().toStdString() +
                                                  " reads at " + std::to_string(ratio) + ":1");
    }
    saw_kit = saw_kit || path.endsWith(QStringLiteral("/devicekit/KitTheme.qml"));
  }
  require(saw_kit, "the kit's own theme is among those checked");
}

// Every segmented choice in every theme shows its chosen option as a fill
// and an ink of its own: the chosen cell is filled with the accent and set
// in the inverse ink, so no theme leaves selection to an LED dot or a
// tinted wash. Segment (horizontal and vertical), SegmentedControl and
// ChoiceTiles (joined buttons and name-only tiles) each name the colours
// they draw a cell with as selectionFill and selectionInk.
constexpr double minimum_selection_contrast = 3.0;
// The harness's SegmentedControl holds this index; the parameter controls
// hold the pan law's default.
constexpr int segmented_current = 1;
const char* const selection_harness = R"(
    import QtQuick
    import "."
    Item {
        id: root
        required property var controller
        required property KitTheme theme
        width: 720
        height: 520
        Column {
            spacing: 12
            Segment {
                objectName: "selectionSegment"
                width: 360
                controller: root.controller
                theme: root.theme
                parameterId: "trim.law"
            }
            Segment {
                objectName: "selectionSegmentVertical"
                width: 200
                height: 120
                vertical: true
                controller: root.controller
                theme: root.theme
                parameterId: "trim.law"
            }
            SegmentedControl {
                objectName: "selectionSegmented"
                theme: root.theme
                labels: ["One", "Two", "Three"]
                currentIndex: 1
            }
            ChoiceTiles {
                objectName: "selectionChoiceSegment"
                width: 360
                segment: true
                controller: root.controller
                theme: root.theme
                parameterId: "trim.law"
            }
            ChoiceTiles {
                objectName: "selectionChoiceTiles"
                width: 360
                controller: root.controller
                theme: root.theme
                parameterId: "trim.law"
            }
        }
    }
)";

void cells_in(QQuickItem* root, std::vector<QQuickItem*>& cells) {
  if (root->property("selectionFill").isValid()) {
    cells.push_back(root);
  }
  for (QQuickItem* child : root->childItems()) {
    cells_in(child, cells);
  }
}

void every_segmented_choice_fills_its_selection() {
  const QString root = QStringLiteral(OSCILLATE_QUICK_QML_DIR);
  const auto law = synth::find_parameter(table(), "trim.law");
  if (!law) {
    throw std::runtime_error("Trim has no pan law");
  }
  const double law_default = table().parameters[*law].default_value;
  InMemoryNativeSynthAdapter adapter(synth::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  QQmlEngine engine;
  QQmlComponent harness(&engine);
  harness.setData(selection_harness, QUrl::fromLocalFile(root + "/devicekit/SelectionHarness.qml"));
  require(harness.isReady(), "selection harness loads: " + harness.errorString().toStdString());
  int themes_checked = 0;
  QDirIterator themes(root, {QStringLiteral("*Theme.qml")}, QDir::Files,
                      QDirIterator::Subdirectories);
  while (themes.hasNext()) {
    const QString path = themes.next();
    if (is_singleton(path)) {
      continue;
    }
    const std::string name = QDir(root).relativeFilePath(path).toStdString();
    QQmlComponent component(&engine, QUrl::fromLocalFile(path));
    std::unique_ptr<QObject> theme(component.create());
    require(theme != nullptr, name + " loads: " + component.errorString().toStdString());
    std::unique_ptr<QObject> object(harness.createWithInitialProperties(
        {{QStringLiteral("controller"), QVariant::fromValue<QObject*>(&controller)},
         {QStringLiteral("theme"), QVariant::fromValue<QObject*>(theme.get())}}));
    require(object != nullptr,
            name + ": the harness is created: " + harness.errorString().toStdString());
    auto* item = qobject_cast<QQuickItem*>(object.get());
    QCoreApplication::processEvents();
    for (const char* control_name :
         {"selectionSegment", "selectionSegmentVertical", "selectionSegmented",
          "selectionChoiceSegment", "selectionChoiceTiles"}) {
      auto* control = find_item(item, QString::fromLatin1(control_name));
      require(control != nullptr, name + ": " + control_name + " exists");
      std::vector<QQuickItem*> cells;
      cells_in(control, cells);
      const auto chosen = std::ranges::count_if(
          cells, [](QQuickItem* cell) { return cell->property("on").toBool(); });
      require(cells.size() >= 2 && chosen == 1,
              name + ": " + control_name + " has one chosen cell among several");
      const auto on = *std::ranges::find_if(
          cells, [](QQuickItem* cell) { return cell->property("on").toBool(); });
      // The chosen cell is the one the control holds: the pan law's default
      // or the segmented control's current index.
      const int expected = std::string_view{control_name} == "selectionSegmented"
                               ? segmented_current
                               : static_cast<int>(law_default);
      require(on->property("index").toInt() == expected,
              name + ": " + control_name + " lights the cell it holds");
      const auto on_fill = on->property("selectionFill").value<QColor>();
      const auto on_ink = on->property("selectionInk").value<QColor>();
      require(on_fill.alpha() == opaque_alpha,
              name + ": " + control_name + "'s chosen cell is filled, not washed");
      const double ratio = contrast(on_ink, on_fill);
      require(ratio >= minimum_selection_contrast, name + ": " + control_name + "'s chosen text " +
                                                       on_ink.name().toStdString() + " on " +
                                                       on_fill.name().toStdString() + " reads at " +
                                                       std::to_string(ratio) + ":1");
      for (QQuickItem* cell : cells) {
        if (cell == on) {
          continue;
        }
        const auto fill = cell->property("selectionFill").value<QColor>();
        const auto ink = cell->property("selectionInk").value<QColor>();
        require(fill.rgba() != on_fill.rgba(), name + ": " + control_name +
                                                   " fills its chosen cell unlike the others (" +
                                                   fill.name(QColor::HexArgb).toStdString() + ")");
        require(ink.rgba() != on_ink.rgba(), name + ": " + control_name +
                                                 " sets its chosen text unlike the others (" +
                                                 ink.name(QColor::HexArgb).toStdString() + ")");
      }
    }
    ++themes_checked;
  }
  require(themes_checked > 1, "the themes under the QML tree are checked");
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  try {
    each_action_is_one_undo_step();
    read_only_panels_edit_nothing();
    every_theme_reads_at_body_text_contrast();
    every_segmented_choice_fills_its_selection();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "device kit contract passed\n";
  return 0;
}
