#pragma once

#include <QObject>

namespace oscillate::quick {

// Which macro MAP mode is mapping (ADR-0059), shared by the macro strip and
// every device panel's knobs: -1 when off. While on, touching a knob maps it
// to that macro instead of turning it.
class MacroMapState final : public QObject {
  Q_OBJECT
  Q_PROPERTY(int armed READ armed WRITE set_armed NOTIFY armed_changed)

public:
  using QObject::QObject;
  [[nodiscard]] int armed() const noexcept { return armed_; }
  void set_armed(int index) {
    const int next = index >= 0 && index < 8 ? index : -1;
    if (next == armed_)
      return;
    armed_ = next;
    emit armed_changed();
  }

signals:
  void armed_changed();

private:
  int armed_ = -1;
};

} // namespace oscillate::quick
