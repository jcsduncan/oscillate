#pragma once

#include <span>
#include <string_view>

// Each built-in device's designed panel: the QML file the editor windows
// load for it, its design size, and what the panel takes from its host
// beyond the controller. The windows look panels up here by engine id
// instead of naming engines themselves.
namespace oscillate::quick {

struct DevicePanel {
  std::string_view id;
  std::string_view url;
  int width = 0;
  int height = 0;
  // Space-separated panel properties the host binds by name: see the
  // editor windows (NativeSynthEditorOverlay, NativeEffectEditorOverlay).
  std::string_view inputs;
  // The panel lays itself out at any size above its minimum and offers a
  // zoom, rather than scaling its design size (ADR-0076).
  bool reflows = false;
};

[[nodiscard]] std::span<const DevicePanel> device_panels() noexcept;
// The device's panel, or null when it has no designed panel.
[[nodiscard]] const DevicePanel* device_panel(std::string_view id) noexcept;

} // namespace oscillate::quick
