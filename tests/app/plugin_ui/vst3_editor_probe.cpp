#include "oscillate/plugins/vst3_bundle_loader.hpp"
#include "oscillate/plugins/vst3_scan.hpp"
#include "vst3_editor_attachment.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QScreen>
#include <QThread>
#include <QWidget>

#include <X11/Xlib.h>
#include <X11/keysym.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <dlfcn.h>

namespace {
using namespace oscillate;

[[nodiscard]] std::string_view status_name(plugin_ui::EditorOpenStatus status) {
  using enum plugin_ui::EditorOpenStatus;
  switch (status) {
  case attached:
    return "attached";
  case native_wayland_unavailable:
    return "native_wayland_unavailable";
  case no_native_window:
    return "no_native_window";
  case no_view:
    return "no_view";
  case no_controller:
    return "no_controller";
  case unsupported_platform:
    return "unsupported_platform";
  case host_unavailable:
    return "host_unavailable";
  case frame_rejected:
    return "frame_rejected";
  case attach_failed:
    return "attach_failed";
  case plugin_exception:
    return "plugin_exception";
  }
  return "unknown";
}

void pump_events(int milliseconds) {
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < milliseconds) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    QThread::msleep(1);
  }
}

struct Drag {
  int from_x;
  int from_y;
  int to_x;
  int to_y;
};

[[nodiscard]] std::uintptr_t inject_x11_input(const QWidget& shell, Drag drag) {
  using Motion = int (*)(Display*, int, int, int, unsigned long);
  using Button = int (*)(Display*, unsigned int, Bool, unsigned long);
  using Key = int (*)(Display*, unsigned int, Bool, unsigned long);
  void* library = dlopen("libXtst.so.6", RTLD_NOW | RTLD_LOCAL);
  if (!library)
    throw std::runtime_error("libXtst.so.6 is unavailable for opt-in pointer check");
  auto* display = XOpenDisplay(nullptr);
  if (!display) {
    dlclose(library);
    throw std::runtime_error("X11 display unavailable for opt-in pointer check");
  }
  const auto motion = reinterpret_cast<Motion>(dlsym(library, "XTestFakeMotionEvent"));
  const auto button = reinterpret_cast<Button>(dlsym(library, "XTestFakeButtonEvent"));
  const auto key = reinterpret_cast<Key>(dlsym(library, "XTestFakeKeyEvent"));
  const auto from = shell.mapToGlobal(QPoint{drag.from_x, drag.from_y});
  const auto to = shell.mapToGlobal(QPoint{drag.to_x, drag.to_y});
  std::cout << "x11_drag_from_global=" << from.x() << ',' << from.y() << " to_global=" << to.x()
            << ',' << to.y() << " shell_window=" << static_cast<std::uintptr_t>(shell.winId())
            << '\n';
  int translated_x = 0;
  int translated_y = 0;
  Window translated_child = None;
  if (XTranslateCoordinates(display, static_cast<Window>(shell.winId()), DefaultRootWindow(display),
                            drag.from_x, drag.from_y, &translated_x, &translated_y,
                            &translated_child))
    std::cout << "x11_drag_from_translated=" << translated_x << ',' << translated_y << '\n';
  const bool available = motion && button && key;
  if (available) {
    motion(display, -1, from.x(), from.y(), 0);
    button(display, 1, True, 0);
    motion(display, -1, to.x(), to.y(), 0);
    button(display, 1, False, 0);
    const auto tab = XKeysymToKeycode(display, XK_Tab);
    key(display, tab, True, 0);
    key(display, tab, False, 0);
    XSync(display, False);
  }
  Window root = None;
  Window child = None;
  int root_x = 0;
  int root_y = 0;
  int window_x = 0;
  int window_y = 0;
  unsigned int mask = 0;
  if (XQueryPointer(display, DefaultRootWindow(display), &root, &child, &root_x, &root_y, &window_x,
                    &window_y, &mask))
    std::cout << "x11_pointer_root=" << root_x << ',' << root_y
              << " first_child=" << static_cast<std::uintptr_t>(child) << '\n';
  Window at_pointer = DefaultRootWindow(display);
  for (int depth = 0; depth < 12; ++depth) {
    if (!XQueryPointer(display, at_pointer, &root, &child, &root_x, &root_y, &window_x, &window_y,
                       &mask) ||
        child == None)
      break;
    std::cout << "x11_pointer_child_" << depth << '=' << static_cast<std::uintptr_t>(child)
              << " local=" << window_x << ',' << window_y << '\n';
    at_pointer = child;
  }
  Window focus = None;
  int revert_to = RevertToNone;
  XGetInputFocus(display, &focus, &revert_to);
  XCloseDisplay(display);
  dlclose(library);
  if (!available)
    throw std::runtime_error("libXtst does not export the expected XTest input calls");
  return static_cast<std::uintptr_t>(focus);
}

[[nodiscard]] const plugins::Vst3Class& select_class(const plugins::ScanResult& scan,
                                                     std::string_view class_id) {
  const auto found = std::ranges::find(scan.metadata.classes, class_id, &plugins::Vst3Class::id);
  if (found == scan.metadata.classes.end())
    throw std::runtime_error("selected class ID was not in the verified scan");
  return *found;
}

int run(const std::filesystem::path& bundle, std::string_view class_id, bool instrument,
        const std::optional<Drag>& drag, bool suspend_host, bool embed) {
  if (QGuiApplication::platformName() != QStringLiteral("xcb"))
    throw std::runtime_error("set QT_QPA_PLATFORM=xcb and use an isolated X11 display");
  plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  const auto scan = scanner.scan(bundle);
  if (!scan.ok())
    throw std::runtime_error("out-of-process scan failed: " + scan.diagnostic);
  const auto& klass = select_class(scan, class_id);
  std::string error;
  auto loaded =
      instrument ? plugins::load_verified_vst3_instrument({klass, scan.binary}, 48000.0, 128, error)
                 : plugins::load_verified_vst3_effect({klass, scan.binary}, 48000.0, 128, error);
  if (!loaded)
    throw std::runtime_error("verified plugin load failed: " + error);

  std::cout << "bundle=" << scan.binary.canonical_bundle << '\n'
            << "class_id=" << klass.id << '\n'
            << "class_name=" << klass.name << '\n'
            << "sha256=" << scan.binary.sha256 << '\n'
            << "platform=" << QGuiApplication::platformName().toStdString() << '\n';
  auto& host = loaded->host();
  if (suspend_host)
    host.suspend();
  host.begin_editor_parameter_capture();
  for (int cycle = 0; cycle < 5; ++cycle) {
    plugin_ui::Vst3EditorAttachment editor;
    const auto status = editor.open(host);
    std::cout << "cycle_" << cycle << "_open=" << status_name(status) << '\n';
    if (status != plugin_ui::EditorOpenStatus::attached)
      return 2;
    auto* shell = editor.window_shell();
    if (!shell || !editor.widget())
      throw std::runtime_error("attached editor has no native shell or child");
    // The host deliberately keeps oversized plugin content at its requested
    // size and constrains the viewport, rather than distorting the editor.
    if (shell->width() > 1200 || shell->height() > 800 || shell->width() < 32 ||
        shell->height() < 32)
      throw std::runtime_error("native editor shell exceeded its size contract");
    std::cout << "cycle_" << cycle << "_content=" << editor.widget()->width() << 'x'
              << editor.widget()->height() << '\n'
              << "cycle_" << cycle << "_shell=" << shell->width() << 'x' << shell->height() << '\n';
    shell->move(40, 40);
    std::unique_ptr<QQmlEngine> qml_engine;
    std::unique_ptr<QObject> qml_root;
    QObject* container = nullptr;
    if (embed) {
      auto* native = shell->windowHandle();
      if (!native)
        throw std::runtime_error("native editor shell has no QWindow");
      qml_engine = std::make_unique<QQmlEngine>();
      qml_engine->rootContext()->setContextProperty("nativeEditorWindow", native);
      QQmlComponent component(qml_engine.get());
      component.setData(R"(
        import QtQuick
        Window {
          visible: true
          x: 40; y: 40
          width: 1200; height: 800
          WindowContainer {
            objectName: "surgeEditorContainer"
            x: 12; y: 46
            width: 1100; height: 700
            window: nativeEditorWindow
          }
        }
      )",
                        QUrl{});
      qml_root.reset(component.create());
      if (!qml_root || !qobject_cast<QQuickWindow*>(qml_root.get()))
        throw std::runtime_error("Qt Quick WindowContainer could not be created: " +
                                 component.errorString().toStdString());
      container = qml_root->findChild<QObject*>(QStringLiteral("surgeEditorContainer"));
      if (!container)
        throw std::runtime_error("Qt Quick native editor container was not created");
    }
    editor.show();
    pump_events(250);
    if (embed && shell->windowHandle()->parent() != qml_root.get())
      throw std::runtime_error("native editor was not embedded in Qt Quick");
    if (!editor.attached() || !shell->isVisible())
      throw std::runtime_error("editor detached or hid while its event loop ran");
    if (cycle == 0 && drag) {
      const auto focus = inject_x11_input(*shell, *drag);
      pump_events(250);
      const auto gestures = host.take_completed_editor_gestures();
      std::cout << "x11_drag_completed_gestures="
                << (gestures ? static_cast<int>(gestures->size()) : -1) << '\n'
                << "x11_focus_window_after_tab=" << focus << '\n';
      if (!gestures || gestures->empty())
        throw std::runtime_error("XTest pointer drag caused no completed plugin gesture");
      for (const auto& gesture : *gestures)
        std::cout << "x11_gesture_id=" << gesture.id
                  << " normalized_value=" << gesture.normalized_value << '\n';
      const Drag reverse{drag->to_x, drag->to_y, drag->from_x, drag->from_y};
      (void)inject_x11_input(*shell, reverse);
      pump_events(250);
      const auto reversed = host.take_completed_editor_gestures();
      if (!reversed || reversed->empty() || reversed->front().id != gestures->front().id ||
          reversed->front().normalized_value == gestures->front().normalized_value)
        throw std::runtime_error("reverse drag did not change the same plugin control");
      std::cout << "x11_reverse_normalized_value=" << reversed->front().normalized_value << '\n';
    }
    const auto screenshot = qgetenv("OSCILLATE_EDITOR_PROBE_SCREENSHOT");
    if (cycle == 0 && !screenshot.isEmpty()) {
      auto* screen = shell->screen();
      if (!screen || !screen->grabWindow(shell->winId()).save(QString::fromLocal8Bit(screenshot)))
        throw std::runtime_error("requested first-cycle editor screenshot failed");
    }
    if (container) {
      container->setProperty("window", QVariant::fromValue(static_cast<QWindow*>(nullptr)));
      QCoreApplication::processEvents();
    }
    editor.close();
    qml_root.reset();
    qml_engine.reset();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    pump_events(25);
    if (editor.attached() || editor.widget() || editor.window_shell())
      throw std::runtime_error("editor resources remained after detach");
    std::cout << "cycle_" << cycle << "_close=ok\n";
  }
  const auto edits = host.take_editor_parameter_edits();
  std::cout << "edit_capture=" << (edits ? "available" : "overflow") << '\n';
  return edits ? 0 : 3;
}
} // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  if (argc < 4) {
    std::cerr << "usage: oscillate_vst3_editor_probe BUNDLE CLASS_ID instrument|effect"
                 " [--drag X0 Y0 X1 Y1] [--suspend] [--embed]\n";
    return 64;
  }
  try {
    const std::string_view kind(argv[3]);
    if (kind != "instrument" && kind != "effect")
      throw std::runtime_error("kind must be instrument or effect");
    std::optional<Drag> drag;
    bool suspend_host = false;
    bool embed = false;
    for (int index = 4; index < argc; ++index) {
      const std::string_view option(argv[index]);
      if (option == "--drag" && index + 4 < argc) {
        drag = {std::stoi(argv[++index]), std::stoi(argv[++index]), std::stoi(argv[++index]),
                std::stoi(argv[++index])};
      } else if (option == "--suspend") {
        suspend_host = true;
      } else if (option == "--embed") {
        embed = true;
      } else {
        throw std::runtime_error("unknown or incomplete probe option");
      }
    }
    return run(argv[1], argv[2], kind == "instrument", drag, suspend_host, embed);
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
