#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include <optional>
#include <string_view>

namespace oscillate::quick {

// Fuzzy matching for the command palette, Qt-free so it is tested alone.
// Every query character must appear in the text in order, case-insensitive.
// A higher score is a better match: consecutive runs, word starts ("at"
// for "Add Track") and an early first match score more; spaces in the query
// are ignored. nullopt when the text does not match; an empty query
// matches everything with score 0.
[[nodiscard]] std::optional<int> fuzzy_score(std::string_view query, std::string_view text);

// The command palette (Ctrl+K): QML builds the entries (actions, tracks,
// devices, presets) and this ranks them. Recently run entries come first
// with no query and score higher with one; they are kept across sessions.
class CommandPalette final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QStringList recents READ recents NOTIFY recentsChanged)
  // The shell's motion preference (QSettings "ui/reducedMotion"): on, the
  // shell's transitions are instant. The palette toggles it.
  Q_PROPERTY(
      bool reducedMotion READ reduced_motion WRITE set_reduced_motion NOTIFY reducedMotionChanged)

public:
  static constexpr int max_recents = 12;

  explicit CommandPalette(QObject* parent = nullptr);

  [[nodiscard]] QStringList recents() const { return recents_; }
  // `entries` are maps with at least "key" and "title", optionally
  // "keywords" (matched too, scored lower) and "enabled". Returns at most
  // `limit` of them, best first, each with "score" added. Disabled entries
  // rank below every enabled one.
  Q_INVOKABLE QVariantList search(const QVariantList& entries, const QString& query,
                                  int limit) const;
  // Records that the entry `key` was run.
  Q_INVOKABLE void note_used(const QString& key);
  [[nodiscard]] bool reduced_motion() const { return reduced_motion_; }
  void set_reduced_motion(bool on);

signals:
  void recentsChanged();
  void reducedMotionChanged();

private:
  QStringList recents_;
  bool reduced_motion_ = false;
};

} // namespace oscillate::quick
