#include "warp_lab_controller.hpp"

#include "oscillate/media/prefetch.hpp"
#include "oscillate/warp_dsp/stream.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QVariantMap>

#include <time.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <fstream>
#include <ranges>
#include <span>

namespace oscillate::warp_lab {
namespace {

namespace warp = warp_dsp;
constexpr std::uint32_t lab_rate = 48'000;
constexpr std::uint64_t max_frames = std::uint64_t{lab_rate} * 600;
constexpr std::size_t waveform_columns = 900;
constexpr std::size_t pull_frames = 1'024;
// Order matches Ableton's warp-mode menu.
constexpr std::array<const char*, 6> mode_names{"Beats",    "Tones",   "Texture",
                                                "Re-Pitch", "Complex", "Complex Pro"};

double thread_seconds() {
  timespec now{};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
  return static_cast<double>(now.tv_sec) + static_cast<double>(now.tv_nsec) * 1e-9;
}

// Min/max envelope of the mid signal in `columns` buckets, as [min, max, ...].
QVariantList waveform(const std::vector<float>& left, const std::vector<float>& right) {
  QVariantList result;
  if (left.empty())
    return result;
  const auto frames = left.size();
  for (std::size_t column = 0; column < waveform_columns; ++column) {
    const auto begin = column * frames / waveform_columns;
    const auto end = std::max(begin + 1, (column + 1) * frames / waveform_columns);
    float low = 0.0F;
    float high = 0.0F;
    for (auto index = begin; index < std::min(end, frames); ++index) {
      const float mid = 0.5F * (left[index] + right[index]);
      low = std::min(low, mid);
      high = std::max(high, mid);
    }
    result.push_back(static_cast<double>(low));
    result.push_back(static_cast<double>(high));
  }
  return result;
}

bool write_float_wav(const QString& path, const std::vector<float>& left,
                     const std::vector<float>& right) {
  if (left.size() != right.size() || left.size() > max_frames)
    return false;
  std::ofstream output(path.toStdString(), std::ios::binary | std::ios::trunc);
  const auto data_bytes = static_cast<std::uint32_t>(left.size() * 2 * sizeof(float));
  const auto put16 = [&output](std::uint16_t value) {
    const std::array<char, 2> bytes{static_cast<char>(value & 0xFFU),
                                    static_cast<char>(value >> 8U)};
    output.write(bytes.data(), 2);
  };
  const auto put32 = [&output](std::uint32_t value) {
    const std::array<char, 4> bytes{
        static_cast<char>(value & 0xFFU), static_cast<char>((value >> 8U) & 0xFFU),
        static_cast<char>((value >> 16U) & 0xFFU), static_cast<char>(value >> 24U)};
    output.write(bytes.data(), 4);
  };
  output.write("RIFF", 4);
  put32(36 + data_bytes);
  output.write("WAVEfmt ", 8);
  put32(16);
  put16(3);
  put16(2);
  put32(lab_rate);
  put32(lab_rate * 2 * sizeof(float));
  put16(2 * sizeof(float));
  put16(32);
  output.write("data", 4);
  put32(data_bytes);
  std::array<float, 8192 * 2> interleaved{};
  for (std::size_t first = 0; first < left.size();) {
    const auto count = std::min<std::size_t>(8192, left.size() - first);
    for (std::size_t index = 0; index < count; ++index) {
      interleaved[2 * index] = left[first + index];
      interleaved[2 * index + 1] = right[first + index];
    }
    output.write(reinterpret_cast<const char*>(interleaved.data()),
                 static_cast<std::streamsize>(count * 2 * sizeof(float)));
    first += count;
  }
  return static_cast<bool>(output);
}

std::uint64_t q16(std::uint64_t frame) { return frame << warp::source_fraction_bits; }

} // namespace

struct WarpLabController::Loaded {
  QString path;
  QString note;
  QString error;
  bool convertible = false;
  std::vector<float> left;
  std::vector<float> right;
  std::vector<std::uint64_t> onsets;
  std::vector<warp::analysis::PitchMark> marks;
  double voiced = 0.0;
};

struct WarpLabController::Rendered {
  QString error;
  std::vector<float> left;
  std::vector<float> right;
  QString status = QStringLiteral("ok");
  double cpu = 0.0;
  double peak = 0.0;
  std::uint64_t clipped = 0;
  QString label;
};

WarpLabController::WarpLabController(QObject* parent) : QObject(parent) {
  cache_directory_ = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
  QDir().mkpath(cache_directory_);
  debounce_.setSingleShot(true);
  debounce_.setInterval(250);
  connect(&debounce_, &QTimer::timeout, this, &WarpLabController::render);
  connect(this, &WarpLabController::settingsChanged, this, [this] {
    if (auto_render_ && has_source())
      debounce_.start();
  });
}

WarpLabController::~WarpLabController() {
  if (player_ != nullptr) {
    player_->kill();
    player_->waitForFinished(1'000);
  }
  if (converter_ != nullptr) {
    converter_->kill();
    converter_->waitForFinished(1'000);
  }
  join_worker();
}

void WarpLabController::join_worker() {
  if (worker_.joinable())
    worker_.join();
}

void WarpLabController::set_status(QString text) {
  status_ = std::move(text);
  emit statusChanged();
}

void WarpLabController::set_busy(bool value) {
  busy_ = value;
  emit statusChanged();
}

void WarpLabController::load(const QUrl& file) {
  const auto path = file.isLocalFile() ? file.toLocalFile() : file.toString();
  load_path(path, QString());
}

void WarpLabController::load_path(const QString& path, const QString& note) {
  if (busy_) {
    set_status(QStringLiteral("Busy; try again in a moment"));
    return;
  }
  stop();
  join_worker();
  set_busy(true);
  set_status(QStringLiteral("Analysing %1…").arg(QFileInfo(path).fileName()));
  worker_ = std::jthread([this, path, note] {
    auto loaded = std::make_shared<Loaded>();
    try {
      loaded->path = path;
      loaded->note = note;
      auto opened = media::WavReader::open(path.toStdString());
      if (!opened) {
        loaded->error = QStringLiteral("Not a readable WAV (%1).")
                            .arg(QString::fromStdString(opened.error().message));
        loaded->convertible = true;
      } else if (opened.value().sample_rate() != lab_rate || opened.value().channels() != 2) {
        loaded->error = QStringLiteral("%1 Hz, %2 channel(s): the lab plays 48 kHz stereo only.")
                            .arg(opened.value().sample_rate())
                            .arg(opened.value().channels());
        loaded->convertible = true;
      } else if (opened.value().frame_count() == 0 || opened.value().frame_count() > max_frames) {
        loaded->error = QStringLiteral("The lab takes 1 frame to 10 minutes of audio.");
      } else {
        auto& reader = opened.value();
        const auto frames = static_cast<std::size_t>(reader.frame_count());
        loaded->left.resize(frames);
        loaded->right.resize(frames);
        std::vector<media::StereoFrame> chunk(65'536);
        std::size_t done = 0;
        while (done < frames && loaded->error.isEmpty()) {
          const auto want = std::min(chunk.size(), frames - done);
          auto read = reader.read_at(done, std::span(chunk).first(want));
          if (!read || read.value() == 0) {
            loaded->error = QStringLiteral("Read failed at frame %1.").arg(done);
            break;
          }
          for (std::size_t index = 0; index < read.value(); ++index) {
            loaded->left[done + index] = chunk[index].left;
            loaded->right[done + index] = chunk[index].right;
          }
          done += read.value();
        }
        if (loaded->error.isEmpty()) {
          warp::MemorySourceView view(loaded->left, loaded->right);
          if (auto found = warp::analysis::detect_transients(view, {lab_rate, 1.0F, 0.030})) {
            for (const auto& transient : found.value())
              loaded->onsets.push_back(transient.frame);
          }
          if (auto track = warp::analysis::track_pitch(view, {lab_rate, 60.0F, 1'000.0F, 0.15F})) {
            const auto voiced = std::ranges::count_if(
                track.value(), [](const auto& frame) { return frame.frequency > 0.0F; });
            loaded->voiced = track.value().empty() ? 0.0
                                                   : static_cast<double>(voiced) /
                                                         static_cast<double>(track.value().size());
            if (auto marks = warp::analysis::place_pitch_marks(view, track.value(), lab_rate))
              loaded->marks = std::move(marks).value();
          }
        }
      }
    } catch (const std::exception& error) {
      loaded->error = QStringLiteral("Analysis failed: %1").arg(error.what());
    } catch (...) {
      loaded->error = QStringLiteral("Analysis failed unexpectedly");
    }
    QMetaObject::invokeMethod(this, [this, loaded] { finish_load(loaded); }, Qt::QueuedConnection);
  });
}

void WarpLabController::finish_load(std::shared_ptr<Loaded> loaded) {
  join_worker();
  set_busy(false);
  if (!loaded->error.isEmpty()) {
    // Anything that is not already a 48 kHz stereo WAV (MP3, FLAC, 44.1 kHz,
    // mono...) is converted with ffmpeg straight away; the source line says so.
    if (loaded->convertible && loaded->note.isEmpty()) {
      conversion_input_ = loaded->path;
      convertAndLoad();
      return;
    }
    conversion_input_.clear();
    emit sourceChanged();
    set_status(loaded->error);
    return;
  }
  conversion_input_.clear();
  source_path_ = loaded->path;
  left_ = std::move(loaded->left);
  right_ = std::move(loaded->right);
  onsets_ = std::move(loaded->onsets);
  marks_ = std::move(loaded->marks);
  source_name_ = QFileInfo(source_path_).fileName();
  source_info_ =
      QStringLiteral("48 kHz stereo · %1 s · %2 transients · %3% voiced%4")
          .arg(static_cast<double>(left_.size()) / lab_rate, 0, 'f', 2)
          .arg(onsets_.size())
          .arg(std::lround(loaded->voiced * 100.0))
          .arg(loaded->note.isEmpty() ? QString() : QStringLiteral(" · ") + loaded->note);
  source_waveform_ = waveform(left_, right_);
  transient_view_.clear();
  for (const auto onset : onsets_)
    transient_view_.push_back(static_cast<double>(onset) / static_cast<double>(left_.size()));
  render_path_.clear();
  render_waveform_.clear();
  render_info_.clear();
  emit sourceChanged();
  emit renderChanged();
  set_status(QStringLiteral("Loaded %1").arg(source_name_));
  render();
}

void WarpLabController::convertAndLoad() {
  if (conversion_input_.isEmpty() || converter_ != nullptr)
    return;
  const auto input = conversion_input_;
  const auto digest = QCryptographicHash::hash(QFileInfo(input).absoluteFilePath().toUtf8(),
                                               QCryptographicHash::Sha256)
                          .toHex()
                          .left(12);
  const auto output = QDir(cache_directory_)
                          .filePath(QFileInfo(input).completeBaseName() + QStringLiteral("_") +
                                    QString::fromLatin1(digest) + QStringLiteral("_48k.wav"));
  auto* converter = new QProcess(this);
  converter_ = converter;
  set_busy(true);
  set_status(QStringLiteral("Converting with ffmpeg to 48 kHz stereo…"));
  connect(converter, &QProcess::finished, this, [this, converter, input, output](int code) {
    converter->deleteLater();
    if (converter_ != converter)
      return;
    converter_ = nullptr;
    set_busy(false);
    if (code != 0) {
      set_status(QStringLiteral("ffmpeg could not convert %1").arg(QFileInfo(input).fileName()));
      return;
    }
    load_path(output,
              QStringLiteral("converted from %1 by ffmpeg").arg(QFileInfo(input).fileName()));
  });
  connect(converter, &QProcess::errorOccurred, this, [this, converter](QProcess::ProcessError) {
    if (converter_ == converter && converter->state() == QProcess::NotRunning) {
      converter->deleteLater();
      converter_ = nullptr;
      set_busy(false);
      set_status(QStringLiteral("ffmpeg is not available; convert the file manually"));
    }
  });
  converter_->start(QStringLiteral("ffmpeg"),
                    {QStringLiteral("-y"), QStringLiteral("-loglevel"), QStringLiteral("error"),
                     QStringLiteral("-i"), input, QStringLiteral("-ar"), QStringLiteral("48000"),
                     QStringLiteral("-ac"), QStringLiteral("2"), QStringLiteral("-c:a"),
                     QStringLiteral("pcm_f32le"), output});
}

void WarpLabController::render() {
  if (!has_source())
    return;
  if (busy_) {
    render_queued_ = true;
    return;
  }
  join_worker();
  set_busy(true);
  const auto generation = ++generation_;
  const auto frames = static_cast<std::uint64_t>(left_.size());
  const double speed = std::clamp(speed_, 0.25, 4.0);
  const auto cents = static_cast<std::int32_t>(semitones_ * 100 + cents_);
  const auto mode = std::clamp(mode_, 0, 5);
  const auto tier = static_cast<warp::QualityTier>(std::clamp(tier_, 0, 2));
  std::vector<warp::TimeMapSegment> segments;
  if (tempo_change_) {
    // Markers at 1/3 and 2/3 of the source: speed, 0.6 x speed, 1.5 x speed.
    const auto third = frames / 3;
    const auto first = static_cast<std::uint64_t>(std::llround(static_cast<double>(third) / speed));
    const auto second = first + static_cast<std::uint64_t>(
                                    std::llround(static_cast<double>(third) / (0.6 * speed)));
    const auto third_end =
        second +
        std::max<std::uint64_t>(1, static_cast<std::uint64_t>(std::llround(
                                       static_cast<double>(frames - 2 * third) / (1.5 * speed))));
    segments = {{0, std::max<std::uint64_t>(first, 1), 0, q16(third), false},
                {std::max<std::uint64_t>(first, 1), std::max(second, first + 2), q16(third),
                 q16(2 * third), false},
                {std::max(second, first + 2), std::max(third_end, second + 3), q16(2 * third),
                 q16(frames), false}};
  } else {
    segments = {{0,
                 std::max<std::uint64_t>(1, static_cast<std::uint64_t>(
                                                std::llround(static_cast<double>(frames) / speed))),
                 0, q16(frames), false}};
  }
  if (segments.back().output_end > max_frames) {
    set_status(
        QStringLiteral("Render exceeds 10 minutes; choose a shorter source or faster speed"));
    return;
  }
  const warp::StreamLimits limits{lab_rate, 8'192};
  const auto settings = std::array<double, 7>{beats_envelope_,
                                              formants_,
                                              envelope_,
                                              tones_grain_,
                                              texture_grain_,
                                              flux_,
                                              static_cast<double>(beats_loop_)};
  const auto* left = &left_;
  const auto* right = &right_;
  const auto* onsets = &onsets_;
  const auto* marks = &marks_;
  set_status(QStringLiteral("Rendering %1…").arg(mode_names[static_cast<std::size_t>(mode)]));
  worker_ = std::jthread([this, generation, segments = std::move(segments), limits, settings, left,
                          right, onsets, marks, mode, tier, cents, speed] {
    auto rendered = std::make_shared<Rendered>();
    try {
      auto map = warp::WarpTimeMap::create(segments);
      if (!map) {
        rendered->error = QString::fromStdString(map.error().message);
      } else {
        foundation::Result<std::unique_ptr<warp::WarpStream>> stream =
            foundation::Result<std::unique_ptr<warp::WarpStream>>::failure(
                {foundation::ErrorCode::invalid_argument, "unknown mode"});
        switch (mode) {
        case 0:
          stream = warp::create_beats_stream(
              limits, tier,
              {*onsets,
               static_cast<warp::BeatsLoop>(std::clamp(static_cast<int>(settings[6]), 0, 2)),
               static_cast<float>(settings[0]), cents});
          break;
        case 1:
          stream = warp::create_tones_stream(limits, tier,
                                             {*marks, static_cast<float>(settings[3]), cents});
          break;
        case 2:
          stream = warp::create_texture_stream(
              limits, tier,
              {static_cast<float>(settings[4]), static_cast<float>(settings[5]), cents, 1});
          break;
        case 3:
          stream = warp::create_repitch_stream(limits, tier);
          break;
        case 4:
          stream = warp::create_complex_stream(limits, tier, {*onsets, cents});
          break;
        default:
          stream = warp::create_complex_pro_stream(
              limits, tier,
              {*onsets, cents, static_cast<float>(settings[1]), static_cast<float>(settings[2])});
          break;
        }
        if (!stream) {
          rendered->error = QString::fromStdString(stream.error().message);
        } else {
          warp::MemorySourceView view(*left, *right);
          const auto total = static_cast<std::size_t>(map.value().output_frames());
          rendered->left.assign(total, 0.0F);
          rendered->right.assign(total, 0.0F);
          const double started = thread_seconds();
          auto status = stream.value()->start(map.value(), view, 0);
          std::size_t done = 0;
          while (status == warp::StreamStatus::ok && done < total) {
            const auto want = std::min(pull_frames, total - done);
            const auto written =
                stream.value()->render(std::span(rendered->left).subspan(done, want),
                                       std::span(rendered->right).subspan(done, want));
            done += written;
            if (written < want) {
              status = stream.value()->status();
              if (status == warp::StreamStatus::ok) {
                rendered->error = QStringLiteral("Warp stream made no progress");
                break;
              }
            }
          }
          rendered->cpu = thread_seconds() - started;
          if (status != warp::StreamStatus::ok) {
            rendered->status = QStringLiteral("stream status %1").arg(static_cast<int>(status));
            rendered->error = rendered->status;
          } else if (done != total && rendered->error.isEmpty()) {
            rendered->error = QStringLiteral("Warp stream ended early");
          }
          for (std::size_t index = 0; index < done; ++index) {
            const float magnitude =
                std::max(std::fabs(rendered->left[index]), std::fabs(rendered->right[index]));
            rendered->peak = std::max(rendered->peak, static_cast<double>(magnitude));
            rendered->clipped += magnitude >= 1.0F ? 1U : 0U;
          }
          rendered->label =
              QStringLiteral("%1 · %2× · %3%4 st")
                  .arg(QString::fromLatin1(mode_names[static_cast<std::size_t>(mode)]))
                  .arg(speed, 0, 'f', 2)
                  .arg(cents >= 0 ? QStringLiteral("+") : QString())
                  .arg(static_cast<double>(cents) / 100.0, 0, 'f', 2);
        }
      }
    } catch (const std::exception& error) {
      rendered->error = QString::fromUtf8(error.what());
    } catch (...) {
      rendered->error = QStringLiteral("Unexpected error");
    }
    QMetaObject::invokeMethod(
        this, [this, rendered, generation] { finish_render(rendered, generation); },
        Qt::QueuedConnection);
  });
}

void WarpLabController::finish_render(std::shared_ptr<Rendered> rendered,
                                      std::uint64_t generation) {
  join_worker();
  set_busy(false);
  if (generation == generation_) {
    if (!rendered->error.isEmpty()) {
      set_status(QStringLiteral("Render failed: %1").arg(rendered->error));
    } else {
      const bool was_playing = playing_ == QStringLiteral("render");
      if (was_playing)
        stop();
      // Alternate files so a playing render is never overwritten in place.
      render_path_ =
          QDir(cache_directory_).filePath(QStringLiteral("render_%1.wav").arg(generation % 2));
      if (!write_float_wav(render_path_, rendered->left, rendered->right)) {
        set_status(QStringLiteral("Could not write %1").arg(render_path_));
        render_path_.clear();
      } else {
        const double seconds = static_cast<double>(rendered->left.size()) / lab_rate;
        render_info_ =
            QStringLiteral("%1 · %2 s · CPU %3 s (%4% of real time) · peak %5 · clipped %6 · %7")
                .arg(rendered->label)
                .arg(seconds, 0, 'f', 2)
                .arg(rendered->cpu, 0, 'f', 3)
                .arg(seconds > 0.0 ? 100.0 * rendered->cpu / seconds : 0.0, 0, 'f', 2)
                .arg(rendered->peak, 0, 'f', 3)
                .arg(rendered->clipped)
                .arg(rendered->status);
        render_waveform_ = waveform(rendered->left, rendered->right);
        set_status(QStringLiteral("Rendered %1").arg(rendered->label));
        if (was_playing)
          playRender();
      }
      emit renderChanged();
    }
  }
  if (render_queued_) {
    render_queued_ = false;
    render();
  }
}

void WarpLabController::play(const QString& path, const QString& which) {
  stop();
  if (path.isEmpty())
    return;
  player_ = new QProcess(this);
  connect(player_, &QProcess::finished, this, [this, process = player_] {
    process->deleteLater();
    if (player_ == process) {
      player_ = nullptr;
      playing_.clear();
      emit playingChanged();
    }
  });
  connect(player_, &QProcess::errorOccurred, this,
          [this, process = player_](QProcess::ProcessError) {
            if (player_ == process && process->state() == QProcess::NotRunning) {
              set_status(QStringLiteral("pw-play is not available for playback"));
              player_ = nullptr;
              playing_.clear();
              emit playingChanged();
              process->deleteLater();
            }
          });
  player_->start(QStringLiteral("pw-play"), {path});
  playing_ = which;
  emit playingChanged();
}

void WarpLabController::playSource() { play(source_path_, QStringLiteral("source")); }

void WarpLabController::playRender() { play(render_path_, QStringLiteral("render")); }

void WarpLabController::stop() {
  if (player_ != nullptr) {
    auto* process = player_;
    player_ = nullptr;
    process->kill();
    process->deleteLater();
  }
  if (!playing_.isEmpty()) {
    playing_.clear();
    emit playingChanged();
  }
}

void WarpLabController::saveRender(const QUrl& file) {
  if (render_path_.isEmpty())
    return;
  const auto target = file.isLocalFile() ? file.toLocalFile() : file.toString();
  QFile source(render_path_);
  QSaveFile target_file(target);
  if (!source.open(QIODevice::ReadOnly) || !target_file.open(QIODevice::WriteOnly)) {
    set_status(QStringLiteral("Could not save %1").arg(target));
    return;
  }
  std::array<char, 65'536> chunk{};
  while (true) {
    const auto read = source.read(chunk.data(), chunk.size());
    if (read < 0 || (read > 0 && target_file.write(chunk.data(), read) != read)) {
      target_file.cancelWriting();
      set_status(QStringLiteral("Could not save %1").arg(target));
      return;
    }
    if (read == 0)
      break;
  }
  set_status(target_file.commit() ? QStringLiteral("Saved %1").arg(target)
                                  : QStringLiteral("Could not save %1").arg(target));
}

} // namespace oscillate::warp_lab
