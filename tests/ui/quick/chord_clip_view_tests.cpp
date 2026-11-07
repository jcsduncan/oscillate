#include "harmony_controller.hpp"

#include "oscillate/application/project_session.hpp"
#include "oscillate/test/check.hpp"

#include <QByteArray>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QImage>
#include <QMetaObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QString>
#include <QStringList>
#include <QTest>
#include <QUrl>
#include <QVariant>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>

// ADR-0084: the piano roll's chord strip and chord tools load against the
// real controller over a real session and a real MIDI clip, show the chords
// the notes make, act on clicks, and log no QML warnings. With
// OSCILLATE_HARMONY_SHOTS set to a directory, each view is saved as a PNG.
namespace {
namespace application = oscillate::application;
namespace timeline = oscillate::timeline;
namespace quick = oscillate::quick;
namespace fs = std::filesystem;

using oscillate::test::require;

template <typename T> T take(oscillate::foundation::Result<T> result) {
  if (!result)
    throw std::runtime_error(result.error().message);
  return std::move(result).value();
}

QStringList qml_warnings;
void collect_warnings(QtMsgType type, const QMessageLogContext&, const QString& message) {
  if (type != QtDebugMsg && type != QtInfoMsg && message.contains(QStringLiteral("qrc:/")))
    qml_warnings.push_back(message);
  std::cerr << message.toStdString() << '\n';
}

class Directory {
public:
  Directory() {
    std::string pattern = (fs::temp_directory_path() / "oscillate-chord-clip-view-XXXXXX").string();
    require(::mkdtemp(pattern.data()) != nullptr, "fixture directory creation failed");
    path_ = pattern;
  }
  ~Directory() {
    std::error_code ignored;
    fs::remove_all(path_, ignored);
  }
  [[nodiscard]] const fs::path& path() const { return path_; }

private:
  fs::path path_;
};

QQuickItem* find_named(QQuickItem* root, const QString& name) {
  if (root->objectName() == name)
    return root;
  for (auto* child : root->childItems())
    if (auto* found = find_named(child, name))
      return found;
  return nullptr;
}

struct Host {
  QQuickWindow window;
  std::unique_ptr<QObject> object;
  QQuickItem* view = nullptr;

  Host(QQmlEngine& engine, const QString& url, const QVariantMap& properties, QSize size) {
    QQmlComponent component(&engine, QUrl(url));
    object.reset(component.createWithInitialProperties(properties));
    require(object != nullptr, (url + " did not load: " + component.errorString()).toStdString());
    view = qobject_cast<QQuickItem*>(object.get());
    require(view != nullptr, "the view is an item");
    window.resize(size);
    view->setParentItem(window.contentItem());
    view->setSize(QSizeF(size));
    window.show();
    require(QTest::qWaitForWindowExposed(&window), "window exposed");
    QCoreApplication::processEvents();
  }
  void shoot(const char* name) {
    const char* directory = std::getenv("OSCILLATE_HARMONY_SHOTS");
    if (directory == nullptr)
      return;
    QCoreApplication::processEvents();
    const auto image = window.grabWindow();
    require(image.save(QString::fromUtf8(directory) + "/" + name), "screenshot saved");
  }
};

void views_load_and_act() {
  Directory directory;
  auto session = take(application::ProjectSession::create(directory.path() / "song"));
  const auto track = take(session->instruments().add_synth_track());
  constexpr std::int64_t bars = 4;
  constexpr std::int64_t beats_per_bar = 4;
  const timeline::TickPosition length{bars * beats_per_bar * timeline::ticks_per_quarter};
  const auto clip = take(session->midi_clips().add_midi_clip_with_notes(track, {0}, length, {}));
  quick::HarmonyController controller(quick::HarmonyController::Hooks{
      .session = [&] { return session.get(); }, .can_edit = [] { return true; }, .commit = [] {}});
  controller.set_clip_id(QString::fromStdString(clip.to_string()));
  controller.choose_suggestion(0);
  require(controller.count() > 0, "the suggestion wrote chords into the clip");

  QQmlEngine engine;
  {
    Host host(
        engine, QStringLiteral("qrc:/harmony/ChordStrip.qml"),
        {{QStringLiteral("controller"), QVariant::fromValue(static_cast<QObject*>(&controller))},
         {QStringLiteral("keyboardWidth"), 52.0},
         {QStringLiteral("gridWidth"), 900.0},
         {QStringLiteral("spanTicks"), static_cast<double>(length.value)}},
        QSize(960, 30));
    require(find_named(host.view, QStringLiteral("chordBlock0")) != nullptr,
            "the strip names the clip's chords");
    host.shoot("chord-strip.png");
  }
  {
    Host host(
        engine, QStringLiteral("qrc:/harmony/ChordPanel.qml"),
        {{QStringLiteral("controller"), QVariant::fromValue(static_cast<QObject*>(&controller))}},
        QSize(340, 900));
    require(find_named(host.view, QStringLiteral("chordSuggestion0")) != nullptr,
            "Write lists the suggestions");
    host.shoot("chord-panel-write.png");
    // Picking a shape rewrites the clip; the panel follows.
    const auto steps = session->history().undo_steps().size();
    controller.set_shape(4);
    QCoreApplication::processEvents();
    require(session->history().undo_steps().size() == steps + 1, "the shape rewrote the clip");
    host.view->setProperty("tab", 1);
    controller.select(1);
    QCoreApplication::processEvents();
    require(!find_named(host.view, QStringLiteral("chordPanelName"))
                 ->property("text")
                 .toString()
                 .isEmpty(),
            "Chord names the selected chord");
    host.shoot("chord-panel-chord.png");
    host.view->setProperty("tab", 2);
    QCoreApplication::processEvents();
    host.shoot("chord-panel-rhythm.png");
  }
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication application(argc, argv);
  Q_INIT_RESOURCE(qml);
  qInstallMessageHandler(collect_warnings);
  try {
    views_load_and_act();
    require(qml_warnings.isEmpty(), "the chord views logged QML warnings");
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    for (const auto& warning : qml_warnings)
      std::cerr << "  " << warning.toStdString() << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "chord clip view contract passed\n";
  return EXIT_SUCCESS;
}
