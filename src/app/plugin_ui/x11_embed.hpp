#pragma once

#include <cstdint>

namespace oscillate::plugin_ui {

// Complete the XEmbed handshake for a VST3 editor that created a native child
// of the Qt host window. A plugin without _XEMBED_INFO needs no handshake.
void notify_xembed_child(std::uintptr_t parent_window) noexcept;

} // namespace oscillate::plugin_ui
