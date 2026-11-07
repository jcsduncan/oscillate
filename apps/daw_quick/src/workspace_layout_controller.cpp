#include "workspace_layout_controller.hpp"

#include <QSettings>

#include <algorithm>

namespace oscillate::quick {
namespace {
constexpr auto dock_height_key = "workspace/deviceDockHeight";
constexpr auto context_open_key = "workspace/contextPanelOpen";
constexpr auto instrument_width_key = "workspace/instrumentWindowWidth";
constexpr auto instrument_height_key = "workspace/instrumentWindowHeight";
constexpr auto nova_zoom_key = "workspace/novaZoom";
// A saved window size beyond any screen, or a zoom no panel offers, is a
// corrupt setting: the panel's own chips pick the zoom within these.
constexpr int largest_window = 16384;
constexpr double smallest_zoom = 0.25;
constexpr double largest_zoom = 4.0;

// 0 (not resized) or the saved size, within 0..largest_window.
int window_size(int size) { return std::clamp(size, 0, largest_window); }
} // namespace

WorkspaceLayoutController::WorkspaceLayoutController(QObject* parent) : QObject(parent) {
  QSettings settings;
  device_dock_height_ = std::clamp(settings.value(dock_height_key, 360).toInt(), 270, 700);
  context_panel_open_ = settings.value(context_open_key, false).toBool();
  instrument_window_width_ = window_size(settings.value(instrument_width_key, 0).toInt());
  instrument_window_height_ = window_size(settings.value(instrument_height_key, 0).toInt());
  nova_zoom_ =
      std::clamp(settings.value(nova_zoom_key, 1.0).toDouble(), smallest_zoom, largest_zoom);
}

void WorkspaceLayoutController::set_device_dock_height(int height) {
  const auto bounded = std::clamp(height, 270, 700);
  if (bounded == device_dock_height_)
    return;
  device_dock_height_ = bounded;
  QSettings{}.setValue(dock_height_key, bounded);
  emit changed();
}

void WorkspaceLayoutController::set_context_panel_open(bool open) {
  if (open == context_panel_open_)
    return;
  context_panel_open_ = open;
  QSettings{}.setValue(context_open_key, open);
  emit changed();
}

void WorkspaceLayoutController::set_instrument_window_width(int width) {
  const auto bounded = window_size(width);
  if (bounded == instrument_window_width_)
    return;
  instrument_window_width_ = bounded;
  QSettings{}.setValue(instrument_width_key, bounded);
  emit changed();
}

void WorkspaceLayoutController::set_instrument_window_height(int height) {
  const auto bounded = window_size(height);
  if (bounded == instrument_window_height_)
    return;
  instrument_window_height_ = bounded;
  QSettings{}.setValue(instrument_height_key, bounded);
  emit changed();
}

void WorkspaceLayoutController::set_nova_zoom(double zoom) {
  const auto bounded = std::clamp(zoom, smallest_zoom, largest_zoom);
  if (bounded == nova_zoom_)
    return;
  nova_zoom_ = bounded;
  QSettings{}.setValue(nova_zoom_key, bounded);
  emit changed();
}

} // namespace oscillate::quick
