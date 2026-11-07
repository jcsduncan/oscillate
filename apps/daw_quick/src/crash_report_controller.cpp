#include "crash_report_controller.hpp"

#include "oscillate/foundation/crash_report.hpp"
#include "oscillate/foundation/log.hpp"

#include <QDesktopServices>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QUrl>
#include <QVariantMap>
#include <QtGlobal>

#include <array>
#include <cstdlib>
#include <format>
#include <utility>

namespace oscillate::quick {
namespace {
namespace crash = foundation::crash_report;
namespace log = foundation::log;
} // namespace

CrashReportStartup start_crash_reports(std::string_view application_version) {
  CrashReportStartup startup;
  const char* home = std::getenv("HOME");
  const auto directory = crash::default_directory(std::getenv("XDG_STATE_HOME"), home);
  if (directory.empty()) {
    startup.arm_error = "no state directory (XDG_STATE_HOME and HOME are unset)";
    return startup;
  }
  auto collected = crash::collect({.directory = directory,
                                   .log_directory = log::options_from_environment().directory,
                                   .home = home != nullptr ? std::string(home) : std::string{}});
  if (collected)
    startup.report = std::move(collected.value());
  else
    startup.collect_error = collected.error().message;

  const std::array facts{
      crash::Fact{"application", std::format("Oscillate {}", application_version)},
      crash::Fact{"qt", qVersion()}};
  if (auto armed = crash::arm(directory, crash::describe_session(facts)); !armed)
    startup.arm_error = armed.error().message;
  return startup;
}

void log_crash_report_startup(const CrashReportStartup& startup) {
  if (startup.report)
    log::warning(log::Category::application,
                 "The previous run stopped unexpectedly; crash report written to {}",
                 startup.report->string());
  if (!startup.collect_error.empty())
    log::error(log::Category::application, "Could not write the crash report: {}",
               startup.collect_error);
  if (!startup.arm_error.empty())
    log::warning(log::Category::application, "Crash reports are off for this run: {}",
                 startup.arm_error);
}

CrashReportController::CrashReportController(std::optional<std::filesystem::path> report,
                                             QObject* parent)
    : QObject(parent), report_path_(report ? QString::fromStdString(report->string()) : QString{}),
      pending_(report.has_value()) {}

void CrashReportController::show_report() const {
  if (report_path_.isEmpty())
    return;
  const auto folder = std::filesystem::path(report_path_.toStdString()).parent_path();
  QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(folder.string())));
}

void CrashReportController::open_bug_form() const {
  QDesktopServices::openUrl(QUrl(bug_form_url()));
}

QQuickItem* show_crash_report_notice(QQmlEngine& engine, QQuickWindow& window,
                                     CrashReportController& controller) {
  if (!controller.pending())
    return nullptr;
  QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/CrashReportNotice.qml")));
  QObject* created = component.createWithInitialProperties(
      QVariantMap{{QStringLiteral("report"), QVariant::fromValue(&controller)}});
  auto* item = qobject_cast<QQuickItem*>(created);
  if (item == nullptr) {
    delete created;
    log::error(log::Category::application, "Crash report notice failed to load: {}",
               component.errorString().toStdString());
    return nullptr;
  }
  item->setParent(window.contentItem());
  item->setParentItem(window.contentItem());
  item->forceActiveFocus();
  return item;
}

void CrashReportController::dismiss() {
  if (!pending_)
    return;
  pending_ = false;
  emit changed();
}

} // namespace oscillate::quick
