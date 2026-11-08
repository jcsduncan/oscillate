#pragma once

#include <QString>
#include <QtTypes>

#include <cstdint>
#include <filesystem>

class QApplication;
class QQuickItem;
class QQuickWindow;

namespace oscillate::quick {
class ProjectBridge;
}

namespace quick_smoke {

enum class ImportScenario : std::uint8_t { none, success, failure, stale, close, undo };

// Shared item lookup for the smoke shell and its import scenario.
qsizetype count_visible_items(const QQuickItem* item, const QString& name);

bool verify_reopened_import(oscillate::quick::ProjectBridge& bridge, QQuickWindow* window);
bool start_import(ImportScenario scenario, QApplication& app,
                  oscillate::quick::ProjectBridge& bridge, QQuickWindow* window,
                  const std::filesystem::path& project_root, const std::filesystem::path& source);
void observe_import(ImportScenario scenario, QApplication& app,
                    oscillate::quick::ProjectBridge& bridge);

} // namespace quick_smoke
