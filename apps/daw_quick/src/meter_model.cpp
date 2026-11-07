#include "meter_model.hpp"

#include "oscillate/foundation/trace.hpp"

#include <algorithm>
#include <cmath>

namespace oscillate::quick {
namespace {

[[nodiscard]] double to_db(double amplitude) {
  return amplitude <= 0.0 ? MeterChannel::floor_db
                          : std::max(MeterChannel::floor_db, 20.0 * std::log10(amplitude));
}

[[nodiscard]] double to_amplitude(double db) {
  return db <= MeterChannel::floor_db ? 0.0 : std::pow(10.0, db / 20.0);
}

// Falls `amplitude` by `db` decibels, to silence below the floor.
[[nodiscard]] double fall(double amplitude, double db) {
  return amplitude <= 0.0 ? 0.0 : to_amplitude(to_db(amplitude) - db);
}

[[nodiscard]] float finite_level(float value) {
  return std::isfinite(value) ? std::clamp(value, 0.0F, 16.0F) : 0.0F;
}

} // namespace

void MeterChannel::advance_side(Side& side, std::optional<float> peak, std::optional<float> rms,
                                double seconds) {
  const double incoming = peak ? finite_level(*peak) : 0.0;
  side.peak = std::max(incoming, fall(side.peak, peak_fall_db_per_second * seconds));
  if (incoming >= side.hold) {
    side.hold = incoming;
    side.hold_left = hold_seconds;
  } else if (side.hold_left > 0.0) {
    side.hold_left = std::max(0.0, side.hold_left - seconds);
  } else {
    side.hold = std::max(side.peak, fall(side.hold, hold_fall_db_per_second * seconds));
  }
  // RMS smooths in power, as an analogue meter's integration does.
  const double target = rms ? static_cast<double>(finite_level(*rms)) : 0.0;
  const double blend = 1.0 - std::exp(-seconds / rms_seconds);
  side.power += (target * target - side.power) * blend;
  side.rms = std::sqrt(std::max(0.0, side.power));
  if (to_db(side.rms) <= floor_db) {
    side.power = 0.0;
    side.rms = 0.0;
  }
}

bool MeterChannel::advance(const std::optional<MeterReading>& reading, double seconds) {
  seconds = std::clamp(std::isfinite(seconds) ? seconds : 0.0, 0.0, 0.25);
  advance_side(left_, reading ? std::optional(reading->left_peak) : std::nullopt,
               reading ? std::optional(reading->left_rms) : std::nullopt, seconds);
  advance_side(right_, reading ? std::optional(reading->right_peak) : std::nullopt,
               reading ? std::optional(reading->right_rms) : std::nullopt, seconds);
  clip_left_ = reading && reading->clipped ? clip_seconds : std::max(0.0, clip_left_ - seconds);
  const bool visible = left_.visible() || right_.visible() || clip_left_ > 0.0;
  // A meter shows only a measured level or its fall from one.
  valid_ = reading.has_value() || visible;
  emit changed();
  return reading.has_value() || visible;
}

MeterModel::MeterModel(Source source, QObject* parent)
    : QObject(parent), source_(std::move(source)), timer_(this) {
  timer_.setInterval(interval_ms);
  timer_.setTimerType(Qt::PreciseTimer);
  connect(&timer_, &QTimer::timeout, this, [this] {
    const double seconds = clock_.isValid() ? static_cast<double>(clock_.restart()) / 1000.0 : 0.0;
    tick(seconds);
  });
}

QObject* MeterModel::channel(const QString& id) {
  auto found = channels_.find(id);
  if (found == channels_.end())
    found = channels_.insert(id, new MeterChannel(this));
  wake();
  return found.value();
}

void MeterModel::wake() {
  if (timer_.isActive())
    return;
  clock_.start();
  timer_.start();
}

void MeterModel::tick(double seconds) {
  OSCILLATE_TRACE_SPAN("ui.meter_tick");
  bool active = false;
  for (auto item = channels_.cbegin(); item != channels_.cend(); ++item) {
    const auto reading = source_ ? source_(item.key()) : std::nullopt;
    active = item.value()->advance(reading, seconds) || active;
  }
  if (!active)
    timer_.stop();
}

} // namespace oscillate::quick
