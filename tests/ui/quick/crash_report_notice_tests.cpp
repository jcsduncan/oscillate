#include "../../../apps/daw_quick/src/crash_report_controller.hpp"
#include "oscillate/test/check.hpp"

#include <QGuiApplication>
#include <QMetaObject>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QString>
#include <QUrl>
#include <QUrlQuery>

#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>

namespace {

namespace quick = oscillate::quick;

const std::filesystem::path report_file =
    "/state/oscillate/crash-reports/oscillate-crash-20261107T120000Z.txt";
constexpr const char* issue_template_directory = ".github/ISSUE_TEMPLATE";

void nothing_to_offer_shows_nothing() {
  QQmlEngine engine;
  QQuickWindow window;
  quick::CrashReportController controller(std::nullopt);
  REQUIRE_FALSE(controller.pending());
  REQUIRE_TRUE(quick::show_crash_report_notice(engine, window, controller) == nullptr);
  REQUIRE_TRUE(window.contentItem()->childItems().isEmpty());
}

void notice_offers_the_report_until_closed() {
  QQmlEngine engine;
  QQuickWindow window;
  quick::CrashReportController controller(report_file);
  auto* notice = quick::show_crash_report_notice(engine, window, controller);
  REQUIRE_TRUE(notice != nullptr, "the notice loads");
  REQUIRE_TRUE(notice->parentItem() == window.contentItem());
  REQUIRE_TRUE(notice->isVisible());

  auto* path = notice->findChild<QQuickItem*>(QStringLiteral("crashReportPath"));
  REQUIRE_TRUE(path != nullptr);
  REQUIRE_EQ(path->property("text").toString().toStdString(), report_file.string(),
             "the user sees where the report is");
  REQUIRE_TRUE(notice->findChild<QQuickItem*>(QStringLiteral("crashReportShow")) != nullptr);
  REQUIRE_TRUE(notice->findChild<QQuickItem*>(QStringLiteral("crashReportBugForm")) != nullptr);

  auto* close = notice->findChild<QQuickItem*>(QStringLiteral("crashReportClose"));
  REQUIRE_TRUE(close != nullptr);
  REQUIRE_TRUE(QMetaObject::invokeMethod(close, "activated"));
  REQUIRE_FALSE(controller.pending());
  REQUIRE_FALSE(notice->isVisible(), "closing hides the notice");
}

void bug_form_is_the_repository_template() {
  const QUrl url(quick::CrashReportController::bug_form_url());
  REQUIRE_EQ(url.scheme().toStdString(), std::string("https"));
  const auto name = QUrlQuery(url).queryItemValue(QStringLiteral("template")).toStdString();
  REQUIRE_FALSE(name.empty());
  REQUIRE_TRUE(std::filesystem::is_regular_file(std::filesystem::path(OSCILLATE_SOURCE_DIR) /
                                                issue_template_directory / name),
               "the bug form link names a template that exists");
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  try {
    nothing_to_offer_shows_nothing();
    notice_offers_the_report_until_closed();
    bug_form_is_the_repository_template();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
