#include "in_memory_native_synth_adapter.hpp"
#include "native_synth_device_controller.hpp"
#include "nova_arp.hpp"

#include "oscillate/application/nova_arp_patterns.hpp"
#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/native_dsp/synth/nova.hpp"
#include "oscillate/test/check.hpp"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSet>
#include <QTemporaryDir>
#include <QTest>
#include <QVariantList>
#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// Nova's arp pattern engine panel (ADR-0078): every factory pattern parses
// and touches only the arp; a pattern loads from the library as one edit,
// keeping locked lanes; ↑/↓ load the next and "/" takes the keys for the
// search; lanes paint, loop at their own lengths, toggle and step; MUTATE
// is seeded and returns to the pattern; the groove comes from the pool.
namespace {
using oscillate::quick::InMemoryNativeSynthAdapter;
using oscillate::quick::NativeSynthDeviceController;
using oscillate::quick::NativeSynthGesture;
using oscillate::quick::NovaArp;
namespace arp = oscillate::application::nova_arp;
namespace nova = oscillate::native_dsp::synth::nova;
namespace synth = oscillate::native_dsp::synth;

constexpr int arp_page = 3;

using oscillate::test::require;

const oscillate::native_dsp::EngineDescriptor& table() { return *nova::engine().descriptor; }

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
                    QUrl(QStringLiteral("qrc:/NovaArpTest.qml")));
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
  panel->root->setProperty("page", arp_page);
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

QPoint at(QQuickItem* target, double fx, double fy) {
  return target->mapToScene(QPointF(target->width() * fx, target->height() * fy)).toPoint();
}

void settle() {
  QCoreApplication::processEvents();
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  QCoreApplication::processEvents();
}

void click(Panel& panel, const QString& name) {
  QTest::mouseClick(panel.window.get(), Qt::LeftButton, Qt::NoModifier,
                    at(item(panel, name), 0.5, 0.5));
  settle();
}

void drag(Panel& panel, QPoint from, QPoint to) {
  QTest::mousePress(panel.window.get(), Qt::LeftButton, Qt::NoModifier, from);
  settle();
  QTest::mouseMove(panel.window.get(), (from + to) / 2);
  settle();
  QTest::mouseMove(panel.window.get(), to);
  settle();
  QTest::mouseRelease(panel.window.get(), Qt::LeftButton, Qt::NoModifier, to);
  settle();
}

void key(Panel& panel, Qt::Key k) {
  QTest::keyClick(panel.window.get(), k);
  settle();
}

NovaArp& model(NativeSynthDeviceController& controller) {
  auto* found = qobject_cast<NovaArp*>(controller.arp());
  require(found != nullptr, "Nova's controller has the arp model");
  return *found;
}

double value(const NativeSynthDeviceController& controller, const std::string& id) {
  return controller.value(QString::fromStdString(id));
}

// Every value outside nova.arp.* is as it was.
void require_only_arp_changed(const std::vector<double>& before, const std::vector<double>& after,
                              std::string_view what) {
  require(before.size() == after.size(), std::string(what) + ": same size");
  for (std::size_t i = 0; i < before.size(); ++i) {
    if (!table().parameters[i].id.starts_with(arp::arp_prefix))
      require(before[i] == after[i],
              std::string(what) + " changed " + std::string(table().parameters[i].id));
  }
}

std::string label(const InMemoryNativeSynthAdapter& adapter, std::string_view id) {
  for (const auto& l : adapter.document().labels)
    if (l.id == id)
      return l.text;
  return {};
}

// ---- Cases ------------------------------------------------------------------------------

void every_factory_pattern_parses() {
  const auto patterns = arp::factory_patterns();
  require(patterns.size() >= 60, "about sixty factory patterns");
  const auto defaults = oscillate::native_dsp::default_values(table());
  std::set<std::string_view> names;
  for (const auto& pattern : patterns) {
    require(names.insert(pattern.name).second, "pattern names are unique");
    require(std::ranges::find(arp::categories, pattern.category) != arp::categories.end(),
            std::string(pattern.name) + " is in a known category");
    const auto applied = arp::apply(table(), defaults, pattern);
    require(applied.has_value(), std::string(pattern.name) + " applies: " +
                                     (applied ? std::string() : applied.error().message));
    require_only_arp_changed(defaults, applied.value(), pattern.name);
  }
  for (const auto category : arp::categories) {
    require(std::ranges::count(patterns, category, &arp::PatternSpec::category) >= 7,
            "each category has at least seven patterns");
  }
}

void loading_a_pattern_is_one_arp_edit() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  auto& arp_model = model(controller);
  controller.set_value(QStringLiteral("nova.arp.lane.note.1"), 3);
  click(*panel, QStringLiteral("novaArpLock:note"));
  require(arp_model.locks().contains(QStringLiteral("note")), "the lock button locks the lane");
  const auto before = adapter.values();
  const auto edits = adapter.state_submissions().size();

  click(*panel, QStringLiteral("novaArpCategory:Techno"));
  click(*panel, QStringLiteral("novaArpPattern:Rolling 16ths"));
  require(adapter.state_submissions().size() == edits + 1 &&
              adapter.state_submissions().back().gesture == NativeSynthGesture::single,
          "a pattern loads as one edit");
  require_only_arp_changed(before, adapter.values(), "loading a pattern");
  require(value(controller, "nova.arp.on") == 1.0 && value(controller, "nova.arp.octaves") == 2.0,
          "the arp is on at the pattern's octaves");
  require(value(controller, "nova.arp.lane.ratchet.length") == 16.0 &&
              value(controller, "nova.arp.lane.ratchet.16") == 3.0,
          "the pattern's ratchet lane");
  require(value(controller, "nova.arp.lane.accent.length") == 4.0 &&
              value(controller, "nova.arp.lane.accent.1") == 1.0,
          "the pattern's accent lane, four long");
  require(value(controller, "nova.arp.lane.note.1") == 3.0, "a locked lane keeps its steps");
  require(label(adapter, nova::arp_pattern_label) == "Rolling 16ths", "the pattern is named");

  // The list keeps the keys: ↓ loads the next; "/" takes them for the search.
  key(*panel, Qt::Key_Down);
  require(label(adapter, nova::arp_pattern_label) == "Warehouse Ratchet", "↓ loads the next");
  require(!panel->root->property("arpSearching").toBool(), "the list leaves the keys playing");
  key(*panel, Qt::Key_Slash);
  require(panel->root->property("arpSearching").toBool(), "/ enters the search");
  require(item(*panel, QStringLiteral("novaKeysText"))
              ->property("text")
              .toString()
              .startsWith(QStringLiteral("TYPING IN SEARCH")),
          "the keys pill says the search has the keys");
  key(*panel, Qt::Key_Escape);
  require(!panel->root->property("arpSearching").toBool(), "Escape returns the keys");
}

void lanes_paint_toggle_and_loop() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  const auto submissions = adapter.submissions().size();
  drag(*panel, at(item(*panel, QStringLiteral("novaArpCell:nova.arp.lane.velocity.1")), 0.5, 0.05),
       at(item(*panel, QStringLiteral("novaArpCell:nova.arp.lane.velocity.4")), 0.5, 0.5));
  require(value(controller, "nova.arp.lane.velocity.1") > 0.85, "the drag starts high");
  require(std::fabs(value(controller, "nova.arp.lane.velocity.4") - 0.5) < 0.15,
          "and paints across to the middle");
  require(adapter.submissions()[submissions].gesture == NativeSynthGesture::begin &&
              adapter.submissions().back().gesture == NativeSynthGesture::commit,
          "a painted drag is one undo step");

  click(*panel, QStringLiteral("novaArpLonger:velocity"));
  require(value(controller, "nova.arp.lane.velocity.length") == 17.0, "+ lengthens the lane");
  click(*panel, QStringLiteral("novaArpShorter:gate"));
  require(value(controller, "nova.arp.lane.gate.length") == 15.0, "− shortens another lane");

  click(*panel, QStringLiteral("novaArpCell:nova.arp.lane.tie.3"));
  require(value(controller, "nova.arp.lane.tie.3") == 1.0, "a click toggles a tie");
  click(*panel, QStringLiteral("novaArpCell:nova.arp.lane.condition.2"));
  require(value(controller, "nova.arp.lane.condition.2") == 1.0, "a click steps a condition on");

  click(*panel, QStringLiteral("novaArpMode:nova.arp.mode:15"));
  require(value(controller, "nova.arp.mode") == 15.0, "the Euclidean mode button");
  require(model(controller).period() == 4080.0, "the period: lcm(17, 15, 16)");
}

void mutate_is_seeded_and_returns() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  auto& arp_model = model(controller);
  require(arp_model.apply_pattern(arp_model.find_pattern(QStringLiteral("Prime Lanes"))),
          "a pattern loads");
  const auto pattern = adapter.values();
  click(*panel, QStringLiteral("novaArpMutate"));
  require(arp_model.mutation() == 1, "MUTATE takes the first seed");
  const auto first = adapter.values();
  require(first != pattern, "a mutation changes the pattern");
  require_only_arp_changed(pattern, first, "mutate");
  click(*panel, QStringLiteral("novaArpMutateBack"));
  require(arp_model.mutation() == 0 && adapter.values() == pattern, "‹ to 0 is the pattern again");
  require(arp_model.mutate(1) && adapter.values() == first, "the same seed, the same mutation");

  require(arp_model.mutate(0) && arp_model.set_lock(QStringLiteral("gate"), true) &&
              arp_model.mutate(7),
          "a locked mutation");
  const auto locked = adapter.values();
  for (std::size_t i = 0; i < table().parameters.size(); ++i) {
    if (table().parameters[i].id.starts_with("nova.arp.lane.gate."))
      require(locked[i] == pattern[i], "a locked lane keeps its steps through MUTATE");
  }
}

void the_groove_comes_from_the_pool() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  auto& arp_model = model(controller);
  const QVariantMap push{{QStringLiteral("name"), QStringLiteral("Push")},
                         {QStringLiteral("stepsPerQuarter"), 4},
                         {QStringLiteral("timings"), QVariantList{0.0, 0.25}},
                         {QStringLiteral("velocities"), QVariantList{1.0, 0.6}}};
  const auto before = adapter.values();
  require(arp_model.set_groove(push), "a pool groove becomes the arp's");
  require_only_arp_changed(before, adapter.values(), "a groove");
  require(value(controller, "nova.arp.groove.steps") == 2.0 &&
              value(controller, "nova.arp.groove.2.timing") == 0.25 &&
              value(controller, "nova.arp.groove.2.velocity") == 0.6,
          "its steps are copied");
  require(label(adapter, nova::arp_groove_label) == "Push", "the groove is named");
  settle();
  require(item(*panel, QStringLiteral("novaArpGroove"))->property("text").toString() ==
              QStringLiteral("Push"),
          "the GROOVE list shows it");
  require(arp_model.clear_groove() && value(controller, "nova.arp.groove.steps") == 0.0,
          "None clears it");
  auto odd = push;
  odd.insert(QStringLiteral("stepsPerQuarter"), 5);
  require(!arp_model.set_groove(odd) && !arp_model.message().isEmpty(),
          "a grid the arp lacks is refused with a reason");
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  // Never the user's own presets or cache.
  QTemporaryDir data;
  QTemporaryDir cache;
  qputenv("XDG_DATA_HOME", data.path().toUtf8());
  qputenv("XDG_CACHE_HOME", cache.path().toUtf8());
  try {
    every_factory_pattern_parses();
    loading_a_pattern_is_one_arp_edit();
    lanes_paint_toggle_and_loop();
    mutate_is_seeded_and_returns();
    the_groove_comes_from_the_pool();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "nova arp contract passed\n";
  return 0;
}
