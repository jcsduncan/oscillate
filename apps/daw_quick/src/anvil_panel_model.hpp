#pragma once

#include "native_synth_device_controller.hpp"

#include "oscillate/native_dsp/synth/anvil.hpp"
#include "oscillate/native_dsp/synth/runtime.hpp"
#include "oscillate/project/model.hpp"

#include <QObject>
#include <QTimer>
#include <QVariantList>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <vector>

namespace oscillate::quick {

// Peaks of `samples[first, first + count)` in `columns` columns: the largest
// magnitude in each, for drawing a waveform symmetric about its middle.
[[nodiscard]] QVariantList anvil_peaks(const std::vector<float>& samples, std::size_t first,
                                       std::size_t count, int columns);

// Anvil's panel beside the generic device controller (ADR-0060): the kick the
// current values and document make, rendered offline by the engine's own
// render_preview whenever either changes, so the editor draws the curves
// over the real waveform with nothing playing. One render per frame at most.
class AnvilPanelModel final : public QObject {
  Q_OBJECT
  // The song's tempo, for the one-beat view; root sets it.
  Q_PROPERTY(double tempo READ tempo WRITE set_tempo NOTIFY changed)
  // Increments with every new render.
  Q_PROPERTY(int revision READ revision NOTIFY changed)
  // Peaks (0..1) over the kick's length, and over one beat of the whole hit
  // and of its rumble alone.
  Q_PROPERTY(QVariantList kick READ kick NOTIFY changed)
  Q_PROPERTY(QVariantList beat READ beat NOTIFY changed)
  Q_PROPERTY(QVariantList rumble READ rumble NOTIFY changed)
  // Where the kick ends within the beat view (0..1), the kick's length, the
  // tail's pitch and the hit's peak.
  Q_PROPERTY(double kick_fraction READ kick_fraction NOTIFY changed)
  Q_PROPERTY(double kick_seconds READ kick_seconds NOTIFY changed)
  Q_PROPERTY(double tail_hz READ tail_hz NOTIFY changed)
  Q_PROPERTY(double peak_db READ peak_db NOTIFY changed)

public:
  // Decodes the document's managed media for the preview (the user's
  // transient); empty when there is none or it cannot be read.
  using MediaLoader = std::function<std::shared_ptr<const native_dsp::synth::SynthMedia>(
      const project::NativeSynthDocument&)>;
  static constexpr int kick_columns = 512;
  static constexpr int beat_columns = 512;

  explicit AnvilPanelModel(NativeSynthDeviceController& controller, MediaLoader load_media = {},
                           std::uint32_t sample_rate = 48'000, QObject* parent = nullptr);
  AnvilPanelModel(const AnvilPanelModel&) = delete;
  AnvilPanelModel& operator=(const AnvilPanelModel&) = delete;
  ~AnvilPanelModel() override;

  [[nodiscard]] double tempo() const noexcept { return tempo_; }
  void set_tempo(double bpm);
  [[nodiscard]] int revision() const noexcept { return revision_; }
  [[nodiscard]] QVariantList kick() const { return kick_; }
  [[nodiscard]] QVariantList beat() const { return beat_; }
  [[nodiscard]] QVariantList rumble() const { return rumble_; }
  [[nodiscard]] double kick_fraction() const noexcept { return kick_fraction_; }
  [[nodiscard]] double kick_seconds() const noexcept { return kick_seconds_; }
  [[nodiscard]] double tail_hz() const noexcept { return tail_hz_; }
  [[nodiscard]] double peak_db() const noexcept { return peak_db_; }

  // Renders now rather than on the next frame (tests, first show).
  // Renders now, on the calling thread. Edits render on a worker instead.
  Q_INVOKABLE void render();

signals:
  void changed();

private:
  // One render's inputs, copied on the UI thread.
  struct Job {
    std::vector<double> values;
    project::NativeSynthDocument document;
    std::shared_ptr<const native_dsp::synth::SynthMedia> media;
    std::uint32_t sample_rate = 0;
    double tempo = 0.0;
    std::uint64_t serial = 0;
  };
  void schedule();
  void start_render();
  [[nodiscard]] Job take_job();
  void apply(const native_dsp::synth::anvil::Preview& preview, std::uint64_t serial);
  void run(const std::stop_token& stop);

  NativeSynthDeviceController& controller_;
  MediaLoader load_media_;
  std::uint32_t sample_rate_;
  QTimer timer_;
  double tempo_ = 124.0;
  int revision_ = 0;
  QVariantList kick_;
  QVariantList beat_;
  QVariantList rumble_;
  double kick_fraction_ = 1.0;
  double kick_seconds_ = 0.0;
  double tail_hz_ = 0.0;
  double peak_db_ = -200.0;
  // The media last loaded, and the document media it came from.
  std::vector<project::NativeMediaSlot> media_slots_;
  std::shared_ptr<const native_dsp::synth::SynthMedia> media_;
  // A render takes milliseconds at 48 kHz, too long for a drag on the UI
  // thread: the worker renders the newest job, older ones are dropped.
  std::uint64_t serial_ = 0;
  std::mutex mutex_;
  std::condition_variable_any wake_;
  std::optional<Job> pending_;
  std::jthread worker_;
};

} // namespace oscillate::quick
