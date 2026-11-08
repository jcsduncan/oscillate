#pragma once

#include "oscillate/test/check.hpp"

#include <QCoreApplication>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRectF>
#include <QString>
#include <QTest>
#include <QVariant>

#include <cmath>
#include <string>
#include <vector>

// Checks that a device panel is built from the device kit's shared parts the
// same way as every other panel (issue #161): one DeviceHeader with its own
// readout, one StatusBadge, and controls whose mode-inactive and linked
// states say why. Shared by the app contracts that open every built-in
// instrument and effect.
namespace oscillate::test::panel_kit {

// The rounding a size or scale comparison absorbs.
inline constexpr double size_tolerance = 1e-6;

inline bool shown(const QQuickItem& item) { return item.isVisible() && item.opacity() > 0.0; }

// Every shown item under root whose `marker` property is true.
inline void marked(QQuickItem* root, const char* marker, std::vector<QQuickItem*>& found) {
  if (!shown(*root)) {
    return;
  }
  if (root->property(marker).toBool()) {
    found.push_back(root);
  }
  for (QQuickItem* child : root->childItems()) {
    marked(child, marker, found);
  }
}

inline bool inside(const QQuickItem& item, const QQuickItem& ancestor) {
  for (const QQuickItem* parent = item.parentItem(); parent != nullptr;
       parent = parent->parentItem()) {
    if (parent == &ancestor) {
      return true;
    }
  }
  return false;
}

inline QRectF scene_rect(const QQuickItem& item) {
  return item.mapRectToScene(item.boundingRect());
}

inline QQuickItem* item_property(const QObject& object, const char* name) {
  return qobject_cast<QQuickItem*>(object.property(name).value<QObject*>());
}

// The panel has exactly one shown DeviceHeader, holding the panel's only
// status badge and its brand. Its readout is hidden while no control is
// touched; once `touched` (a parameter id) is touched it shows in a box of
// its own, clear of the preset browser.
inline void require_device_header(QQuickItem* panel, const std::string& name,
                                  const QString& touched) {
  std::vector<QQuickItem*> headers;
  marked(panel, "deviceHeader", headers);
  require(headers.size() == 1,
          name + " has one device header (found " + std::to_string(headers.size()) + ")");
  QQuickItem& header = *headers.front();
  std::vector<QQuickItem*> badges;
  marked(panel, "deviceStatus", badges);
  require(badges.size() == 1,
          name + " has one status badge (found " + std::to_string(badges.size()) + ")");
  require(inside(*badges.front(), header), name + "'s status badge sits in its header");
  const QQuickItem* brand = item_property(header, "brandItem");
  require(brand != nullptr && brand->width() > 0.0, name + "'s header shows its brand");

  QQuickItem* readout = item_property(header, "readout");
  require(readout != nullptr, name + "'s header has its readout");
  auto* theme = header.property("theme").value<QObject*>();
  require(theme != nullptr, name + "'s header has its theme");
  const auto before = theme->property("lastTouched");
  theme->setProperty("lastTouched", QString());
  QCoreApplication::processEvents();
  require(!readout->isVisible(), name + "'s readout hides while no control is touched");
  theme->setProperty("lastTouched", touched);
  QCoreApplication::processEvents();
  require(readout->isVisible(), name + "'s readout shows the control last touched");
  if (header.property("presets").toBool()) {
    QObject* browser = header.property("browser").value<QObject*>();
    require(browser != nullptr, name + "'s header has its preset browser");
    QQuickItem* display = item_property(*browser, "display");
    require(display != nullptr, name + "'s preset browser has its display");
    require(!inside(*readout, *display) && !scene_rect(*readout).intersects(scene_rect(*display)),
            name + "'s readout sits in its own box, clear of the preset display");
  }
  theme->setProperty("lastTouched", before);
  QCoreApplication::processEvents();
}

// Runs `check` while the panel's header reads `touched` as the control last
// touched, then restores the header.
template <typename Check>
void with_touched(QQuickItem* panel, const QString& touched, Check check) {
  std::vector<QQuickItem*> headers;
  marked(panel, "deviceHeader", headers);
  require(!headers.empty(), "the panel has a device header");
  auto* theme = headers.front()->property("theme").value<QObject*>();
  const auto before = theme->property("lastTouched");
  theme->setProperty("lastTouched", touched);
  QCoreApplication::processEvents();
  check();
  theme->setProperty("lastTouched", before);
  QCoreApplication::processEvents();
}

// Every shown kit control in a mode-inactive state says why, and every
// linked one says what drives it (issue #163).
inline void require_control_states(QQuickItem* panel, const std::string& name) {
  std::vector<QQuickItem*> stack{panel};
  std::string problems;
  while (!stack.empty()) {
    QQuickItem* item = stack.back();
    stack.pop_back();
    if (!shown(*item)) {
      continue;
    }
    const QString state = item->property("controlState").toString();
    if (state == QStringLiteral("inactive") &&
        item->property("inactiveReason").toString().trimmed().isEmpty()) {
      problems += "\n  " + item->objectName().toStdString() + " is inactive without a reason";
    }
    if (state == QStringLiteral("linked") &&
        item->property("linkReason").toString().trimmed().isEmpty()) {
      problems += "\n  " + item->objectName().toStdString() + " is linked without a reason";
    }
    for (QQuickItem* child : item->childItems()) {
      stack.push_back(child);
    }
  }
  require(problems.empty(), name + "'s control states do not say why:" + problems);
}

// The least hit area a pointer control offers, in the panel's own pixels
// (issue #170).
inline constexpr double min_hit_px = 24.0;

// Every shown, enabled pointer area under root that takes a click, with
// its size in root's coordinates.
inline void pointer_areas(QQuickItem* root, std::vector<QQuickItem*>& found) {
  if (!shown(*root) || !root->isEnabled()) {
    return;
  }
  // A MouseArea's own `enabled` (whether it takes the pointer) is not the
  // item's: read the property.
  if (root->inherits("QQuickMouseArea") && root->property("enabled").toBool() &&
      root->width() > 0.0 && root->height() > 0.0 &&
      root->property("acceptedButtons").toInt() != 0 &&
      !root->property("pointerOnlyReason").isValid()) {
    found.push_back(root);
  }
  for (QQuickItem* child : root->childItems()) {
    pointer_areas(child, found);
  }
}

inline std::string describe_area(const QQuickItem& area) {
  const QQuickItem* control = area.parentItem();
  std::string text = area.objectName().toStdString();
  for (const QQuickItem* item = control; text.empty() && item != nullptr;
       item = item->parentItem()) {
    text = item->objectName().toStdString();
  }
  return text.empty() ? std::string("(unnamed)") : text;
}

// Every pointer control in the panel is at least min_hit_px square in the
// panel's own pixels (issue #170), and can be reached and used from the
// keyboard: it or a control it sits in takes Tab focus (issue #112). An area the keyboard reaches
// another way (a plot's drag handles, edited with the arrow keys on the focused plot) names that
// way in `keyboardAlternative`; a pointer convenience that duplicates a keyboard control says why
// in `pointerOnlyReason`.
inline void require_pointer_controls(QQuickItem* panel, const std::string& name) {
  std::vector<QQuickItem*> areas;
  pointer_areas(panel, areas);
  std::string small;
  std::string unreachable;
  for (QQuickItem* area : areas) {
    const QRectF box = area->mapRectToItem(panel, area->boundingRect());
    if (box.width() + size_tolerance < min_hit_px || box.height() + size_tolerance < min_hit_px) {
      small += "\n  " + describe_area(*area) + " (" + std::to_string(box.width()) + " x " +
               std::to_string(box.height()) + ")";
    }
    bool reachable = !area->property("keyboardAlternative").toString().isEmpty();
    for (const QQuickItem* item = area; !reachable && item != nullptr && item != panel;
         item = item->parentItem()) {
      reachable = item->activeFocusOnTab();
    }
    if (!reachable) {
      unreachable += "\n  " + describe_area(*area);
    }
  }
  require(small.empty(),
          name + " has pointer targets under " + std::to_string(min_hit_px) + " px:" + small);
  require(unreachable.empty(),
          name + " has pointer controls the keyboard cannot reach:" + unreachable);
}

// A drawing at least this large, in the panel's own pixels, is a plot: it
// sits in a Kit.PlotFrame (issue #169) unless it says why not
// (`plotExemptReason`: a level meter's own scale, a decorative glyph).
inline constexpr double plot_min_width_px = 120.0;
inline constexpr double plot_min_height_px = 60.0;

inline bool inside_plot_frame(const QQuickItem& item, const QQuickItem* panel) {
  for (const QQuickItem* parent = &item; parent != nullptr && parent != panel;
       parent = parent->parentItem()) {
    if (parent->property("plotFrame").toBool() ||
        !parent->property("plotExemptReason").toString().isEmpty()) {
      return true;
    }
  }
  return false;
}

// Every plot in the panel sits in a PlotFrame; every PlotFrame labels each
// axis it ticks with its unit and says LIVE or PREVIEW.
inline void require_plot_frames(QQuickItem* panel, const std::string& name) {
  std::vector<QQuickItem*> stack{panel};
  std::string problems;
  while (!stack.empty()) {
    QQuickItem* item = stack.back();
    stack.pop_back();
    if (!shown(*item)) {
      continue;
    }
    if (item->inherits("QQuickCanvasItem")) {
      const QRectF box = item->mapRectToItem(panel, item->boundingRect());
      if (box.width() >= plot_min_width_px && box.height() >= plot_min_height_px &&
          !inside_plot_frame(*item, panel)) {
        problems += "\n  a plot outside a PlotFrame: " + describe_area(*item) + " (" +
                    std::to_string(box.width()) + " x " + std::to_string(box.height()) + ")";
      }
    }
    if (item->property("plotFrame").toBool()) {
      const bool x_ticked = !item->property("xTicks").toList().isEmpty();
      const bool y_ticked = !item->property("yTicks").toList().isEmpty();
      if ((x_ticked && item->property("xUnit").toString().isEmpty()) ||
          (y_ticked && item->property("yUnit").toString().isEmpty())) {
        problems += "\n  a PlotFrame axis without its unit: " + describe_area(*item);
      }
      // A frame with something drawn in it has its scale and its chip; an
      // empty one (a strip with nothing to show) has neither.
      const bool chip = item->property("showsChip").toBool();
      if (chip && !x_ticked && !y_ticked) {
        problems += "\n  a PlotFrame without a scale: " + describe_area(*item);
      }
      if ((x_ticked || y_ticked) && !chip) {
        problems += "\n  a PlotFrame without LIVE / PREVIEW: " + describe_area(*item);
      }
    }
    for (QQuickItem* child : item->childItems()) {
      stack.push_back(child);
    }
  }
  require(problems.empty(), name + "'s plots are not framed:" + problems);
}

// The panel zoom steps the host offers (ADR-0103), in percent, and the
// rounding a scale comparison absorbs.
inline constexpr int zoom_out_percent = 90;
inline constexpr int zoom_unity_percent = 100;
inline constexpr int zoom_in_percent = 140;

inline QQuickItem* named(QQuickItem* root, const QString& name) {
  if (root->objectName() == name) {
    return root;
  }
  for (QQuickItem* child : root->childItems()) {
    if (QQuickItem* found = named(child, name)) {
      return found;
    }
  }
  return nullptr;
}

// The panel's window offers the zoom chips, and they scale the panel the
// same way for every panel: 90 % draws it smaller than 100 %, by that
// ratio, unless the window already holds it smaller; 140 % never draws it
// smaller. The zoom is left at 100 %.
inline void require_panel_zoom(QQuickWindow& window, const std::string& name,
                               const QString& frame_name, const QString& host_name) {
  QQuickItem* root = window.contentItem();
  const auto choose = [&](int percent) {
    QQuickItem* frame = named(root, frame_name);
    require(frame != nullptr, name + "'s window is open");
    QQuickItem* chip = named(frame, QStringLiteral("panelZoom:") + QString::number(percent));
    require(chip != nullptr && chip->isVisible(),
            name + "'s window offers the " + std::to_string(percent) + "% zoom");
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier,
                      chip->mapToScene({chip->width() / 2.0, chip->height() / 2.0}).toPoint());
    QCoreApplication::processEvents();
    QQuickItem* host = named(root, host_name);
    require(host != nullptr, name + "'s panel host is there");
    return host->scale();
  };
  const double unity = choose(zoom_unity_percent);
  const double out = choose(zoom_out_percent);
  const double in = choose(zoom_in_percent);
  choose(zoom_unity_percent);
  const double ratio = static_cast<double>(zoom_out_percent) / zoom_unity_percent;
  require(out < unity && out <= unity * ratio + size_tolerance,
          name + "'s panel draws smaller at " + std::to_string(zoom_out_percent) + "%");
  require(in + size_tolerance >= unity,
          name + "'s panel draws no smaller at " + std::to_string(zoom_in_percent) + "%");
}

} // namespace oscillate::test::panel_kit
