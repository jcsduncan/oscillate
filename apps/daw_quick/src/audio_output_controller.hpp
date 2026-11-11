#pragma once

#include "alsa_playback_output.hpp"

#include "oscillate/application/fallback_output.hpp"
#include "oscillate/application/instrument_playback.hpp"
#include "oscillate/audio_io/alsa_output.hpp"
#include "oscillate/audio_io/pipewire_sinks.hpp"
#include "oscillate/audio_io/render_ahead.hpp"
#include "oscillate/foundation/error.hpp"

#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

namespace oscillate::quick {

// The audio output settings (QML: projectBridge.audioOutput): the backend
// (PipeWire, JACK or ALSA direct), the PipeWire sink, discovered on a
// worker, the ALSA device and period count, and the buffer request, each
// remembered in QSettings. Playback reads the chosen
// factory, target and buffer from here; the bridge says when a change is
// allowed and applies a new backend or buffer to its playback service.
class AudioOutputController final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QVariantList audioDevices READ audio_devices NOTIFY audioDevicesChanged)
  Q_PROPERTY(QString selectedAudioDevice READ selected_audio_device NOTIFY audioDevicesChanged)
  Q_PROPERTY(
      QString selectedAudioDeviceName READ selected_audio_device_name NOTIFY audioDevicesChanged)
  Q_PROPERTY(QString audioDeviceStatus READ audio_device_status NOTIFY audioDevicesChanged)
  Q_PROPERTY(int requestedBufferFrames READ requested_buffer_frames NOTIFY audioDevicesChanged)
  Q_PROPERTY(QVariantList bufferFrameChoices READ buffer_frame_choices CONSTANT)
  Q_PROPERTY(QString bufferSizeStatus READ buffer_size_status NOTIFY playbackChanged)
  Q_PROPERTY(bool audioDeviceBusy READ audio_device_busy NOTIFY audioDevicesChanged)
  Q_PROPERTY(QString audioBackend READ audio_backend NOTIFY audioDevicesChanged)
  Q_PROPERTY(bool jackPlaybackAvailable READ jack_playback_available CONSTANT)
  // Direct ALSA (ADR-0130): the card's playback devices, the chosen one,
  // its period count and the latency it measures while playing.
  Q_PROPERTY(bool alsaPlaybackAvailable READ alsa_playback_available CONSTANT)
  Q_PROPERTY(QVariantList alsaDevices READ alsa_devices NOTIFY audioDevicesChanged)
  Q_PROPERTY(QString selectedAlsaDevice READ selected_alsa_device NOTIFY audioDevicesChanged)
  Q_PROPERTY(int alsaPeriods READ alsa_periods NOTIFY audioDevicesChanged)
  Q_PROPERTY(QVariantList alsaPeriodChoices READ alsa_period_choices CONSTANT)
  Q_PROPERTY(QString latencyStatus READ latency_status NOTIFY playbackChanged)
  // Why the last Play fell back to PipeWire; empty when it did not.
  Q_PROPERTY(QString alsaFallback READ alsa_fallback NOTIFY audioDevicesChanged)
  Q_PROPERTY(
      bool audioDeviceSelectionEnabled READ audio_device_selection_enabled NOTIFY playbackChanged)
  // Render-ahead (ADR-0128): how far ahead playback renders, 0 for off.
  Q_PROPERTY(int renderAheadMs READ render_ahead_ms NOTIFY renderAheadChanged)
  Q_PROPERTY(QVariantList renderAheadChoices READ render_ahead_choices CONSTANT)
  Q_PROPERTY(QString renderAheadStatus READ render_ahead_status NOTIFY renderAheadChanged)

public:
  using Discovery = std::function<foundation::Result<std::vector<audio_io::PipeWireSink>>()>;
  struct Host {
    // No playback, export or import is running or closing.
    std::function<bool()> selection_allowed;
    // The block size the running output reports, if one runs.
    std::function<std::optional<std::uint32_t>()> observed_block_frames;
    // The open project's rate, if one is open.
    std::function<std::optional<std::uint32_t>()> sample_rate;
    // The backend changed: playback is rebuilt on the new factory.
    std::function<void()> backend_changed;
    // A new buffer request for the playback service; a failure keeps the old one.
    std::function<foundation::Result<void>(std::uint32_t)> apply_buffer_frames;
    // The backend or buffer request changed: playback gates and texts re-read.
    std::function<void()> settings_changed;
    // The render-ahead setting changed: the running playback takes it.
    std::function<void()> apply_render_ahead;
    // The running output's measured latency in frames, if it measures one.
    std::function<std::optional<std::uint32_t>()> output_latency_frames;
  };
  using AlsaDiscovery = std::function<foundation::Result<std::vector<audio_io::AlsaDevice>>()>;
  // Direct ALSA playback: its factory (falling back to PipeWire), the
  // device choice the factory reads, the fallback's report and the device
  // listing. An empty factory means no ALSA backend.
  struct Alsa {
    application::PlaybackOutputFactory factory;
    std::shared_ptr<AlsaOutputChoice> choice;
    std::shared_ptr<application::OutputFallbackReport> fallback;
    AlsaDiscovery discovery;
  };

  // The factories arrive wrapped by the metering output. An empty JACK
  // factory means the build has no JACK playback.
  AudioOutputController(Host host, application::PlaybackOutputFactory pipewire_factory,
                        application::PlaybackOutputFactory jack_factory,
                        std::function<bool()> jack_server_probe, Discovery discovery,
                        Alsa alsa = {}, QObject* parent = nullptr);
  ~AudioOutputController() override;
  AudioOutputController(const AudioOutputController&) = delete;
  AudioOutputController& operator=(const AudioOutputController&) = delete;

  [[nodiscard]] QVariantList audio_devices() const { return audio_devices_; }
  [[nodiscard]] QString selected_audio_device() const { return selected_audio_device_; }
  [[nodiscard]] QString selected_audio_device_name() const;
  [[nodiscard]] QString audio_device_status() const { return audio_device_status_; }
  [[nodiscard]] int requested_buffer_frames() const { return requested_buffer_frames_; }
  [[nodiscard]] static QVariantList buffer_frame_choices();
  [[nodiscard]] QString buffer_size_status() const;
  [[nodiscard]] bool audio_device_busy() const { return completion_ != nullptr; }
  [[nodiscard]] QString audio_backend() const { return backend_; }
  [[nodiscard]] bool jack_playback_available() const { return static_cast<bool>(jack_factory_); }
  [[nodiscard]] bool audio_device_selection_enabled() const { return host_.selection_allowed(); }
  [[nodiscard]] bool alsa_playback_available() const { return static_cast<bool>(alsa_.factory); }
  [[nodiscard]] QVariantList alsa_devices() const { return alsa_devices_; }
  [[nodiscard]] QString selected_alsa_device() const { return selected_alsa_device_; }
  [[nodiscard]] int alsa_periods() const { return alsa_periods_; }
  [[nodiscard]] static QVariantList alsa_period_choices();
  [[nodiscard]] QString latency_status() const;
  [[nodiscard]] QString alsa_fallback() const {
    return alsa() && shown_fallback_ ? QString::fromStdString(*shown_fallback_) : QString{};
  }

  // What playback opens: the backend's factory, the PipeWire sink to target
  // (none for JACK or the system default) and the buffer request.
  [[nodiscard]] bool jack() const;
  [[nodiscard]] bool alsa() const;
  [[nodiscard]] const application::PlaybackOutputFactory& factory() const;
  [[nodiscard]] std::optional<std::uint64_t> target_serial() const;
  [[nodiscard]] std::uint32_t buffer_frames() const {
    return static_cast<std::uint32_t>(requested_buffer_frames_);
  }
  [[nodiscard]] int render_ahead_ms() const { return render_ahead_ms_; }
  [[nodiscard]] static QVariantList render_ahead_choices();
  // What the setting does, with the longest an edit made while playing can
  // take to be heard at the current buffer.
  [[nodiscard]] QString render_ahead_status() const;
  // "JACK server", the ALSA device's name, or the PipeWire sink's name.
  [[nodiscard]] QString output_name() const;
  [[nodiscard]] bool selected_audio_device_available() const;

  Q_INVOKABLE void refresh_audio_devices();
  Q_INVOKABLE void select_audio_device(const QString& serial);
  Q_INVOKABLE void select_audio_backend(const QString& backend);
  Q_INVOKABLE void select_buffer_frames(int frames);
  Q_INVOKABLE void select_alsa_device(const QString& id);
  Q_INVOKABLE void select_alsa_periods(int periods);
  // Takes effect at once, also while playing.
  Q_INVOKABLE void select_render_ahead(int milliseconds);
  // Playback started, stopped or changed: the status and gates may differ.
  void notify_playback() {
    report_fallback();
    emit playbackChanged();
    // The edit-delay bound follows the buffer the device plays.
    emit renderAheadChanged();
  }

signals:
  void audioDevicesChanged();
  void playbackChanged();
  void renderAheadChanged();

private:
  struct Completion {
    std::atomic<bool> done{false};
    std::optional<foundation::Result<std::vector<audio_io::PipeWireSink>>> result;
  };
  void poll();
  void report(QString status);
  void refresh_alsa_devices();
  [[nodiscard]] bool alsa_device_listed() const;
  void publish_alsa_choice();
  // Says so when the last start fell back from ALSA to PipeWire.
  void report_fallback();

  Host host_;
  application::PlaybackOutputFactory pipewire_factory_;
  application::PlaybackOutputFactory jack_factory_;
  std::function<bool()> jack_server_probe_;
  Discovery discovery_;
  QTimer timer_;
  std::shared_ptr<Completion> completion_;
  std::jthread worker_;
  QVariantList audio_devices_;
  QString selected_audio_device_;
  QString backend_;
  Alsa alsa_;
  QVariantList alsa_devices_;
  QString selected_alsa_device_;
  int alsa_periods_ = static_cast<int>(audio_io::alsa_default_periods);
  std::optional<std::string> shown_fallback_;
  int requested_buffer_frames_ = static_cast<int>(application::default_playback_buffer_frames);
  int render_ahead_ms_ = static_cast<int>(audio_io::RenderAhead::default_limit_ms);
  QString audio_device_status_;
};

} // namespace oscillate::quick
