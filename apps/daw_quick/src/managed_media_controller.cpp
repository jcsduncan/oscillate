#include "managed_media_controller.hpp"

#include "oscillate/project/model.hpp"

#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QVariantMap>

#include <exception>
#include <string>
#include <utility>

namespace oscillate::quick {

ManagedMediaController::ManagedMediaController(RepairAllowed repair_allowed, QObject* parent)
    : QObject(parent), repair_allowed_(std::move(repair_allowed)), timer_(this) {
  timer_.setInterval(50);
  connect(&timer_, &QTimer::timeout, this, &ManagedMediaController::poll);
}

bool ManagedMediaController::busy() const {
  return external_relink_ || (session_ && session_->media_repair().managed_source_repair_busy());
}

void ManagedMediaController::set_project(application::ProjectSession* session) {
  timer_.stop();
  if (external_relink_)
    external_relink_->worker.request_stop();
  external_relink_.reset();
  session_ = session;
  close_after_repair_ = false;
  issues_.clear();
  status_.clear();
  emit changed();
}

void ManagedMediaController::refresh() {
  if (!session_) {
    issues_.clear();
    status_.clear();
    emit changed();
    return;
  }
  if (busy())
    return;
  auto listed = session_->media_repair().managed_media_issues();
  if (!listed) {
    issues_.clear();
    status_ = QStringLiteral("Could not check managed media: %1")
                  .arg(QString::fromStdString(listed.error().message));
    emit changed();
    return;
  }
  QVariantList issues;
  std::size_t external_count = 0;
  for (const auto& item : listed.value()) {
    const bool external = item.source && item.source->external_link.has_value();
    if (external)
      ++external_count;
    QString status;
    switch (item.status) {
    case persistence::LoadedGeneration::SourceDiagnostic::Status::missing:
      status = external ? QStringLiteral("External link missing") : QStringLiteral("Missing");
      break;
    case persistence::LoadedGeneration::SourceDiagnostic::Status::damaged:
      status = external ? QStringLiteral("External link damaged") : QStringLiteral("Damaged");
      break;
    case persistence::LoadedGeneration::SourceDiagnostic::Status::unresolved:
      status = QStringLiteral("Unresolved reference");
      break;
    }
    const auto& source = item.source;
    QString last_known_path;
    if (source && source->external_link)
      last_known_path = QString::fromStdString(source->external_link.value().last_known_path);
    const auto encoding =
        !source                                             ? QStringLiteral("Unknown")
        : source->encoding == project::SourceEncoding::wav  ? QStringLiteral("WAV")
        : source->encoding == project::SourceEncoding::aiff ? QStringLiteral("AIFF")
        : source->encoding == project::SourceEncoding::flac ? QStringLiteral("FLAC")
                                                            : QStringLiteral("MP3");
    issues.push_back(
        QVariantMap{{"id", QString::fromStdString(item.id.to_string())},
                    {"status", status},
                    {"repairable", source && !external && status == QStringLiteral("Missing")},
                    {"external", external},
                    {"encoding", encoding},
                    {"bytes", source ? static_cast<qulonglong>(source->byte_length) : 0ULL},
                    {"sampleRate", source ? source->sample_rate : 0U},
                    {"channels", source ? source->channels : 0U},
                    {"lastKnownPath", last_known_path},
                    {"sha256", source ? QString::fromStdString(source->sha256) : QString{}}});
  }
  issues_ = std::move(issues);
  if (issues_.empty()) {
    status_ = QStringLiteral("No unavailable saved sources detected");
  } else if (external_count == 0) {
    status_ = QStringLiteral("%1 saved managed source issue(s); only missing assets can be "
                             "repaired here")
                  .arg(issues_.size());
  } else {
    status_ = QStringLiteral("%1 unavailable saved source(s), including %2 external link(s). "
                             "Choose the exact original audio to relink an external source")
                  .arg(issues_.size())
                  .arg(external_count);
  }
  emit changed();
}

void ManagedMediaController::reject_busy() {
  status_ = QStringLiteral("Stop playback and finish project jobs before changing media");
  emit changed();
}

void ManagedMediaController::choose_replacement(const QString& source_id) {
  if (repair_allowed_ && !repair_allowed_()) {
    reject_busy();
    return;
  }
  const auto chosen = QFileDialog::getOpenFileName(
      nullptr, QStringLiteral("Choose exact replacement for missing managed source"),
      QDir::homePath(),
      QStringLiteral("Audio files (*.wav *.wave *.aiff *.aif *.flac *.mp3);;All files (*)"));
  if (!chosen.isEmpty())
    repair(source_id, QUrl::fromLocalFile(chosen));
}

void ManagedMediaController::repair(const QString& source_id, const QUrl& replacement) {
  if ((repair_allowed_ && !repair_allowed_()) || !session_ || busy() || close_after_repair_) {
    reject_busy();
    return;
  }
  const auto parsed = project::SourceId::parse(source_id.toStdString());
  if (!parsed || !replacement.isLocalFile()) {
    status_ = QStringLiteral("Choose a saved source and a local replacement file");
    emit changed();
    return;
  }
  QFile input(replacement.toLocalFile());
  if (!input.open(QIODevice::ReadOnly)) {
    status_ = QStringLiteral("Could not open replacement file: %1").arg(input.errorString());
    emit changed();
    return;
  }
  const auto begun =
      session_->media_repair().begin_managed_source_repair(parsed.value(), input.handle());
  if (!begun) {
    status_ =
        QStringLiteral("Repair unavailable: %1").arg(QString::fromStdString(begun.error().message));
    emit changed();
    return;
  }
  status_ = QStringLiteral("Verifying exact replacement in the background; "
                           "project media is unchanged until verification passes");
  emit changed();
  emit started();
  timer_.start();
}

void ManagedMediaController::choose_external_relink(const QString& source_id) {
  if (repair_allowed_ && !repair_allowed_()) {
    reject_busy();
    return;
  }
  QString initial_folder = QDir::homePath();
  for (const auto& issue : issues_) {
    const auto fields = issue.toMap();
    if (fields.value(QStringLiteral("id")).toString() != source_id)
      continue;
    const auto folder =
        QFileInfo(fields.value(QStringLiteral("lastKnownPath")).toString()).absolutePath();
    if (QDir(folder).exists())
      initial_folder = folder;
    break;
  }
  const auto chosen = QFileDialog::getOpenFileName(
      nullptr, QStringLiteral("Find the exact audio file for this external source"), initial_folder,
      QStringLiteral("Audio files (*.wav *.wave *.aiff *.aif *.flac *.mp3);;All files (*)"));
  if (!chosen.isEmpty())
    relink(source_id, QUrl::fromLocalFile(chosen));
}

void ManagedMediaController::relink(const QString& source_id, const QUrl& replacement) {
  if ((repair_allowed_ && !repair_allowed_()) || !session_ || busy() || close_after_repair_) {
    reject_busy();
    return;
  }
  const auto parsed = project::SourceId::parse(source_id.toStdString());
  if (!parsed || !replacement.isLocalFile()) {
    status_ = QStringLiteral("Choose an external source and a local audio file");
    emit changed();
    return;
  }
  auto begun = session_->imports().begin_external_source_relink(
      parsed.value(), replacement.toLocalFile().toStdString());
  if (!begun) {
    status_ =
        QStringLiteral("Relink unavailable: %1").arg(QString::fromStdString(begun.error().message));
    emit changed();
    return;
  }
  try {
    auto state = std::make_unique<ExternalRelinkState>();
    auto* worker_state = state.get();
    state->worker = std::jthread(
        [worker_state, task = std::move(begun).value()](const std::stop_token& stop) mutable {
          try {
            auto result = std::move(task).run(stop);
            std::lock_guard lock(worker_state->mutex);
            worker_state->result.emplace(std::move(result));
          } catch (const std::exception& error) {
            std::lock_guard lock(worker_state->mutex);
            worker_state->result.emplace(
                foundation::Result<application::PreparedExternalSourceRelink>::failure(
                    {foundation::ErrorCode::internal,
                     "External verification failed: " + std::string(error.what())}));
          }
        });
    external_relink_ = std::move(state);
  } catch (const std::exception& error) {
    status_ = QStringLiteral("Could not start external verification: %1")
                  .arg(QString::fromLocal8Bit(error.what()));
    emit changed();
    return;
  }
  status_ = QStringLiteral("Checking the full audio identity in the background");
  emit changed();
  emit started();
  timer_.start();
}

void ManagedMediaController::cancel() {
  if (!busy())
    return;
  if (external_relink_)
    external_relink_->worker.request_stop();
  else
    session_->media_repair().cancel_managed_source_repair();
  status_ = QStringLiteral("Cancelling media verification; waiting for worker to finish");
  emit changed();
}

void ManagedMediaController::cancel_for_close() {
  if (!busy())
    return;
  close_after_repair_ = true;
  if (external_relink_)
    external_relink_->worker.request_stop();
  else
    session_->media_repair().cancel_managed_source_repair();
  status_ = QStringLiteral("Cancelling media verification before closing");
  emit changed();
  timer_.start();
}

void ManagedMediaController::poll() {
  if (external_relink_) {
    std::optional<foundation::Result<application::PreparedExternalSourceRelink>> completed;
    {
      std::lock_guard lock(external_relink_->mutex);
      if (!external_relink_->result)
        return;
      completed.emplace(std::move(*external_relink_->result));
    }
    timer_.stop();
    external_relink_.reset();
    if (close_after_repair_) {
      emit readyToClose();
      return;
    }
    if (!completed->has_value()) {
      refresh();
      status_ = QStringLiteral("External relink was not confirmed: %1")
                    .arg(QString::fromStdString(completed->error().message));
      emit changed();
      emit finished(false, true);
      return;
    }
    const auto before_revision = session_->snapshot().revision;
    auto saved = session_->imports().finish_external_source_relink(std::move(completed->value()));
    const bool model_changed = session_->snapshot().revision != before_revision;
    refresh();
    if (!saved) {
      status_ = QStringLiteral("External relink was not confirmed: %1")
                    .arg(QString::fromStdString(saved.error().message));
    } else {
      status_ = QStringLiteral("External source relinked and saved; original media identity "
                               "and clip references are preserved");
    }
    emit changed();
    emit finished(model_changed || saved.has_value(), true);
    return;
  }
  if (!busy()) {
    timer_.stop();
    return;
  }
  auto completed = session_->media_repair().take_managed_source_repair_result();
  if (!completed)
    return;
  timer_.stop();
  if (close_after_repair_) {
    emit readyToClose();
    return;
  }
  refresh();
  if (!*completed) {
    status_ = QStringLiteral("Repair not confirmed for saved CURRENT: %1")
                  .arg(QString::fromStdString(completed->error().message));
  } else if (completed->value() == persistence::RestoreStatus::uncertain) {
    status_ = QStringLiteral("Source is readable now, but durability is uncertain. "
                             "Keep the replacement until reopen verifies it.");
  } else {
    status_ = QStringLiteral("Managed source restored and verified; source IDs and "
                             "project edits are unchanged");
  }
  emit changed();
  emit finished(completed->has_value(), false);
}

} // namespace oscillate::quick
