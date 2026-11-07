#pragma once

#include "native_synth_device_controller.hpp"
#include "native_synth_panel_adapter.hpp"

#include "oscillate/native_dsp/synth/parameters.hpp"

#include <QObject>
#include <QVariantList>
#include <QVariantMap>

#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace oscillate::quick {

// One repeat of Relay's echo train, as the panel draws it.
struct RelayEcho {
  double seconds = 0.0;    // After the dry hit.
  double level = 0.0;      // Linear, relative to the input, at 1 kHz.
  int channel = 0;         // 0 left, 1 right, 2 both (mono routing).
  double spread_ms = 0.0;  // How far diffusion smears it.
  double brightness = 1.0; // Its 4 kHz level relative to its 1 kHz level.
};

// Relay's line lengths for `values` (dense, in table order) at a tempo and
// meter, clamped to the engine's range: {left, right} in seconds.
[[nodiscard]] std::array<double, 2> relay_times(const native_dsp::synth::EngineDescriptor& engine,
                                                std::span<const double> values, double bpm,
                                                double beats_per_bar);

// The repeats Relay makes from one hit on both inputs, in time order, from
// the kernel's own routing and loop laws: every pass runs the feedback cuts
// (12 dB/oct Butterworth) and the character's loop filter, and each repeat
// after the first scales by the feedback. Stops at `horizon` seconds,
// `max_echoes` repeats, or -60 dB.
[[nodiscard]] std::vector<RelayEcho>
relay_echo_train(const native_dsp::synth::EngineDescriptor& engine, std::span<const double> values,
                 double bpm, double beats_per_bar, double horizon, std::size_t max_echoes);

// Relay-specific panel state beside the generic device controller: the echo
// train for the current values and Relay's decoded live state.
class RelayPanelModel final : public QObject {
  Q_OBJECT
  // The project tempo and meter, for synced times; root sets them.
  Q_PROPERTY(double tempo READ tempo WRITE set_tempo NOTIFY changed)
  Q_PROPERTY(double beats_per_bar READ beats_per_bar WRITE set_beats_per_bar NOTIFY changed)
  // Increments whenever the values, tempo or meter change.
  Q_PROPERTY(int revision READ revision NOTIFY changed)

public:
  RelayPanelModel(NativeSynthDeviceController& controller, NativeSynthPanelAdapter& adapter,
                  QObject* parent = nullptr);

  [[nodiscard]] double tempo() const noexcept { return tempo_; }
  void set_tempo(double bpm);
  [[nodiscard]] double beats_per_bar() const noexcept { return beats_per_bar_; }
  void set_beats_per_bar(double beats);
  [[nodiscard]] int revision() const noexcept { return revision_; }

  // [{seconds, level, db, channel, spread_ms, brightness}].
  Q_INVOKABLE QVariantList echoes(double horizon, int max_echoes) const;
  // The left (0) or right (1) line in milliseconds.
  Q_INVOKABLE double time_ms(int channel) const;
  // The last audible repeat's time, within `horizon`; 0 with no repeats.
  Q_INVOKABLE double tail_seconds(double horizon) const;
  // {live} plus {time_left_ms, time_right_ms, duck_db, wet_rms, wet_db} while
  // Relay publishes telemetry.
  Q_INVOKABLE QVariantMap telemetry();

signals:
  void changed();

private:
  NativeSynthPanelAdapter& adapter_;
  double tempo_ = 120.0;
  double beats_per_bar_ = 4.0;
  int revision_ = 0;
};

} // namespace oscillate::quick
