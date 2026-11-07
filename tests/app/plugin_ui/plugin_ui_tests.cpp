#include "oscillate/plugins/vst3_bundle_loader.hpp"
#include "oscillate/plugins/vst3_scan.hpp"
#include "oscillate/test/check.hpp"
#include "qt_vst3_run_loop.hpp"
#include "vst3_editor_attachment.hpp"
#include "vst3_effect_host_internal.hpp"

#include <QApplication>
#include <QEvent>
#include <QEventLoop>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QScreen>
#include <QTimer>
#include <QWindow>

#include <array>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

#include <unistd.h>

namespace {
using oscillate::test::require;

class EventHandler final : public Steinberg::Linux::IEventHandler {
public:
  EventHandler(QEventLoop& loop, int fd) : loop_(loop), fd_(fd) {}
  void add_fd(int fd) { extra_fd_ = fd; }
  Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** object) override {
    if (!object)
      return Steinberg::kInvalidArgument;
    *object = nullptr;
    if (std::memcmp(iid, Steinberg::Linux::IEventHandler_iid, sizeof(Steinberg::TUID)) != 0 &&
        std::memcmp(iid, Steinberg::FUnknown_iid, sizeof(Steinberg::TUID)) != 0)
      return Steinberg::kNoInterface;
    *object = this;
    addRef();
    return Steinberg::kResultOk;
  }
  Steinberg::uint32 PLUGIN_API addRef() override { return ++references_; }
  Steinberg::uint32 PLUGIN_API release() override { return --references_; }
  void PLUGIN_API onFDIsSet(Steinberg::Linux::FileDescriptor fd) override {
    std::array<char, 1> byte{};
    if ((fd == fd_ || fd == extra_fd_) && ::read(fd, byte.data(), byte.size()) == 1)
      ++calls;
    loop_.quit();
  }
  int calls{0};
  [[nodiscard]] Steinberg::uint32 references() const { return references_; }

private:
  QEventLoop& loop_;
  int fd_;
  int extra_fd_{-1};
  Steinberg::uint32 references_{1};
};

class TimerHandler final : public Steinberg::Linux::ITimerHandler {
public:
  explicit TimerHandler(QEventLoop& loop) : loop_(loop) {}
  Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** object) override {
    if (!object)
      return Steinberg::kInvalidArgument;
    *object = nullptr;
    if (std::memcmp(iid, Steinberg::Linux::ITimerHandler_iid, sizeof(Steinberg::TUID)) != 0 &&
        std::memcmp(iid, Steinberg::FUnknown_iid, sizeof(Steinberg::TUID)) != 0)
      return Steinberg::kNoInterface;
    *object = this;
    addRef();
    return Steinberg::kResultOk;
  }
  Steinberg::uint32 PLUGIN_API addRef() override { return ++references_; }
  Steinberg::uint32 PLUGIN_API release() override { return --references_; }
  void PLUGIN_API onTimer() override {
    ++calls;
    loop_.quit();
    if (throw_on_timer)
      throw std::runtime_error("synthetic timer exception");
  }
  int calls{0};
  bool throw_on_timer{false};
  [[nodiscard]] Steinberg::uint32 references() const { return references_; }

private:
  QEventLoop& loop_;
  Steinberg::uint32 references_{1};
};

struct EditorStats {
  int destroyed{0};
  int attached{0};
  int removed{0};
  int resized{0};
  int timer_fired{0};
  bool reject_frame{false};
  bool reject_attach{false};
  bool throw_attach{false};
  bool throw_resize{false};
  Steinberg::IPlugFrame* frame{nullptr};
  Steinberg::IPlugView* view{nullptr};
};

class EditorView final : public Steinberg::IPlugView, public Steinberg::Linux::ITimerHandler {
public:
  explicit EditorView(EditorStats& stats) : stats_(stats) { stats_.view = this; }
  ~EditorView() {
    ++stats_.destroyed;
    stats_.view = nullptr;
  }
  Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** object) override {
    if (!object)
      return Steinberg::kInvalidArgument;
    *object = nullptr;
    if (std::memcmp(iid, Steinberg::IPlugView_iid, sizeof(Steinberg::TUID)) == 0 ||
        std::memcmp(iid, Steinberg::FUnknown_iid, sizeof(Steinberg::TUID)) == 0)
      *object = static_cast<Steinberg::IPlugView*>(this);
    else if (std::memcmp(iid, Steinberg::Linux::ITimerHandler_iid, sizeof(Steinberg::TUID)) == 0)
      *object = static_cast<Steinberg::Linux::ITimerHandler*>(this);
    else
      return Steinberg::kNoInterface;
    addRef();
    return Steinberg::kResultOk;
  }
  Steinberg::uint32 PLUGIN_API addRef() override { return ++references_; }
  Steinberg::uint32 PLUGIN_API release() override {
    const auto remaining = --references_;
    if (remaining == 0)
      delete this;
    return remaining;
  }
  Steinberg::tresult PLUGIN_API isPlatformTypeSupported(Steinberg::FIDString type) override {
    return type && std::strcmp(type, Steinberg::kPlatformTypeX11EmbedWindowID) == 0
               ? Steinberg::kResultTrue
               : Steinberg::kResultFalse;
  }
  Steinberg::tresult PLUGIN_API attached(void* parent, Steinberg::FIDString type) override {
    if (stats_.throw_attach)
      throw std::runtime_error("synthetic editor attach exception");
    if (!parent || isPlatformTypeSupported(type) != Steinberg::kResultTrue || !frame_)
      return Steinberg::kResultFalse;
    ++stats_.attached;
    if (stats_.reject_attach)
      return Steinberg::kResultFalse;
    void* queried = nullptr;
    if (frame_->queryInterface(Steinberg::Linux::IRunLoop_iid, &queried) != Steinberg::kResultOk)
      return Steinberg::kResultFalse;
    run_loop_ = static_cast<Steinberg::Linux::IRunLoop*>(queried);
    if (run_loop_->registerTimer(this, 1) != Steinberg::kResultTrue)
      return Steinberg::kResultFalse;
    Steinberg::ViewRect size{0, 0, 200, 100};
    return frame_->resizeView(this, &size);
  }
  Steinberg::tresult PLUGIN_API removed() override {
    ++stats_.removed;
    if (run_loop_) {
      run_loop_->unregisterTimer(this);
      run_loop_->release();
      run_loop_ = nullptr;
    }
    return Steinberg::kResultTrue;
  }
  Steinberg::tresult PLUGIN_API onWheel(float) override { return Steinberg::kResultFalse; }
  Steinberg::tresult PLUGIN_API onKeyDown(Steinberg::char16, Steinberg::int16,
                                          Steinberg::int16) override {
    return Steinberg::kResultFalse;
  }
  Steinberg::tresult PLUGIN_API onKeyUp(Steinberg::char16, Steinberg::int16,
                                        Steinberg::int16) override {
    return Steinberg::kResultFalse;
  }
  Steinberg::tresult PLUGIN_API getSize(Steinberg::ViewRect* size) override {
    if (!size)
      return Steinberg::kInvalidArgument;
    *size = {0, 0, 200, 100};
    return Steinberg::kResultTrue;
  }
  Steinberg::tresult PLUGIN_API onSize(Steinberg::ViewRect*) override {
    if (stats_.throw_resize)
      throw std::runtime_error("synthetic resize exception");
    ++stats_.resized;
    return Steinberg::kResultTrue;
  }
  Steinberg::tresult PLUGIN_API onFocus(Steinberg::TBool) override {
    return Steinberg::kResultTrue;
  }
  Steinberg::tresult PLUGIN_API setFrame(Steinberg::IPlugFrame* frame) override {
    frame_ = frame;
    stats_.frame = frame;
    return stats_.reject_frame && frame ? Steinberg::kResultFalse : Steinberg::kResultTrue;
  }
  Steinberg::tresult PLUGIN_API canResize() override { return Steinberg::kResultTrue; }
  Steinberg::tresult PLUGIN_API checkSizeConstraint(Steinberg::ViewRect*) override {
    return Steinberg::kResultTrue;
  }
  void PLUGIN_API onTimer() override { ++stats_.timer_fired; }

private:
  EditorStats& stats_;
  Steinberg::IPlugFrame* frame_{nullptr};
  Steinberg::Linux::IRunLoop* run_loop_{nullptr};
  Steinberg::uint32 references_{1};
};

void editor_contract(bool x11, bool wayland) {
  EditorStats stats;
  auto open_flag = std::make_shared<bool>(true);
  auto view =
      std::make_unique<oscillate::plugins::Vst3EditorView>(new EditorView(stats), open_flag);
  oscillate::plugin_ui::Vst3EditorAttachment editor;
  const auto result = editor.open_view(std::move(view));
  if (!x11) {
    const auto expected = wayland
                              ? oscillate::plugin_ui::EditorOpenStatus::native_wayland_unavailable
                              : oscillate::plugin_ui::EditorOpenStatus::no_native_window;
    REQUIRE_EQ(result, expected, "platform reports its native-editor fallback reason");
    REQUIRE_EQ(stats.attached, 0, "fallback releases unattached view");
    REQUIRE_EQ(stats.destroyed, 1, "fallback releases unattached view");
    REQUIRE_TRUE(!*open_flag, "fallback releases unattached view");
    return;
  }
  REQUIRE_EQ(result, oscillate::plugin_ui::EditorOpenStatus::attached,
             "synthetic view attached to Qt X11 native window");
  REQUIRE_TRUE(editor.attached(), "synthetic view attached to Qt X11 native window");
  REQUIRE_TRUE(editor.widget(), "editor size and resize request applied");
  REQUIRE_EQ(editor.widget()->width(), 200, "editor size and resize request applied");
  REQUIRE_EQ(editor.widget()->height(), 100, "editor size and resize request applied");
  REQUIRE_EQ(stats.resized, 1, "editor size and resize request applied");
  void* queried = nullptr;
  REQUIRE_EQ(stats.frame->queryInterface(Steinberg::IPlugFrame_iid, &queried), Steinberg::kResultOk,
             "editor frame interface available");
  REQUIRE_EQ(queried, stats.frame, "editor frame interface available");
  REQUIRE_EQ(static_cast<Steinberg::IPlugFrame*>(queried)->release(), 1,
             "frame query reference balanced");
  REQUIRE_EQ(stats.frame->queryInterface(Steinberg::IPlugView_iid, &queried),
             Steinberg::kNoInterface, "unrelated frame interface rejected");
  REQUIRE_EQ(stats.frame->queryInterface(Steinberg::IPlugFrame_iid, nullptr),
             Steinberg::kInvalidArgument, "null frame query rejected");
  Steinberg::ViewRect resized{0, 0, 256, 120};
  REQUIRE_EQ(stats.frame->resizeView(nullptr, &resized), Steinberg::kInvalidArgument,
             "foreign view and null size rejected");
  REQUIRE_EQ(stats.frame->resizeView(stats.view, nullptr), Steinberg::kInvalidArgument,
             "foreign view and null size rejected");
  Steinberg::ViewRect oversized{0, 0, 5000, 120};
  REQUIRE_EQ(stats.frame->resizeView(stats.view, &oversized), Steinberg::kInvalidArgument,
             "oversized editor rejected");
  REQUIRE_EQ(stats.frame->resizeView(stats.view, &resized), Steinberg::kResultTrue,
             "attached editor resize forwarded");
  REQUIRE_EQ(stats.resized, 2, "attached editor resize forwarded");
  REQUIRE_EQ(editor.widget()->width(), 256, "attached editor resize forwarded");
  stats.throw_resize = true;
  REQUIRE_EQ(stats.frame->resizeView(stats.view, &resized), Steinberg::kResultFalse,
             "editor resize exception contained");
  stats.throw_resize = false;
  Steinberg::ViewRect large{0, 0, 3000, 2000};
  REQUIRE_EQ(stats.frame->resizeView(stats.view, &large), Steinberg::kResultTrue,
             "large plugin view remains full-sized inside a bounded scrollable host");
  REQUIRE_EQ(editor.widget()->width(), 3000,
             "large plugin view remains full-sized inside a bounded scrollable host");
  REQUIRE_EQ(editor.widget()->height(), 2000,
             "large plugin view remains full-sized inside a bounded scrollable host");
  REQUIRE_LE(editor.window_shell()->width(), 1200,
             "large plugin view remains full-sized inside a bounded scrollable host");
  REQUIRE_LE(editor.window_shell()->height(), 800,
             "large plugin view remains full-sized inside a bounded scrollable host");
  QEventLoop events;
  QTimer::singleShot(20, &events, &QEventLoop::quit);
  events.exec();
  REQUIRE_GT(stats.timer_fired, 0, "attached editor timer dispatched");
  editor.close();
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  REQUIRE_EQ(stats.removed, 1, "editor detached before view and window destruction");
  REQUIRE_EQ(stats.destroyed, 1, "editor detached before view and window destruction");
  REQUIRE_TRUE(!*open_flag, "editor detached before view and window destruction");

  const auto failure_case = [](EditorStats& failure_stats,
                               oscillate::plugin_ui::EditorOpenStatus expected) {
    auto lease_flag = std::make_shared<bool>(true);
    auto lease = std::make_unique<oscillate::plugins::Vst3EditorView>(new EditorView(failure_stats),
                                                                      lease_flag);
    oscillate::plugin_ui::Vst3EditorAttachment failed_editor;
    REQUIRE_EQ(failed_editor.open_view(std::move(lease)), expected,
               "failed attach returns specific fallback reason");
    REQUIRE_TRUE(!failed_editor.attached(), "failed attach releases editor view");
    REQUIRE_EQ(failure_stats.destroyed, 1, "failed attach releases editor view");
    REQUIRE_TRUE(!*lease_flag, "failed attach releases editor view");
  };
  EditorStats rejected_frame;
  rejected_frame.reject_frame = true;
  failure_case(rejected_frame, oscillate::plugin_ui::EditorOpenStatus::frame_rejected);
  REQUIRE_EQ(rejected_frame.attached, 0, "rejected frame prevents attachment");
  EditorStats rejected_attach;
  rejected_attach.reject_attach = true;
  failure_case(rejected_attach, oscillate::plugin_ui::EditorOpenStatus::attach_failed);
  REQUIRE_EQ(rejected_attach.attached, 1, "failed attachment receives best-effort removal");
  REQUIRE_EQ(rejected_attach.removed, 1, "failed attachment receives best-effort removal");
  EditorStats thrown_attach;
  thrown_attach.throw_attach = true;
  failure_case(thrown_attach, oscillate::plugin_ui::EditorOpenStatus::plugin_exception);
  REQUIRE_EQ(thrown_attach.removed, 1, "throwing attachment receives best-effort removal");
}

void loaded_host_fallback() {
  oscillate::plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  const auto receipt = scanner.scan(VST3_CONNECTED_BUNDLE);
  REQUIRE_EQ(receipt.status, oscillate::plugins::ScanStatus::ok,
             "connected fixture scan succeeded");
  const auto found = std::ranges::find_if(receipt.metadata.classes, [](const auto& klass) {
    return klass.category == "Audio Module Class" && klass.name == "Connected Gain";
  });
  REQUIRE_NE(found, receipt.metadata.classes.end(), "connected processor class present");
  std::string error;
  auto host =
      oscillate::plugins::load_verified_vst3_effect({*found, receipt.binary}, 48000.0, 128, error);
  REQUIRE_NE(host, nullptr, "connected fixture host loaded");
  oscillate::plugin_ui::Vst3EditorAttachment editor;
  REQUIRE_EQ(editor.open(host->host()), oscillate::plugin_ui::EditorOpenStatus::no_view,
             "controller without editor reports generic fallback");
  REQUIRE_TRUE(!editor.attached(), "missing view creates no native window");
  REQUIRE_TRUE(!editor.widget(), "missing view creates no native window");
}

void embedded_editor_contract() {
  EditorStats stats;
  auto open_flag = std::make_shared<bool>(true);
  auto view =
      std::make_unique<oscillate::plugins::Vst3EditorView>(new EditorView(stats), open_flag);
  oscillate::plugin_ui::Vst3EditorAttachment editor;
  REQUIRE_EQ(editor.open_view(std::move(view)), oscillate::plugin_ui::EditorOpenStatus::attached,
             "synthetic editor did not attach before Qt Quick embedding");
  auto* native = editor.window_shell()->windowHandle();
  REQUIRE_NE(native, nullptr, "native editor shell has no QWindow");
  QQmlEngine engine;
  engine.rootContext()->setContextProperty("nativeEditorWindow", native);
  QQmlComponent component(&engine);
  component.setData(R"(
    import QtQuick
    Window {
      visible: true
      x: -10000; y: -10000
      width: 500; height: 300
      WindowContainer {
        objectName: "embeddedPlugin"
        x: 10; y: 10
        width: 400; height: 200
        window: nativeEditorWindow
      }
    }
  )",
                    QUrl{});
  std::unique_ptr<QObject> root(component.create());
  REQUIRE_TRUE(root, "Qt Quick native editor container failed to load");
  REQUIRE_TRUE(qobject_cast<QQuickWindow*>(root.get()),
               "Qt Quick native editor container failed to load");
  editor.show();
  QCoreApplication::processEvents();
  REQUIRE_EQ(native->parent(), root.get(),
             "Qt Quick did not own the embedded editor geometry and visibility");
  REQUIRE_TRUE(editor.window_shell()->isVisible(),
               "Qt Quick did not own the embedded editor geometry and visibility");
  auto* container = root->findChild<QObject*>(QStringLiteral("embeddedPlugin"));
  REQUIRE_TRUE(container, "embedded plugin lost its native window");
  REQUIRE_EQ(container->property("window").value<QWindow*>(), native,
             "embedded plugin lost its native window");
  container->setProperty("window", QVariant::fromValue(static_cast<QWindow*>(nullptr)));
  QCoreApplication::processEvents();
  editor.close();
  REQUIRE_EQ(stats.removed, 1, "embedded editor did not detach before native window deletion");
  REQUIRE_EQ(stats.destroyed, 1, "embedded editor did not detach before native window deletion");
  REQUIRE_TRUE(!*open_flag, "embedded editor did not detach before native window deletion");
}

void run() {
  oscillate::plugin_ui::QtVst3RunLoop loop;
  void* queried = nullptr;
  REQUIRE_EQ(loop.queryInterface(Steinberg::Linux::IRunLoop_iid, &queried), Steinberg::kResultOk,
             "run loop interface available");
  REQUIRE_EQ(queried, &loop, "run loop interface available");
  REQUIRE_EQ(loop.release(), 1, "query reference balanced");
  REQUIRE_EQ(loop.queryInterface(Steinberg::IPlugFrame_iid, &queried), Steinberg::kNoInterface,
             "other interface rejected");

  QEventLoop events;
  int pipe_fd[2]{};
  REQUIRE_EQ(::pipe(pipe_fd), 0, "pipe fixture");
  EventHandler fd_handler(events, pipe_fd[0]);
  REQUIRE_EQ(loop.registerEventHandler(&fd_handler, pipe_fd[0]), Steinberg::kResultTrue,
             "fd handler registered");
  REQUIRE_EQ(fd_handler.references(), 2, "fd handler retained");
  REQUIRE_EQ(loop.registerEventHandler(&fd_handler, pipe_fd[0]), Steinberg::kInvalidArgument,
             "duplicate fd rejected");
  const char byte = 'x';
  REQUIRE_EQ(::write(pipe_fd[1], &byte, 1), 1, "fd made readable");
  QTimer::singleShot(1000, &events, &QEventLoop::quit);
  events.exec();
  REQUIRE_EQ(fd_handler.calls, 1, "readable fd dispatched once");
  int second_pipe[2]{};
  REQUIRE_EQ(::pipe(second_pipe), 0, "second pipe fixture");
  fd_handler.add_fd(second_pipe[0]);
  REQUIRE_EQ(loop.registerEventHandler(&fd_handler, second_pipe[0]), Steinberg::kResultTrue,
             "one plugin handler may service multiple descriptors");
  REQUIRE_EQ(fd_handler.references(), 3, "each registered descriptor retains its handler");
  REQUIRE_EQ(::write(second_pipe[1], &byte, 1), 1, "second fd made readable");
  QTimer::singleShot(1000, &events, &QEventLoop::quit);
  events.exec();
  REQUIRE_EQ(fd_handler.calls, 2, "second descriptor dispatched to same handler");
  REQUIRE_EQ(loop.unregisterEventHandler(&fd_handler), Steinberg::kResultTrue,
             "all descriptors of fd handler unregistered");
  REQUIRE_EQ(fd_handler.references(), 1, "fd handler released");
  REQUIRE_EQ(loop.unregisterEventHandler(&fd_handler), Steinberg::kResultFalse,
             "missing fd handler reported");
  ::close(pipe_fd[0]);
  ::close(pipe_fd[1]);
  ::close(second_pipe[0]);
  ::close(second_pipe[1]);

  TimerHandler timer_handler(events);
  REQUIRE_EQ(loop.registerTimer(&timer_handler, 1), Steinberg::kResultTrue, "timer registered");
  REQUIRE_EQ(loop.registerTimer(&timer_handler, 1), Steinberg::kInvalidArgument,
             "duplicate timer rejected");
  QTimer::singleShot(1000, &events, &QEventLoop::quit);
  events.exec();
  REQUIRE_GE(timer_handler.calls, 1, "timer dispatched");
  REQUIRE_EQ(loop.unregisterTimer(&timer_handler), Steinberg::kResultTrue, "timer unregistered");
  REQUIRE_EQ(timer_handler.references(), 1, "timer released");
  REQUIRE_EQ(loop.registerTimer(&timer_handler, 0), Steinberg::kInvalidArgument,
             "zero interval rejected");
  Steinberg::tresult other_thread_result = Steinberg::kResultOk;
  std::thread other([&] { other_thread_result = loop.registerTimer(&timer_handler, 1); });
  other.join();
  REQUIRE_EQ(other_thread_result, Steinberg::kInvalidArgument,
             "registration from non-UI thread rejected");
  {
    oscillate::plugin_ui::QtVst3RunLoop cleanup_loop;
    REQUIRE_EQ(cleanup_loop.registerTimer(&timer_handler, 1000), Steinberg::kResultTrue,
               "active timer registered for owner cleanup");
    REQUIRE_EQ(timer_handler.references(), 2, "owner retains active timer");
  }
  REQUIRE_EQ(timer_handler.references(), 1, "owner destruction releases active timer");
  timer_handler.throw_on_timer = true;
  REQUIRE_EQ(loop.registerTimer(&timer_handler, 1), Steinberg::kResultTrue,
             "throwing timer registered");
  QTimer::singleShot(1000, &events, &QEventLoop::quit);
  events.exec();
  REQUIRE_GE(timer_handler.calls, 2, "throwing timer quarantined and released");
  REQUIRE_EQ(timer_handler.references(), 1, "throwing timer quarantined and released");
  REQUIRE_EQ(loop.unregisterTimer(&timer_handler), Steinberg::kResultFalse,
             "throwing timer quarantined and released");
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}
} // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  try {
    run();
    const bool x11 = argc > 1 && std::strcmp(argv[1], "--x11") == 0;
    const bool wayland = argc > 1 && std::strcmp(argv[1], "--wayland") == 0;
    editor_contract(x11, wayland);
    if (x11) {
      embedded_editor_contract();
      loaded_host_fallback();
    }
    std::cout << "plugin UI run loop contracts passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
