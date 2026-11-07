#pragma once

#include <QObject>
#include <QString>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

class QQmlEngine;
class QQuickItem;
class QQuickWindow;

namespace oscillate::quick {

// What startup found and did about crash reports (foundation/crash_report).
struct CrashReportStartup {
  // The report gathered from the previous run's crash, if it crashed.
  std::optional<std::filesystem::path> report;
  // Problems to log once logging is configured.
  std::string collect_error;
  std::string arm_error;
};

// Gathers the previous run's crash report, if any, then records this session
// and arms the crash handlers. Reads XDG_STATE_HOME and HOME. Call first in
// main, before logging starts, so the report holds the crashed run's log
// without this run's first lines.
[[nodiscard]] CrashReportStartup start_crash_reports(std::string_view application_version);

// Logs what start_crash_reports found. Call once logging is configured.
void log_crash_report_startup(const CrashReportStartup& startup);

// Offers the previous run's crash report after a crash: where it is, a way
// to open its folder and a link to the bug form. Opening the folder or the
// form is the user's action; nothing is sent.
class CrashReportController final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool pending READ pending NOTIFY changed)
  Q_PROPERTY(QString reportPath READ report_path CONSTANT)
  Q_PROPERTY(QString bugFormUrl READ bug_form_url CONSTANT)

public:
  // The issue tracker's bug form, which asks for the report.
  static constexpr const char* bug_form = "https://github.com/jcsduncan/oscillate/issues/new"
                                          "?template=bug_report.yml";

  explicit CrashReportController(std::optional<std::filesystem::path> report,
                                 QObject* parent = nullptr);

  [[nodiscard]] bool pending() const { return pending_; }
  [[nodiscard]] QString report_path() const { return report_path_; }
  [[nodiscard]] static QString bug_form_url() { return QString::fromLatin1(bug_form); }

  // Opens the folder holding the report in the file manager.
  Q_INVOKABLE void show_report() const;
  // Opens the bug form in the browser.
  Q_INVOKABLE void open_bug_form() const;
  // Hides the notice for this run. The report stays on disk.
  Q_INVOKABLE void dismiss();

signals:
  void changed();

private:
  QString report_path_;
  bool pending_;
};

// Puts the crash report notice (CrashReportNotice.qml) over the window's
// content when a report is pending. Returns the notice, owned by `window`'s
// content item, or nullptr when there is nothing to offer or it failed to load.
QQuickItem* show_crash_report_notice(QQmlEngine& engine, QQuickWindow& window,
                                     CrashReportController& controller);

} // namespace oscillate::quick
