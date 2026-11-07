#pragma once

#include "oscillate/test/check.hpp"

#include <QFont>
#include <QQuickItem>
#include <QRegularExpression>
#include <QString>

#include <string>
#include <utility>
#include <vector>

// Checks on the text a device panel shows, shared by the app contracts that
// open every built-in instrument and effect.
namespace oscillate::test::panel_text {

// A text item's shown string: its text as drawn, capitals applied by the
// font included and rich-text tags dropped.
inline QString shown_text(const QQuickItem& item) {
  QString text = item.property("text").toString();
  if (item.property("textFormat").toInt() != 0) {
    static const QRegularExpression tags(QStringLiteral("<[^>]*>"));
    text.remove(tags);
  }
  if (item.property("font").value<QFont>().capitalization() == QFont::AllUppercase) {
    text = text.toUpper();
  }
  return text;
}

inline bool is_text_item(const QQuickItem& item) {
  return item.inherits("QQuickText") || item.inherits("QQuickTextEdit") ||
         item.inherits("QQuickTextInput");
}

// Every visible text item under root.
inline void visible_text_items(QQuickItem* root, std::vector<QQuickItem*>& items) {
  if (!root->isVisible() || root->opacity() <= 0.0) {
    return;
  }
  if (is_text_item(*root) && !root->property("text").toString().isEmpty()) {
    items.push_back(root);
  }
  for (QQuickItem* child : root->childItems()) {
    visible_text_items(child, items);
  }
}

// The display formatter's rules (foundation/value_text.hpp) as patterns a
// shown string must not match, each with what it means.
inline const std::vector<std::pair<QRegularExpression, const char*>>& value_rules() {
  static const std::vector<std::pair<QRegularExpression, const char*>> rules{
      {QRegularExpression(QStringLiteral("(^|[^\\w\\-])-(\\d|\\x{221E}|inf)")),
       "a hyphen-minus where a minus sign (U+2212) belongs"},
      {QRegularExpression(QStringLiteral("[\\x{2212}\\-]0(\\.0+)?(?![\\d.])")), "a negative zero"},
      {QRegularExpression(QStringLiteral("\\b(KHZ|HZ|DB|DBFS|MS)\\b")),
       "a unit set in capitals (kHz, Hz, dB, ms keep their case)"},
      {QRegularExpression(QStringLiteral("\\d S\\b")), "seconds set in capitals (s)"},
      {QRegularExpression(QStringLiteral("\\bdeg\\b"), QRegularExpression::CaseInsensitiveOption),
       "degrees as \"deg\" (°)"},
      {QRegularExpression(QStringLiteral("\\b[A-G]#")), "a sharp written # (♯)"},
      {QRegularExpression(QStringLiteral("\\d\\s*(\\x{00A2}|cents?\\b)"),
                          QRegularExpression::CaseInsensitiveOption),
       "cents not written ct"},
      {QRegularExpression(QStringLiteral("\\d%")), "a percent sign not set apart from its number"},
  };
  return rules;
}

// The first formatter rule `text` breaks, or nullptr.
inline const char* broken_value_rule(const QString& text) {
  for (const auto& [pattern, meaning] : value_rules()) {
    if (pattern.match(text).hasMatch()) {
      return meaning;
    }
  }
  return nullptr;
}

// The rules catch each mistake #166 found in the panels, and pass the
// formatter's own spelling of the same values.
inline void rules_catch_known_mistakes() {
  for (const char* wrong : {"-0.0 dB", "\u22120.0 dB", "LATE -16 DB", "48 KHZ", "2.00 MS LATENCY",
                            "3 S", "90 deg", "C#3", "12 cents", "+5 \u00A2", "75%"}) {
    require(broken_value_rule(QString::fromUtf8(wrong)) != nullptr,
            std::string("the text rules catch \"") + wrong + "\"");
  }
  for (const char* right :
       {"0.0 dB", "LATE \u221216 dB", "48 kHz", "2.00 ms LATENCY", "HELD 1 s", "90\u00B0",
        "C\u266F3", "+12 ct", "75 %", "KICK-AWARE LOW", "M / S", "\u22120.05"}) {
    require(broken_value_rule(QString::fromUtf8(right)) == nullptr,
            std::string("the text rules pass \"") + right + "\"");
  }
}

// No visible text under root breaks a formatter rule.
inline void require_formatted_values(QQuickItem* root, const std::string& panel) {
  std::vector<QQuickItem*> items;
  visible_text_items(root, items);
  require(!items.empty(), panel + " shows text");
  for (const QQuickItem* item : items) {
    const QString text = shown_text(*item);
    if (const char* broken = broken_value_rule(text)) {
      require(false, panel + " shows \"" + text.toStdString() + "\" (" +
                         item->objectName().toStdString() + "): " + broken);
    }
  }
}

} // namespace oscillate::test::panel_text
