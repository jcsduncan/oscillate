#include "native_synth_device_controller.hpp"
#include "native_synth_panel_adapter.hpp"

#include "oscillate/native_dsp/synth/nova.hpp"
#include "oscillate/native_dsp/synth/parameters.hpp"
#include "oscillate/test/check.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QGuiApplication>
#include <QJSValue>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>
#include <QVariantMap>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// Nova's preset browser (NovaPresetBrowser.qml): search by name, category
// or tag, favourites that persist, user presets saved, renamed and deleted
// from its footer, loading from the list and the arrow keys, EDITED in the
// preset bar, and the bar's arrows walking a filtered list.
namespace {
using oscillate::quick::NativeSynthDeviceController;
using oscillate::quick::NativeSynthGesture;
using oscillate::quick::NativeSynthPanelAdapter;
using oscillate::quick::NativeSynthPanelStatus;
namespace synth = oscillate::native_dsp::synth;

using oscillate::test::require;

const synth::EngineDescriptor& table() { return *synth::nova::engine().descriptor; }

// Accepts every edit, and writes a preset as its values, one per line, so
// user presets save and load without a project.
class PresetAdapter final : public NativeSynthPanelAdapter {
public:
  PresetAdapter() : values_(synth::default_values(table())) {}
  [[nodiscard]] std::vector<double> values() const override { return values_; }
  void submit(std::span<const double> values, NativeSynthGesture) override {
    values_.assign(values.begin(), values.end());
    ++status_.saved_revision;
    status_.audible_revision = status_.saved_revision;
  }
  [[nodiscard]] NativeSynthPanelStatus status() const override { return status_; }
  [[nodiscard]] std::optional<std::string> preset_text() const override {
    std::string text;
    for (const double value : values_) {
      std::array<char, 32> digits{};
      const auto written = std::to_chars(digits.data(), digits.data() + digits.size(), value);
      text.append(digits.data(), written.ptr);
      text += '\n';
    }
    return text;
  }
  bool submit_preset_text(std::string_view text) override {
    std::vector<double> values;
    while (!text.empty()) {
      const auto end = text.find('\n');
      const auto line = text.substr(0, end);
      double value = 0.0;
      if (std::from_chars(line.data(), line.data() + line.size(), value).ec != std::errc{})
        return false;
      values.push_back(value);
      text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
    }
    if (values.size() != values_.size())
      return false;
    submit(values, NativeSynthGesture::single);
    return true;
  }

private:
  std::vector<double> values_;
  NativeSynthPanelStatus status_{};
};

// ---- Harness (as nova_panel_tests.cpp) --------------------------------------------------

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
                    QUrl(QStringLiteral("qrc:/NovaBrowserTest.qml")));
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

void settle() {
  QCoreApplication::processEvents();
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  QCoreApplication::processEvents();
}

void click(Panel& panel, QPoint point) {
  QTest::mouseClick(panel.window.get(), Qt::LeftButton, Qt::NoModifier, point);
  settle();
}

void click(Panel& panel, const QString& name) { click(panel, centre(item(panel, name))); }

void type(Panel& panel, const QString& text) {
  // QTest::keyClicks takes widgets only: one key per character.
  for (const QChar c : text)
    QTest::keyClick(panel.window.get(), c.toLatin1());
  settle();
}

void key(Panel& panel, Qt::Key k) {
  QTest::keyClick(panel.window.get(), k);
  settle();
}

QQuickItem* browser(Panel& panel) { return item(panel, QStringLiteral("novaPresetBrowser")); }

// The browser's entries that are presets: {user, name, category, tags}.
std::vector<QVariantMap> shown(Panel& panel) {
  std::vector<QVariantMap> out;
  const auto entries = browser(panel)->property("entries").value<QJSValue>().toVariant().toList();
  for (const auto& entry : entries) {
    const auto map = entry.toMap();
    if (!map.contains(QStringLiteral("header")))
      out.push_back(map);
  }
  return out;
}

QStringList names(const std::vector<QVariantMap>& entries) {
  QStringList out;
  for (const auto& entry : entries)
    out.append(entry.value(QStringLiteral("name")).toString());
  return out;
}

void clear_filters(Panel& panel) {
  auto* b = browser(panel);
  b->setProperty("search", QString());
  b->setProperty("category", QString());
  b->setProperty("tag", QString());
  b->setProperty("favouritesOnly", false);
  settle();
}

// Scrolls the chip row so the chip `name` shows, and clicks it. The chips'
// Flow places new chips when it polishes, so let it first.
void click_chip(Panel& panel, const QString& name) {
  auto* scroller = item(panel, QStringLiteral("novaPresetChipScroller"));
  QMetaObject::invokeMethod(
      scroller->property("contentItem").value<QQuickItem*>()->childItems().front(), "forceLayout");
  settle();
  auto* chip = item(panel, name);
  const double end = scroller->property("contentHeight").toDouble() - scroller->height();
  const double top =
      chip->mapToItem(scroller->property("contentItem").value<QQuickItem*>(), QPointF(0, 0)).y();
  scroller->setProperty("contentY", std::max(0.0, std::min(top, end)));
  settle();
  click(panel, name);
}

void open_browser(Panel& panel) {
  click(panel, QStringLiteral("novaPresetDisplay"));
  require(browser(panel)->isVisible(), "the preset name opens the browser");
}

// ---- Tests -----------------------------------------------------------------------------

// Whether the window's focus is in a text field: the typing keyboard yields
// to any focus object with a cursor (ComputerKeyboard::typing_text).
bool typing_text(Panel& panel) {
  const auto* focus = panel.window->activeFocusItem();
  return focus != nullptr && focus->property("cursorPosition").isValid();
}

QString keys_pill(Panel& panel) {
  return item(panel, QStringLiteral("novaKeysText"))->property("text").toString();
}

// ADR-0076: the browser opens with its list focused, so letters still play
// notes; "/" or a click enters the search and the pill turns amber; Escape
// in the search returns to the list, and Escape in the list closes.
void browser_keeps_the_keys_playing() {
  PresetAdapter adapter;
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(keys_pill(*panel) == QStringLiteral("KEYS PLAY NOTES"), "the keys play while closed");
  open_browser(*panel);
  require(item(*panel, QStringLiteral("novaPresetList"))->hasActiveFocus() && !typing_text(*panel),
          "the browser opens with the list focused, not a text field");
  require(keys_pill(*panel) == QStringLiteral("KEYS PLAY NOTES"), "the pill says the keys play");
  type(*panel, QStringLiteral("ad"));
  require(browser(*panel)->property("search").toString().isEmpty(),
          "letters on the list type nothing");

  key(*panel, Qt::Key_Slash);
  require(typing_text(*panel) && browser(*panel)->property("searching").toBool(),
          "/ puts the keys in the search");
  require(keys_pill(*panel) == QStringLiteral("TYPING IN SEARCH · ESC TO PLAY"),
          "the pill says the search has the keys");
  type(*panel, QStringLiteral("pad"));
  require(browser(*panel)->property("search").toString() == QStringLiteral("pad"),
          "letters in the search type");
  key(*panel, Qt::Key_Escape);
  require(browser(*panel)->isVisible() && !typing_text(*panel) &&
              item(*panel, QStringLiteral("novaPresetList"))->hasActiveFocus(),
          "Escape in the search returns to the list, browser open");
  require(keys_pill(*panel) == QStringLiteral("KEYS PLAY NOTES"), "and the keys play again");

  click(*panel, QStringLiteral("novaPresetSearch"));
  require(typing_text(*panel), "a click enters the search too");
  key(*panel, Qt::Key_Escape);
  key(*panel, Qt::Key_Escape);
  require(!browser(*panel)->isVisible(), "Escape in the list closes the browser");

  // The drawer sits beside the pages instead of over them.
  click(*panel, QStringLiteral("novaPresetBrowse"));
  auto* page = item(*panel, QStringLiteral("novaPage:0"));
  require(page->x() >= browser(*panel)->x() + browser(*panel)->width(),
          "the pages move right of the open drawer");
}

void search_filters_by_name_category_and_tag() {
  PresetAdapter adapter;
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  click(*panel, QStringLiteral("novaPresetBrowse"));
  require(browser(*panel)->isVisible(), "☰ opens the browser");
  key(*panel, Qt::Key_Escape);
  require(!browser(*panel)->isVisible(), "Escape closes it");

  open_browser(*panel);
  const auto all = shown(*panel);
  require(all.size() == table().presets.size(), "every factory preset is listed");

  // Typed into the search box once "/" has given it the keys.
  key(*panel, Qt::Key_Slash);
  type(*panel, QStringLiteral("PLUCK"));
  auto found = shown(*panel);
  require(!found.empty() && names(found).contains(QStringLiteral("Pluck")),
          "the search finds a name, in any case");
  for (const auto& entry : found) {
    const auto haystack = (entry.value(QStringLiteral("name")).toString() + ' ' +
                           entry.value(QStringLiteral("category")).toString() + ' ' +
                           entry.value(QStringLiteral("tags")).toStringList().join(' '))
                              .toLower();
    require(haystack.contains(QStringLiteral("pluck")), "only matches are shown");
  }
  require(find_item(panel->root, QStringLiteral("novaPresetRow:factory/Init")) == nullptr,
          "a preset that does not match has no row");

  // A category, by search and by chip.
  clear_filters(*panel);
  const auto category = all.back().value(QStringLiteral("category")).toString();
  browser(*panel)->setProperty("search", category.toLower());
  settle();
  for (const auto& entry : shown(*panel)) {
    require(
        entry.value(QStringLiteral("category")).toString() == category ||
            entry.value(QStringLiteral("name")).toString().toLower().contains(category.toLower()) ||
            entry.value(QStringLiteral("tags"))
                .toStringList()
                .join(' ')
                .toLower()
                .contains(category.toLower()),
        "a search finds a category");
  }
  clear_filters(*panel);
  click(*panel, QStringLiteral("novaPresetCategory:") + category);
  require(browser(*panel)->property("category").toString() == category, "a chip picks a category");
  for (const auto& entry : shown(*panel))
    require(entry.value(QStringLiteral("category")).toString() == category, "only that category");

  // A tag, by chip and by search.
  clear_filters(*panel);
  const auto tags = controller.preset_tags();
  require(!tags.isEmpty(), "the factory presets are tagged");
  const auto tag = tags.front();
  // The tags sit behind the #TAGS chip.
  click(*panel, QStringLiteral("novaPresetTagsToggle"));
  click_chip(*panel, QStringLiteral("novaPresetTag:") + tag);
  const auto tagged = shown(*panel);
  require(!tagged.empty(), "a tag chip shows its presets");
  for (const auto& entry : tagged)
    require(entry.value(QStringLiteral("tags")).toStringList().contains(tag), "only that tag");
  clear_filters(*panel);
  browser(*panel)->setProperty("search", tag.toUpper());
  settle();
  require(names(shown(*panel)).contains(names(tagged).front()), "a search finds a tag");

  browser(*panel)->setProperty("search", QStringLiteral("no such preset anywhere"));
  settle();
  require(shown(*panel).empty() && item(*panel, QStringLiteral("novaPresetEmpty"))->isVisible(),
          "no match says so");
}

void loading_from_the_list_and_the_keys() {
  PresetAdapter adapter;
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  open_browser(*panel);
  const auto first = QString::fromUtf8(table().presets[1].name.data(),
                                       static_cast<qsizetype>(table().presets[1].name.size()));
  click(*panel, QStringLiteral("novaPresetRow:factory/") + first);
  require(controller.preset_index() == 1 && controller.preset_name() == first,
          "a click loads the preset");
  require(browser(*panel)->isVisible(), "and the browser stays open to audition");
  key(*panel, Qt::Key_Down);
  require(controller.preset_index() == 2, "Down loads the next preset");
  key(*panel, Qt::Key_Up);
  require(controller.preset_index() == 1, "Up loads the previous one");
  key(*panel, Qt::Key_Return);
  require(!browser(*panel)->isVisible() && controller.preset_index() == 1,
          "Return closes, keeping the preset");

  // EDITED shows once the loaded preset changes.
  require(!item(*panel, QStringLiteral("novaPresetEdited"))->isVisible(), "a fresh preset");
  controller.set_value(QStringLiteral("nova.osc.a.level"), 0.123);
  settle();
  require(item(*panel, QStringLiteral("novaPresetEdited"))->isVisible(),
          "EDITED shows after a change");
}

void favourites_persist() {
  QTemporaryDir data;
  require(data.isValid(), "temporary data directory");
  qputenv("XDG_DATA_HOME", data.path().toUtf8());
  PresetAdapter adapter;
  {
    NativeSynthDeviceController controller(table(), adapter);
    auto panel = load_panel(controller);
    open_browser(*panel);
    const auto key_name = controller.preset_key(false, QStringLiteral("Pluck"));
    click(*panel, QStringLiteral("novaPresetStar:") + key_name);
    require(controller.is_favourite(key_name), "★ marks a favourite");
    click(*panel, QStringLiteral("novaPresetFavourites"));
    require(names(shown(*panel)) == QStringList{QStringLiteral("Pluck")},
            "the favourites filter shows only favourites");
  }
  NativeSynthDeviceController again(table(), adapter);
  require(again.is_favourite(again.preset_key(false, QStringLiteral("Pluck"))),
          "a favourite persists for the next panel");
  auto panel = load_panel(again);
  open_browser(*panel);
  click(*panel,
        QStringLiteral("novaPresetStar:") + again.preset_key(false, QStringLiteral("Pluck")));
  require(again.favourites().isEmpty(), "★ again removes it");
  qunsetenv("XDG_DATA_HOME");
}

void saving_renaming_and_deleting_user_presets() {
  QTemporaryDir data;
  require(data.isValid(), "temporary data directory");
  qputenv("XDG_DATA_HOME", data.path().toUtf8());
  PresetAdapter adapter;
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  controller.set_value(QStringLiteral("nova.osc.a.level"), 0.5);
  open_browser(*panel);
  require(!item(*panel, QStringLiteral("novaPresetSave"))->property("enabledAction").toBool(),
          "Save needs a name for a sound that is not a user preset");

  click(*panel, QStringLiteral("novaPresetSaveName"));
  type(*panel, QStringLiteral("My Lead"));
  click(*panel, QStringLiteral("novaPresetSave"));
  require(controller.user_presets().size() == 1 &&
              controller.preset_name() == QStringLiteral("My Lead") &&
              controller.preset_category() == QStringLiteral("User"),
          "Save stores the sound under the typed name");
  require(find_item(panel->root, QStringLiteral("novaPresetRow:user/My Lead")) != nullptr,
          "the user preset lists under USER");

  // Save replaces the loaded user preset without asking for a name.
  controller.set_value(QStringLiteral("nova.osc.a.level"), 0.25);
  require(controller.preset_modified(), "the user preset is edited");
  click(*panel, QStringLiteral("novaPresetSave"));
  require(!controller.preset_modified() && controller.user_presets().size() == 1,
          "Save replaces the loaded user preset");

  click(*panel, QStringLiteral("novaPresetSaveName"));
  type(*panel, QStringLiteral("My Lead 2"));
  click(*panel, QStringLiteral("novaPresetSaveAs"));
  require(controller.user_presets().size() == 2 &&
              controller.preset_name() == QStringLiteral("My Lead 2"),
          "Save As keeps both");

  click(*panel, QStringLiteral("novaPresetRow:user/My Lead"));
  require(controller.preset_name() == QStringLiteral("My Lead") &&
              controller.value(QStringLiteral("nova.osc.a.level")) == 0.25,
          "a user preset loads from the list");
  click(*panel, QStringLiteral("novaPresetSaveName"));
  type(*panel, QStringLiteral("Big Lead"));
  click(*panel, QStringLiteral("novaPresetRename"));
  require(controller.preset_name() == QStringLiteral("Big Lead") &&
              find_item(panel->root, QStringLiteral("novaPresetRow:user/Big Lead")) != nullptr &&
              find_item(panel->root, QStringLiteral("novaPresetRow:user/My Lead")) == nullptr,
          "Rename renames the selected user preset");

  click(*panel, QStringLiteral("novaPresetRow:user/Big Lead"));
  click(*panel, QStringLiteral("novaPresetDelete"));
  require(controller.user_presets().size() == 1 &&
              find_item(panel->root, QStringLiteral("novaPresetRow:user/Big Lead")) == nullptr,
          "Delete removes the selected user preset");
  qunsetenv("XDG_DATA_HOME");
}

void arrows_follow_the_filter() {
  PresetAdapter adapter;
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  open_browser(*panel);
  const auto tags = controller.preset_tags();
  require(!tags.isEmpty(), "the factory presets are tagged");
  click(*panel, QStringLiteral("novaPresetTagsToggle"));
  click_chip(*panel, QStringLiteral("novaPresetTag:") + tags.front());
  const auto filtered = names(shown(*panel));
  require(filtered.size() >= 2, "a tag with two presets or more");
  key(*panel, Qt::Key_Escape);
  require(browser(*panel)->property("filterActive").toBool(), "the filter outlives the browser");

  // From a preset outside the list ▶ loads its first; inside it, the next.
  const auto next_of = [&](const QString& name) {
    const auto at = filtered.indexOf(name);
    return at < 0 ? filtered.front() : filtered.at((at + 1) % filtered.size());
  };
  controller.apply_preset(0);
  settle();
  const auto start = controller.preset_name();
  click(*panel, QStringLiteral("novaPresetNext"));
  require(controller.preset_name() == next_of(start), "▶ steps into the filtered list");
  const auto first = controller.preset_name();
  click(*panel, QStringLiteral("novaPresetNext"));
  require(controller.preset_name() == next_of(first), "▶ walks the filtered list");
  click(*panel, QStringLiteral("novaPresetPrevious"));
  require(controller.preset_name() == first, "◀ walks back");

  clear_filters(*panel);
  controller.apply_preset(0);
  settle();
  click(*panel, QStringLiteral("novaPresetNext"));
  require(controller.preset_index() == 1, "with no filter ▶ steps every preset");
}

} // namespace

// With NOVA_PANEL_SCREENSHOT_DIR set: the browser open on the library,
// then filtered to one tag.
void screenshots_if_requested() {
  const char* directory = std::getenv("NOVA_PANEL_SCREENSHOT_DIR");
  if (directory == nullptr || *directory == '\0')
    return;
  QDir().mkpath(QString::fromLocal8Bit(directory));
  PresetAdapter adapter;
  NativeSynthDeviceController controller(table(), adapter);
  controller.apply_preset(1);
  auto panel = load_panel(controller);
  open_browser(*panel);
  const QString base = QString::fromLocal8Bit(directory);
  require(panel->window->grabWindow().save(base + QStringLiteral("/nova_browser.png"), "PNG"),
          "screenshot saved");
  browser(*panel)->setProperty("search", QStringLiteral("dark"));
  settle();
  require(
      panel->window->grabWindow().save(base + QStringLiteral("/nova_browser_search.png"), "PNG"),
      "screenshot saved");
}

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  // Never the user's own presets: each test that saves sets its own.
  QTemporaryDir data;
  qputenv("XDG_DATA_HOME", data.path().toUtf8());
  try {
    search_filters_by_name_category_and_tag();
    browser_keeps_the_keys_playing();
    loading_from_the_list_and_the_keys();
    favourites_persist();
    qputenv("XDG_DATA_HOME", data.path().toUtf8());
    saving_renaming_and_deleting_user_presets();
    qputenv("XDG_DATA_HOME", data.path().toUtf8());
    arrows_follow_the_filter();
    screenshots_if_requested();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "nova browser contract passed\n";
  return 0;
}
