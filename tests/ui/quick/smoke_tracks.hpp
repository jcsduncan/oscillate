#pragma once

#include "project_bridge.hpp"

#include <QString>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

namespace quick_smoke {

// Projects made from the welcome screen start with the default return
// tracks; smoke checks look at the tracks the scenario itself made.
inline QVariantList content_tracks(const oscillate::quick::ProjectBridge& bridge) {
  QVariantList result;
  for (const auto& track : bridge.tracks()) {
    if (track.toMap().value(QStringLiteral("kind")).toString() != QStringLiteral("return"))
      result.push_back(track);
  }
  return result;
}

// The project's one audio track, or null when there is none or several.
inline QVariant audio_track(const oscillate::quick::ProjectBridge& bridge) {
  QVariant found;
  int count = 0;
  for (const auto& track : bridge.tracks()) {
    if (track.toMap().value(QStringLiteral("kind")).toString() == QStringLiteral("audio")) {
      found = track;
      ++count;
    }
  }
  return count == 1 ? found : QVariant{};
}

} // namespace quick_smoke
