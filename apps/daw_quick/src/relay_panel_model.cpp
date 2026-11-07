#include "relay_panel_model.hpp"

#include "oscillate/native_dsp/effects/relay.hpp"
#include "oscillate/native_dsp/synth/tempo_sync.hpp"

#include <QString>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string_view>

namespace oscillate::quick {
namespace {

namespace synth = native_dsp::synth;
namespace relay = native_dsp::effects::relay;

// The kernel's loop constants (src/native_dsp/effects/relay.cpp).
constexpr double tape_hz = 11'000.0;
constexpr double analog_reference_seconds = 0.05;
constexpr double analog_max_hz = 12'000.0;
constexpr double analog_min_hz = 2'500.0;
// The diffusion chain's length: what one pass smears at full diffusion.
constexpr double diffusion_ms = 12.3;
constexpr double level_hz = 1'000.0;
constexpr double bright_hz = 4'000.0;

double value(const synth::EngineDescriptor& engine, std::span<const double> values,
             std::string_view id) {
  const auto index = synth::find_parameter(engine, id);
  return index && *index < values.size() ? values[*index] : 0.0;
}

double butterworth_low(double hz, double cutoff) {
  return 1.0 / std::sqrt(1.0 + std::pow(hz / cutoff, 4.0));
}
double butterworth_high(double hz, double cutoff) {
  return 1.0 / std::sqrt(1.0 + std::pow(cutoff / hz, 4.0));
}

// One pass's gain at `hz` through the feedback cuts and the character's
// loop filter, for a line `seconds` long.
double pass_gain(int character, double low_cut, double high_cut, double seconds, double hz) {
  double gain = butterworth_high(hz, low_cut) * butterworth_low(hz, high_cut);
  if (character == 1) {
    gain /= std::sqrt(1.0 + (hz / tape_hz) * (hz / tape_hz));
  } else if (character == 2) {
    const double clock = std::clamp(analog_max_hz * std::sqrt(analog_reference_seconds / seconds),
                                    analog_min_hz, analog_max_hz);
    gain *= butterworth_low(hz, clock);
  }
  return gain;
}

} // namespace

std::array<double, 2> relay_times(const synth::EngineDescriptor& engine,
                                  std::span<const double> values, double bpm,
                                  double beats_per_bar) {
  double seconds = value(engine, values, "relay.time") * 1e-3;
  if (value(engine, values, "relay.sync") != 0.0) {
    const auto division =
        std::min(static_cast<std::size_t>(value(engine, values, "relay.division")),
                 synth::sync_division_count - 1);
    const double beats = synth::sync_in_bars(division)
                             ? synth::sync_lengths[division] * beats_per_bar
                             : synth::sync_lengths[division];
    seconds = beats * 60.0 / std::max(1.0, bpm);
  }
  seconds = std::clamp(seconds, 1e-3, relay::max_seconds);
  return {seconds, seconds * (1.0 + value(engine, values, "relay.offset"))};
}

std::vector<RelayEcho> relay_echo_train(const synth::EngineDescriptor& engine,
                                        std::span<const double> values, double bpm,
                                        double beats_per_bar, double horizon,
                                        std::size_t max_echoes) {
  const auto times = relay_times(engine, values, bpm, beats_per_bar);
  const auto character = static_cast<int>(value(engine, values, "relay.character"));
  const auto routing = static_cast<int>(value(engine, values, "relay.routing"));
  const double feedback = value(engine, values, "relay.feedback");
  const double low_cut = value(engine, values, "relay.low_cut");
  const double high_cut = value(engine, values, "relay.high_cut");
  const double diffusion = value(engine, values, "relay.diffusion");

  // Repeat `pass` (1-based), having run `passes[c]` times through line c.
  const auto echo = [&](double at, int pass, int channel, std::array<int, 2> passes) {
    double level = std::pow(feedback, pass - 1);
    double bright = 1.0;
    for (std::size_t c = 0; c < 2; ++c) {
      const double g = pass_gain(character, low_cut, high_cut, times[c], level_hz);
      const double b = pass_gain(character, low_cut, high_cut, times[c], bright_hz);
      level *= std::pow(g, passes[c]);
      bright *= std::pow(b / g, passes[c]);
    }
    return RelayEcho{at, level, channel, diffusion_ms * diffusion * std::sqrt(pass), bright};
  };
  const auto ended = [&](const RelayEcho& e) { return e.seconds > horizon || e.level < 1e-3; };

  std::vector<RelayEcho> out;
  if (routing == 1) {
    // Ping-pong: the mono sum enters the left line, and each repeat
    // crosses to the other side.
    double at = 0.0;
    std::array<int, 2> passes{0, 0};
    for (int pass = 1; out.size() < max_echoes; ++pass) {
      const std::size_t side = pass % 2 == 1 ? 0 : 1;
      at += times[side];
      ++passes[side];
      const auto e = echo(at, pass, static_cast<int>(side), passes);
      if (ended(e)) {
        break;
      }
      out.push_back(e);
    }
    return out;
  }
  // Stereo: two independent lanes; mono: one lane heard on both sides.
  const int lanes = routing == 2 ? 1 : 2;
  for (int lane = 0; lane < lanes; ++lane) {
    const auto side = static_cast<std::size_t>(lane);
    for (int pass = 1; static_cast<std::size_t>(pass) <= max_echoes; ++pass) {
      std::array<int, 2> passes{0, 0};
      passes[side] = pass;
      const auto e = echo(times[side] * pass, pass, routing == 2 ? 2 : lane, passes);
      if (ended(e)) {
        break;
      }
      out.push_back(e);
    }
  }
  std::stable_sort(out.begin(), out.end(),
                   [](const RelayEcho& a, const RelayEcho& b) { return a.seconds < b.seconds; });
  if (out.size() > max_echoes) {
    out.resize(max_echoes);
  }
  return out;
}

RelayPanelModel::RelayPanelModel(NativeSynthDeviceController& controller,
                                 NativeSynthPanelAdapter& adapter, QObject* parent)
    : QObject(parent), adapter_(adapter) {
  connect(&controller, &NativeSynthDeviceController::values_changed, this, [this] {
    ++revision_;
    emit changed();
  });
}

void RelayPanelModel::set_tempo(double bpm) {
  if (bpm > 0.0 && bpm != tempo_) {
    tempo_ = bpm;
    ++revision_;
    emit changed();
  }
}

void RelayPanelModel::set_beats_per_bar(double beats) {
  if (beats > 0.0 && beats != beats_per_bar_) {
    beats_per_bar_ = beats;
    ++revision_;
    emit changed();
  }
}

QVariantList RelayPanelModel::echoes(double horizon, int max_echoes) const {
  const auto values = adapter_.values();
  QVariantList out;
  for (const auto& e :
       relay_echo_train(*relay::engine().descriptor, values, tempo_, beats_per_bar_, horizon,
                        static_cast<std::size_t>(std::max(0, max_echoes)))) {
    QVariantMap entry;
    entry[QStringLiteral("seconds")] = e.seconds;
    entry[QStringLiteral("level")] = e.level;
    entry[QStringLiteral("db")] = 20.0 * std::log10(std::max(e.level, 1e-9));
    entry[QStringLiteral("channel")] = e.channel;
    entry[QStringLiteral("spread_ms")] = e.spread_ms;
    entry[QStringLiteral("brightness")] = e.brightness;
    out.push_back(entry);
  }
  return out;
}

double RelayPanelModel::time_ms(int channel) const {
  const auto values = adapter_.values();
  const auto times = relay_times(*relay::engine().descriptor, values, tempo_, beats_per_bar_);
  return times[channel == 1 ? 1 : 0] * 1e3;
}

double RelayPanelModel::tail_seconds(double horizon) const {
  const auto values = adapter_.values();
  const auto train =
      relay_echo_train(*relay::engine().descriptor, values, tempo_, beats_per_bar_, horizon, 512);
  return train.empty() ? 0.0 : train.back().seconds;
}

QVariantMap RelayPanelModel::telemetry() {
  namespace t = relay::telemetry;
  QVariantMap out;
  std::vector<float> frame;
  const bool live = adapter_.telemetry(frame) && frame.size() >= t::size;
  out[QStringLiteral("live")] = live;
  if (!live) {
    return out;
  }
  out[QStringLiteral("time_left_ms")] = static_cast<double>(frame[t::time_left_ms]);
  out[QStringLiteral("time_right_ms")] = static_cast<double>(frame[t::time_right_ms]);
  out[QStringLiteral("duck_db")] = static_cast<double>(frame[t::duck_db]);
  const double rms = std::max(0.0, static_cast<double>(frame[t::wet_rms]));
  out[QStringLiteral("wet_rms")] = rms;
  out[QStringLiteral("wet_db")] = 20.0 * std::log10(std::max(rms, 1e-9));
  return out;
}

} // namespace oscillate::quick
