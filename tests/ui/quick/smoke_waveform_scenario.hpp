#pragma once

#include <QString>

class QApplication;
class QQmlApplicationEngine;
class QQuickItem;

namespace oscillate::quick {
class ProjectBridge;
}

namespace quick_smoke {

using WaveformItemLookup = QQuickItem* (*)(QQuickItem*, const QString&);

void start_waveform(QApplication& app, QQmlApplicationEngine& engine,
                    oscillate::quick::ProjectBridge& bridge, WaveformItemLookup find_item);

} // namespace quick_smoke
