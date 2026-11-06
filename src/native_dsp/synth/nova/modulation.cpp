#include "modulation.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace oscillate::native_dsp::synth::nova {

CurveShape sample_curve(std::span<const project::NativeCurvePoint> points) {
  CurveShape shape;
  std::size_t segment = 0;
  for (std::size_t i = 0; i <= curve_resolution; ++i) {
    const double x = static_cast<double>(i) / curve_resolution;
    while (segment + 2 < points.size() && x >= points[segment + 1].x)
      ++segment;
    const auto& a = points[segment];
    const auto& b = points[std::min(segment + 1, points.size() - 1)];
    double y = a.y;
    if (a.segment == project::NativeCurveSegment::curve && b.x > a.x) {
      const double t = std::clamp((x - a.x) / (b.x - a.x), 0.0, 1.0);
      y = a.y + (b.y - a.y) * bend(t, a.tension);
    }
    shape.y[i] = static_cast<float>(y);
  }
  // The last sample is the cycle's end: a held final segment keeps its value.
  return shape;
}

const CurveShape& default_lfo_shape() {
  static const CurveShape shape = [] {
    const std::array<project::NativeCurvePoint, 3> points{{{0.0, 0.0}, {0.5, 1.0}, {1.0, 0.0}}};
    return sample_curve(points);
  }();
  return shape;
}

const CurveShape& identity_shape() {
  static const CurveShape shape = [] {
    const std::array<project::NativeCurvePoint, 2> points{{{0.0, 0.0}, {1.0, 1.0}}};
    return sample_curve(points);
  }();
  return shape;
}

std::optional<Source> source_from_id(std::string_view id) noexcept {
  static constexpr std::array<std::string_view, source_count> ids{
      "env.1",   "env.2",    "env.3",       "env.4",       "lfo.1",        "lfo.2",   "lfo.3",
      "lfo.4",   "lfo.5",    "lfo.6",       "lfo.7",       "lfo.8",        "macro.1", "macro.2",
      "macro.3", "macro.4",  "macro.5",     "macro.6",     "macro.7",      "macro.8", "velocity",
      "note",    "modwheel", "aftertouch",  "mpe.slide",   "mpe.pressure", "chaos.1", "chaos.2",
      "random",  "bend",     "voice.mod_a", "voice.mod_b", "voice.spread", "arp.mod", "arp.accent"};
  for (std::size_t i = 0; i < ids.size(); ++i) {
    if (ids[i] == id)
      return static_cast<Source>(i);
  }
  return std::nullopt;
}

void Modulated::evaluate(std::span<const double> base, std::span<const CompiledRoute> routes,
                         const SourceValues& sources) noexcept {
  const auto& table = descriptor().parameters;
  const auto& slots = index().route_amount;
  for (const auto touched : list_)
    touched_[touched] = 0;
  list_.clear();
  base_ = base;

  const auto run = [&](bool amounts) {
    const std::size_t first = list_.size();
    for (const auto& route : routes) {
      if (route.targets_amount != amounts)
        continue;
      const double s = sources.value[route.source];
      const double signal = route.bipolar ? 2.0 * s - 1.0 : s;
      const double shaped = std::copysign(bend(std::fabs(signal), route.tension), signal);
      const double aux = route.aux == aux_none ? 1.0 : sources.value[route.aux];
      const double contribution =
          get(slots[route.slot]) * shaped * aux * route.scale * sources.gain[route.source];
      contribution_[route.slot] = contribution;
      const auto d = route.destination;
      if (!touched_[d]) {
        value_[d] = to_normalised(table[d], base[d]);
        touched_[d] = 1;
        list_.push_back(d);
      }
      value_[d] += contribution;
    }
    // Back to plain values, so get() reads them like base values.
    for (std::size_t i = first; i < list_.size(); ++i) {
      const auto d = list_[i];
      value_[d] = from_normalised(table[d], std::clamp(value_[d], 0.0, 1.0));
    }
  };
  for (const auto& route : routes)
    contribution_[route.slot] = 0.0;
  run(true);
  run(false);
}

double Envelope::seconds(Stage stage) const noexcept {
  constexpr double shortest = 0.0005; // Even a zero attack never clicks.
  switch (stage) {
  case Stage::attack:
    return std::max(shortest, settings_.attack);
  case Stage::hold:
    return settings_.hold;
  case Stage::decay:
    return std::max(shortest, settings_.decay);
  case Stage::release:
    return std::max(shortest, settings_.release);
  default:
    return 0.0;
  }
}

void Envelope::enter(Stage stage) noexcept {
  stage_ = stage;
  start_ = level_;
  progress_ = 0.0;
  if (stage == Stage::hold && settings_.hold <= 0.0)
    enter(Stage::decay);
}

void Envelope::gate_on(bool legato_note) noexcept {
  if (legato_note && settings_.legato && stage_ != Stage::idle && stage_ != Stage::release)
    return;
  enter(Stage::attack);
}

void Envelope::gate_off() noexcept {
  if (stage_ != Stage::idle && stage_ != Stage::release)
    enter(Stage::release);
}

// Segment shapes: a positive curve starts fast, like an analog RC stage.
double Envelope::advance(std::uint32_t frames) noexcept {
  double remaining = frames;
  while (remaining > 0.0) {
    switch (stage_) {
    case Stage::idle:
      level_ = 0.0;
      return level_;
    case Stage::sustain:
      level_ = settings_.sustain;
      if (settings_.loop) {
        enter(Stage::attack);
        continue;
      }
      return level_;
    default:
      break;
    }
    const double length = seconds(stage_) * rate_;
    const double step = length > 0.0 ? 1.0 / length : 1.0;
    const double needed = (1.0 - progress_) / step;
    const double used = std::min(remaining, needed);
    progress_ = std::min(1.0, progress_ + used * step);
    remaining -= used;
    switch (stage_) {
    case Stage::attack:
      level_ = start_ + (1.0 - start_) * bend(progress_, -settings_.attack_curve);
      break;
    case Stage::hold:
      level_ = 1.0;
      break;
    case Stage::decay:
      level_ = start_ + (settings_.sustain - start_) * bend(progress_, -settings_.decay_curve);
      break;
    case Stage::release:
      level_ = start_ * (1.0 - bend(progress_, -settings_.release_curve));
      break;
    default:
      break;
    }
    if (progress_ >= 1.0) {
      switch (stage_) {
      case Stage::attack:
        level_ = 1.0;
        enter(Stage::hold);
        break;
      case Stage::hold:
        enter(Stage::decay);
        break;
      case Stage::decay:
        level_ = settings_.sustain;
        if (settings_.loop)
          enter(Stage::attack);
        else
          stage_ = Stage::sustain;
        break;
      case Stage::release:
        level_ = 0.0;
        stage_ = Stage::idle;
        return level_;
      default:
        break;
      }
    }
  }
  return level_;
}

void Lfo::reset() noexcept {
  phase_ = 0.0;
  elapsed_ = 0.0;
  value_ = 0.0;
  smoothed_ = 0.0;
  gain_ = 1.0;
  started_ = false;
  finished_ = false;
}

void Lfo::trigger() noexcept {
  phase_ = 0.0;
  elapsed_ = 0.0;
  finished_ = false;
  started_ = false;
}

double Lfo::tick(const LfoSettings& settings, const CurveShape& shape, double seconds,
                 double song_cycles) noexcept {
  elapsed_ += seconds;
  const bool waiting = elapsed_ < settings.delay;
  double read = 0.0;
  if (settings.mode == LfoMode::free) {
    read = song_cycles + settings.phase;
  } else {
    if (!waiting && !finished_) {
      phase_ += settings.rate_hz * seconds;
    }
    if (settings.mode == LfoMode::env && phase_ >= 1.0) {
      phase_ = 1.0;
      finished_ = true;
    }
    read = phase_ + settings.phase;
  }
  double raw = 0.0;
  if (settings.mode == LfoMode::env && finished_)
    raw = shape.y.back();
  else
    raw = shape.at(read);
  const double tau = settings.smooth * 0.25;
  if (!started_ || tau <= 0.0) {
    smoothed_ = raw;
  } else {
    smoothed_ += (raw - smoothed_) * (1.0 - std::exp(-seconds / tau));
  }
  started_ = true;
  value_ = smoothed_;
  if (waiting)
    gain_ = 0.0;
  else if (settings.rise > 0.0)
    gain_ = std::min(1.0, (elapsed_ - settings.delay) / settings.rise);
  else
    gain_ = 1.0;
  return value_;
}

void Chaos::reset() noexcept {
  x_ = 0.1;
  y_ = 0.0;
  z_ = 0.0;
  logistic_ = 0.3141;
  logistic_phase_ = 0.0;
  raw_ = {};
  smoothed_ = {};
}

double Chaos::tick(std::size_t which, double rate_hz, double smooth, double seconds) noexcept {
  if (which == 0) {
    // Lorenz at its classic constants; one unit of time is about a loop, so
    // the rate scales time directly. Integrated in small Euler steps.
    constexpr double sigma = 10.0, rho = 28.0, beta = 8.0 / 3.0;
    double time = rate_hz * seconds;
    while (time > 0.0) {
      const double dt = std::min(time, 0.002);
      const double dx = sigma * (y_ - x_);
      const double dy = x_ * (rho - z_) - y_;
      const double dz = x_ * y_ - beta * z_;
      x_ += dx * dt;
      y_ += dy * dt;
      z_ += dz * dt;
      time -= dt;
    }
    raw_[0] = std::clamp(0.5 + x_ / 40.0, 0.0, 1.0);
  } else {
    logistic_phase_ += rate_hz * seconds;
    while (logistic_phase_ >= 1.0) {
      logistic_phase_ -= 1.0;
      logistic_ = 3.9 * logistic_ * (1.0 - logistic_);
    }
    raw_[1] = logistic_;
  }
  const double tau = smooth * 0.5;
  smoothed_[which] = tau <= 0.0 ? raw_[which]
                                : smoothed_[which] + (raw_[which] - smoothed_[which]) *
                                                         (1.0 - std::exp(-seconds / tau));
  return smoothed_[which];
}

} // namespace oscillate::native_dsp::synth::nova
