#include "ui_contracts.hpp"

#include <QCoreApplication>
#include <QQmlEngine>
#include <QStringList>
#include <QVariantMap>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <mutex>
#include <string_view>
#include <vector>

namespace oscillate::quick {
namespace {

// The bend's exponent per unit of tension (spec 08a §3.2).
constexpr double bend_exponent_per_tension = 4.0;
// A segment whose midpoint sits closer than this to either end needs more
// tension than a curve holds; the inverse clamps there.
constexpr double midpoint_margin = 0.03;
constexpr double midpoint_t = 0.5;
constexpr double max_tension = 1.0;

constexpr std::array<std::string_view, UiContracts::octave_semitones> sharp_names{
    "C", "C♯", "D", "D♯", "E", "F", "F♯", "G", "G♯", "A", "A♯", "B"};
constexpr std::array<std::string_view, UiContracts::octave_semitones> ascii_names{
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
constexpr std::array<std::string_view, UiContracts::octave_semitones> key_names{
    "C", "C♯", "D", "E♭", "E", "F", "F♯", "G", "A♭", "A", "B♭", "B"};
constexpr std::array<std::string_view, UiContracts::octave_semitones> flat_names{
    "C", "D♭", "D", "E♭", "E", "F", "G♭", "G", "A♭", "A", "B♭", "B"};

// Concert pitch: A4, MIDI 69, at 440 Hz.
constexpr double concert_a_hz = 440.0;
constexpr double concert_a_key = 69.0;

// The octave a MIDI key's numbering starts from: key 0 is C(-1) when middle
// C is C4.
constexpr int scientific_octave_of_key_zero = -1;

int wrap_pitch_class(int pitch_class) noexcept {
  return ((pitch_class % UiContracts::octave_semitones) + UiContracts::octave_semitones) %
         UiContracts::octave_semitones;
}

std::vector<project::NativeCurvePoint> points_of(const QVariantList& list) {
  std::vector<project::NativeCurvePoint> points;
  points.reserve(static_cast<std::size_t>(list.size()));
  for (const auto& entry : list) {
    const QVariantMap map = entry.toMap();
    points.push_back(
        {map.value(QStringLiteral("x")).toDouble(), map.value(QStringLiteral("y")).toDouble(),
         map.value(QStringLiteral("tension")).toDouble(),
         map.value(QStringLiteral("step")).toBool() ? project::NativeCurveSegment::step
                                                    : project::NativeCurveSegment::curve});
  }
  return points;
}

QObject* make_contracts(QQmlEngine*, QJSEngine*) { return new UiContracts(); }

} // namespace

double curve_bend(double t, double tension) noexcept {
  t = std::clamp(t, 0.0, 1.0);
  if (tension > 0.0)
    return std::pow(t, 1.0 + bend_exponent_per_tension * tension);
  if (tension < 0.0)
    return 1.0 - std::pow(1.0 - t, 1.0 - bend_exponent_per_tension * tension);
  return t;
}

double curve_value(std::span<const project::NativeCurvePoint> points, double x,
                   double empty_value) noexcept {
  if (points.empty())
    return empty_value;
  if (x <= points.front().x)
    return points.front().y;
  if (x >= points.back().x)
    return points.back().y;
  std::size_t segment = 0;
  while (segment + 2 < points.size() && x >= points[segment + 1].x)
    ++segment;
  const auto& a = points[segment];
  const auto& b = points[segment + 1];
  if (a.segment == project::NativeCurveSegment::step || b.x <= a.x)
    return a.y;
  return a.y + (b.y - a.y) * curve_bend((x - a.x) / (b.x - a.x), a.tension);
}

double curve_tension_for_midpoint(double fraction) noexcept {
  // bend(0.5, k) = f solved for k on each side of a straight line.
  const double f = std::clamp(fraction, midpoint_margin, 1.0 - midpoint_margin);
  const double tension =
      f < midpoint_t ? (std::log(f) / std::log(midpoint_t) - 1.0) / bend_exponent_per_tension
                     : (1.0 - std::log(1.0 - f) / std::log(midpoint_t)) / bend_exponent_per_tension;
  return std::clamp(tension, -max_tension, max_tension);
}

double UiContracts::curve_at(const QVariantList& points, double x, double empty_value) {
  return curve_value(points_of(points), x, empty_value);
}

QVariantList UiContracts::sample_curve(const QVariantList& points, int count, double empty_value) {
  const auto parsed = points_of(points);
  QVariantList values;
  if (count <= 0)
    return values;
  values.reserve(count + 1);
  for (int i = 0; i <= count; ++i)
    values.append(curve_value(parsed, static_cast<double>(i) / count, empty_value));
  return values;
}

QString UiContracts::pitch_class_name(int pitch_class, int spelling) {
  const auto index = static_cast<std::size_t>(wrap_pitch_class(pitch_class));
  const auto& names = spelling == ascii_spelling  ? ascii_names
                      : spelling == key_spelling  ? key_names
                      : spelling == flat_spelling ? flat_names
                                                  : sharp_names;
  return QString::fromUtf8(names[index].data(), static_cast<qsizetype>(names[index].size()));
}

QStringList UiContracts::pitch_class_names(int spelling) {
  QStringList names;
  for (int pitch_class = 0; pitch_class < octave_semitones; ++pitch_class)
    names.push_back(pitch_class_name(pitch_class, spelling));
  return names;
}

double UiContracts::midi_of_hz(double hz) noexcept {
  return concert_a_key + octave_semitones * std::log2(hz / concert_a_hz);
}

QString UiContracts::note_name(int key, int spelling, int middle_c_octave) {
  const int below = key < 0 ? octave_semitones - 1 : 0;
  const int octave = (key - below) / octave_semitones + scientific_octave_of_key_zero +
                     (middle_c_octave - scientific_middle_c_octave);
  return pitch_class_name(key, spelling) + QString::number(octave);
}

void register_ui_contracts() {
  static std::once_flag registered;
  std::call_once(registered, [] {
    qmlRegisterSingletonType<UiContracts>("Oscillate.Contracts", 1, 0, "Contracts",
                                          &make_contracts);
  });
}

} // namespace oscillate::quick

namespace {
// Any program that loads this library can load QML that imports the module,
// also those that build no device controller or bridge first.
void register_at_startup() { oscillate::quick::register_ui_contracts(); }
} // namespace
Q_COREAPP_STARTUP_FUNCTION(register_at_startup)
