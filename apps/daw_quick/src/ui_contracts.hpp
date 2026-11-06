#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include <span>

#include "oscillate/project/model.hpp"

// Small engine contracts the panels draw with, in one place for QML
// (`import Oscillate.Contracts` gives the singleton `Contracts`): the bend
// every drawn curve uses and the names of notes. Panels read these instead of
// keeping their own copies in JavaScript.
namespace oscillate::quick {

// The one bend every drawn curve uses (Nova's curves and routes, Pump's
// shape, Anvil's body; spec 08a §3.2): tension above 0 holds the start
// longer, below 0 leaves it sooner. t is clamped to 0..1.
[[nodiscard]] double curve_bend(double t, double tension) noexcept;
// A drawn curve's value at x: the segment holding x, bent by its start
// point's tension, or held flat after a step point. Before the first point
// and after the last the curve holds their values; an empty curve is
// `empty_value`.
[[nodiscard]] double curve_value(std::span<const project::NativeCurvePoint> points, double x,
                                 double empty_value) noexcept;
// The tension that puts a segment's midpoint `fraction` of the way from its
// start value to its end value (the inverse of the bend at t = 0.5).
[[nodiscard]] double curve_tension_for_midpoint(double fraction) noexcept;

class UiContracts final : public QObject {
  Q_OBJECT
  // How a pitch class is spelled: C♯ (sharps), C# (ASCII sharps), as key
  // names (C♯, E♭, F♯, A♭, B♭) or all flats (D♭, E♭, G♭, A♭, B♭).
  Q_PROPERTY(int sharp READ sharp CONSTANT)
  Q_PROPERTY(int ascii READ ascii CONSTANT)
  Q_PROPERTY(int key READ key CONSTANT)
  Q_PROPERTY(int flat READ flat CONSTANT)
  // Octave numbering: middle C (MIDI 60) is C4 in scientific pitch notation
  // and C3 in the numbering most DAWs and samplers show.
  Q_PROPERTY(int scientific_middle_c READ scientific_middle_c CONSTANT)
  Q_PROPERTY(int daw_middle_c READ daw_middle_c CONSTANT)
  Q_PROPERTY(int semitones_per_octave READ semitones_per_octave CONSTANT)

public:
  enum Spelling : int {
    sharp_spelling = 0,
    ascii_spelling = 1,
    key_spelling = 2,
    flat_spelling = 3
  };
  static constexpr int scientific_middle_c_octave = 4;
  static constexpr int daw_middle_c_octave = 3;
  static constexpr int octave_semitones = 12;

  explicit UiContracts(QObject* parent = nullptr) : QObject(parent) {}

  [[nodiscard]] static int sharp() noexcept { return sharp_spelling; }
  [[nodiscard]] static int ascii() noexcept { return ascii_spelling; }
  [[nodiscard]] static int key() noexcept { return key_spelling; }
  [[nodiscard]] static int flat() noexcept { return flat_spelling; }
  [[nodiscard]] static int scientific_middle_c() noexcept { return scientific_middle_c_octave; }
  [[nodiscard]] static int daw_middle_c() noexcept { return daw_middle_c_octave; }
  [[nodiscard]] static int semitones_per_octave() noexcept { return octave_semitones; }

  Q_INVOKABLE static double bend(double t, double tension) noexcept {
    return curve_bend(t, tension);
  }
  // points: [{x, y, tension, step}] as the document model hands them out.
  Q_INVOKABLE static double curve_at(const QVariantList& points, double x, double empty_value);
  // `count` + 1 values of the curve at x = 0, 1/count, ... 1.
  Q_INVOKABLE static QVariantList sample_curve(const QVariantList& points, int count,
                                               double empty_value);
  Q_INVOKABLE static double tension_for_midpoint(double fraction) noexcept {
    return curve_tension_for_midpoint(fraction);
  }

  // A pitch class (any integer, wrapped to 0..11) spelled as `spelling`.
  Q_INVOKABLE static QString pitch_class_name(int pitch_class, int spelling);
  // The twelve pitch class names from C, spelled as `spelling`.
  Q_INVOKABLE static QStringList pitch_class_names(int spelling);
  // A frequency as a fractional MIDI note (A4 = 440 Hz is 69).
  Q_INVOKABLE static double midi_of_hz(double hz) noexcept;
  // A MIDI key as a name and octave, numbered so that key 60 is
  // C<middle_c_octave>.
  Q_INVOKABLE static QString note_name(int key, int spelling, int middle_c_octave);
};

// Registers the `Oscillate.Contracts` module once; safe to call repeatedly.
// Programs that load panel or shell QML call it before loading (the device
// controller and the bridge do so on construction).
void register_ui_contracts();

} // namespace oscillate::quick
