#include "audio_output_controller.hpp"

#include <QSettings>
#include <QVariantMap>

#include <algorithm>
#include <array>
#include <exception>
#include <string>
#include <utility>

namespace oscillate::quick {
namespace {
// How often the UI thread looks for finished output discovery.
constexpr int poll_interval_ms = 50;
// The buffer-size text assumes this rate while no project is open.
constexpr double fallback_sample_rate = 48'000.0;
constexpr double milliseconds_per_second = 1'000.0;
// The render-ahead limits offered: off, and windows up to the engine's most.
constexpr std::array render_ahead_choice_ms{0, 25, 50, 100,
                                            static_cast<int>(audio_io::RenderAhead::max_limit_ms)};

const QString& pipewire_backend() {
  static const QString value = QStringLiteral("pipewire");
  return value;
}
const QString& jack_backend() {
  static const QString value = QStringLiteral("jack");
  return value;
}
const QString& alsa_backend() {
  static const QString value = QStringLiteral("alsa");
  return value;
}
const QString& default_device() {
  static const QString value = QStringLiteral("default");
  return value;
}

[[nodiscard]] QVariantMap default_device_entry() {
  return QVariantMap{{"serial", default_device()}, {"name", QStringLiteral("System default")}};
}

[[nodiscard]] bool lists_serial(const QVariantList& devices, const QString& serial) {
  return std::ranges::any_of(devices, [&serial](const auto& entry) {
    return entry.toMap().value(QStringLiteral("serial")).toString() == serial;
  });
}
} // namespace

AudioOutputController::AudioOutputController(Host host,
                                             application::PlaybackOutputFactory pipewire_factory,
                                             application::PlaybackOutputFactory jack_factory,
                                             std::function<bool()> jack_server_probe,
                                             Discovery discovery, Alsa alsa, QObject* parent)
    : QObject(parent), host_(std::move(host)), pipewire_factory_(std::move(pipewire_factory)),
      jack_factory_(std::move(jack_factory)), jack_server_probe_(std::move(jack_server_probe)),
      discovery_(discovery ? std::move(discovery)
                           : Discovery{[] { return audio_io::list_pipewire_sinks(); }}),
      timer_(this), backend_(pipewire_backend()), alsa_(std::move(alsa)),
      audio_device_status_(QStringLiteral("Discovering audio outputs")) {
  timer_.setInterval(poll_interval_ms);
  connect(&timer_, &QTimer::timeout, this, &AudioOutputController::poll);
  QSettings settings;
  bool valid_buffer = false;
  const auto saved_buffer =
      settings.value(QStringLiteral("audioBufferFrames")).toString().toInt(&valid_buffer);
  requested_buffer_frames_ =
      valid_buffer && saved_buffer > 0 &&
              application::valid_playback_buffer_frames(static_cast<std::uint32_t>(saved_buffer))
          ? saved_buffer
          : static_cast<int>(application::default_playback_buffer_frames);
  bool valid_ahead = false;
  const auto saved_ahead =
      settings.value(QStringLiteral("audioRenderAheadMs")).toString().toInt(&valid_ahead);
  if (valid_ahead &&
      std::ranges::find(render_ahead_choice_ms, saved_ahead) != render_ahead_choice_ms.end())
    render_ahead_ms_ = saved_ahead;
  const auto saved_backend =
      settings.value(QStringLiteral("audioBackend"), pipewire_backend()).toString();
  if (jack_playback_available() && saved_backend == jack_backend() && jack_server_probe_())
    backend_ = jack_backend();
  if (alsa_playback_available()) {
    if (!alsa_.choice)
      alsa_.choice = std::make_shared<AlsaOutputChoice>();
    if (!alsa_.discovery)
      alsa_.discovery = [] { return audio_io::list_alsa_playback_devices(); };
    selected_alsa_device_ = settings.value(QStringLiteral("alsaDevice")).toString();
    bool valid_periods = false;
    const auto saved_periods =
        settings.value(QStringLiteral("alsaPeriods")).toString().toInt(&valid_periods);
    if (valid_periods && saved_periods >= static_cast<int>(audio_io::alsa_min_periods) &&
        saved_periods <= static_cast<int>(audio_io::alsa_max_periods))
      alsa_periods_ = saved_periods;
    if (saved_backend == alsa_backend()) {
      backend_ = alsa_backend();
      refresh_alsa_devices();
    }
    publish_alsa_choice();
  }
  selected_audio_device_ =
      settings.value(QStringLiteral("audioOutputSerial"), default_device()).toString();
  if (selected_audio_device_ != default_device()) {
    bool valid = false;
    if (selected_audio_device_.toULongLong(&valid) == 0 || !valid)
      selected_audio_device_ = default_device();
  }
  audio_devices_.append(default_device_entry());
  refresh_audio_devices();
}

AudioOutputController::~AudioOutputController() = default;

QVariantList AudioOutputController::buffer_frame_choices() {
  QVariantList choices;
  for (const auto frames : application::playback_buffer_frames)
    choices.append(static_cast<int>(frames));
  return choices;
}

QVariantList AudioOutputController::alsa_period_choices() {
  QVariantList choices;
  for (auto periods = audio_io::alsa_min_periods; periods <= audio_io::alsa_max_periods; ++periods)
    choices.append(static_cast<int>(periods));
  return choices;
}

bool AudioOutputController::jack() const { return backend_ == jack_backend(); }
bool AudioOutputController::alsa() const { return backend_ == alsa_backend(); }

const application::PlaybackOutputFactory& AudioOutputController::factory() const {
  if (alsa())
    return alsa_.factory;
  return jack() ? jack_factory_ : pipewire_factory_;
}

std::optional<std::uint64_t> AudioOutputController::target_serial() const {
  bool valid_serial = false;
  const auto parsed_serial = selected_audio_device_.toULongLong(&valid_serial);
  if (jack() || alsa() || selected_audio_device_ == default_device() || !valid_serial)
    return std::nullopt;
  return static_cast<std::uint64_t>(parsed_serial);
}

QString AudioOutputController::output_name() const {
  return jack() ? QStringLiteral("JACK server") : selected_audio_device_name();
}

QString AudioOutputController::selected_audio_device_name() const {
  if (jack())
    return QStringLiteral("JACK server");
  if (alsa()) {
    const auto found = std::ranges::find_if(alsa_devices_, [this](const auto& entry) {
      return entry.toMap().value(QStringLiteral("id")).toString() == selected_alsa_device_;
    });
    return found == alsa_devices_.end()
               ? QStringLiteral("Missing ALSA device (%1)").arg(selected_alsa_device_)
               : found->toMap().value(QStringLiteral("name")).toString();
  }
  const auto found = std::ranges::find_if(audio_devices_, [this](const auto& entry) {
    return entry.toMap().value(QStringLiteral("serial")).toString() == selected_audio_device_;
  });
  return found == audio_devices_.end()
             ? QStringLiteral("Missing output (%1)").arg(selected_audio_device_)
             : found->toMap().value(QStringLiteral("name")).toString();
}

QString AudioOutputController::buffer_size_status() const {
  const auto observed = host_.observed_block_frames();
  if (jack())
    return observed ? QStringLiteral("Prepared %1 frames · JACK server uses %2 frames.")
                          .arg(requested_buffer_frames_)
                          .arg(*observed)
                    : QStringLiteral("JACK server sets the buffer size; choose the frames to "
                                     "prepare the graph for.");
  const auto project_rate = host_.sample_rate();
  const double rate = project_rate ? static_cast<double>(*project_rate) : fallback_sample_rate;
  const QString milliseconds =
      QString::number(requested_buffer_frames_ * milliseconds_per_second / rate, 'f', 1);
  if (alsa())
    return QStringLiteral("Period %1 frames (%2 ms) × %3 periods. The device may round the period.")
        .arg(requested_buffer_frames_)
        .arg(milliseconds)
        .arg(alsa_periods_);
  return observed ? QStringLiteral("Requested %1 frames (%2 ms) · observed %3 frames. PipeWire may "
                                   "vary its quantum.")
                        .arg(requested_buffer_frames_)
                        .arg(milliseconds)
                        .arg(*observed)
                  : QStringLiteral("Request %1 frames (%2 ms). PipeWire may use a different "
                                   "quantum.")
                        .arg(requested_buffer_frames_)
                        .arg(milliseconds);
}

QString AudioOutputController::latency_status() const {
  if (!alsa())
    return {};
  const auto project_rate = host_.sample_rate();
  const double rate = project_rate ? static_cast<double>(*project_rate) : fallback_sample_rate;
  if (const auto measured = host_.output_latency_frames ? host_.output_latency_frames()
                                                        : std::optional<std::uint32_t>{})
    return QStringLiteral("Measured output latency %1 ms (%2 frames queued to the converter).")
        .arg(QString::number(*measured * milliseconds_per_second / rate, 'f', 1))
        .arg(*measured);
  const auto buffer = static_cast<double>(requested_buffer_frames_) * alsa_periods_;
  return QStringLiteral("Output latency up to %1 ms (the device buffer); measured while playing.")
      .arg(QString::number(buffer * milliseconds_per_second / rate, 'f', 1));
}

bool AudioOutputController::alsa_device_listed() const {
  return std::ranges::any_of(alsa_devices_, [this](const auto& entry) {
    return entry.toMap().value(QStringLiteral("id")).toString() == selected_alsa_device_;
  });
}

bool AudioOutputController::selected_audio_device_available() const {
  // ALSA playback always starts: on the card, or on PipeWire when it cannot.
  if (alsa())
    return true;
  if (jack())
    return jack_playback_available() && jack_server_probe_();
  return lists_serial(audio_devices_, selected_audio_device_);
}

void AudioOutputController::report(QString status) {
  audio_device_status_ = std::move(status);
  emit audioDevicesChanged();
}

void AudioOutputController::refresh_alsa_devices() {
  auto listed = alsa_.discovery();
  if (!listed) {
    alsa_devices_.clear();
    // The saved choice still plays, or falls back, on the next Play.
    publish_alsa_choice();
    if (alsa())
      audio_device_status_ = QString::fromStdString(listed.error().message);
    emit audioDevicesChanged();
    return;
  }
  QVariantList devices;
  for (const auto& device : listed.value())
    devices.append(QVariantMap{{"id", QString::fromStdString(device.id)},
                               {"name", QString::fromStdString(device.name)}});
  alsa_devices_ = std::move(devices);
  if (selected_alsa_device_.isEmpty() && !alsa_devices_.isEmpty())
    selected_alsa_device_ = alsa_devices_.front().toMap().value(QStringLiteral("id")).toString();
  publish_alsa_choice();
  if (alsa())
    audio_device_status_ =
        alsa_devices_.isEmpty() ? QStringLiteral("No ALSA playback devices found.")
        : !alsa_device_listed()
            ? QStringLiteral("The chosen ALSA device is not connected. Choose another.")
            : QStringLiteral("Play takes %1 from PipeWire for Oscillate alone; Stop gives it "
                             "back.")
                  .arg(selected_audio_device_name());
  emit audioDevicesChanged();
}

void AudioOutputController::publish_alsa_choice() {
  if (alsa_.choice)
    alsa_.choice->set(
        {selected_alsa_device_.toStdString(), static_cast<std::uint32_t>(alsa_periods_)});
}

void AudioOutputController::report_fallback() {
  if (!alsa_.fallback)
    return;
  auto reason = alsa_.fallback->reason();
  if (reason == shown_fallback_)
    return;
  shown_fallback_ = reason;
  if (!alsa())
    return;
  // The reason has its own wrapped line in the panel (alsaFallback).
  report(reason
             ? QStringLiteral("Playing through PipeWire instead.")
             : QStringLiteral("%1 plays for Oscillate alone.").arg(selected_audio_device_name()));
}

void AudioOutputController::refresh_audio_devices() {
  if (alsa()) {
    refresh_alsa_devices();
    return;
  }
  if (jack()) {
    report(jack_server_probe_()
               ? QStringLiteral("JACK server ready. Playback ports connect on Play.")
               : QStringLiteral("No JACK server is running. Use PipeWire or start "
                                "JACK, then select JACK again."));
    return;
  }
  if (completion_)
    return;
  auto completion = std::make_shared<Completion>();
  try {
    worker_ = std::jthread([completion, discover = discovery_] {
      try {
        completion->result.emplace(discover());
      } catch (const std::exception& error) {
        completion->result.emplace(foundation::Result<std::vector<audio_io::PipeWireSink>>::failure(
            {foundation::ErrorCode::unavailable,
             std::string("Audio output discovery failed: ") + error.what()}));
      } catch (...) {
        completion->result.emplace(foundation::Result<std::vector<audio_io::PipeWireSink>>::failure(
            {foundation::ErrorCode::unavailable, "Audio output discovery failed"}));
      }
      completion->done.store(true, std::memory_order_release);
    });
  } catch (const std::exception& error) {
    report(QStringLiteral("Cannot refresh audio outputs: %1").arg(error.what()));
    return;
  }
  completion_ = std::move(completion);
  report(QStringLiteral("Looking for PipeWire outputs…"));
  timer_.start();
}

void AudioOutputController::poll() {
  if (!completion_ || !completion_->done.load(std::memory_order_acquire))
    return;
  worker_.join();
  if (!completion_->result) {
    completion_.reset();
    timer_.stop();
    report(QStringLiteral("Audio output discovery did not return a result"));
    return;
  }
  auto result = std::move(*completion_->result);
  completion_.reset();
  timer_.stop();
  if (!result) {
    if (!jack() && !alsa())
      audio_device_status_ = QString::fromStdString(result.error().message);
  } else {
    QVariantList devices{default_device_entry()};
    for (const auto& sink : result.value())
      devices.append(QVariantMap{{"serial", QString::number(sink.serial)},
                                 {"name", QString::fromStdString(sink.name)}});
    audio_devices_ = std::move(devices);
    if (!jack() && !alsa())
      audio_device_status_ =
          !selected_audio_device_available()
              ? QStringLiteral("Selected output is unavailable. Choose another output.")
              : QStringLiteral("PipeWire outputs ready");
  }
  emit audioDevicesChanged();
}

void AudioOutputController::select_audio_device(const QString& serial) {
  if (!audio_device_selection_enabled()) {
    report(QStringLiteral("Stop playback before changing audio output"));
    return;
  }
  if (!lists_serial(audio_devices_, serial)) {
    report(QStringLiteral("Refresh the output list and choose an available device"));
    return;
  }
  selected_audio_device_ = serial;
  QSettings{}.setValue(QStringLiteral("audioOutputSerial"), selected_audio_device_);
  report(QStringLiteral("Selected %1. Press Play to use it.").arg(selected_audio_device_name()));
}

void AudioOutputController::select_audio_backend(const QString& backend) {
  if (!audio_device_selection_enabled()) {
    report(QStringLiteral("Stop playback before changing audio backend"));
    return;
  }
  const bool known = backend == pipewire_backend() ||
                     (backend == jack_backend() && jack_playback_available()) ||
                     (backend == alsa_backend() && alsa_playback_available());
  if (!known) {
    report(QStringLiteral("Audio backend is unavailable in this build"));
    return;
  }
  if (backend == jack_backend() && !jack_server_probe_()) {
    report(QStringLiteral("No JACK server is running. Use PipeWire, or start "
                          "JACK before selecting it."));
    return;
  }
  if (backend_ == backend)
    return;
  backend_ = backend;
  QSettings{}.setValue(QStringLiteral("audioBackend"), backend);
  host_.backend_changed();
  if (backend == alsa_backend()) {
    refresh_alsa_devices();
  } else {
    report(backend == jack_backend()
               ? QStringLiteral("JACK selected. Press Play; physical playback ports "
                                "connect automatically when available.")
               : QStringLiteral("PipeWire selected. Press Play to use %1.")
                     .arg(selected_audio_device_name()));
  }
  host_.settings_changed();
  if (backend == pipewire_backend())
    refresh_audio_devices();
}

void AudioOutputController::select_buffer_frames(int frames) {
  if (frames <= 0 ||
      !application::valid_playback_buffer_frames(static_cast<std::uint32_t>(frames))) {
    report(QStringLiteral("Choose a %1 frame buffer")
               .arg(QString::fromStdString(application::playback_buffer_frames_text())));
    return;
  }
  if (!audio_device_selection_enabled()) {
    report(QStringLiteral("Stop playback before changing the buffer request"));
    return;
  }
  if (auto updated = host_.apply_buffer_frames(static_cast<std::uint32_t>(frames)); !updated) {
    report(QString::fromStdString(updated.error().message));
    return;
  }
  requested_buffer_frames_ = frames;
  QSettings{}.setValue(QStringLiteral("audioBufferFrames"), frames);
  report(QStringLiteral("%1-frame request saved for next playback").arg(frames));
  host_.settings_changed();
  emit renderAheadChanged();
}

void AudioOutputController::select_alsa_device(const QString& id) {
  if (!audio_device_selection_enabled()) {
    report(QStringLiteral("Stop playback before changing the ALSA device"));
    return;
  }
  const bool listed = std::ranges::any_of(alsa_devices_, [&id](const auto& entry) {
    return entry.toMap().value(QStringLiteral("id")).toString() == id;
  });
  if (!listed) {
    report(QStringLiteral("Refresh the device list and choose a connected ALSA device"));
    return;
  }
  selected_alsa_device_ = id;
  QSettings{}.setValue(QStringLiteral("alsaDevice"), id);
  publish_alsa_choice();
  refresh_alsa_devices();
  host_.settings_changed();
}

void AudioOutputController::select_alsa_periods(int periods) {
  if (periods < static_cast<int>(audio_io::alsa_min_periods) ||
      periods > static_cast<int>(audio_io::alsa_max_periods))
    return;
  if (!audio_device_selection_enabled()) {
    report(QStringLiteral("Stop playback before changing the period count"));
    return;
  }
  alsa_periods_ = periods;
  QSettings{}.setValue(QStringLiteral("alsaPeriods"), periods);
  publish_alsa_choice();
  report(QStringLiteral("%1 periods saved for next playback").arg(periods));
  host_.settings_changed();
}

QVariantList AudioOutputController::render_ahead_choices() {
  QVariantList choices;
  for (const auto milliseconds : render_ahead_choice_ms)
    choices.append(milliseconds);
  return choices;
}

QString AudioOutputController::render_ahead_status() const {
  if (render_ahead_ms_ == 0)
    return QStringLiteral("Off: every block renders just in time, so edits made while playing "
                          "are heard at once, and a slow moment can click.");
  const auto project_rate = host_.sample_rate();
  const double rate = project_rate ? static_cast<double>(*project_rate) : fallback_sample_rate;
  const auto block = static_cast<double>(
      host_.observed_block_frames().value_or(static_cast<std::uint32_t>(requested_buffer_frames_)));
  const QString worst =
      QString::number(render_ahead_ms_ + block * milliseconds_per_second / rate, 'f', 0);
  return QStringLiteral("Renders up to %1 ms ahead on spare cores, so a slow moment does not "
                        "click. An edit made while playing is heard within %2 ms, without "
                        "stopping. The computer keyboard and recording always play at once.")
      .arg(render_ahead_ms_)
      .arg(worst);
}

void AudioOutputController::select_render_ahead(int milliseconds) {
  if (std::ranges::find(render_ahead_choice_ms, milliseconds) == render_ahead_choice_ms.end())
    return;
  render_ahead_ms_ = milliseconds;
  QSettings{}.setValue(QStringLiteral("audioRenderAheadMs"), milliseconds);
  if (host_.apply_render_ahead)
    host_.apply_render_ahead();
  emit renderAheadChanged();
}

} // namespace oscillate::quick
