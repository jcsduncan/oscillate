#pragma once

#include "oscillate/test/check.hpp"

#include <QFont>
#include <QFontMetricsF>
#include <QQuickItem>
#include <QRectF>
#include <QRegularExpression>
#include <QString>

#include <cstddef>
#include <string>
#include <vector>

// Checks that a device panel's text shows whole: no label cut short, no
// text drawn over other text, none clipped by its container. Shared by the
// app contracts that open every built-in instrument and effect.
namespace oscillate::test::panel_fit {

// Clips smaller than this are antialiasing, not lost text; two texts'
// line boxes may touch by this much (a wordmark's descent over its
// subtitle) without their glyphs meeting.
inline constexpr double slack_px = 1.0;
inline constexpr double overlap_px = 3.0;

inline bool is_text_item(const QQuickItem& item) {
  return item.inherits("QQuickText") || item.inherits("QQuickTextEdit") ||
         item.inherits("QQuickTextInput");
}

inline bool shown(const QQuickItem& item) { return item.isVisible() && item.opacity() > 0.0; }

// The part of the scene an item's clipping ancestors let it draw in.
inline QRectF drawable(const QQuickItem& item) {
  QRectF area(-1e9, -1e9, 2e9, 2e9);
  for (const QQuickItem* parent = item.parentItem(); parent != nullptr;
       parent = parent->parentItem()) {
    if (parent->clip()) {
      area = area.intersected(parent->mapRectToScene(parent->boundingRect()));
    }
  }
  return area;
}

inline QRectF painted(const QQuickItem& item);

// Every visible, non-empty text item under root that draws somewhere a
// clipping ancestor (a scrolled list, say) leaves in view.
inline void texts_in(QQuickItem* root, std::vector<QQuickItem*>& items) {
  if (!shown(*root)) {
    return;
  }
  if (is_text_item(*root) && !root->property("text").toString().trimmed().isEmpty() &&
      painted(*root).intersects(drawable(*root))) {
    items.push_back(root);
  }
  for (QQuickItem* child : root->childItems()) {
    texts_in(child, items);
  }
}

// Every visible device-kit card under root: the items that say whether their
// note fits.
inline void cards_in(QQuickItem* root, std::vector<QQuickItem*>& cards) {
  if (!shown(*root)) {
    return;
  }
  if (root->property("noteFits").isValid()) {
    cards.push_back(root);
  }
  for (QQuickItem* child : root->childItems()) {
    cards_in(child, cards);
  }
}

// Where a text item paints its glyphs, in scene coordinates.
inline QRectF painted(const QQuickItem& item) {
  const double width = item.property("contentWidth").toDouble();
  double height = item.property("contentHeight").toDouble();
  // A line with no descenders (a capitalised wordmark, a note name) leaves
  // the font's descent unpainted: another text may sit in it.
  static const QRegularExpression descenders(QStringLiteral("[gjpqyQ,;()\\[\\]{}_/|@$]"));
  const QString text = item.property("text").toString();
  if (!text.contains(descenders) && !text.contains(QLatin1Char('\n'))) {
    height -= QFontMetricsF(item.property("font").value<QFont>()).descent();
  }
  const int horizontal = item.property("horizontalAlignment").toInt();
  const int vertical = item.property("verticalAlignment").toInt();
  double x = 0.0;
  if ((horizontal & Qt::AlignRight) != 0) {
    x = item.width() - width;
  } else if ((horizontal & Qt::AlignHCenter) != 0) {
    x = (item.width() - width) / 2.0;
  }
  double y = 0.0;
  if ((vertical & Qt::AlignBottom) != 0) {
    y = item.height() - height;
  } else if ((vertical & Qt::AlignVCenter) != 0) {
    y = (item.height() - height) / 2.0;
  }
  return item.mapRectToScene(QRectF(x, y, width, height));
}

inline std::string describe(const QQuickItem& item) {
  return "\"" + item.property("text").toString().toStdString() + "\" (" +
         item.objectName().toStdString() + ")";
}

// An elided text may stay cut short only when it says a tooltip shows it
// whole: the last resort for text of any length, such as a user's names.
inline bool elides_with_tip(const QQuickItem& item) {
  return item.property("tipShowsWhole").toBool();
}

// No visible text under root is cut short (unless its tooltip shows it
// whole), drawn over another text or clipped by a container, and every
// card's note ends inside its card.
inline void require_text_fits(QQuickItem* root, const std::string& panel) {
  std::vector<QQuickItem*> items;
  texts_in(root, items);
  // Every problem, so one run lists them all.
  std::string problems;
  const auto check = [&](bool fine, const std::string& problem) {
    if (!fine) {
      problems += "\n  " + problem;
    }
  };
  for (const QQuickItem* item : items) {
    check(!item->property("truncated").toBool() || elides_with_tip(*item),
          "cuts short " + describe(*item));
    const QRectF glyphs = painted(*item);
    for (const QQuickItem* parent = item->parentItem(); parent != nullptr;
         parent = parent->parentItem()) {
      if (!parent->clip()) {
        continue;
      }
      // A scrolled view clips what is scrolled out of sight; that text is
      // shown whole once scrolled to.
      if (parent->inherits("QQuickFlickable")) {
        break;
      }
      const QRectF box = parent->mapRectToScene(parent->boundingRect());
      const QRectF inner = glyphs.adjusted(slack_px, slack_px, -slack_px, -slack_px);
      check(inner.intersected(box) == inner, "clips " + describe(*item));
      break;
    }
  }
  for (std::size_t i = 0; i < items.size(); ++i) {
    const QRectF a = painted(*items[i]);
    for (std::size_t j = i + 1; j < items.size(); ++j) {
      const QRectF both = a.intersected(painted(*items[j]));
      check(both.width() < overlap_px || both.height() < overlap_px,
            "draws " + describe(*items[i]) + " over " + describe(*items[j]));
    }
  }
  std::vector<QQuickItem*> cards;
  cards_in(root, cards);
  for (const QQuickItem* card : cards) {
    check(card->property("noteFits").toBool(),
          "runs the note \"" + card->property("note").toString().toStdString() +
              "\" past its card " + card->property("title").toString().toStdString());
  }
  require(problems.empty(), panel + "'s text does not fit:" + problems);
}

} // namespace oscillate::test::panel_fit
