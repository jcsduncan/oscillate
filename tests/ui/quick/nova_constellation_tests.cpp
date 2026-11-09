#include "native_synth_device_controller.hpp"
#include "native_synth_panel_adapter.hpp"
#include "nova_constellation.hpp"

#include "oscillate/application/sound_map.hpp"
#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/native_dsp/synth/nova.hpp"
#include "oscillate/native_inserts/instrument_binding.hpp"
#include "oscillate/persistence/effect_preset.hpp"
#include "oscillate/test/check.hpp"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTest>
#include <QVariantMap>

#include <cmath>
#include <cstddef>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// Nova's Constellation view (NovaConstellationView.qml, ADR-0077) over a
// controller whose measurements are already cached: the stars are drawn,
// clicking a star loads it exactly, a drag morphs as one gesture, KEEP MINE
// holds the envelopes, KEEP THIS saves a preset that becomes a star, and
// MUTATE's variations land and load.
namespace {
using oscillate::quick::NativeSynthDeviceController;
using oscillate::quick::NativeSynthGesture;
using oscillate::quick::NativeSynthPanelAdapter;
using oscillate::quick::NativeSynthPanelStatus;
using oscillate::quick::NovaConstellation;
namespace map = oscillate::application::sound_map;
namespace project = oscillate::project;
namespace synth = oscillate::native_dsp::synth;

// Generous: the worker reads the cache at once, but KEEP THIS and MUTATE
// listen to real renders.
constexpr int listen_timeout_ms = 20'000;
constexpr int view_width = 1000;
constexpr int view_height = 560;
// A drag starts this far from every star, so it is a morph, not a click.
constexpr double clear_of_stars = 24.0;

using oscillate::test::require;

template <typename T> T take(oscillate::foundation::Result<T> result) {
  if (!result)
    throw std::runtime_error(result.error().message);
  return std::move(result).value();
}

const synth::SynthEngine& nova() { return synth::nova::engine(); }
const oscillate::native_dsp::EngineDescriptor& table() { return *nova().descriptor; }

// Holds values and a document as a track would, writes presets in the real
// file format and records each submission's gesture.
class SoundAdapter final : public NativeSynthPanelAdapter {
public:
  SoundAdapter() {
    auto sound = take(map::factory_sound(nova(), 0));
    values_ = std::move(sound.values);
    document_ = std::move(sound.document);
  }
  [[nodiscard]] std::vector<double> values() const override { return values_; }
  void submit(std::span<const double> values, NativeSynthGesture gesture) override {
    if (gesture != NativeSynthGesture::cancel)
      values_.assign(values.begin(), values.end());
    record(gesture);
  }
  [[nodiscard]] NativeSynthPanelStatus status() const override { return status_; }
  [[nodiscard]] project::NativeSynthDocument document() const override { return document_; }
  void submit_document(project::NativeSynthDocument document, NativeSynthGesture gesture) override {
    document_ = std::move(document);
    record(gesture);
    ++documents_;
  }
  void submit_state(std::span<const double> values, project::NativeSynthDocument document,
                    NativeSynthGesture gesture) override {
    values_.assign(values.begin(), values.end());
    document_ = std::move(document);
    record(gesture);
    ++documents_;
  }
  bool submit_preset(std::size_t preset) override {
    auto sound = take(map::factory_sound(nova(), preset));
    values_ = std::move(sound.values);
    document_ = std::move(sound.document);
    record(NativeSynthGesture::single);
    return true;
  }
  [[nodiscard]] std::optional<std::string> preset_text() const override {
    auto patch = take(oscillate::native_inserts::make_native_patch(nova(), values_, document_));
    project::NativeEffectPatch saved;
    saved.engine = patch.engine;
    saved.engine_version = patch.engine_version;
    saved.parameters = patch.parameters;
    saved.document = patch.document;
    return oscillate::persistence::encode_native_effect_preset(saved);
  }
  bool submit_preset_text(std::string_view text) override {
    auto sound = map::sound_from_preset_text(nova(), text);
    if (!sound)
      return false;
    values_ = std::move(sound.value().values);
    document_ = std::move(sound.value().document);
    record(NativeSynthGesture::single);
    return true;
  }

  [[nodiscard]] const std::vector<NativeSynthGesture>& gestures() const { return gestures_; }
  [[nodiscard]] int documents() const { return documents_; }
  void forget() {
    gestures_.clear();
    documents_ = 0;
  }

private:
  void record(NativeSynthGesture gesture) {
    gestures_.push_back(gesture);
    ++status_.saved_revision;
    status_.audible_revision = status_.saved_revision;
  }
  std::vector<double> values_;
  project::NativeSynthDocument document_;
  NativeSynthPanelStatus status_{};
  std::vector<NativeSynthGesture> gestures_;
  int documents_ = 0;
};

// Every factory preset measured already, spread over the sky by index, so
// opening listens to nothing.
void prime_cache() {
  const auto& presets = table().presets;
  auto cache = map::FeatureCache::load(NovaConstellation::default_cache_file().toStdString());
  const double last = static_cast<double>(presets.size() - 1);
  for (std::size_t i = 0; i < presets.size(); ++i) {
    const auto sound = take(map::factory_sound(nova(), i));
    const auto key = take(map::sound_key(nova(), sound, map::register_for(presets[i].category)));
    // Two coprime walks so no two stars share a place.
    constexpr std::size_t stride = 7;
    const auto mixed = static_cast<double>((i * stride) % presets.size());
    cache.insert(key, {static_cast<double>(i) / last, mixed / last, 0.5, 0.2, true});
  }
  require(cache.save().has_value(), "the cache is primed");
}

struct View {
  QQmlEngine engine;
  std::unique_ptr<QQuickWindow> window = std::make_unique<QQuickWindow>();
  QQuickItem* root = nullptr;
  QQuickItem* view = nullptr;
  QQuickItem* sky = nullptr;
};

QQuickItem* find_item(QQuickItem* parent, const QString& name) {
  if (parent->objectName() == name)
    return parent;
  for (QQuickItem* child : parent->childItems())
    if (QQuickItem* found = find_item(child, name))
      return found;
  return nullptr;
}

QQuickItem* item(View& view, const QString& name) {
  auto* found = find_item(view.root, name);
  require(found != nullptr, "missing item " + name.toStdString());
  return found;
}

std::unique_ptr<View> load_view(NativeSynthDeviceController& controller) {
  auto view = std::make_unique<View>();
  QQmlComponent component(&view->engine);
  // A stand-in panel: the controller and Nova's theme.
  component.setData(R"(
      import QtQuick
      import "qrc:/nova"
      Item {
          id: host
          required property var controller
          property NovaTheme theme: NovaTheme {}
          NovaConstellationView { anchors.fill: parent; panel: host }
      }
    )",
                    QUrl(QStringLiteral("qrc:/NovaConstellationTest.qml")));
  if (!component.isReady())
    throw std::runtime_error("Constellation failed to load: " +
                             component.errorString().toStdString());
  QObject* object = component.createWithInitialProperties(
      {{QStringLiteral("controller"), QVariant::fromValue<QObject*>(&controller)}});
  require(object != nullptr,
          "Constellation was not created: " + component.errorString().toStdString());
  view->root = qobject_cast<QQuickItem*>(object);
  require(view->root != nullptr, "the host is a Quick item");
  view->root->setWidth(view_width);
  view->root->setHeight(view_height);
  view->window->resize(view_width, view_height);
  view->root->setParentItem(view->window->contentItem());
  view->window->show();
  require(QTest::qWaitForWindowExposed(view->window.get()), "the window is exposed");
  view->view = item(*view, QStringLiteral("novaConstellation"));
  view->sky = item(*view, QStringLiteral("novaConstellationSky"));
  QCoreApplication::processEvents();
  return view;
}

void settle() {
  QCoreApplication::processEvents();
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  QCoreApplication::processEvents();
}

// A star's place in the window, as the view draws it.
QPointF star_point(View& view, const QVariantMap& star) {
  const double pad = view.view->property("skyPad").toDouble();
  const double w = view.sky->width();
  const double h = view.sky->height();
  const QPointF local(pad + star.value(QStringLiteral("x")).toDouble() * (w - 2 * pad),
                      h - pad - star.value(QStringLiteral("y")).toDouble() * (h - 2 * pad));
  return view.sky->mapToScene(local);
}

QVariantMap star_named(const QVariantList& stars, const QString& key) {
  for (const auto& star : stars)
    if (star.toMap().value(QStringLiteral("key")).toString() == key)
      return star.toMap();
  throw std::runtime_error("no star " + key.toStdString());
}

// A point in the sky clear of every star, and so is the point one step to
// its right (where a drag from it ends).
QPoint open_sky(View& view, const QVariantList& stars) {
  const auto step = static_cast<int>(clear_of_stars);
  const auto clear = [&](QPointF point) {
    for (const auto& star : stars) {
      const auto at = star_point(view, star.toMap());
      if (std::hypot(at.x() - point.x(), at.y() - point.y()) <= clear_of_stars)
        return false;
    }
    return true;
  };
  for (int y = step; y < static_cast<int>(view.sky->height()) - step; y += step) {
    for (int x = step; x < static_cast<int>(view.sky->width()) - 2 * step; x += step) {
      const QPointF point = view.sky->mapToScene(QPointF(x, y));
      if (clear(point) && clear(point + QPointF(clear_of_stars, 0)))
        return point.toPoint();
    }
  }
  throw std::runtime_error("the sky is full");
}

void drag(View& view, QPoint from, QPoint to) {
  QTest::mousePress(view.window.get(), Qt::LeftButton, Qt::NoModifier, from);
  settle();
  QTest::mouseMove(view.window.get(), (from + to) / 2);
  settle();
  QTest::mouseMove(view.window.get(), to);
  settle();
  QTest::mouseRelease(view.window.get(), Qt::LeftButton, Qt::NoModifier, to);
  settle();
}

void click(View& view, QPoint point) {
  QTest::mouseClick(view.window.get(), Qt::LeftButton, Qt::NoModifier, point);
  settle();
}

void click(View& view, const QString& name) {
  auto* target = item(view, name);
  click(view, target->mapToScene(QPointF(target->width() / 2, target->height() / 2)).toPoint());
}

void the_constellation() {
  prime_cache();
  SoundAdapter adapter;
  NativeSynthDeviceController controller(table(), adapter);
  auto* constellation =
      qobject_cast<NovaConstellation*>(controller.property("constellation").value<QObject*>());
  require(constellation != nullptr, "Nova has a constellation");
  require(!constellation->started(), "nothing listens before the view opens");
  auto view = load_view(controller);

  // Opening listens; the cache answers at once.
  require(QTest::qWaitFor([&] { return constellation->ready(); }, listen_timeout_ms),
          "every preset is placed");
  const auto presets = static_cast<int>(table().presets.size());
  require(constellation->total() == presets && constellation->stars().size() == presets,
          "a star per preset");
  require(!item(*view, QStringLiteral("novaConstellationProgress"))->isVisible(),
          "the progress goes when it is ready");
  (void)view->window->grabWindow();
  require(
      QTest::qWaitFor([&] { return view->view->property("drawnStars").toInt() == presets; }, 2'000),
      "every star is drawn");
  require(view->view->property("labelledStars").toInt() == presets, "a wide sky labels every star");

  // A star itself loads exactly.
  const auto sub_key = QStringLiteral("factory/Deep Sub");
  click(*view, star_point(*view, star_named(constellation->stars(), sub_key)).toPoint());
  require(constellation->selected() == sub_key, "the star is selected");
  require(controller.preset_name() == QStringLiteral("Deep Sub"), "the preset loaded exactly");

  // A drag between stars is one gesture: begin, updates, commit.
  adapter.forget();
  const auto before = controller.current_values();
  const auto start = open_sky(*view, constellation->stars());
  const QPoint end(start.x() + static_cast<int>(clear_of_stars), start.y());
  drag(*view, start, end);
  const auto& gestures = adapter.gestures();
  require(gestures.size() >= 3 && gestures.front() == NativeSynthGesture::begin &&
              gestures.back() == NativeSynthGesture::commit,
          "a drag is begin … commit");
  for (std::size_t i = 1; i + 1 < gestures.size(); ++i)
    require(gestures[i] == NativeSynthGesture::update, "between them only updates");
  require(controller.current_values() != before, "the sound moved");
  const auto blend = constellation->blend();
  require(blend.size() == 3, "the three nearest stars blend");
  double total = 0.0;
  for (const auto& part : blend)
    total += part.toMap().value(QStringLiteral("weight")).toDouble();
  require(std::abs(total - 1.0) < 1e-9, "their weights sum to one");
  require(item(*view, QStringLiteral("novaConstellationMix"))
              ->property("text")
              .toString()
              .contains(blend.front().toMap().value(QStringLiteral("name")).toString()),
          "the readout names the heaviest star");

  // KEEP MINE: the envelopes stay through a morph.
  click(*view, QStringLiteral("novaConstellationLock:env"));
  require(constellation->is_locked(QStringLiteral("env")), "envelopes are kept");
  const auto attack =
      oscillate::test::take(oscillate::native_dsp::find_parameter(table(), "nova.env.1.attack"));
  const double kept = controller.current_values()[attack];
  drag(*view, end, start);
  require(controller.current_values()[attack] == kept, "the envelope stayed");

  // KEEP THIS saves the blend; the new preset becomes a star.
  click(*view, QStringLiteral("novaConstellationKeep"));
  const auto blend_key = QStringLiteral("user/Blend 1");
  require(QTest::qWaitFor(
              [&] { return constellation->ready() && constellation->total() == presets + 1; },
              listen_timeout_ms),
          "the kept blend is listened to");
  require(constellation->selected() == blend_key, "the kept blend is selected");
  (void)star_named(constellation->stars(), blend_key);
  require(constellation->next_blend_name() == QStringLiteral("Blend 2"), "names count up");

  // MUTATE scatters six measured variations; one loads exactly.
  click(*view, QStringLiteral("novaConstellationMutate"));
  require(QTest::qWaitFor([&] { return !constellation->mutating(); }, listen_timeout_ms),
          "the variations are listened to");
  const auto variations = constellation->variations();
  require(variations.size() ==
              static_cast<qsizetype>(oscillate::application::nova_morph::variation_count),
          "six variations");
  adapter.forget();
  const auto variation = variations.front().toMap();
  require(constellation->apply_star(variation.value(QStringLiteral("key")).toString()),
          "a variation loads");
  require(adapter.gestures().size() == 1 &&
              adapter.gestures().front() == NativeSynthGesture::single,
          "loading a variation is one edit");
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
    the_constellation();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "nova constellation contract passed\n";
  return 0;
}
