#pragma once

#include <QObject>
#include <QString>

// The display formatter (foundation/value_text.hpp) for QML: `import
// Oscillate.Format` gives the singleton `Format`. Every number a device
// panel shows goes through it, so minus signs, decimals and unit spellings
// follow one rule. Values are in the unit named: seconds for seconds(), a
// 0..1 fraction for fraction(), 0..100 for percent().
namespace oscillate::quick {

class ValueFormat final : public QObject {
  Q_OBJECT
  // U+2212, for text that joins a minus to something other than a number.
  Q_PROPERTY(QString minus READ minus CONSTANT)
  Q_PROPERTY(QString sharp READ sharp CONSTANT)
  Q_PROPERTY(QString flat READ flat CONSTANT)
  // The decimals argument's "use the unit's rule".
  Q_PROPERTY(int automatic READ automatic CONSTANT)

public:
  explicit ValueFormat(QObject* parent = nullptr) : QObject(parent) {}

  [[nodiscard]] static QString minus();
  [[nodiscard]] static QString sharp();
  [[nodiscard]] static QString flat();
  [[nodiscard]] static int automatic() noexcept;

  // A bare number, and one with "+" above zero.
  Q_INVOKABLE static QString number(double value, int decimals = automatic_places);
  Q_INVOKABLE static QString signed_number(double value, int decimals = automatic_places);
  // A number in a unit ("dB", "Hz", "s", "ms", "%", "ct", "st", "°", …).
  Q_INVOKABLE static QString quantity(double value, const QString& unit,
                                      int decimals = automatic_places);
  Q_INVOKABLE static QString signed_quantity(double value, const QString& unit,
                                             int decimals = automatic_places);
  // Shorthands for the common units.
  Q_INVOKABLE static QString decibels(double value, int decimals = automatic_places);
  Q_INVOKABLE static QString signed_decibels(double value, int decimals = automatic_places);
  Q_INVOKABLE static QString hertz(double value);
  Q_INVOKABLE static QString seconds(double value);
  Q_INVOKABLE static QString milliseconds(double value);
  Q_INVOKABLE static QString percent(double value, int decimals = automatic_places);
  Q_INVOKABLE static QString fraction(double value, int decimals = automatic_places);
  Q_INVOKABLE static QString cents(double value);
  Q_INVOKABLE static QString semitones(double value, int decimals = automatic_places);
  Q_INVOKABLE static QString degrees(double value);

private:
  static constexpr int automatic_places = -1;
};

// Registers `Oscillate.Format` once; safe to call from any thread before
// QML loads.
void register_value_format();

} // namespace oscillate::quick
