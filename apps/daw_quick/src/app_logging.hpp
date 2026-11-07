#pragma once

#include <QString>

#include <map>
#include <string>

namespace oscillate::quick {

// Configures foundation logging from the environment (OSCILLATE_LOG,
// XDG_STATE_HOME) before Qt starts, routes Qt and QML messages into the
// same log, and reports OSCILLATE_LOG items it could not use. Call once,
// first thing in main.
void start_logging();

// Keeps each distinct status line a source shows (the status bar, playback,
// export, the VST3 browser...) in the log, so a transient message (a failed
// save, a refused edit) can be found after the UI has replaced it.
class StatusLog {
public:
  // Logs `text` under `source` when its wording differs from that source's
  // last line; changes that only move numbers (positions, progress) are not
  // logged again.
  void note(const std::string& source, const QString& text);

private:
  std::map<std::string, QString> last_;
};

} // namespace oscillate::quick
