#include "computer_keyboard.hpp"
#include "oscillate/test/check.hpp"

#include <QGuiApplication>
#include <QKeyEvent>

#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// The computer keyboard as a musical keyboard: the layout other DAWs use,
// octave and velocity keys, held notes across octave changes, and taking its
// keys from the app's shortcuts only while it is on.
namespace {
using oscillate::quick::ComputerKeyboard;

using oscillate::test::require;

struct Recorder {
  std::vector<std::pair<int, int>> played; // key, velocity
  std::vector<int> released;
};

bool send(ComputerKeyboard& keyboard, QObject& target, QEvent::Type type, int key,
          Qt::KeyboardModifiers modifiers = Qt::NoModifier, bool repeat = false) {
  QKeyEvent event(type, key, modifiers, QString(), repeat);
  return keyboard.eventFilter(&target, &event);
}

void layout_octaves_and_velocity() {
  Recorder r;
  ComputerKeyboard keyboard([&](int key, int velocity) { r.played.emplace_back(key, velocity); },
                            [&](int key) { r.released.push_back(key); });
  QObject target;
  require(!send(keyboard, target, QEvent::KeyPress, Qt::Key_A), "off, the keyboard plays nothing");
  keyboard.set_enabled(true);
  require(keyboard.range() == QStringLiteral("C4–F5"), "the range starts at middle C");
  require(send(keyboard, target, QEvent::KeyPress, Qt::Key_A) &&
              send(keyboard, target, QEvent::KeyPress, Qt::Key_W) &&
              send(keyboard, target, QEvent::KeyPress, Qt::Key_K),
          "note keys are taken");
  require(r.played == std::vector<std::pair<int, int>>{{60, 100}, {61, 100}, {72, 100}},
          "A, W and K play C4, C♯4 and C5");
  require(send(keyboard, target, QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, true) &&
              r.played.size() == 3,
          "auto-repeat does not retrigger");
  // An octave change while A is held still releases the note A started.
  require(send(keyboard, target, QEvent::KeyPress, Qt::Key_X) && keyboard.octave() == 5,
          "X moves up an octave");
  require(send(keyboard, target, QEvent::KeyRelease, Qt::Key_A) && r.released == std::vector{60},
          "a held key releases the note it started");
  require(send(keyboard, target, QEvent::KeyPress, Qt::Key_A) && r.played.back().first == 72,
          "after X, A plays C5");
  require(send(keyboard, target, QEvent::KeyPress, Qt::Key_C) && keyboard.velocity() == 80,
          "C lowers the velocity");
  require(send(keyboard, target, QEvent::KeyPress, Qt::Key_D) &&
              r.played.back() == std::pair{76, 80},
          "notes use the new velocity");
  for (int i = 0; i < 12; ++i)
    (void)send(keyboard, target, QEvent::KeyPress, Qt::Key_Z);
  require(keyboard.octave() == -1, "Z stops at the lowest octave");
  keyboard.set_enabled(false);
  require(r.released.size() == 5, "turning off releases every held note");
}

void shortcuts_and_modifiers() {
  Recorder r;
  ComputerKeyboard keyboard([&](int key, int velocity) { r.played.emplace_back(key, velocity); },
                            [&](int key) { r.released.push_back(key); });
  QObject target;
  keyboard.set_enabled(true);
  QKeyEvent override_a(QEvent::ShortcutOverride, Qt::Key_A, Qt::NoModifier);
  require(keyboard.eventFilter(&target, &override_a) && override_a.isAccepted(),
          "the keyboard takes A from the shortcuts while on");
  QKeyEvent override_space(QEvent::ShortcutOverride, Qt::Key_Space, Qt::NoModifier);
  require(!keyboard.eventFilter(&target, &override_space), "Space stays play / pause");
  require(!send(keyboard, target, QEvent::KeyPress, Qt::Key_S, Qt::ControlModifier) &&
              r.played.empty(),
          "Ctrl+S still saves");
  require(!send(keyboard, target, QEvent::KeyPress, Qt::Key_M), "M stays a shortcut");
}

// In key, the home row walks up the project key's scale from its root and
// the row above plays the same notes an octave up; with no key set it stays
// chromatic.
void in_key_follows_the_scale() {
  Recorder r;
  ComputerKeyboard keyboard([&](int key, int velocity) { r.played.emplace_back(key, velocity); },
                            [&](int key) { r.released.push_back(key); });
  QObject target;
  keyboard.set_enabled(true);
  keyboard.set_in_key(true);
  require(send(keyboard, target, QEvent::KeyPress, Qt::Key_W) && r.played.back().first == 61,
          "with no key set, In key is chromatic");
  (void)send(keyboard, target, QEvent::KeyRelease, Qt::Key_W);
  // C minor: C D E♭ F G A♭ B♭.
  keyboard.set_scale({0, 2, 3, 5, 7, 8, 10}, QStringLiteral("C minor"));
  const std::vector<std::pair<int, int>> home{{Qt::Key_A, 60}, {Qt::Key_S, 62}, {Qt::Key_D, 63},
                                              {Qt::Key_F, 65}, {Qt::Key_G, 67}, {Qt::Key_H, 68},
                                              {Qt::Key_J, 70}, {Qt::Key_K, 72}, {Qt::Key_L, 74}};
  for (const auto& [key, note] : home) {
    require(send(keyboard, target, QEvent::KeyPress, key) && r.played.back().first == note,
            "the home row walks up C minor");
    (void)send(keyboard, target, QEvent::KeyRelease, key);
  }
  require(send(keyboard, target, QEvent::KeyPress, Qt::Key_W) && r.played.back().first == 72,
          "W plays the root an octave up");
  require(send(keyboard, target, QEvent::KeyPress, Qt::Key_R) && r.played.back().first == 75,
          "R plays the third an octave up");
  require(keyboard.range() == QStringLiteral("C minor · C4–D6"), "the range names the key");
  // A minor's root is A: the home row starts there.
  keyboard.set_scale({9, 11, 0, 2, 4, 5, 7}, QStringLiteral("A minor"));
  require(r.released.size() >= 2, "a new key releases the held notes");
  require(send(keyboard, target, QEvent::KeyPress, Qt::Key_A) && r.played.back().first == 69 &&
              send(keyboard, target, QEvent::KeyPress, Qt::Key_D) && r.played.back().first == 72,
          "the home row starts on the key's root");
  keyboard.set_in_key(false);
  require(send(keyboard, target, QEvent::KeyPress, Qt::Key_E) && r.played.back().first == 63,
          "off again, the layout is chromatic");
  require(!send(keyboard, target, QEvent::KeyPress, Qt::Key_R), "chromatic leaves R alone");
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  try {
    layout_octaves_and_velocity();
    shortcuts_and_modifiers();
    in_key_follows_the_scale();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "computer keyboard contract passed\n";
  return 0;
}
