#include "device_panels.hpp"
#include "ui_contracts.hpp"

#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/native_dsp/synth/runtime.hpp"
#include "oscillate/test/check.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QString>
#include <QVariantMap>

#include <array>
#include <cmath>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

// The panels' engine contracts (ui_contracts.hpp) against hand-worked values,
// and the device panel catalogue against the registered engines and the
// shell's resources.
namespace {
namespace quick = oscillate::quick;
using quick::UiContracts;

constexpr double tolerance = 1e-12;
constexpr int middle_c = 60;
constexpr int a4 = 69;
constexpr double a4_hz = 440.0;
constexpr int c_minus_one = 0;
constexpr int b_minus_two = -1;

using oscillate::test::require;

bool near(double a, double b) { return std::abs(a - b) < tolerance; }

QVariantMap point(double x, double y, double tension, bool step = false) {
  return {{QStringLiteral("x"), x},
          {QStringLiteral("y"), y},
          {QStringLiteral("tension"), tension},
          {QStringLiteral("step"), step}};
}

void bend_law() {
  // Straight at zero tension; t^(1+4k) above; 1-(1-t)^(1-4k) below.
  require(near(UiContracts::bend(0.25, 0.0), 0.25), "zero tension is straight");
  require(near(UiContracts::bend(0.5, 0.5), std::pow(0.5, 3.0)), "positive tension holds");
  require(near(UiContracts::bend(0.5, -0.5), 1.0 - std::pow(0.5, 3.0)), "negative tension leaves");
  // t is clamped, as the engines clamp it.
  require(near(UiContracts::bend(-0.5, 0.5), 0.0) && near(UiContracts::bend(1.5, -0.5), 1.0),
          "t outside 0..1 is clamped");
  // The midpoint inverse undoes the bend at t = 0.5.
  constexpr double fraction = 0.2;
  require(near(UiContracts::bend(0.5, UiContracts::tension_for_midpoint(fraction)), fraction),
          "the midpoint tension puts the midpoint at the fraction");
}

void curve_values() {
  const QVariantList curve{point(0.0, 0.0, 0.5), point(0.5, 1.0, 0.0, true), point(1.0, 0.25, 0.0)};
  constexpr double empty = 0.75;
  require(near(UiContracts::curve_at({}, 0.3, empty), empty), "an empty curve is the empty value");
  require(near(UiContracts::curve_at(curve, 0.25, empty), std::pow(0.5, 3.0)),
          "a curve segment bends by its start point's tension");
  require(near(UiContracts::curve_at(curve, 0.75, empty), 1.0), "a step holds its value");
  require(near(UiContracts::curve_at(curve, 1.0, empty), 0.25), "the last point ends the curve");
  const auto samples = UiContracts::sample_curve(curve, 4, empty);
  require(samples.size() == 5 && near(samples[1].toDouble(), std::pow(0.5, 3.0)) &&
              near(samples[2].toDouble(), 1.0),
          "sampling reads the curve at i / count");
}

void note_names() {
  require(UiContracts::note_name(middle_c, UiContracts::sharp(),
                                 UiContracts::scientific_middle_c()) == QStringLiteral("C4"),
          "middle C is C4 in scientific numbering");
  require(UiContracts::note_name(middle_c, UiContracts::sharp(), UiContracts::daw_middle_c()) ==
              QStringLiteral("C3"),
          "middle C is C3 in DAW numbering");
  require(UiContracts::note_name(c_minus_one, UiContracts::ascii(),
                                 UiContracts::scientific_middle_c()) == QStringLiteral("C-1"),
          "key 0 is C-1");
  require(UiContracts::note_name(b_minus_two, UiContracts::ascii(),
                                 UiContracts::scientific_middle_c()) == QStringLiteral("B-2"),
          "keys below 0 count octaves down");
  require(UiContracts::note_name(c_minus_one, UiContracts::sharp(),
                                 UiContracts::scientific_middle_c()) == QStringLiteral("C−1"),
          "a displayed octave below 0 takes the minus sign");
  require(UiContracts::pitch_class_name(1, UiContracts::sharp()) == QStringLiteral("C♯") &&
              UiContracts::pitch_class_name(1, UiContracts::ascii()) == QStringLiteral("C#") &&
              UiContracts::pitch_class_name(1, UiContracts::key()) == QStringLiteral("C♯") &&
              UiContracts::pitch_class_name(3, UiContracts::key()) == QStringLiteral("E♭") &&
              UiContracts::pitch_class_name(1, UiContracts::flat()) == QStringLiteral("D♭"),
          "pitch classes are spelled as asked");
  require(UiContracts::pitch_class_names(UiContracts::sharp()).size() ==
              UiContracts::semitones_per_octave(),
          "twelve pitch class names");
  require(near(UiContracts::midi_of_hz(a4_hz), a4) &&
              near(UiContracts::midi_of_hz(2 * a4_hz), a4 + 12),
          "A4 is 440 Hz and an octave doubles it");
}

void panel_catalogue() {
  for (const auto& panel : quick::device_panels()) {
    const bool synth = oscillate::native_dsp::synth::find_engine(panel.id) != nullptr;
    const bool effect = oscillate::native_dsp::effects::find_engine(panel.id) != nullptr;
    require(synth || effect, "a catalogued panel names a registered engine");
    const auto url = QString::fromUtf8(panel.url.data(), static_cast<qsizetype>(panel.url.size()));
    require(url.startsWith(QStringLiteral("qrc:")) && QFile::exists(url.mid(3)),
            "a catalogued panel's QML is in the shell's resources");
    require(panel.reflows || (panel.width > 0 && panel.height > 0),
            "a fixed-size panel names its design size");
  }
  // Every instrument has a panel; an unknown engine has none (the editor
  // window says so rather than showing another engine's panel).
  for (const auto* engine : oscillate::native_dsp::synth::engines())
    require(quick::device_panel(engine->descriptor->id) != nullptr, "an instrument lacks a panel");
  require(quick::device_panel("no-such-engine") == nullptr, "an unknown engine has a panel");
}

} // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  // The shell's resources live in the Quick core library.
  Q_INIT_RESOURCE(qml);
  try {
    bend_law();
    curve_values();
    note_names();
    panel_catalogue();
  } catch (const std::exception& error) {
    std::cerr << "UI contracts test: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
