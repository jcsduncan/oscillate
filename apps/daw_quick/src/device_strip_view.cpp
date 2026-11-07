#include "device_strip_view.hpp"

#include "oscillate/native_dsp/effects/ceiling.hpp"
#include "oscillate/native_dsp/effects/lens.hpp"
#include "oscillate/native_dsp/effects/loudness.hpp"
#include "oscillate/native_dsp/effects/press.hpp"
#include "oscillate/native_dsp/effects/reduction_bands.hpp"
#include "oscillate/native_dsp/effects/scope.hpp"
#include "oscillate/native_dsp/effects/trim.hpp"

#include <QChar>
#include <QString>
#include <QVariantList>

#include <algorithm>
#include <cmath>

namespace oscillate::quick {
namespace {

namespace fx = native_dsp::effects;

constexpr QChar minus{0x2212};
constexpr QChar en_dash{0x2013};
constexpr QChar em_dash{0x2014};
constexpr int one_decimal = 1;
constexpr int two_decimals = 2;
constexpr double kilo = 1'000.0;
// Kilohertz under this keep a decimal ("3.6 kHz"), over it none ("20 kHz").
constexpr double whole_kilohertz_from = 10'000.0;

// Between two readings: " · ".
QString separator() { return QStringLiteral(" \u00b7 "); }

// Callers pass the named decimal constants.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
QString number(double value, int decimals) {
  return QString::number(value, 'f', decimals).replace(QLatin1Char('-'), minus);
}

QString hz_part(double hz) {
  if (hz < kilo)
    return QString::number(std::lround(hz));
  return hz < whole_kilohertz_from ? QString::number(hz / kilo, 'f', one_decimal)
                                   : QString::number(std::lround(hz / kilo));
}

QString hz_text(double hz) {
  return hz_part(hz) + (hz < kilo ? QStringLiteral(" Hz") : QStringLiteral(" kHz"));
}

QVariantList pair(std::span<const float> frame, std::size_t at) {
  return QVariantList{static_cast<double>(frame[at]), static_cast<double>(frame[at + 1])};
}

QVariantMap meters(std::span<const float> frame, double gain_db) {
  namespace t = fx::trim::telemetry;
  return {{QStringLiteral("kind"), QStringLiteral("meters")},
          {QStringLiteral("input_peak"), pair(frame, t::input_peak)},
          {QStringLiteral("input_rms"), pair(frame, t::input_rms)},
          {QStringLiteral("output_peak"), pair(frame, t::output_peak)},
          {QStringLiteral("output_rms"), pair(frame, t::output_rms)},
          {QStringLiteral("floor_db"), strip_view::meter_floor_db},
          {QStringLiteral("ceiling_db"), strip_view::meter_ceiling_db},
          {QStringLiteral("hot_db"), strip_view::meter_hot_db},
          {QStringLiteral("over_db"), strip_view::meter_over_db},
          {QStringLiteral("text"), DeviceStripView::signed_db(gain_db) + QStringLiteral(" dB")}};
}

QVariantMap curve(std::span<const float> frame) {
  namespace lens = fx::lens;
  namespace t = lens::telemetry;
  QVariantList values;
  values.reserve(static_cast<qsizetype>(lens::points / strip_view::lens_points_per_point));
  for (std::size_t first = 0; first + strip_view::lens_points_per_point <= lens::points;
       first += strip_view::lens_points_per_point) {
    const auto group = frame.subspan(t::curve_a + first, strip_view::lens_points_per_point);
    values.push_back(static_cast<double>(*std::ranges::max_element(group)));
  }
  // The strongest resonance leads the peaks as (hz, dB); 0 Hz when none.
  const double strongest_hz = frame[t::peaks];
  return {{QStringLiteral("kind"), QStringLiteral("curve")},
          {QStringLiteral("values"), values},
          {QStringLiteral("floor_db"), strip_view::spectrum_floor_db},
          {QStringLiteral("ceiling_db"), strip_view::spectrum_ceiling_db},
          {QStringLiteral("text"),
           strongest_hz > 0.0 ? QStringLiteral("peak ") + hz_text(strongest_hz) : QString()}};
}

QVariantMap scope(std::span<const float> frame) {
  namespace sc = fx::scope;
  namespace t = sc::telemetry;
  constexpr std::size_t per = strip_view::scope_columns_per_column;
  constexpr std::size_t columns = sc::columns / per;
  const auto key_values = frame.subspan(t::sidechain, 2 * sc::columns);
  const bool keyed = std::ranges::any_of(key_values, [](float v) { return v != 0.0F; });
  const auto reduce = [&](std::size_t first) {
    QVariantList out;
    out.reserve(static_cast<qsizetype>(2 * columns));
    for (std::size_t column = 0; column < columns; ++column) {
      float low = frame[first + 2 * column * per];
      float high = frame[first + 2 * column * per + 1];
      for (std::size_t k = 1; k < per; ++k) {
        low = std::min(low, frame[first + 2 * (column * per + k)]);
        high = std::max(high, frame[first + 2 * (column * per + k) + 1]);
      }
      out.push_back(static_cast<double>(low));
      out.push_back(static_cast<double>(high));
    }
    return out;
  };
  const auto write = std::clamp(static_cast<long>(std::lround(frame[t::write_column])), 0L,
                                static_cast<long>(sc::columns) - 1);
  const double correlation = std::clamp(static_cast<double>(frame[t::correlation]), 0.0, 1.0);
  return {{QStringLiteral("kind"), QStringLiteral("scope")},
          {QStringLiteral("track"), reduce(t::track)},
          {QStringLiteral("key"), keyed ? reduce(t::sidechain) : QVariantList{}},
          {QStringLiteral("columns"), static_cast<int>(columns)},
          {QStringLiteral("write_column"), static_cast<int>(static_cast<std::size_t>(write) / per)},
          {QStringLiteral("keyed"), keyed},
          {QStringLiteral("text"), keyed
                                       ? QStringLiteral("corr ") + number(correlation, two_decimals)
                                       : QStringLiteral("no key")}};
}

QString lufs_text(double lufs) {
  return lufs > strip_view::silent_below ? number(lufs, one_decimal) : QString(em_dash);
}

QVariantMap loudness(std::span<const float> frame, double target, double ceiling) {
  namespace ld = fx::loudness;
  namespace t = ld::telemetry;
  QVariantList history;
  history.reserve(static_cast<qsizetype>(ld::history));
  for (std::size_t i = 0; i < ld::history; ++i)
    history.push_back(static_cast<double>(frame[t::short_term_history + i]));
  const double true_peak = frame[t::true_peak];
  const bool peaked = true_peak > strip_view::silent_below;
  return {{QStringLiteral("kind"), QStringLiteral("loudness")},
          {QStringLiteral("history"), history},
          {QStringLiteral("target"), target},
          {QStringLiteral("below_lu"), strip_view::loudness_below_lu},
          {QStringLiteral("above_lu"), strip_view::loudness_above_lu},
          {QStringLiteral("text"), QStringLiteral("S ") + lufs_text(frame[t::short_term]) +
                                       separator() + QStringLiteral("I ") +
                                       lufs_text(frame[t::integrated]) + QStringLiteral(" LUFS")},
          {QStringLiteral("peak_text"), QStringLiteral("TP ") + lufs_text(true_peak)},
          {QStringLiteral("over"), peaked && true_peak > ceiling}};
}

} // namespace

QString DeviceStripView::signed_db(double db) {
  if (std::fabs(db) < strip_view::unchanged_db)
    return number(0.0, one_decimal);
  return (db > 0.0 ? QStringLiteral("+") : QString()) + number(db, one_decimal);
}

QString DeviceStripView::band_text(std::size_t band) {
  const double low = fx::reduction_bands::edge_hz(band);
  const double high = fx::reduction_bands::edge_hz(band + 1);
  if (high < kilo || low >= kilo)
    return hz_part(low) + en_dash + hz_text(high);
  return hz_text(low) + en_dash + hz_text(high);
}

void DeviceStripView::clear() noexcept {
  next_ = 0;
  filled_ = 0;
}

QVariantMap DeviceStripView::reduction(std::span<const float> frame, std::size_t reduction_at,
                                       std::size_t bands_at, double range_db) {
  const double now = std::max(0.0, static_cast<double>(frame[reduction_at]));
  history_[next_] = now;
  next_ = (next_ + 1) % history_.size();
  filled_ = std::min(filled_ + 1, history_.size());
  QVariantList history;
  history.reserve(static_cast<qsizetype>(filled_));
  double most = 0.0;
  for (std::size_t i = 0; i < filled_; ++i) {
    const double db = history_[(next_ + history_.size() - filled_ + i) % history_.size()];
    history.push_back(db);
    most = std::max(most, db);
  }
  QVariantList bands;
  int lead = -1;
  double lead_share = strip_view::least_leading_share;
  for (std::size_t band = 0; band < fx::reduction_bands::count; ++band) {
    const double share = frame[bands_at + band];
    bands.push_back(share);
    if (share >= lead_share) {
      lead_share = share;
      lead = static_cast<int>(band);
    }
  }
  return {{QStringLiteral("kind"), QStringLiteral("reduction")},
          {QStringLiteral("history"), history},
          {QStringLiteral("slots"), static_cast<int>(strip_view::history_points)},
          {QStringLiteral("range_db"), range_db},
          {QStringLiteral("text"), signed_db(-now) + QStringLiteral(" dB") + separator() +
                                       QStringLiteral("max ") + signed_db(-most)},
          {QStringLiteral("bands"), bands},
          {QStringLiteral("lead"), lead},
          {QStringLiteral("lead_text"),
           lead >= 0 ? band_text(static_cast<std::size_t>(lead)) : QString()}};
}

QVariantMap DeviceStripView::decode(std::string_view engine, std::span<const float> frame,
                                    const ValueOf& value) {
  if (engine == "ceiling" && frame.size() >= fx::ceiling::telemetry::size) {
    namespace t = fx::ceiling::telemetry;
    return reduction(frame, t::reduction, t::reduction_bands, strip_view::ceiling_range_db);
  }
  if (engine == "press" && frame.size() >= fx::press::telemetry::size) {
    namespace t = fx::press::telemetry;
    return reduction(frame, t::reduction, t::reduction_bands, strip_view::press_range_db);
  }
  if (engine == "trim" && frame.size() >= fx::trim::telemetry::size)
    return meters(frame, value("trim.gain"));
  if (engine == "lens" && frame.size() >= fx::lens::telemetry::size)
    return curve(frame);
  if (engine == "scope" && frame.size() >= fx::scope::telemetry::size)
    return scope(frame);
  if (engine == "loudness" && frame.size() >= fx::loudness::telemetry::size)
    return loudness(frame, value("loudness.target"), value("loudness.ceiling"));
  return {};
}

} // namespace oscillate::quick
