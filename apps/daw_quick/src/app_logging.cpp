#include "app_logging.hpp"

#include "oscillate/foundation/crash_report.hpp"
#include "oscillate/foundation/log.hpp"

#include <QMessageLogContext>
#include <QRegularExpression>
#include <QString>
#include <QtGlobal>

#include <cstdlib>

namespace oscillate::quick {
namespace {
namespace log = foundation::log;

[[nodiscard]] log::Level level_for(QtMsgType type) noexcept {
  switch (type) {
  case QtDebugMsg:
    return log::Level::debug;
  case QtInfoMsg:
    return log::Level::info;
  case QtWarningMsg:
    return log::Level::warning;
  case QtCriticalMsg:
  case QtFatalMsg:
    return log::Level::error;
  }
  return log::Level::warning;
}

// Qt and QML warnings (binding loops, missing properties, failed imports)
// go to the same log as everything else instead of a bare stderr line.
void qt_message(QtMsgType type, const QMessageLogContext& context, const QString& text) {
  const auto level = level_for(type);
  const char* category = context.category != nullptr ? context.category : "qt";
  if (context.file != nullptr)
    log::message(level, log::Category::application, "{}: {} ({}:{})", category, text.toStdString(),
                 context.file, context.line);
  else
    log::message(level, log::Category::application, "{}: {}", category, text.toStdString());
  if (type == QtFatalMsg) {
    log::flush();
    foundation::crash_report::note_fatal_error(text.toStdString());
    std::abort();
  }
}

} // namespace

namespace {

// A status line's wording with every run of digits collapsed, so progress
// that only moves numbers (playback position, revision, percent done) counts
// as the same line.
QString wording(const QString& text) {
  static const QRegularExpression digits(QStringLiteral("[0-9]+"));
  return QString(text).replace(digits, QStringLiteral("#"));
}

} // namespace

void StatusLog::note(const std::string& source, const QString& text) {
  auto& last = last_[source];
  const auto shape = wording(text);
  if (shape == last)
    return;
  last = shape;
  if (!text.isEmpty())
    log::info(log::Category::application, "{}: {}", source, text.toStdString());
}

void start_logging() {
  log::configure(log::options_from_environment());
  qInstallMessageHandler(qt_message);
  if (const char* spec = std::getenv("OSCILLATE_LOG"))
    for (const auto& item : log::parse_filter(spec).rejected)
      log::warning(log::Category::application, "OSCILLATE_LOG: ignoring \"{}\"", item);
  log::info(log::Category::application, "Oscillate starting; log file {}",
            log::current_file().string());
}

} // namespace oscillate::quick
