#pragma once

#include <QString>
#include <QtTypes>

class QQuickItem;
class QQuickWindow;

namespace oscillate::quick {
class ProjectBridge;
}

namespace quick_smoke {

using AutomationItemLookup = QQuickItem* (*)(QQuickItem*, const QString&);

// Shows automation over the track's clips with A, then adds, drags, deletes
// and draws Level points with the pointer: each gesture one undo step.
int edit_arrangement_automation(oscillate::quick::ProjectBridge& bridge, QQuickWindow* window,
                                AutomationItemLookup find_item);

} // namespace quick_smoke
