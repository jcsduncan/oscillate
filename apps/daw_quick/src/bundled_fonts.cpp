#include "bundled_fonts.hpp"

#include <QFontDatabase>
#include <QString>

#include <array>

namespace oscillate::quick {

bool register_bundled_fonts() {
  // Inter's static weights register as "Inter" (its variable file is
  // "Inter Variable", which the panels don't name).
  constexpr std::array fonts{":/fonts/Inter-Light.ttf",
                             ":/fonts/Inter-Regular.ttf",
                             ":/fonts/Inter-Medium.ttf",
                             ":/fonts/Inter-SemiBold.ttf",
                             ":/fonts/Inter-Bold.ttf",
                             ":/fonts/Inter-ExtraBold.ttf",
                             ":/fonts/JetBrainsMono-Variable.ttf"};
  bool loaded = true;
  for (const auto* font : fonts)
    loaded = QFontDatabase::addApplicationFont(QString::fromLatin1(font)) >= 0 && loaded;
  return loaded;
}

} // namespace oscillate::quick
