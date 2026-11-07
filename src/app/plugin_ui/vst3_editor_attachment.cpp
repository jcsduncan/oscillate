#include "vst3_editor_attachment.hpp"
#include "qt_vst3_run_loop.hpp"
#include "x11_embed.hpp"

#include "vst3_effect_host_internal.hpp"

#include <QGuiApplication>
#include <QPalette>
#include <QScreen>
#include <QScrollArea>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>

namespace oscillate::plugin_ui {

namespace {
void fit_editor_shell(QWidget& shell, const QWidget& content) {
  const auto* screen = QGuiApplication::primaryScreen();
  const auto available = screen ? screen->availableGeometry().size() : QSize{1280, 800};
  const auto max_width = std::max(320, std::min(1200, available.width() - 96));
  const auto max_height = std::max(240, std::min(800, available.height() - 96));
  shell.resize(std::min(content.width() + 24, max_width),
               std::min(content.height() + 24, max_height));
}
} // namespace

class Vst3EditorAttachment::Frame final : public Steinberg::IPlugFrame {
public:
  Frame(QtVst3RunLoop& run_loop, QWidget& window, QWidget& shell, Steinberg::IPlugView& view)
      : run_loop_(run_loop), window_(window), shell_(shell), view_(view) {}
  Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** object) override {
    if (!object || !iid)
      return Steinberg::kInvalidArgument;
    *object = nullptr;
    if (std::memcmp(iid, Steinberg::Linux::IRunLoop_iid, sizeof(Steinberg::TUID)) == 0)
      return run_loop_.queryInterface(iid, object);
    if (std::memcmp(iid, Steinberg::IPlugFrame_iid, sizeof(Steinberg::TUID)) != 0 &&
        std::memcmp(iid, Steinberg::FUnknown_iid, sizeof(Steinberg::TUID)) != 0)
      return Steinberg::kNoInterface;
    *object = this;
    addRef();
    return Steinberg::kResultOk;
  }
  Steinberg::uint32 PLUGIN_API addRef() override { return ++references_; }
  Steinberg::uint32 PLUGIN_API release() override { return --references_; }
  Steinberg::tresult PLUGIN_API resizeView(Steinberg::IPlugView* view,
                                           Steinberg::ViewRect* rect) override {
    if (QThread::currentThread() != window_.thread() || view != &view_ || !rect)
      return Steinberg::kInvalidArgument;
    const auto width = static_cast<int>(rect->right) - static_cast<int>(rect->left);
    const auto height = static_cast<int>(rect->bottom) - static_cast<int>(rect->top);
    if (width < 32 || height < 32 || width > 4096 || height > 4096)
      return Steinberg::kInvalidArgument;
    window_.resize(width, height);
    fit_editor_shell(shell_, window_);
    if (!attached_) {
      pending_ = *rect;
      return Steinberg::kResultTrue;
    }
    try {
      return view_.onSize(rect);
    } catch (...) {
      return Steinberg::kResultFalse;
    }
  }
  void set_attached() {
    attached_ = true;
    if (pending_) {
      auto rect = *pending_;
      pending_.reset();
      try {
        view_.onSize(&rect);
      } catch (...) { // NOLINT(bugprone-empty-catch)
      }
    }
  }

private:
  QtVst3RunLoop& run_loop_;
  QWidget& window_;
  QWidget& shell_;
  Steinberg::IPlugView& view_;
  std::optional<Steinberg::ViewRect> pending_;
  Steinberg::uint32 references_{1};
  bool attached_{false};
};

Vst3EditorAttachment::Vst3EditorAttachment() = default;
Vst3EditorAttachment::~Vst3EditorAttachment() { close(); }

EditorOpenStatus Vst3EditorAttachment::open(plugins::Vst3EffectHost& host) {
  close();
  if (QGuiApplication::platformName() != QStringLiteral("xcb"))
    return QGuiApplication::platformName().startsWith(QStringLiteral("wayland"))
               ? EditorOpenStatus::native_wayland_unavailable
               : EditorOpenStatus::no_native_window;
  plugins::Vst3EditorProbe status{};
  auto view = host.create_editor_view(&status);
  if (!view) {
    switch (status) {
    case plugins::Vst3EditorProbe::no_controller:
      return EditorOpenStatus::no_controller;
    case plugins::Vst3EditorProbe::unsupported_platform:
      return EditorOpenStatus::unsupported_platform;
    case plugins::Vst3EditorProbe::host_unavailable:
      return EditorOpenStatus::host_unavailable;
    case plugins::Vst3EditorProbe::plugin_exception:
      return EditorOpenStatus::plugin_exception;
    default:
      return EditorOpenStatus::no_view;
    }
  }
  return open_view(std::move(view));
}

EditorOpenStatus Vst3EditorAttachment::open_view(std::unique_ptr<plugins::Vst3EditorView> view) {
  close();
  if (QGuiApplication::platformName() != QStringLiteral("xcb"))
    return QGuiApplication::platformName().startsWith(QStringLiteral("wayland"))
               ? EditorOpenStatus::native_wayland_unavailable
               : EditorOpenStatus::no_native_window;
  view_ = std::move(view);
  if (!view_)
    return EditorOpenStatus::no_view;
  try {
    shell_ = std::make_unique<QWidget>();
    auto palette = shell_->palette();
    palette.setColor(QPalette::Window, QColor{11, 14, 19});
    shell_->setPalette(palette);
    shell_->setAutoFillBackground(true);
    auto* layout = new QVBoxLayout(shell_.get());
    layout->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(shell_.get());
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(false);
    scroll->viewport()->setPalette(palette);
    scroll->viewport()->setAutoFillBackground(true);
    window_ = new QWidget();
    scroll->setWidget(window_);
    layout->addWidget(scroll);
    window_->setAttribute(Qt::WA_NativeWindow);
    window_->setMinimumSize(32, 32);
    window_->setMaximumSize(4096, 4096);
    window_->resize(32, 32);
    const auto native_id = window_->winId();
    if (native_id == 0) {
      close();
      return EditorOpenStatus::no_native_window;
    }
    run_loop_ = std::make_unique<QtVst3RunLoop>();
    frame_ = std::make_unique<Frame>(*run_loop_, *window_, *shell_, *view_->view());
    auto* plugin_view = view_->view();
    if (plugin_view->setFrame(frame_.get()) != Steinberg::kResultTrue) {
      close();
      return EditorOpenStatus::frame_rejected;
    }
    Steinberg::ViewRect size{};
    if (plugin_view->getSize(&size) == Steinberg::kResultTrue) {
      const auto width = static_cast<int>(size.right) - static_cast<int>(size.left);
      const auto height = static_cast<int>(size.bottom) - static_cast<int>(size.top);
      if (width >= 32 && height >= 32 && width <= 4096 && height <= 4096) {
        window_->resize(width, height);
        fit_editor_shell(*shell_, *window_);
      }
    }
    attach_attempted_ = true;
    // The VST3 X11 ABI requires the numeric Window ID in this pointer slot.
    const auto result = plugin_view->attached( // NOLINTNEXTLINE(performance-no-int-to-ptr)
        reinterpret_cast<void*>(native_id), Steinberg::kPlatformTypeX11EmbedWindowID);
    if (result != Steinberg::kResultTrue) {
      close();
      return EditorOpenStatus::attach_failed;
    }
    attached_ = true;
    frame_->set_attached();
    return EditorOpenStatus::attached;
  } catch (...) {
    close();
    return EditorOpenStatus::plugin_exception;
  }
}

void Vst3EditorAttachment::show() {
  if (!attached_ || !shell_ || !window_)
    return;
  shell_->show();
  notify_xembed_child(window_->winId());
  try {
    (void)view_->view()->onFocus(true);
  } catch (...) { // NOLINT(bugprone-empty-catch)
  }
  QTimer::singleShot(0, shell_.get(), [this] {
    if (attached_ && window_)
      notify_xembed_child(window_->winId());
  });
}

void Vst3EditorAttachment::close() noexcept {
  if (view_) {
    auto* plugin_view = view_->view();
    if (attached_) {
      try {
        (void)plugin_view->onFocus(false);
      } catch (...) { // NOLINT(bugprone-empty-catch)
      }
    }
    if (attach_attempted_) {
      try {
        plugin_view->removed();
      } catch (...) { // NOLINT(bugprone-empty-catch)
      }
    }
    try {
      plugin_view->setFrame(nullptr);
    } catch (...) { // NOLINT(bugprone-empty-catch)
    }
  }
  attached_ = false;
  attach_attempted_ = false;
  // The view goes first: references it still holds to the frame or the run
  // loop are released while both are alive.
  view_.reset();
  frame_.reset();
  run_loop_.reset();
  shell_.reset();
  window_ = nullptr;
}

} // namespace oscillate::plugin_ui
