#pragma once

#include <QObject>

#include <array>
#include <functional>
#include <vector>

class QEvent;

namespace oscillate::quick {

// The computer keyboard as a musical keyboard, laid out as in other DAWs:
// the home row plays white keys and the row above black keys, from C
// (A W S E D F T G Y H U J K O L P ; '). Z and X move an octave down and up,
// C and V lower and raise the velocity. In key, with a scale set (the
// project key), the home row (A S D F G H J K L ; ') plays the scale's notes
// up from its root and the row above (W E R T Y U I O P) the same notes an
// octave higher, so every key is in key. While it is on it takes those keys
// from the app's single-letter shortcuts; text fields and keys with Ctrl,
// Alt or Meta are left alone. Installed as an application event filter, so
// it plays wherever focus is.
class ComputerKeyboard final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool enabled READ enabled WRITE set_enabled NOTIFY changed)
  Q_PROPERTY(int octave READ octave NOTIFY changed)
  Q_PROPERTY(int velocity READ velocity NOTIFY changed)
  Q_PROPERTY(QString range READ range NOTIFY changed)
  Q_PROPERTY(bool inKey READ in_key WRITE set_in_key NOTIFY changed)
  // The scale In key plays ("C minor"), empty with no project key.
  Q_PROPERTY(QString scaleName READ scale_name NOTIFY changed)

public:
  using Play = std::function<void(int key, int velocity)>;
  using Release = std::function<void(int key)>;

  ComputerKeyboard(Play play, Release release, QObject* parent = nullptr);

  [[nodiscard]] bool enabled() const noexcept { return enabled_; }
  void set_enabled(bool enabled);
  Q_INVOKABLE void toggle() { set_enabled(!enabled_); }
  // The octave of the A key's C: 4 is middle C (MIDI 60).
  [[nodiscard]] int octave() const noexcept { return octave_; }
  [[nodiscard]] int velocity() const noexcept { return velocity_; }
  // The playable range, e.g. "C4–F5", for the transport's button.
  [[nodiscard]] QString range() const;
  // Releases every key held down, e.g. when the window loses focus.
  void release_all();

  [[nodiscard]] bool in_key() const noexcept { return in_key_; }
  void set_in_key(bool in_key);
  Q_INVOKABLE void toggle_in_key() { set_in_key(!in_key_); }
  [[nodiscard]] QString scale_name() const { return scale_name_; }
  // The project key's seven pitch classes, root first, and its name; empty
  // clears it (In key then plays chromatically).
  void set_scale(const std::vector<int>& pitch_classes, const QString& name);

  bool eventFilter(QObject* watched, QEvent* event) override;

signals:
  void changed();

private:
  // The key's slot in the keys the keyboard reads, or -1.
  [[nodiscard]] static int slot(int qt_key) noexcept;
  // The MIDI note a key's slot plays now, or -1 when it plays none.
  [[nodiscard]] int note_for(int slot) const noexcept;
  [[nodiscard]] bool scale_active() const noexcept { return in_key_ && !scale_.empty(); }
  [[nodiscard]] static bool typing_text() noexcept;

  Play play_;
  Release release_;
  bool enabled_ = false;
  int octave_ = 4;
  int velocity_ = 100;
  bool in_key_ = false;
  // Semitones above the root for each degree, and the root's pitch class.
  std::vector<int> scale_;
  int root_ = 0;
  QString scale_name_;
  // The MIDI key each held key started, so an octave or mode change
  // mid-note still releases the right one; -1 when up.
  std::array<int, 20> held_{};
};

} // namespace oscillate::quick
