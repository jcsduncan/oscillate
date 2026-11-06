#pragma once

#include "oscillate/plugins/vst3_effect_host.hpp"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/gui/iplugview.h"

#include <memory>

namespace oscillate::plugins {

// Private SDK-facing half of the public opaque lease. Only plugin_ui may use
// view(); callers must detach and destroy the lease before the host/module.
class Vst3EditorView final {
public:
  Vst3EditorView(Steinberg::IPlugView* view, std::shared_ptr<bool> open_flag) noexcept
      : view_(view), open_flag_(std::move(open_flag)) {}
  ~Vst3EditorView() noexcept {
    if (view_) {
      try {
        view_->release();
      } catch (...) { // NOLINT(bugprone-empty-catch)
      }
    }
    *open_flag_ = false;
  }
  Vst3EditorView(const Vst3EditorView&) = delete;
  Vst3EditorView& operator=(const Vst3EditorView&) = delete;
  [[nodiscard]] Steinberg::IPlugView* view() const noexcept { return view_; }

private:
  Steinberg::IPlugView* view_;
  std::shared_ptr<bool> open_flag_;
};

} // namespace oscillate::plugins

namespace oscillate::plugins::detail {

// The factory remains caller-owned. This narrow seam lets fixture factories
// verify hosting semantics before the bundle loader and project graph are wired.
[[nodiscard]] std::unique_ptr<Vst3EffectHost>
create_effect_host(Steinberg::IPluginFactory& factory, Vst3EffectConfig config, std::string& error);

} // namespace oscillate::plugins::detail
