#include "in_memory_native_synth_adapter.hpp"
#include "native_synth_device_controller.hpp"

#include "oscillate/native_dsp/synth/nova.hpp"
#include "oscillate/native_dsp/synth/parameters.hpp"
#include "oscillate/test/check.hpp"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QJSValue>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRectF>
#include <QTest>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// Hands-on editing in the Nova panel: knobs whose captions never collide,
// typed values, Ctrl+click reset, tooltips naming a parameter in full,
// counts without a sign, the amp envelope first in the modulation strip,
// and the displays as controls: drag the oscillator to scan its position,
// the filter curve for cutoff and resonance, an envelope's nodes for its
// times and sustain. Each edit is one undo step.
namespace {
using oscillate::quick::InMemoryNativeSynthAdapter;
using oscillate::quick::NativeSynthDeviceController;
using oscillate::quick::NativeSynthGesture;
namespace synth = oscillate::native_dsp::synth;

using oscillate::test::require;

const synth::EngineDescriptor& table() { return *synth::nova::engine().descriptor; }

// ---- Harness ---------------------------------------------------------------------------------

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
      import "qrc:/nova"
      NovaPanel {}
    )",
                    QUrl(QStringLiteral("qrc:/NovaEditingTest.qml")));
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
  // Typing needs the keyboard.
  panel->window->requestActivate();
  require(QTest::qWaitForWindowActive(panel->window.get()), "panel window is active");
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

// Every visible item under `parent` whose name starts with `prefix`.
void collect(QQuickItem* parent, const QString& prefix, std::vector<QQuickItem*>& out) {
  if (parent->objectName().startsWith(prefix) && parent->isVisible()) {
    out.push_back(parent);
  }
  for (QQuickItem* child : parent->childItems()) {
    collect(child, prefix, out);
  }
}

QRectF scene_rect(QQuickItem* target) {
  return target->mapRectToScene(QRectF(0, 0, target->width(), target->height()));
}

// The rectangle a centred, single-line Text paints.
QRectF painted_text(QQuickItem* text) {
  const QRectF box = scene_rect(text);
  const double painted = std::min(box.width(), text->property("contentWidth").toDouble());
  return {box.center().x() - painted / 2, box.y(), painted, box.height()};
}

QPoint centre(QQuickItem* target) {
  return target->mapToScene(QPointF(target->width() / 2, target->height() / 2)).toPoint();
}

QPoint dial_centre(QQuickItem* knob) {
  const double dial = knob->property("dial").toDouble();
  return knob->mapToScene(QPointF(knob->width() / 2, dial / 2)).toPoint();
}

QPoint at(QQuickItem* target, double fx, double fy) {
  return target->mapToScene(QPointF(target->width() * fx, target->height() * fy)).toPoint();
}

void settle() {
  QCoreApplication::processEvents();
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  QCoreApplication::processEvents();
}

void click(Panel& panel, QPoint point, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
  QTest::mouseClick(panel.window.get(), Qt::LeftButton, modifiers, point);
  settle();
}

void drag(Panel& panel, QPoint from, QPoint to, int steps = 8) {
  QTest::mousePress(panel.window.get(), Qt::LeftButton, Qt::NoModifier, from);
  for (int step = 1; step <= steps; ++step) {
    QTest::mouseMove(panel.window.get(), from + (to - from) * step / steps);
  }
  QTest::mouseRelease(panel.window.get(), Qt::LeftButton, Qt::NoModifier, to);
  settle();
}

std::size_t count(const InMemoryNativeSynthAdapter& adapter, NativeSynthGesture gesture) {
  std::size_t n = 0;
  for (const auto& submission : adapter.submissions()) {
    n += submission.gesture == gesture ? 1 : 0;
  }
  return n;
}

// Shows `key`'s controls on MAIN (ADR-0076 revision 2): every sound module
// is always there; an envelope or LFO is selected on the MODULATION card,
// as a click on its tab does.
void focus(Panel& panel, const char* key) {
  const QString name = QString::fromLatin1(key);
  if (name.startsWith(QStringLiteral("env.")) || name.startsWith(QStringLiteral("lfo."))) {
    panel.root->setProperty("modTab", name);
  }
  settle();
}

QString text_of(Panel& panel, const QString& name) {
  return item(panel, name)->property("text").toString();
}

// ---- Cases -----------------------------------------------------------------------------------

// No knob caption collides with another or with a dropdown, none is cut
// short, and every knob sits inside its controls: on MAIN's cards for each
// oscillator type, Sub · Noise, the filters, an envelope and an LFO, and on
// the Global page.
void captions_never_overlap() {
  InMemoryNativeSynthAdapter adapter(synth::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  const auto check_card = [&](QQuickItem* card, const std::string& where) {
    std::vector<QQuickItem*> captions;
    std::vector<QQuickItem*> dropdowns;
    collect(card, QStringLiteral("novaKnobCaption:"), captions);
    collect(card, QStringLiteral("novaDropdown:"), dropdowns);
    const QRectF bounds = scene_rect(card);
    for (std::size_t i = 0; i < captions.size(); ++i) {
      const QRectF a = painted_text(captions[i]);
      const auto name = captions[i]->objectName().toStdString();
      require(captions[i]->property("implicitWidth").toDouble() <= captions[i]->width() + 0.5,
              // A failure message in a test loop.
              // NOLINTNEXTLINE(performance-inefficient-string-concatenation)
              where + ": caption cut short: " + name);
      auto* knob = captions[i]->parentItem();
      const QRectF k = scene_rect(knob);
      require(k.left() >= bounds.left() - 0.5 && k.right() <= bounds.right() + 0.5,
              // A failure message in a test loop.
              // NOLINTNEXTLINE(performance-inefficient-string-concatenation)
              where + ": knob outside its card: " + name);
      for (std::size_t j = i + 1; j < captions.size(); ++j) {
        require(!a.intersects(painted_text(captions[j])),
                // A failure message in a test loop.
                // NOLINTNEXTLINE(performance-inefficient-string-concatenation)
                where + ": captions overlap: " + name + " and " +
                    captions[j]->objectName().toStdString());
      }
      for (auto* dropdown : dropdowns) {
        // A failure message in a test loop.
        // NOLINTNEXTLINE(performance-inefficient-string-concatenation)
        require(!a.intersects(scene_rect(dropdown)), where + ": caption under a dropdown: " + name);
      }
    }
  };
  for (int type = 0; type < 6; ++type) {
    for (const char* osc : {"a", "b", "c"}) {
      controller.set_choice(QStringLiteral("nova.osc.%1.type").arg(QLatin1String(osc)), type);
      controller.set_choice(QStringLiteral("nova.osc.%1.warp.1.mode").arg(QLatin1String(osc)), 12);
    }
    settle();
    for (const char* osc : {"a", "b", "c"}) {
      focus(*panel, osc);
      auto* card = item(*panel, QStringLiteral("novaOsc:%1").arg(QLatin1String(osc)));
      check_card(card, "osc " + std::string(osc) + " type " + std::to_string(type));
      const QRectF lower =
          scene_rect(item(*panel, QStringLiteral("novaOscLower:%1").arg(QLatin1String(osc))));
      require(lower.right() <= scene_rect(card).right(), "the unison and pitch rows fit the card");
    }
  }
  for (const int filter : {1, 2}) {
    focus(*panel, filter == 1 ? "filter1" : "filter2");
    check_card(item(*panel, QStringLiteral("novaFilter:%1").arg(filter)), "filter");
  }
  focus(*panel, "subnoise");
  check_card(item(*panel, QStringLiteral("novaSubNoise")), "sub and noise");
  focus(*panel, "env.1");
  check_card(item(*panel, QStringLiteral("novaSourceControls:env.1")), "envelope controls");
  focus(*panel, "lfo.1");
  check_card(item(*panel, QStringLiteral("novaSourceControls:lfo.1")), "LFO controls");
  panel->root->setProperty("page", 4);
  settle();
  check_card(item(*panel, QStringLiteral("novaPage:4")), "global page");
}

// The source tabs start with ENV 1, which says it is the amp envelope; a
// click on it draws the amp envelope on the MODULATION card.
void amp_envelope_comes_first() {
  InMemoryNativeSynthAdapter adapter(synth::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  panel->root->setProperty("modTab", QStringLiteral("lfo.2"));
  // The tabs' Flow places them when it polishes.
  QMetaObject::invokeMethod(item(*panel, QStringLiteral("novaModTabs")), "forceLayout");
  settle();
  QQuickItem* chip = item(*panel, QStringLiteral("novaSource:env.1"));
  require(chip->property("text").toString().contains(QStringLiteral("AMP")),
          "ENV 1's chip names the amp");
  const QQuickItem* lfo = item(*panel, QStringLiteral("novaSource:lfo.1"));
  require(chip->y() < lfo->y() || (chip->y() == lfo->y() && chip->x() < lfo->x()),
          "the envelopes come first among the tabs");
  click(*panel, centre(chip));
  require(panel->root->property("modTab").toString() == QStringLiteral("env.1") &&
              find_item(panel->root, QStringLiteral("novaEnvelopeView:env.1")) != nullptr,
          "a click draws the amp envelope");
}

// Click a value to type one: units read as shown, Enter sets it in one undo
// step, Esc and nonsense change nothing.
void typing_a_value() {
  InMemoryNativeSynthAdapter adapter(synth::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  // The knob and the typed entry are named at every call.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  const auto type = [&](const QString& id, const QString& entry, bool enter) {
    click(*panel, centre(item(*panel, QStringLiteral("novaKnobValue:") + id)));
    auto* field = item(*panel, QStringLiteral("novaKnobField:") + id);
    require(field->hasActiveFocus(), "the value field takes the keyboard");
    QTest::keyClick(panel->window.get(), Qt::Key_A, Qt::ControlModifier);
    // QTest::keyClicks takes widgets only: one key per character.
    for (const QChar c : entry)
      QTest::keyClick(panel->window.get(), c.toLatin1());
    QTest::keyClick(panel->window.get(), enter ? Qt::Key_Return : Qt::Key_Escape);
    settle();
  };
  focus(*panel, "filter1");
  const QString cutoff = QStringLiteral("nova.filter.1.cutoff");
  const auto begins = count(adapter, NativeSynthGesture::begin);
  const auto commits = count(adapter, NativeSynthGesture::commit);
  type(cutoff, QStringLiteral("440"), true);
  require(std::fabs(controller.value(cutoff) - 440.0) < 1e-6, "a typed cutoff in Hz");
  require(count(adapter, NativeSynthGesture::begin) == begins + 1 &&
              count(adapter, NativeSynthGesture::commit) == commits + 1,
          "a typed value is one undo step");
  type(cutoff, QStringLiteral("5k"), true);
  require(std::fabs(controller.value(cutoff) - 5'000.0) < 1e-6, "k reads as kHz");
  type(cutoff, QStringLiteral("123"), false);
  require(std::fabs(controller.value(cutoff) - 5'000.0) < 1e-6, "Esc changes nothing");
  type(cutoff, QStringLiteral("loud"), true);
  require(std::fabs(controller.value(cutoff) - 5'000.0) < 1e-6, "nonsense changes nothing");
  const QString resonance = QStringLiteral("nova.filter.1.resonance");
  type(resonance, QStringLiteral("55"), true);
  require(std::fabs(controller.value(resonance) - 0.55) < 1e-6, "percent values in percent");
  focus(*panel, "env.1");
  const QString attack = QStringLiteral("nova.env.1.attack");
  type(attack, QStringLiteral("250 ms"), true);
  require(std::fabs(controller.value(attack) - 0.25) < 1e-6, "ms reads as milliseconds");
  type(attack, QStringLiteral("3 s"), true);
  require(std::fabs(controller.value(attack) - 3.0) < 1e-6, "s reads as seconds");
  focus(*panel, "filter1");
  type(cutoff, QStringLiteral("99999"), true);
  require(std::fabs(controller.value(cutoff) - 22'000.0) < 1e-6, "out of range is held at the end");
}

// Ctrl+click resets a knob, as double-click does; a tooltip names it in full.
void ctrl_click_resets_and_tips_name_it() {
  InMemoryNativeSynthAdapter adapter(synth::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  const QString cutoff = QStringLiteral("nova.filter.1.cutoff");
  const double home = controller.parameter(cutoff).value(QStringLiteral("default")).toDouble();
  controller.set_value(cutoff, 500.0);
  focus(*panel, "filter1");
  auto* knob = item(*panel, QStringLiteral("novaKnob:") + cutoff);
  click(*panel, dial_centre(knob), Qt::ControlModifier);
  require(std::fabs(controller.value(cutoff) - home) < 1e-6, "Ctrl+click resets to the default");
  QTest::mouseMove(panel->window.get(), dial_centre(knob));
  settle();
  auto* tip = item(*panel, QStringLiteral("novaKnobTip:") + cutoff);
  require(tip->property("shown").toBool(), "hovering shows the tooltip");
  require(tip->property("text").toString().startsWith(QStringLiteral("Filter 1 · Cutoff")),
          "the tooltip names the parameter in full: " +
              tip->property("text").toString().toStdString());
  panel->root->setProperty("page", 3);
  settle();
  require(item(*panel, QStringLiteral("novaArpLockTip:note"))
              ->property("text")
              .toString()
              .startsWith(QStringLiteral("Lock Note")),
          "a lane's lock tooltip names the lane");
}

// Counts read as plain numbers: unison voices, polyphony, arp octaves.
void counts_have_no_sign() {
  InMemoryNativeSynthAdapter adapter(synth::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(text_of(*panel, QStringLiteral("novaKnobValue:nova.osc.a.unison.voices")) ==
              QStringLiteral("1"),
          "unison voices are a count");
  require(text_of(*panel, QStringLiteral("novaKnobValue:nova.osc.a.octave")) == QStringLiteral("0"),
          "an octave offset at zero has no sign");
  panel->root->setProperty("page", 4);
  settle();
  require(text_of(*panel, QStringLiteral("novaKnobValue:nova.global.polyphony")) ==
              QStringLiteral("16"),
          "polyphony is a count");
  panel->root->setProperty("page", 3);
  settle();
  require(text_of(*panel, QStringLiteral("novaKnobValue:nova.arp.octaves")) == QStringLiteral("1"),
          "arp octaves are a count");
}

// Dragging across the wavetable display scans the position in one undo step.
void dragging_the_oscillator_scans() {
  InMemoryNativeSynthAdapter adapter(synth::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  const QString position = QStringLiteral("nova.osc.a.position");
  require(controller.normalised(position) == 0.0, "the position starts at 0");
  auto* display = item(*panel, QStringLiteral("novaOscDisplay:a"));
  const auto begins = count(adapter, NativeSynthGesture::begin);
  drag(*panel, at(display, 0.1, 0.5), at(display, 0.6, 0.5));
  require(std::fabs(controller.normalised(position) - 0.5) < 0.02,
          "half the width scans half the range: " +
              std::to_string(controller.normalised(position)));
  require(count(adapter, NativeSynthGesture::begin) == begins + 1, "one undo step");
  QTest::mouseDClick(panel->window.get(), Qt::LeftButton, Qt::NoModifier, at(display, 0.5, 0.5));
  settle();
  require(controller.normalised(position) == 0.0, "double-click resets the position");
}

// Dragging on the filter curve sets the cutoff across and the resonance up.
void dragging_the_filter_curve() {
  InMemoryNativeSynthAdapter adapter(synth::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  focus(*panel, "filter1");
  auto* curve = item(*panel, QStringLiteral("novaFilterResponse:1"));
  const auto begins = count(adapter, NativeSynthGesture::begin);
  drag(*panel, at(curve, 0.3, 0.7), at(curve, 0.75, 0.25));
  const double fx = std::round(curve->width() * 0.75) / curve->width();
  const double expected = 20.0 * std::pow(1'000.0, fx);
  // A pixel is about 3 % of cutoff on a 240-pixel log axis.
  require(std::fabs(controller.value(QStringLiteral("nova.filter.1.cutoff")) / expected - 1.0) <
              0.06,
          "across is the cutoff: " +
              std::to_string(controller.value(QStringLiteral("nova.filter.1.cutoff"))));
  require(std::fabs(controller.value(QStringLiteral("nova.filter.1.resonance")) - 0.75) < 0.03,
          "up is the resonance");
  require(count(adapter, NativeSynthGesture::begin) == begins + 1, "one undo step");
  QTest::mouseDClick(panel->window.get(), Qt::LeftButton, Qt::NoModifier, at(curve, 0.5, 0.5));
  settle();
  require(controller.is_default(QStringLiteral("nova.filter.1.cutoff")) &&
              controller.is_default(QStringLiteral("nova.filter.1.resonance")),
          "double-click resets both");
}

// An envelope node, dragged: right lengthens its stage; the sustain node
// moves the sustain level.
void dragging_envelope_nodes() {
  InMemoryNativeSynthAdapter adapter(synth::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  focus(*panel, "env.1");
  auto* view = item(*panel, QStringLiteral("novaEnvelopeView:env.1"));
  const auto node = [&](int index) {
    QVariant nodes;
    require(QMetaObject::invokeMethod(view, "nodes", Q_RETURN_ARG(QVariant, nodes)),
            "the envelope lists its nodes");
    const QJSValue list = nodes.value<QJSValue>();
    const QJSValue n = list.property(static_cast<quint32>(index));
    QVariant x;
    QVariant y;
    require(
        QMetaObject::invokeMethod(view, "plotX", Q_RETURN_ARG(QVariant, x),
                                  Q_ARG(QVariant, n.property(QStringLiteral("x")).toVariant())) &&
            QMetaObject::invokeMethod(view, "plotY", Q_RETURN_ARG(QVariant, y),
                                      Q_ARG(QVariant, n.property(QStringLiteral("y")).toVariant())),
        "nodes map onto the view");
    return view->mapToScene(QPointF(x.toDouble(), y.toDouble())).toPoint();
  };
  const QString attack = QStringLiteral("nova.env.1.attack");
  const QString sustain = QStringLiteral("nova.env.1.sustain");
  const double attack_before = controller.value(attack);
  const auto begins = count(adapter, NativeSynthGesture::begin);
  const QPoint from = node(0);
  drag(*panel, from, from + QPoint(40, 0));
  require(controller.value(attack) > attack_before + 0.05,
          "dragging the attack node right lengthens it");
  require(count(adapter, NativeSynthGesture::begin) == begins + 1, "one undo step");
  const QPoint hold = node(3);
  drag(*panel, hold, hold + QPoint(0, static_cast<int>(view->height() / 3)));
  require(controller.value(sustain) < 0.8, "dragging the sustain node down lowers it");
  // Between nodes, a drag still bends the segment.
  const double curve_before = controller.value(QStringLiteral("nova.env.1.decay_curve"));
  const QPoint decay_start = node(1);
  const QPoint decay_end = node(2);
  const QPoint middle = (decay_start + decay_end) / 2;
  drag(*panel, middle, middle + QPoint(0, 30));
  require(controller.value(QStringLiteral("nova.env.1.decay_curve")) != curve_before,
          "a segment still bends");
}

} // namespace

// The arrows beside the wavetable list step through every factory table,
// wrapping at the ends.
void table_arrows_step_through_the_tables() {
  InMemoryNativeSynthAdapter adapter(synth::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  const auto shown = [&] { return text_of(*panel, QStringLiteral("novaTable:a")); };
  require(shown() == QStringLiteral("Basic Shapes"), "the default table");
  click(*panel, centre(item(*panel, QStringLiteral("novaTableNext:a"))));
  settle();
  require(shown() == QStringLiteral("Analog Saw"), "next steps on: " + shown().toStdString());
  click(*panel, centre(item(*panel, QStringLiteral("novaTablePrev:a"))));
  click(*panel, centre(item(*panel, QStringLiteral("novaTablePrev:a"))));
  settle();
  require(shown() == QStringLiteral("Noise Bands"), "previous wraps: " + shown().toStdString());
}

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  try {
    captions_never_overlap();
    amp_envelope_comes_first();
    typing_a_value();
    ctrl_click_resets_and_tips_name_it();
    counts_have_no_sign();
    dragging_the_oscillator_scans();
    dragging_the_filter_curve();
    dragging_envelope_nodes();
    table_arrows_step_through_the_tables();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "nova editing contract passed\n";
  return 0;
}
