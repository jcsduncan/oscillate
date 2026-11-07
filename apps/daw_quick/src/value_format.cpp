#include "value_format.hpp"

#include "oscillate/foundation/value_text.hpp"

#include <QQmlEngine>

#include <mutex>
#include <string>
#include <string_view>

namespace oscillate::quick {
namespace {

namespace text = oscillate::foundation;

// A 0..1 fraction in percent.
constexpr double percent_per_unit = 100.0;

static_assert(text::automatic_decimals == -1, "ValueFormat's default decimals mirror the rule's");

QString q(const std::string& value) { return QString::fromStdString(value); }
QString q(std::string_view value) {
  return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

QString format(double value, std::string_view unit, text::ValueSign sign, int decimals) {
  return q(text::format_quantity(value, unit, sign, decimals));
}

QObject* make_format(QQmlEngine*, QJSEngine*) { return new ValueFormat; }

} // namespace

QString ValueFormat::minus() { return q(text::minus_sign); }
QString ValueFormat::sharp() { return q(text::sharp_sign); }
QString ValueFormat::flat() { return q(text::flat_sign); }
int ValueFormat::automatic() noexcept { return text::automatic_decimals; }

QString ValueFormat::number(double value, int decimals) {
  return q(text::format_number(value, decimals, text::ValueSign::plain));
}
QString ValueFormat::signed_number(double value, int decimals) {
  return q(text::format_number(value, decimals, text::ValueSign::explicit_plus));
}
QString ValueFormat::quantity(double value, const QString& unit, int decimals) {
  const auto utf8 = unit.toStdString();
  return format(value, utf8, text::ValueSign::plain, decimals);
}
QString ValueFormat::signed_quantity(double value, const QString& unit, int decimals) {
  const auto utf8 = unit.toStdString();
  return format(value, utf8, text::ValueSign::explicit_plus, decimals);
}
QString ValueFormat::decibels(double value, int decimals) {
  return format(value, text::decibels_unit, text::ValueSign::plain, decimals);
}
QString ValueFormat::signed_decibels(double value, int decimals) {
  return format(value, text::decibels_unit, text::ValueSign::explicit_plus, decimals);
}
QString ValueFormat::hertz(double value) {
  return format(value, text::hertz_unit, text::ValueSign::plain, text::automatic_decimals);
}
QString ValueFormat::seconds(double value) {
  return format(value, text::seconds_unit, text::ValueSign::plain, text::automatic_decimals);
}
QString ValueFormat::milliseconds(double value) {
  return format(value, text::milliseconds_unit, text::ValueSign::plain, text::automatic_decimals);
}
QString ValueFormat::percent(double value, int decimals) {
  return format(value, text::percent_unit, text::ValueSign::plain, decimals);
}
QString ValueFormat::fraction(double value, int decimals) {
  return percent(value * percent_per_unit, decimals);
}
QString ValueFormat::cents(double value) {
  return format(value, text::cents_unit, text::ValueSign::explicit_plus, text::automatic_decimals);
}
QString ValueFormat::semitones(double value, int decimals) {
  return format(value, text::semitones_unit, text::ValueSign::explicit_plus, decimals);
}
QString ValueFormat::degrees(double value) {
  return format(value, text::degrees_unit, text::ValueSign::plain, text::automatic_decimals);
}

void register_value_format() {
  static std::once_flag registered;
  std::call_once(registered, [] {
    qmlRegisterSingletonType<ValueFormat>("Oscillate.Format", 1, 0, "Format", &make_format);
  });
}

} // namespace oscillate::quick
