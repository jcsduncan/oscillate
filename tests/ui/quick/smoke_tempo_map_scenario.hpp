#pragma once

#include <QString>

class QQuickItem;
class QQuickWindow;

namespace oscillate::quick {
class ProjectBridge;
}

namespace quick_smoke {

using VisibleItemLookup = QQuickItem* (*)(QQuickItem*, const QString&);

bool edit_tempo_map(oscillate::quick::ProjectBridge& bridge, QQuickWindow* window,
                    VisibleItemLookup find_item);
bool verify_tempo_map(oscillate::quick::ProjectBridge& bridge, QQuickWindow* window);

} // namespace quick_smoke
