#include "oscillate/application/project_session.hpp"
#include "oscillate/test/check.hpp"
#include "project_bridge.hpp"

#include <QApplication>
#include <QTemporaryDir>
#include <QTest>

#include <cmath>
#include <filesystem>
#include <stdexcept>

namespace {

using oscillate::test::require;

// The master path's built-in effects, as the device chain shows them.
qsizetype master_effect_count(oscillate::quick::ProjectBridge& bridge) {
  return bridge.insert_chains().back().toMap().value(QStringLiteral("effects")).toList().size();
}

void run() {
  QTemporaryDir directory;
  require(directory.isValid(), "temporary project directory unavailable");
  qputenv("XDG_CONFIG_HOME", (directory.path() + QStringLiteral("/config")).toUtf8());
  const auto home = std::filesystem::path(directory.path().toStdString());
  const auto first_root = home / "first";
  const auto second_root = home / "second";
  auto first = oscillate::application::ProjectSession::create(first_root);
  require(first.has_value(), "first project creation failed");
  oscillate::quick::ProjectBridge bridge(home);
  bridge.set_project(std::move(first).value(), first_root);
  require(bridge.project_format().contains(QStringLiteral("kHz")) &&
              bridge.project_format().contains(QStringLiteral("4/4")),
          "new project format absent");
  require(std::abs(bridge.master_level() - 1.0) < 0.000001 && master_effect_count(bridge) == 0,
          "new project has unexpected master controls");

  // Insert edits wait for the bridge's startup work (the plug-in scan).
  require(QTest::qWaitFor([&bridge] { return bridge.insert_chain_edit_enabled(); }, 10'000),
          "insert editing never became available");
  const auto before = bridge.revision();
  bridge.set_master_level(0.5);
  bridge.add_effect_engine(QStringLiteral("master"), 0, QStringLiteral("ceiling"));
  require(bridge.revision() == before + 2 && std::abs(bridge.master_level() - 0.5) < 0.000001 &&
              master_effect_count(bridge) == 1,
          "master controls did not follow accepted project edits");
  bridge.save();
  require(bridge.status().contains(QStringLiteral("saved"), Qt::CaseInsensitive),
          "edited first project did not save");

  auto second = oscillate::application::ProjectSession::create(second_root);
  require(second.has_value(), "second project creation failed");
  bridge.set_project(std::move(second).value(), second_root);
  require(std::abs(bridge.master_level() - 1.0) < 0.000001 && master_effect_count(bridge) == 0,
          "master controls leaked across project switch");

  auto reopened = oscillate::application::ProjectSession::open(first_root);
  require(reopened.has_value(), "edited first project did not reopen");
  bridge.set_project(std::move(reopened).value(), first_root);
  require(std::abs(bridge.master_level() - 0.5) < 0.000001 && master_effect_count(bridge) == 1 &&
              bridge.project_format().contains(QStringLiteral("4/4")),
          "saved master controls did not return after reopen");
}

} // namespace

// NOLINTNEXTLINE(bugprone-exception-escape): an uncaught exception is the test's failure report.
int main(int argc, char** argv) {
  QApplication app(argc, argv);
  run();
}
