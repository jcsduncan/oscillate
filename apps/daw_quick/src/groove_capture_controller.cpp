#include "groove_capture_controller.hpp"

#include "oscillate/application/browser_preview.hpp"
#include "oscillate/export/wav_writer.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileDialog>
#include <QFileInfo>
#include <QStandardPaths>
#include <QUrl>

#include <algorithm>
#include <chrono>
#include <exception>
#include <utility>

namespace oscillate::quick {
namespace {

namespace groove = application::groove_capture;

constexpr int poll_interval_ms = 50;
// The A/B loop: four bars, so a two-bar groove plays twice.
constexpr std::size_t audition_bars = 4;
constexpr double fallback_bpm = 120.0;
constexpr std::uint32_t fallback_rate = 48'000;
constexpr int max_name_characters = 64;

QString text(const std::string& value) { return QString::fromStdString(value); }

QString local_path(const QString& path) {
  const QUrl url(path);
  return url.isLocalFile() ? url.toLocalFile() : path;
}

QString clip_name(const project::Clip& clip, const project::Track& track) {
  if (!clip.name.empty())
    return text(clip.name);
  if (!track.name.empty())
    return text(track.name);
  return QStringLiteral("Audio clip");
}

} // namespace

GrooveCaptureController::GrooveCaptureController(Session session, Keep keep, Report report,
                                                 QObject* parent)
    : QObject(parent), session_(std::move(session)), keep_(std::move(keep)),
      report_(std::move(report)) {
  timer_.setInterval(poll_interval_ms);
  connect(&timer_, &QTimer::timeout, this, &GrooveCaptureController::poll);
  project_changed();
}

GrooveCaptureController::~GrooveCaptureController() {
  if (job_)
    job_->cancel.store(true, std::memory_order_release);
}

void GrooveCaptureController::set_open(bool open) {
  if (open_ == open)
    return;
  open_ = open;
  if (!open_)
    cancel();
  emit openChanged();
}

QStringList GrooveCaptureController::band_names() {
  QStringList names;
  for (const auto name : groove::band_names)
    names.append(QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size())));
  return names;
}

int GrooveCaptureController::min_bars() noexcept { return static_cast<int>(groove::min_bars); }
int GrooveCaptureController::max_bars() noexcept { return static_cast<int>(groove::max_bars); }

QString GrooveCaptureController::stage() const {
  if (!job_)
    return {};
  const std::lock_guard lock(job_->mutex);
  return text(job_->stage);
}

void GrooveCaptureController::project_changed() {
  QVariantList clips;
  auto* session = session_ ? session_() : nullptr;
  bool still_there = !source_.startsWith(QStringLiteral("clip:"));
  if (session != nullptr) {
    for (const auto& track : session->snapshot().tracks) {
      for (const auto& clip : track.clips) {
        const auto id = text(clip.id.to_string());
        clips.append(QVariantMap{{QStringLiteral("id"), id},
                                 {QStringLiteral("name"), clip_name(clip, track)},
                                 {QStringLiteral("track"), text(track.name)}});
        if (source_ == QStringLiteral("clip:") + id)
          still_there = true;
      }
    }
  }
  if (clips != clips_) {
    clips_ = std::move(clips);
    emit clipsChanged();
  }
  if (!still_there)
    set_source({}, {});
}

void GrooveCaptureController::set_source(QString source, QString name) {
  if (source == source_ && name == source_name_)
    return;
  source_ = std::move(source);
  source_name_ = std::move(name);
  if (!source_name_.isEmpty()) {
    name_ = (source_name_ + QStringLiteral(" groove")).left(max_name_characters);
    emit settingsChanged();
  }
  emit sourceChanged();
}

void GrooveCaptureController::open_for_clip(const QString& clip_id) {
  use_clip(clip_id);
  set_open(true);
}

void GrooveCaptureController::use_clip(const QString& clip_id) {
  for (const auto& entry : clips_) {
    const auto map = entry.toMap();
    if (map.value(QStringLiteral("id")).toString() == clip_id) {
      set_source(QStringLiteral("clip:") + clip_id, map.value(QStringLiteral("name")).toString());
      return;
    }
  }
  set_status(QStringLiteral("That audio clip is not in the project"));
}

void GrooveCaptureController::use_file(const QString& path) {
  const auto file = local_path(path);
  const QFileInfo info(file);
  if (!info.isFile()) {
    set_status(QStringLiteral("Not a file: %1").arg(file));
    return;
  }
  set_source(QStringLiteral("file:") + info.absoluteFilePath(), info.completeBaseName());
}

void GrooveCaptureController::choose_file() {
  const auto chosen = QFileDialog::getOpenFileName(
      nullptr, QStringLiteral("Take a groove from a record"), QDir::homePath(),
      QStringLiteral("Audio (*.wav *.aif *.aiff *.flac *.mp3)"));
  if (!chosen.isEmpty())
    use_file(chosen);
}

std::optional<groove::Request> GrooveCaptureController::request() const {
  if (source_.isEmpty() || band_ < 0 || band_ >= static_cast<int>(groove::band_count))
    return std::nullopt;
  groove::Request request;
  request.name = name_.trimmed().toStdString();
  request.band = static_cast<groove::Band>(band_);
  request.bars = static_cast<std::size_t>(std::clamp(bars_, min_bars(), max_bars()));
  request.cycle_steps = steps_ == 32 ? 32 : 16;
  request.steps_per_quarter = static_cast<std::uint32_t>(steps_per_quarter_);
  if (bpm_ > 0.0)
    request.bpm = bpm_;
  else if (source_is_file())
    request.bpm = application::tempo_from_file_name(
        QFileInfo(source_.mid(5)).completeBaseName().toStdString());
  return request;
}

void GrooveCaptureController::take() {
  if (job_) {
    set_status(QStringLiteral("Already listening"));
    return;
  }
  auto wanted = request();
  if (!wanted) {
    set_status(QStringLiteral("Choose a clip or a record to take the groove from"));
    return;
  }
  auto job = std::make_shared<Job>();
  const auto band = text(std::string(groove::band_names[static_cast<std::size_t>(band_)]));
  job->stage = QStringLiteral("Listening to the %1").arg(band.toLower()).toStdString();
  try {
    if (source_is_file()) {
      worker_ =
          std::jthread([job, request = std::move(*wanted), path = source_.mid(5).toStdString()] {
            try {
              job->result = groove::capture_file(
                  path, request, [&] { return job->cancel.load(std::memory_order_acquire); });
            } catch (const std::exception& error) {
              job->result = foundation::Result<groove::Capture>::failure(
                  {foundation::ErrorCode::internal, error.what()});
            }
            job->done.store(true, std::memory_order_release);
          });
    } else {
      auto* session = session_ ? session_() : nullptr;
      if (session == nullptr) {
        set_status(QStringLiteral("Open a project first"));
        return;
      }
      auto pinned = session->exports().pin_snapshot();
      if (!pinned) {
        set_status(QStringLiteral("Cannot read the clip: %1").arg(text(pinned.error().message)));
        return;
      }
      const auto clip = project::ClipId::parse(source_.mid(5).toStdString());
      if (!clip) {
        set_status(QStringLiteral("That audio clip is not in the project"));
        return;
      }
      worker_ = std::jthread([job, request = std::move(*wanted), clip = clip.value(),
                              pinned = std::move(pinned).value()] {
        try {
          job->result = groove::capture_clip(pinned.reader, *pinned.snapshot, clip, request, [&] {
            return job->cancel.load(std::memory_order_acquire);
          });
        } catch (const std::exception& error) {
          job->result = foundation::Result<groove::Capture>::failure(
              {foundation::ErrorCode::internal, error.what()});
        }
        job->done.store(true, std::memory_order_release);
      });
    }
  } catch (const std::exception& error) {
    set_status(QStringLiteral("Cannot start listening: %1").arg(QString::fromUtf8(error.what())));
    return;
  }
  job_ = std::move(job);
  timer_.start();
  emit runningChanged();
}

void GrooveCaptureController::cancel() {
  if (!job_)
    return;
  job_->cancel.store(true, std::memory_order_release);
}

void GrooveCaptureController::poll() {
  if (job_ && job_->done.load(std::memory_order_acquire)) {
    if (worker_.joinable())
      worker_.join();
    finish();
  } else if (job_) {
    emit runningChanged();
  }
  if (!job_)
    timer_.stop();
}

void GrooveCaptureController::finish() {
  auto job = std::move(job_);
  job_.reset();
  emit runningChanged();
  if (job->cancel.load(std::memory_order_acquire)) {
    set_status(QStringLiteral("Stopped listening"));
    return;
  }
  auto& result = *job->result;
  if (!result) {
    set_status(QStringLiteral("No groove: %1").arg(text(result.error().message)));
    return;
  }
  publish(result.value());
  capture_ = std::move(result).value();
  write_audition();
  emit resultChanged();
  set_status(text(groove::describe(capture_->measurement)));
}

void GrooveCaptureController::publish(const groove::Capture& capture) {
  const auto& m = capture.measurement;
  QVariantList steps;
  QVariantList timings;
  QVariantList velocities;
  for (std::size_t i = 0; i < m.steps.size(); ++i) {
    const auto& feel = m.steps[i];
    steps.append(QVariantMap{{QStringLiteral("timing"), feel.timing},
                             {QStringLiteral("timingMs"), feel.timing_ms},
                             {QStringLiteral("spread"), feel.spread},
                             {QStringLiteral("spreadMs"), feel.spread_ms},
                             {QStringLiteral("velocity"), feel.velocity},
                             {QStringLiteral("hits"), static_cast<int>(feel.hits)}});
    timings.append(m.groove.steps[i].timing);
    velocities.append(m.groove.steps[i].velocity);
  }
  QVariantList folded;
  for (const auto& hit : m.hits)
    folded.append(QVariantMap{{QStringLiteral("position"), hit.position},
                              {QStringLiteral("strength"), hit.strength}});
  QVariantList peaks;
  for (const auto peak : capture.bar_peaks)
    peaks.append(static_cast<double>(peak));
  QVariantList onsets;
  for (const auto onset : capture.bar_onsets)
    onsets.append(onset);
  result_ =
      QVariantMap{{QStringLiteral("bpm"), capture.bpm},
                  {QStringLiteral("gridFit"), capture.grid_fit},
                  {QStringLiteral("confidence"), m.confidence},
                  {QStringLiteral("swing"), m.swing_percent},
                  {QStringLiteral("offbeatMs"), m.offbeat_ms},
                  {QStringLiteral("accents"), static_cast<int>(m.accents)},
                  {QStringLiteral("hits"), static_cast<int>(m.hit_count)},
                  {QStringLiteral("stepMs"), m.step_ms},
                  {QStringLiteral("stepsPerQuarter"), static_cast<int>(m.groove.steps_per_quarter)},
                  {QStringLiteral("summary"), text(groove::describe(m))},
                  {QStringLiteral("steps"), steps},
                  {QStringLiteral("folded"), folded},
                  {QStringLiteral("barPeaks"), peaks},
                  {QStringLiteral("barOnsets"), onsets},
                  {QStringLiteral("timings"), timings},
                  {QStringLiteral("velocities"), velocities}};
}

void GrooveCaptureController::write_audition() {
  audition_straight_.clear();
  audition_grooved_.clear();
  if (!capture_)
    return;
  double bpm = fallback_bpm;
  std::uint32_t rate = fallback_rate;
  if (auto* session = session_ ? session_() : nullptr) {
    const auto& snapshot = session->snapshot();
    rate = snapshot.sample_rate;
    if (!snapshot.tempo.events().empty())
      bpm = snapshot.tempo.events().front().bpm;
  } else if (capture_->bpm > 0.0) {
    bpm = capture_->bpm;
  }
  const auto folder =
      QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/grooves");
  if (!QDir().mkpath(folder))
    return;
  // A new name each take, so a preview never plays the last one's file.
  static std::uint64_t serial = 0;
  ++serial;
  const auto write = [&](bool grooved) -> QString {
    const auto samples =
        groove::audition(capture_->measurement.groove, bpm, rate, audition_bars, grooved);
    const auto path =
        QStringLiteral("%1/%2-%3.wav")
            .arg(folder, grooved ? QStringLiteral("grooved") : QStringLiteral("straight"))
            .arg(serial);
    auto writer = exporting::WavWriter::begin(
        path.toStdString(), {rate, exporting::WavFormat::float32, samples.size() / 2, 0});
    if (!writer || !writer.value().write_interleaved_stereo(samples) || !writer.value().finish())
      return {};
    return path;
  };
  audition_straight_ = write(false);
  audition_grooved_ = write(true);
}

bool GrooveCaptureController::save() {
  if (!capture_) {
    set_status(QStringLiteral("Take a groove first"));
    return false;
  }
  auto kept = capture_->measurement.groove;
  kept.name = name_.trimmed().toStdString();
  if (kept.name.empty()) {
    set_status(QStringLiteral("Give the groove a name"));
    return false;
  }
  if (!keep_ || !keep_(std::move(kept)))
    return false;
  set_status(QStringLiteral("%1 is in the groove pool").arg(name_.trimmed()));
  return true;
}

bool GrooveCaptureController::wait_for_take(int timeout_ms) {
  QElapsedTimer clock;
  clock.start();
  while (job_ && clock.elapsed() < timeout_ms) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, poll_interval_ms);
    if (job_ && job_->done.load(std::memory_order_acquire))
      poll();
    else
      std::this_thread::sleep_for(std::chrono::milliseconds(poll_interval_ms / 5));
  }
  return !job_;
}

void GrooveCaptureController::set_status(QString status) {
  if (report_)
    report_(status);
  if (status_.set(std::move(status)))
    emit statusChanged();
}

} // namespace oscillate::quick
