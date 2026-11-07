#pragma once

#include <QString>
#include <QtTypes>

#include <atomic>
#include <cstdint>

class QApplication;
class QQuickItem;
class QQuickWindow;

namespace oscillate::quick {
class ProjectBridge;
}

namespace quick_smoke {

using RoutingItemLookup = QQuickItem* (*)(QQuickItem*, const QString&);
using RoutingLabeledItemLookup = QQuickItem* (*)(QQuickItem*, const QString&, const QString&);
using RoutingVisibleCount = qsizetype (*)(const QQuickItem*, const QString&);

struct RoutingPlaybackProbe {
  std::atomic<float>& first_sample;
  std::atomic<std::uint64_t>& first_completed_sample;
  std::atomic<bool>& stopped_off_ui;
  std::atomic<bool>& delay_start;
};

int run_routing(QApplication& app, oscillate::quick::ProjectBridge& bridge, QQuickWindow* window,
                const RoutingPlaybackProbe& probe, RoutingItemLookup find_item,
                RoutingLabeledItemLookup find_labeled_item);
int verify_routing(oscillate::quick::ProjectBridge& bridge, QQuickWindow* window,
                   RoutingVisibleCount visible_count);

} // namespace quick_smoke
