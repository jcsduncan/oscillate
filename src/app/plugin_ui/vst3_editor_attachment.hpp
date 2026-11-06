#pragma once

#include "oscillate/plugins/vst3_effect_host.hpp"

#include <QWidget>

#include <cstdint>
#include <memory>

namespace oscillate::plugin_ui {

class QtVst3RunLoop;

enum class EditorOpenStatus : std::uint8_t {
  attached,
  native_wayland_unavailable,
  no_native_window,
  no_view,
  no_controller,
  unsupported_platform,
  host_unavailable,
  frame_rejected,
  attach_failed,
  plugin_exception,
};

// UI-thread native editor owner. Its QWidget can be placed in a Qt Widgets
// container; Qt Quick composition may host that container in a later slice.
// The host, its bundle and the Qt application outlive this object.
class Vst3EditorAttachment final {
public:
  Vst3EditorAttachment();
  ~Vst3EditorAttachment();
  Vst3EditorAttachment(const Vst3EditorAttachment&) = delete;
  Vst3EditorAttachment& operator=(const Vst3EditorAttachment&) = delete;

  [[nodiscard]] EditorOpenStatus open(plugins::Vst3EffectHost& host);
  // Takes an already-created lease; useful when the composition root owns
  // view creation separately. The host must outlive this attachment.
  [[nodiscard]] EditorOpenStatus open_view(std::unique_ptr<plugins::Vst3EditorView> view);
  void show();
  void close() noexcept;
  [[nodiscard]] QWidget* widget() const noexcept { return window_; }
  // Bounded top-level host around the plugin's native child. A large fixed
  // plugin view scrolls inside it instead of filling the desktop.
  [[nodiscard]] QWidget* window_shell() const noexcept { return shell_.get(); }
  [[nodiscard]] bool attached() const noexcept { return attached_; }

private:
  class Frame;
  std::unique_ptr<QWidget> shell_;
  // Owned by the scroll area inside shell_.
  QWidget* window_ = nullptr;
  std::unique_ptr<plugins::Vst3EditorView> view_;
  std::unique_ptr<QtVst3RunLoop> run_loop_;
  std::unique_ptr<Frame> frame_;
  bool attached_{false};
  bool attach_attempted_{false};
};

} // namespace oscillate::plugin_ui
