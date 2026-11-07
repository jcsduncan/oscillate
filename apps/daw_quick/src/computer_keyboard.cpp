#include "computer_keyboard.hpp"

#include <QGuiApplication>
#include <QKeyEvent>
#include <QVariant>

#include <algorithm>

namespace oscillate::quick {
namespace {

// Chromatic: semitones up from the octave's C, in this order.
constexpr std::array<int, 18> chromatic_keys{
    Qt::Key_A, Qt::Key_W, Qt::Key_S, Qt::Key_E, Qt::Key_D,         Qt::Key_F,
    Qt::Key_T, Qt::Key_G, Qt::Key_Y, Qt::Key_H, Qt::Key_U,         Qt::Key_J,
    Qt::Key_K, Qt::Key_O, Qt::Key_L, Qt::Key_P, Qt::Key_Semicolon, Qt::Key_Apostrophe};
// In key: scale degrees up from the root. The home row from degree 0, the
// row above from an octave up (W sits over A and S).
constexpr std::array<int, 11> home_row{Qt::Key_A, Qt::Key_S,         Qt::Key_D,         Qt::Key_F,
                                       Qt::Key_G, Qt::Key_H,         Qt::Key_J,         Qt::Key_K,
                                       Qt::Key_L, Qt::Key_Semicolon, Qt::Key_Apostrophe};
constexpr std::array<int, 9> upper_row{Qt::Key_W, Qt::Key_E, Qt::Key_R, Qt::Key_T, Qt::Key_Y,
                                       Qt::Key_U, Qt::Key_I, Qt::Key_O, Qt::Key_P};
// Every key either layout reads: the chromatic keys, then R and I.
constexpr std::array<int, 20> keys{
    Qt::Key_A, Qt::Key_W, Qt::Key_S,         Qt::Key_E,          Qt::Key_D, Qt::Key_F, Qt::Key_T,
    Qt::Key_G, Qt::Key_Y, Qt::Key_H,         Qt::Key_U,          Qt::Key_J, Qt::Key_K, Qt::Key_O,
    Qt::Key_L, Qt::Key_P, Qt::Key_Semicolon, Qt::Key_Apostrophe, Qt::Key_R, Qt::Key_I};
constexpr int velocity_step = 20;

QString note_name(int key) {
  static const std::array<const char*, 12> names{"C",  "C♯", "D",  "D♯", "E",  "F",
                                                 "F♯", "G",  "G♯", "A",  "A♯", "B"};
  return QString::fromUtf8(names[static_cast<std::size_t>(key % 12)]) +
         QString::number(key / 12 - 1);
}

} // namespace

ComputerKeyboard::ComputerKeyboard(Play play, Release release, QObject* parent)
    : QObject(parent), play_(std::move(play)), release_(std::move(release)) {
  held_.fill(-1);
}

void ComputerKeyboard::set_enabled(bool enabled) {
  if (enabled == enabled_)
    return;
  if (!enabled)
    release_all();
  enabled_ = enabled;
  emit changed();
}

void ComputerKeyboard::set_in_key(bool in_key) {
  if (in_key == in_key_)
    return;
  release_all();
  in_key_ = in_key;
  emit changed();
}

void ComputerKeyboard::set_scale(const std::vector<int>& pitch_classes, const QString& name) {
  release_all();
  scale_.clear();
  root_ = pitch_classes.empty() ? 0 : pitch_classes.front();
  for (const int pitch : pitch_classes)
    scale_.push_back(((pitch - root_) % 12 + 12) % 12);
  scale_name_ = pitch_classes.empty() ? QString() : name;
  emit changed();
}

int ComputerKeyboard::note_for(int slot) const noexcept {
  if (slot < 0)
    return -1;
  const int key = keys[static_cast<std::size_t>(slot)];
  if (!scale_active()) {
    const auto found = std::ranges::find(chromatic_keys, key);
    return found == chromatic_keys.end()
               ? -1
               : 12 * (octave_ + 1) + static_cast<int>(found - chromatic_keys.begin());
  }
  int degree = -1;
  if (const auto home = std::ranges::find(home_row, key); home != home_row.end())
    degree = static_cast<int>(home - home_row.begin());
  else if (const auto upper = std::ranges::find(upper_row, key); upper != upper_row.end())
    degree = static_cast<int>(scale_.size()) + static_cast<int>(upper - upper_row.begin());
  if (degree < 0)
    return -1;
  const int count = static_cast<int>(scale_.size());
  return 12 * (octave_ + 1) + root_ + 12 * (degree / count) +
         scale_[static_cast<std::size_t>(degree % count)];
}

QString ComputerKeyboard::range() const {
  if (!scale_active()) {
    const int low = 12 * (octave_ + 1);
    return note_name(low) + QStringLiteral("–") +
           note_name(low + static_cast<int>(chromatic_keys.size()) - 1);
  }
  const auto highest = std::ranges::find(keys, upper_row.back());
  return scale_name_ + QStringLiteral(" · ") + note_name(note_for(0)) + QStringLiteral("–") +
         note_name(note_for(static_cast<int>(highest - keys.begin())));
}

void ComputerKeyboard::release_all() {
  for (auto& key : held_) {
    if (key >= 0)
      release_(key);
    key = -1;
  }
}

int ComputerKeyboard::slot(int qt_key) noexcept {
  const auto found = std::ranges::find(keys, qt_key);
  return found == keys.end() ? -1 : static_cast<int>(found - keys.begin());
}

bool ComputerKeyboard::typing_text() noexcept {
  const QObject* focus = QGuiApplication::focusObject();
  return focus != nullptr && focus->property("cursorPosition").isValid();
}

bool ComputerKeyboard::eventFilter(QObject* watched, QEvent* event) {
  const auto type = event->type();
  if (type == QEvent::ApplicationStateChange && enabled_ &&
      QGuiApplication::applicationState() != Qt::ApplicationActive) {
    release_all();
    return QObject::eventFilter(watched, event);
  }
  if (!enabled_ ||
      (type != QEvent::KeyPress && type != QEvent::KeyRelease && type != QEvent::ShortcutOverride))
    return QObject::eventFilter(watched, event);
  // Key events reach the filter once per object they are sent to; act on
  // the one sent to the focus object (or the window when nothing has focus).
  if (watched != QGuiApplication::focusObject() && QGuiApplication::focusObject() != nullptr)
    return QObject::eventFilter(watched, event);
  auto* key_event = static_cast<QKeyEvent*>(event);
  // A held key's release always ends its note, even once a modifier is down
  // or a text field has focus, so no note is left sounding.
  if (type == QEvent::KeyRelease && !key_event->isAutoRepeat()) {
    const int released = slot(key_event->key());
    if (released >= 0 && held_[static_cast<std::size_t>(released)] >= 0) {
      release_(held_[static_cast<std::size_t>(released)]);
      held_[static_cast<std::size_t>(released)] = -1;
      return true;
    }
  }
  if (key_event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier) ||
      typing_text())
    return QObject::eventFilter(watched, event);
  const int key = key_event->key();
  const int pressed = slot(key);
  const int note = note_for(pressed);
  const bool control = key == Qt::Key_Z || key == Qt::Key_X || key == Qt::Key_C || key == Qt::Key_V;
  if (note < 0 && !control)
    return QObject::eventFilter(watched, event);
  // Ours: keep the app's single-letter shortcuts from also firing.
  if (type == QEvent::ShortcutOverride) {
    event->accept();
    return true;
  }
  if (key_event->isAutoRepeat())
    return true;
  if (type == QEvent::KeyRelease) {
    if (pressed >= 0 && held_[static_cast<std::size_t>(pressed)] >= 0) {
      release_(held_[static_cast<std::size_t>(pressed)]);
      held_[static_cast<std::size_t>(pressed)] = -1;
    }
    return true;
  }
  if (control) {
    if (key == Qt::Key_Z)
      octave_ = std::max(-1, octave_ - 1);
    else if (key == Qt::Key_X)
      octave_ = std::min(8, octave_ + 1);
    else if (key == Qt::Key_C)
      velocity_ = std::max(1, velocity_ - velocity_step);
    else
      velocity_ = std::min(127, velocity_ + velocity_step);
    emit changed();
    return true;
  }
  if (note > 127 || held_[static_cast<std::size_t>(pressed)] >= 0)
    return true;
  held_[static_cast<std::size_t>(pressed)] = note;
  play_(note, velocity_);
  return true;
}

} // namespace oscillate::quick
