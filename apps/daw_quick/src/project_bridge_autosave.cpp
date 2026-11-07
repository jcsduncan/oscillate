#include "native_synth_device_controller.hpp"
#include "project_bridge.hpp"

#include <QSettings>

#include <string>
#include <utility>

namespace oscillate::quick {
namespace {

// A save worker's exception as a failed save.
foundation::Result<persistence::SaveOutcome> save_worker_failure(const char* worker,
                                                                 const std::string& what) {
  return foundation::Result<persistence::SaveOutcome>::failure(
      {foundation::ErrorCode::io_failure,
       what.empty() ? std::string(worker) : std::string(worker) + ": " + what});
}

} // namespace

void ProjectBridge::set_autosave_interval_seconds(int seconds) {
  if (seconds != 0 && (seconds < 15 || seconds > 1800)) {
    set_status(QStringLiteral("Choose an autosave interval from 15 to 1800 seconds, or Off"));
    return;
  }
  if (seconds == autosave_interval_seconds_)
    return;
  autosave_interval_seconds_ = seconds;
  QSettings{}.setValue(QStringLiteral("autosaveIntervalSeconds"), seconds);
  if (seconds == 0) {
    autosave_timer_.stop();
    autosave_status_ = QStringLiteral("Autosave off");
  } else {
    autosave_timer_.setInterval(seconds * 1000);
    if (session_)
      autosave_timer_.start();
    autosave_status_ = QStringLiteral("Autosave every %1 seconds").arg(seconds);
  }
  emit autosaveIntervalChanged();
  emit autosaveStatusChanged();
}

void ProjectBridge::start_autosave() {
  if (!session_ || autosave_job_.running() || background_save_job_.running() ||
      background_save_timer_.isActive() || !session_->saving().has_unsaved_changes() ||
      session_->snapshot().revision == last_autosave_revision_ || managed_media_busy() ||
      import_busy() || close_after_autosave_ || close_after_playback_ || closing_after_export())
    return;
  auto task = session_->saving().begin_autosave();
  if (!task) {
    autosave_status_ =
        QStringLiteral("Autosave deferred: %1").arg(QString::fromStdString(task.error().message));
    set_status(autosave_status_);
    emit autosaveStatusChanged();
    return;
  }
  const SaveTicket ticket{task.value().project_id(), task.value().revision()};
  if (const auto failed = autosave_job_.start(
          project_generation_,
          [task = std::move(task.value())]() mutable { return std::move(task).run(); },
          [](const std::string& what) {
            return save_worker_failure("Autosave worker failed", what);
          })) {
    autosave_status_ =
        QStringLiteral("Autosave could not start: %1").arg(QString::fromStdString(*failed));
    set_status(autosave_status_);
    emit autosaveStatusChanged();
    return;
  }
  autosave_ticket_ = ticket;
  autosave_status_ = QStringLiteral("Saving recovery snapshot…");
  emit autosaveStatusChanged();
  autosave_poll_timer_.start();
}

void ProjectBridge::schedule_background_save() {
  if (!session_ || !session_->saving().has_unsaved_changes())
    return;
  background_save_pending_ = true;
  if (!background_save_job_.running())
    background_save_timer_.start();
}

void ProjectBridge::start_background_save() {
  if (!session_ || !background_save_pending_ || background_save_job_.running())
    return;
  if (autosave_job_.running() || managed_media_busy() || import_busy()) {
    background_save_timer_.start();
    return;
  }
  if (!session_->saving().has_unsaved_changes()) {
    background_save_pending_ = false;
    return;
  }
  auto task = session_->saving().begin_background_save();
  if (!task) {
    background_save_pending_ = false;
    set_save_problem(QStringLiteral("Project edit kept in memory; background save unavailable: %1")
                         .arg(QString::fromStdString(task.error().message)));
    return;
  }
  const SaveTicket ticket{task.value().project_id(), task.value().revision()};
  if (const auto failed = background_save_job_.start(
          project_generation_,
          [task = std::move(task.value())]() mutable { return std::move(task).run(); },
          [](const std::string& what) {
            return save_worker_failure("Project save worker failed", what);
          })) {
    background_save_pending_ = false;
    set_save_problem(QStringLiteral("Project edit kept in memory; save worker failed to start: %1")
                         .arg(QString::fromStdString(*failed)));
    return;
  }
  background_save_pending_ = false;
  background_save_ticket_ = ticket;
  background_save_poll_timer_.start();
}

void ProjectBridge::poll_background_save() {
  auto finished = background_save_job_.take();
  if (!finished)
    return;
  background_save_poll_timer_.stop();
  const auto& result = finished->result;
  const auto& ticket = background_save_ticket_;
  if (session_ && finished->generation == project_generation_ &&
      ticket.project == session_->snapshot().id) {
    if (!result) {
      if (session_->saving().persisted_revision() < ticket.revision &&
          result.error().code == foundation::ErrorCode::conflict)
        background_save_pending_ = true;
      else if (session_->saving().persisted_revision() < ticket.revision)
        set_save_problem(QStringLiteral("Project edit kept in memory; save failed: %1")
                             .arg(QString::fromStdString(result.error().message)));
    } else if (result.value().status != persistence::SaveStatus::committed)
      set_save_problem(QStringLiteral("Project save durability is uncertain; inspect recovery"));
    else {
      session_->saving().finish_background_save(ticket.revision, result.value());
      set_save_problem({});
      if (native_synth_controller_)
        native_synth_controller_->refresh();
      if (native_effect_controller_)
        native_effect_controller_->refresh();
      if (result.value().maintenance_warning)
        set_status(QStringLiteral("Project saved; storage cleanup needs attention: %1")
                       .arg(QString::fromStdString(result.value().maintenance_warning->message)));
      else if (!session_->saving().has_unsaved_changes())
        set_status(QStringLiteral("Project saved · revision %1").arg(ticket.revision));
    }
  }
  if (close_after_background_save_) {
    close_after_background_save_ = false;
    emit readyToClose();
  } else if (background_save_pending_) {
    background_save_timer_.start();
  }
}

void ProjectBridge::poll_autosave() {
  auto finished = autosave_job_.take();
  if (!finished)
    return;
  autosave_poll_timer_.stop();
  const auto& result = finished->result;
  const auto& ticket = autosave_ticket_;
  if (!session_ || finished->generation != project_generation_ ||
      ticket.project != session_->snapshot().id)
    return;
  if (!result) {
    autosave_status_ =
        QStringLiteral("Autosave failed: %1").arg(QString::fromStdString(result.error().message));
    set_status(autosave_status_);
  } else if (result.value().status != persistence::SaveStatus::committed) {
    autosave_status_ =
        QStringLiteral("Autosave durability is uncertain; inspect recovery before retrying");
    set_status(autosave_status_);
  } else {
    last_autosave_revision_ = ticket.revision;
    autosave_status_ = QStringLiteral("Recovery snapshot saved · revision %1").arg(ticket.revision);
    if (result.value().maintenance_warning)
      autosave_status_ +=
          QStringLiteral(" · Storage cleanup needs attention: %1")
              .arg(QString::fromStdString(result.value().maintenance_warning->message));
  }
  emit autosaveStatusChanged();
  if (close_after_autosave_) {
    close_after_autosave_ = false;
    emit readyToClose();
  }
}

void ProjectBridge::finish_in_flight_saves() {
  // The workers only write the store; waiting for them on this thread is
  // safe. The poll then installs the result as its timer would have.
  if (background_save_job_.running()) {
    background_save_job_.wait();
    poll_background_save();
  }
  if (autosave_job_.running()) {
    autosave_job_.wait();
    poll_autosave();
  }
}

void ProjectBridge::set_save_problem(QString problem) {
  if (!problem.isEmpty())
    set_status(problem);
  if (problem == save_problem_)
    return;
  save_problem_ = std::move(problem);
  emit saveProblemChanged();
}

} // namespace oscillate::quick
