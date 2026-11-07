#pragma once

#include <QString>

#include <utility>

// A controller's one-line status, as QML shows it: set() says whether the
// text changed, so the owner emits its own notify signal only then.
namespace oscillate::quick {

class StatusText {
public:
  StatusText() = default;
  explicit StatusText(QString text) : text_(std::move(text)) {}

  [[nodiscard]] const QString& text() const noexcept { return text_; }
  // Returns true when the text changed.
  bool set(QString text) {
    if (text == text_)
      return false;
    text_ = std::move(text);
    return true;
  }

private:
  QString text_;
};

} // namespace oscillate::quick
