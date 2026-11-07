#include "command_palette.hpp"

#include <QSettings>
#include <QVariantMap>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <string>
#include <vector>

namespace oscillate::quick {
namespace {

constexpr auto recents_key = "commandPalette/recents";
constexpr auto reduced_motion_key = "ui/reducedMotion";
constexpr int word_start_bonus = 6;
constexpr int consecutive_bonus = 4;
constexpr int keyword_penalty = 8;
constexpr int recent_bonus = 12;
constexpr int disabled_penalty = 1'000;

char lower(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }
bool separator(char c) noexcept {
  return c == ' ' || c == '-' || c == '_' || c == '.' || c == '/' || c == ':' || c == '(' ||
         c == '\'' || c == '"';
}
bool word_start(std::string_view text, std::size_t at) noexcept {
  if (at == 0)
    return true;
  const auto previous = text[at - 1];
  const auto current = text[at];
  // "PumpPanel": the P of Panel starts a word too.
  return separator(previous) ||
         (previous >= 'a' && previous <= 'z' && current >= 'A' && current <= 'Z');
}

} // namespace

std::optional<int> fuzzy_score(std::string_view query, std::string_view text) {
  std::string needle;
  needle.reserve(query.size());
  for (const auto c : query)
    if (c != ' ')
      needle.push_back(lower(c));
  if (needle.empty())
    return 0;
  const auto m = needle.size();
  const auto n = text.size();
  if (m > n)
    return std::nullopt;
  constexpr int none = std::numeric_limits<int>::min() / 2;
  // best[j]: the best score with the current query character at text[j].
  std::vector<int> previous(n, none);
  std::vector<int> current(n, none);
  for (std::size_t i = 0; i < m; ++i) {
    int running = none; // Best of previous[0 .. j - 2].
    for (std::size_t j = 0; j < n; ++j) {
      if (j >= 2)
        running = std::max(running, previous[j - 2]);
      current[j] = none;
      if (lower(text[j]) != needle[i])
        continue;
      const int bonus = 1 + (word_start(text, j) ? word_start_bonus : 0);
      if (i == 0) {
        // An early start reads better: "Add" over "Load".
        current[j] = bonus - static_cast<int>(std::min<std::size_t>(j, 8));
        continue;
      }
      if (j == 0)
        continue;
      const int joined = previous[j - 1] == none ? none : previous[j - 1] + consecutive_bonus;
      const int best = std::max(running, joined);
      if (best != none)
        current[j] = best + bonus;
    }
    std::swap(previous, current);
  }
  const auto best = *std::ranges::max_element(previous);
  if (best == none)
    return std::nullopt;
  // A whole-text match ("Undo" for "undo") beats a longer title.
  return best + (m == n ? 4 : 0);
}

CommandPalette::CommandPalette(QObject* parent)
    : QObject(parent), recents_(QSettings{}.value(QString::fromLatin1(recents_key)).toStringList()),
      reduced_motion_(QSettings{}.value(QString::fromLatin1(reduced_motion_key)).toBool()) {}

QVariantList CommandPalette::search(const QVariantList& entries, const QString& query,
                                    int limit) const {
  struct Ranked {
    QVariantMap entry;
    int score;
    int order;
  };
  const auto needle = query.trimmed().toUtf8();
  std::vector<Ranked> ranked;
  ranked.reserve(static_cast<std::size_t>(entries.size()));
  int order = 0;
  for (const auto& value : entries) {
    auto entry = value.toMap();
    const auto key = entry.value(QStringLiteral("key")).toString();
    const auto recent = recents_.indexOf(key);
    std::optional<int> score;
    if (needle.isEmpty()) {
      // No query: recents first, newest at the top, then the given order.
      score = recent >= 0 ? max_recents - static_cast<int>(recent) : 0;
    } else {
      const auto title = entry.value(QStringLiteral("title")).toString().toUtf8();
      score = fuzzy_score(needle.toStdString(), title.toStdString());
      const auto keywords = entry.value(QStringLiteral("keywords")).toString().toUtf8();
      if (!keywords.isEmpty()) {
        if (const auto other = fuzzy_score(needle.toStdString(), keywords.toStdString()))
          score =
              std::max(score.value_or(std::numeric_limits<int>::min()), *other - keyword_penalty);
      }
      if (score && recent >= 0)
        *score += recent_bonus - static_cast<int>(recent);
    }
    if (!score)
      continue;
    if (!needle.isEmpty() && !entry.value(QStringLiteral("enabled"), true).toBool())
      *score -= disabled_penalty;
    entry.insert(QStringLiteral("score"), *score);
    ranked.push_back({std::move(entry), *score, order++});
  }
  std::ranges::stable_sort(ranked, [](const Ranked& a, const Ranked& b) {
    return a.score != b.score ? a.score > b.score : a.order < b.order;
  });
  QVariantList result;
  const auto count =
      std::min<std::size_t>(ranked.size(), static_cast<std::size_t>(std::max(limit, 0)));
  result.reserve(static_cast<qsizetype>(count));
  for (std::size_t index = 0; index < count; ++index)
    result.push_back(ranked[index].entry);
  return result;
}

void CommandPalette::note_used(const QString& key) {
  if (key.isEmpty())
    return;
  recents_.removeAll(key);
  recents_.prepend(key);
  while (recents_.size() > max_recents)
    recents_.removeLast();
  QSettings{}.setValue(QString::fromLatin1(recents_key), recents_);
  emit recentsChanged();
}

void CommandPalette::set_reduced_motion(bool on) {
  if (on == reduced_motion_)
    return;
  reduced_motion_ = on;
  QSettings{}.setValue(QString::fromLatin1(reduced_motion_key), on);
  emit reducedMotionChanged();
}

} // namespace oscillate::quick
