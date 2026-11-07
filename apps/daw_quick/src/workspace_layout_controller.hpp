#pragma once

#include <QObject>

namespace oscillate::quick {

// Local workspace preference, separate from the revisioned project model.
class WorkspaceLayoutController final : public QObject {
  Q_OBJECT
  Q_PROPERTY(
      int deviceDockHeight READ device_dock_height WRITE set_device_dock_height NOTIFY changed)
  Q_PROPERTY(
      bool contextPanelOpen READ context_panel_open WRITE set_context_panel_open NOTIFY changed)
  // The floating instrument window's size, dragged by its grip; 0 until the
  // user resizes it (the panel's own size then).
  Q_PROPERTY(int instrumentWindowWidth READ instrument_window_width WRITE
                 set_instrument_window_width NOTIFY changed)
  Q_PROPERTY(int instrumentWindowHeight READ instrument_window_height WRITE
                 set_instrument_window_height NOTIFY changed)
  // Nova's zoom chip (ADR-0076), as a scale (1 is 100 %).
  Q_PROPERTY(double novaZoom READ nova_zoom WRITE set_nova_zoom NOTIFY changed)

public:
  explicit WorkspaceLayoutController(QObject* parent = nullptr);

  [[nodiscard]] int device_dock_height() const noexcept { return device_dock_height_; }
  [[nodiscard]] bool context_panel_open() const noexcept { return context_panel_open_; }
  [[nodiscard]] int instrument_window_width() const noexcept { return instrument_window_width_; }
  [[nodiscard]] int instrument_window_height() const noexcept { return instrument_window_height_; }
  [[nodiscard]] double nova_zoom() const noexcept { return nova_zoom_; }
  void set_device_dock_height(int height);
  void set_context_panel_open(bool open);
  void set_instrument_window_width(int width);
  void set_instrument_window_height(int height);
  void set_nova_zoom(double zoom);

signals:
  void changed();

private:
  int device_dock_height_ = 340;
  bool context_panel_open_ = false;
  int instrument_window_width_ = 0;
  int instrument_window_height_ = 0;
  double nova_zoom_ = 1.0;
};

} // namespace oscillate::quick
