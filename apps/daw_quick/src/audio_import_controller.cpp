#include "audio_import_controller.hpp"

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

#include <exception>
#include <string>
#include <type_traits>
#include <utility>

namespace oscillate::quick {

QString song_name(const QString& file_stem) {
  // Store downloads arrive as "13002646_Brightest Lights_(Original Mix)":
  // drop the catalogue number and use spaces.
  static const QRegularExpression catalogue(QStringLiteral("^\\d{4,}[_ -]+"));
  auto name = file_stem;
  name.remove(catalogue);
  name.replace(QLatin1Char('_'), QLatin1Char(' '));
  name = name.simplified();
  return name.isEmpty() ? file_stem : name;
}

AudioImportController::AudioImportController(QObject* parent) : QObject(parent), timer_(this) {
  timer_.setInterval(50);
  connect(&timer_, &QTimer::timeout, this, &AudioImportController::poll);
}

void AudioImportController::set_project(application::ProjectSession* session) {
  timer_.stop();
  if (worker_.joinable()) {
    worker_.request_stop();
    worker_.join();
  }
  completion_.reset();
  session_ = session;
  close_after_import_ = false;
  status_.clear();
  emit changed();
}

void AudioImportController::reject(QString message) {
  status_ = std::move(message);
  emit changed();
}

bool AudioImportController::start(const QUrl& source, std::optional<std::size_t> insert_index) {
  if (!session_) {
    reject(QStringLiteral("Open a project first"));
    return false;
  }
  if (busy() || close_after_import_) {
    reject(QStringLiteral("Finish the current audio import first"));
    return false;
  }
  if (!source.isLocalFile()) {
    reject(QStringLiteral("Choose a local audio file"));
    return false;
  }
  QFile input(source.toLocalFile());
  if (!input.open(QIODevice::ReadOnly)) {
    reject(QStringLiteral("Could not open audio input: %1").arg(input.errorString()));
    return false;
  }
  const auto header = input.read(12);
  if (!input.seek(0)) {
    reject(QStringLiteral("Could not seek audio input: %1").arg(input.errorString()));
    return false;
  }
  const bool wav = header.size() >= 12 && header.startsWith("RIFF") && header.mid(8, 4) == "WAVE";
  using ImportTask =
      std::variant<application::WavAudioImportTask, application::EncodedAudioImportTask>;
  std::optional<ImportTask> task;
  foundation::Error begin_error{foundation::ErrorCode::internal, "audio import not started"};
  if (wav || OSCILLATE_HAVE_CODEC == 0) {
    auto begun = session_->imports().begin_wav_audio_import(input.handle());
    if (begun) {
      task.emplace(std::in_place_type<application::WavAudioImportTask>, std::move(begun).value());
    } else {
      begin_error = begun.error();
    }
  } else {
    auto begun = session_->imports().begin_encoded_audio_import(input.handle());
    if (begun) {
      task.emplace(std::in_place_type<application::EncodedAudioImportTask>,
                   std::move(begun).value());
    } else {
      begin_error = begun.error();
    }
  }
  if (!task) {
    reject(
        QStringLiteral("Audio import failed: %1").arg(QString::fromStdString(begin_error.message)));
    return false;
  }
  auto completion = std::make_shared<Completion>();
  completion->revision =
      std::visit([](const auto& pending) { return pending.origin_revision(); }, *task);
  completion->insert_index = insert_index;
  completion->clip_name =
      song_name(QFileInfo(source.toLocalFile()).completeBaseName()).toStdString();
  try {
    worker_ =
        std::jthread([task = std::move(*task), completion](const std::stop_token& stop) mutable {
          try {
            completion->result.emplace(std::visit(
                [&stop](auto& pending) -> foundation::Result<Completion::Prepared> {
                  auto prepared = std::move(pending).run(stop);
                  if (!prepared)
                    return foundation::Result<Completion::Prepared>::failure(prepared.error());
                  return foundation::Result<Completion::Prepared>::success(
                      Completion::Prepared(std::move(prepared).value()));
                },
                task));
          } catch (const std::exception& error) {
            completion->result.emplace(foundation::Result<Completion::Prepared>::failure(
                {foundation::ErrorCode::internal,
                 std::string("Audio import failed: ") + error.what()}));
          } catch (...) {
            completion->result.emplace(foundation::Result<Completion::Prepared>::failure(
                {foundation::ErrorCode::internal, "Audio import failed unexpectedly"}));
          }
          completion->done.store(true, std::memory_order_release);
        });
  } catch (const std::exception& error) {
    reject(QStringLiteral("Cannot start audio import: %1").arg(error.what()));
    return false;
  }
  completion_ = std::move(completion);
  status_ = QStringLiteral("Importing %1 from project revision %2")
                .arg(QFileInfo(source.toLocalFile()).fileName())
                .arg(completion_->revision);
  emit changed();
  timer_.start();
  return true;
}

void AudioImportController::cancel_for_close() {
  if (!completion_)
    return;
  close_after_import_ = true;
  worker_.request_stop();
  status_ = QStringLiteral("Cancelling audio import before closing");
  emit changed();
  timer_.start();
}

void AudioImportController::poll() {
  if (!completion_ || !completion_->done.load(std::memory_order_acquire))
    return;
  timer_.stop();
  if (worker_.joinable())
    worker_.join();
  auto completion = std::move(completion_);
  int created_index = -1;
  bool refresh_project = false;
  if (close_after_import_) {
    status_ = QStringLiteral("Audio import cancelled before closing");
  } else if (!completion->result) {
    status_ = QStringLiteral("Audio import failed unexpectedly");
  } else if (!*completion->result) {
    status_ = QStringLiteral("Audio import failed: %1")
                  .arg(QString::fromStdString(completion->result->error().message));
  } else {
    const auto finished = std::visit(
        [this, insert_index = completion->insert_index,
         name = completion->clip_name](auto&& prepared) -> foundation::Result<project::TrackId> {
          using Prepared = std::decay_t<decltype(prepared)>;
          if constexpr (std::is_same_v<Prepared, application::PreparedWavAudioImport>) {
            return session_->imports().finish_wav_audio_import(
                std::forward<decltype(prepared)>(prepared), insert_index, name);
          } else {
            return session_->imports().finish_encoded_audio_import(
                std::forward<decltype(prepared)>(prepared), insert_index, name);
          }
        },
        std::move(completion->result->value()));
    refresh_project = true;
    if (finished) {
      created_index =
          static_cast<int>(session_->tracks().track_index(finished.value()).value_or(0));
      status_ = QStringLiteral("Audio imported and saved · revision %1")
                    .arg(session_->snapshot().revision);
    } else {
      status_ =
          QStringLiteral("Audio import: %1").arg(QString::fromStdString(finished.error().message));
    }
  }
  emit changed();
  emit completed(created_index, refresh_project);
  if (close_after_import_)
    emit readyToClose();
}

} // namespace oscillate::quick
