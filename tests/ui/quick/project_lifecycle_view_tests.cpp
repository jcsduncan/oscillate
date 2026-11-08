#include "oscillate/test/check.hpp"
#include "oscillate/test/wait.hpp"

#include <QByteArray>
#include <QGuiApplication>
#include <QMetaObject>
#include <QPointF>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QString>
#include <QStringList>
#include <QTest>
#include <QUrl>
#include <QVariant>
#include <QVariantMap>

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

// The project-lifecycle views against fakes shaped like their controllers:
// the welcome screen's default name, Save a copy, and forgetting a project's
// cache with a confirmation.
namespace {

using oscillate::test::require;

constexpr int window_width = 1400;
constexpr int window_height = 900;

QStringList qml_warnings;
void collect_warnings(QtMsgType type, const QMessageLogContext&, const QString& message) {
  if (type != QtDebugMsg && type != QtInfoMsg && message.contains(QStringLiteral("qrc:/")))
    qml_warnings.push_back(message);
  std::cerr << message.toStdString() << '\n';
}

// The shell's colours and panel switches the panels read from their host.
constexpr const char* fake_host = R"QML(
import QtQuick
QtObject {
    property bool saveCopyPanelOpen: false
    property bool cachePanelOpen: true
    property int browserWidth: 0
    property color panel: "#1b1d22"
    property color panelRaised: "#23262c"
    property color line: "#33363d"
    property color textMain: "#e8e8ea"
    property color textDim: "#9a9ca3"
    property color accent: "#4fd1a5"
    property color amber: "#e0b04f"
}
)QML";

constexpr const char* fake_bridge = R"QML(
import QtQuick
QtObject {
    property bool projectOpen: true
    property var availableProjects: []
    property int autosaveIntervalSeconds: 0
    property var recoveryChoice: ({ active: false, projectName: "", currentIssue: "",
                                    autosaveIssue: "", hasCurrent: false, options: [] })
    property string status: ""
    property string suggestedProjectName: "Untitled Project 2"
    property var commandPalette: ({ reducedMotion: false })
    property string log: ""
    function suggested_copy_name() { return "Song copy" }
    function save_copy(name) { log += "copy:" + name + ";" }
    function create_default_project(name) { log += "create:" + name + ";" }
    function choose_open_project() {}
    function open_listed(path) {}
    function forget_project(path) {}
    function trash_project(path) {}
    function rename_project(path, name) {}
    function choose_recovery(generation, kind) {}
    function cancel_recovery() {}
}
)QML";

constexpr const char* fake_cache_controls = R"QML(
import QtQuick
QtObject {
    property bool busy: false
    property string status: "Cache status is current"
    property int budgetMiB: 512
    property real usageBytes: 3145728
    property int entryCount: 2
    property real sharedUsageBytes: 0
    property int sharedEntryCount: 0
    property string sharedStatus: "Shared analysis cache ready"
    property var projects: [
        { name: "Open song", path: "/projects/Open song", current: true, status: "Ready",
          detail: "", bytes: 0, entries: 0 },
        { name: "Old song", path: "/projects/Old song", current: false, status: "Ready",
          detail: "", bytes: 3145728, entries: 2 }
    ]
    property string log: ""
    function refresh() {}
    function set_budget_mib(mib) {}
    function clear_current() {}
    function clear_all() {}
    function forget_project(path) { log += "forget:" + path + ";" }
}
)QML";

QQuickItem* find_named(QQuickItem* parent, const QString& name) {
  if (parent->objectName() == name)
    return parent;
  for (auto* child : parent->childItems()) {
    if (auto* found = find_named(child, name))
      return found;
  }
  return nullptr;
}

QQuickItem* named(QQuickItem* view, const QString& name) {
  auto* item = find_named(view, name);
  require(item != nullptr, ("missing " + name).toStdString());
  return item;
}

std::unique_ptr<QObject> make_fake(QQmlEngine& engine, const char* source, const QString& name) {
  QQmlComponent component(&engine);
  component.setData(QByteArray(source), QUrl::fromLocalFile(name));
  std::unique_ptr<QObject> fake(component.create());
  require(fake != nullptr, ("fake did not load: " + component.errorString()).toStdString());
  return fake;
}

// One view in a shown window, parented before its bindings complete.
struct ViewHost {
  QQuickWindow window;
  std::unique_ptr<QObject> object;
  QQuickItem* view = nullptr;

  ViewHost(QQmlEngine& engine, const QString& url, const QVariantMap& properties) {
    window.resize(window_width, window_height);
    QQmlComponent component(&engine, QUrl(url));
    object.reset(component.beginCreate(engine.rootContext()));
    require(object != nullptr, (url + " did not load: " + component.errorString()).toStdString());
    view = qobject_cast<QQuickItem*>(object.get());
    require(view != nullptr, (url + " is not an item").toStdString());
    view->setParentItem(window.contentItem());
    component.setInitialProperties(object.get(), properties);
    component.completeCreate();
    window.show();
    require(QTest::qWaitForWindowExposed(&window), "view window exposed");
    QCoreApplication::processEvents();
  }

  void click(QQuickItem* item) {
    const auto centre = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, centre.toPoint());
    QCoreApplication::processEvents();
  }
};

void call(QObject* object, const char* method) {
  require(QMetaObject::invokeMethod(object, method), std::string("could not call ") + method);
  QCoreApplication::processEvents();
}

// The name field starts at the first free name, follows it while untouched,
// and Create sends that name.
void welcome_offers_the_first_free_name() {
  QQmlEngine engine;
  auto bridge = make_fake(engine, fake_bridge, QStringLiteral("FakeBridge.qml"));
  engine.rootContext()->setContextProperty(QStringLiteral("projectBridge"), bridge.get());
  ViewHost host(engine, QStringLiteral("qrc:/WelcomeScreen.qml"), {});
  host.view->setSize(QSizeF(window_width, window_height));
  auto* name = named(host.view, QStringLiteral("newProjectNameInput"));
  require(name->property("text").toString() == QStringLiteral("Untitled Project 2"),
          "the name field does not start at the suggested name");
  call(host.view, "create");
  require(bridge->property("log").toString() == QStringLiteral("create:Untitled Project 2;"),
          "Create did not send the suggested name");
  bridge->setProperty("suggestedProjectName", QStringLiteral("Untitled Project 3"));
  QCoreApplication::processEvents();
  require(name->property("text").toString() == QStringLiteral("Untitled Project 3"),
          "the name field did not move on to the next free name");
}

// Opening offers the suggested copy name; Save sends the typed name and
// closes the panel; an empty name cannot be saved.
void save_copy_panel_sends_the_name() {
  QQmlEngine engine;
  auto host_object = make_fake(engine, fake_host, QStringLiteral("FakeHost.qml"));
  auto bridge = make_fake(engine, fake_bridge, QStringLiteral("FakeBridge.qml"));
  ViewHost host(engine, QStringLiteral("qrc:/SaveCopyPanel.qml"),
                {{QStringLiteral("host"), QVariant::fromValue(host_object.get())},
                 {QStringLiteral("bridge"), QVariant::fromValue(bridge.get())}});
  require(!host.view->isVisible(), "the panel shows before it is asked for");
  host_object->setProperty("saveCopyPanelOpen", true);
  QCoreApplication::processEvents();
  require(host.view->isVisible(), "the panel does not open");
  auto* name = named(host.view, QStringLiteral("saveCopyNameInput"));
  auto* save = named(host.view, QStringLiteral("saveCopyButton"));
  require(name->property("text").toString() == QStringLiteral("Song copy"),
          "the panel does not offer the suggested copy name");
  require(save->isEnabled(), "Save is disabled with a name");

  name->setProperty("text", QString());
  QCoreApplication::processEvents();
  require(!save->isEnabled(), "Save is enabled without a name");
  call(host.view, "save");
  require(bridge->property("log").toString().isEmpty(), "an empty name was sent");

  name->setProperty("text", QStringLiteral(" Radio edit "));
  QCoreApplication::processEvents();
  host.click(save);
  require(bridge->property("log").toString() == QStringLiteral("copy:Radio edit;"),
          "Save did not send the trimmed name");
  require(!host_object->property("saveCopyPanelOpen").toBool(), "the panel stayed open");
}

// Forget is offered on other projects only, asks first, and Cancel keeps the
// project; Forget in the question sends its path.
void cache_panel_confirms_forgetting() {
  QQmlEngine engine;
  auto host_object = make_fake(engine, fake_host, QStringLiteral("FakeHost.qml"));
  auto controls = make_fake(engine, fake_cache_controls, QStringLiteral("FakeCache.qml"));
  ViewHost host(engine, QStringLiteral("qrc:/CachePanel.qml"),
                {{QStringLiteral("host"), QVariant::fromValue(host_object.get())},
                 {QStringLiteral("controls"), QVariant::fromValue(controls.get())}});
  const QString old_path = QStringLiteral("/projects/Old song");
  auto* forget_old = named(host.view, QStringLiteral("cacheForget:") + old_path);
  require(forget_old->isVisible(), "an unopened project offers no Forget");
  require(!named(host.view, QStringLiteral("cacheForget:/projects/Open song"))->isVisible(),
          "the open project offers Forget");
  auto* confirm = named(host.view, QStringLiteral("cacheForgetConfirm"));
  require(!confirm->isVisible(), "the question shows before Forget is pressed");

  host.click(forget_old);
  require(confirm->isVisible(), "Forget did not ask first");
  const auto question =
      named(host.view, QStringLiteral("cacheForgetQuestion"))->property("text").toString();
  require(question.contains(QStringLiteral("Old song")) &&
              question.contains(QStringLiteral("3.0 MiB")),
          "the question does not name the project and its space: " + question.toStdString());
  host.click(named(host.view, QStringLiteral("cacheForgetCancelButton")));
  require(!confirm->isVisible() && controls->property("log").toString().isEmpty(),
          "Cancel forgot the project");

  host.click(forget_old);
  host.click(named(host.view, QStringLiteral("cacheForgetConfirmButton")));
  require(controls->property("log").toString() == QStringLiteral("forget:") + old_path + ';',
          "confirming did not forget the project");
  require(!confirm->isVisible(), "the question stayed after it was answered");

  // From the keyboard; and a project that leaves the list drops its question.
  forget_old->forceActiveFocus();
  QTest::keyClick(&host.window, Qt::Key_Return);
  require(confirm->isVisible(), "Return on Forget did not ask");
  controls->setProperty("projects", controls->property("projects").toList().mid(0, 1));
  oscillate::test::wait_until(
      [&] {
        QCoreApplication::processEvents();
        return host.view->property("confirmPath").toString().isEmpty();
      },
      "the question was dropped with its project");
  require(!confirm->isVisible() && host.view->property("confirmPath").toString().isEmpty(),
          "the question outlived its project");
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication application(argc, argv);
  // The shell's QML lives in the core library's resources; nothing else here
  // pulls them in.
  Q_INIT_RESOURCE(qml);
  qInstallMessageHandler(collect_warnings);
  try {
    welcome_offers_the_first_free_name();
    save_copy_panel_sends_the_name();
    cache_panel_confirms_forgetting();
    require(qml_warnings.isEmpty(), "the project-lifecycle views logged QML warnings");
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    for (const auto& warning : qml_warnings)
      std::cerr << "  " << warning.toStdString() << '\n';
    return 1;
  }
}
