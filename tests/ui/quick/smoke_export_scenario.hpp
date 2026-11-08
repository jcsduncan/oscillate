#pragma once

#include <QString>
#include <QtTypes>

#include <cstdint>
#include <filesystem>

class QApplication;
class QQuickItem;
class QQmlApplicationEngine;

namespace oscillate::quick {
class ProjectBridge;
}

namespace quick_smoke {

enum class MasterExportScenario : std::uint8_t { none, success, failure, close };

struct ExportOptions {
  MasterExportScenario master = MasterExportScenario::none;
  bool stems = false;
  std::filesystem::path project_root;
  std::filesystem::path target;
  int format = 0;
  int output_rate = 0;
};

using VisibleItemCount = qsizetype (*)(const QQuickItem*, const QString&);

bool start_master_export(const ExportOptions& options, QApplication& app,
                         QQmlApplicationEngine& engine, oscillate::quick::ProjectBridge& bridge,
                         VisibleItemCount count_visible_items);
void observe_master_export(const ExportOptions& options, QApplication& app,
                           oscillate::quick::ProjectBridge& bridge);
bool start_stem_export(const ExportOptions& options, QApplication& app,
                       QQmlApplicationEngine& engine, oscillate::quick::ProjectBridge& bridge,
                       VisibleItemCount count_visible_items);

} // namespace quick_smoke
