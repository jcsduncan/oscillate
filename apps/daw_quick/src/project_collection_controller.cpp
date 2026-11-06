#include "project_collection_controller.hpp"

#include "oscillate/application/project_collection.hpp"
#include "oscillate/application/project_session.hpp"

#include <string>
#include <system_error>
#include <utility>

namespace oscillate::quick {

ProjectCollectionController::ProjectCollectionController(QObject* parent) : QObject(parent) {
  timer_.setInterval(50);
  connect(&timer_, &QTimer::timeout, this, &ProjectCollectionController::poll);
}

ProjectCollectionController::~ProjectCollectionController() {
  timer_.stop();
  if (state_)
    state_->worker.request_stop();
}

std::optional<QString> ProjectCollectionController::collect(application::ProjectSession& session,
                                                            const std::filesystem::path& source,
                                                            const std::filesystem::path& library) {
  if (busy())
    return QStringLiteral("Finish active project work before collecting a copy");
  const auto saved = session.saving().save();
  if (!saved)
    return QStringLiteral("Save before collection failed: %1")
        .arg(QString::fromStdString(saved.error().message));

  std::error_code error;
  std::filesystem::create_directories(library, error);
  if (error)
    return QStringLiteral("Could not create project library: %1")
        .arg(QString::fromStdString(error.message()));

  const auto base_name = source.filename().string() + " collected";
  for (int suffix = 1; suffix <= 999; ++suffix) {
    const auto name = suffix == 1 ? base_name : base_name + " (" + std::to_string(suffix) + ")";
    const auto destination = library / name;
    if (std::filesystem::exists(destination, error))
      continue;
    if (error)
      return QStringLiteral("Could not inspect project library: %1")
          .arg(QString::fromStdString(error.message()));
    start(source, destination, session.snapshot().revision);
    return std::nullopt;
  }
  return QStringLiteral("Project library has no available collected-copy name");
}

void ProjectCollectionController::start(std::filesystem::path source,
                                        std::filesystem::path destination,
                                        std::uint64_t expected_revision) {
  if (state_)
    return;
  auto state = std::make_unique<State>();
  state->destination = destination;
  state->captured_revision = expected_revision;
  State* completion = state.get();
  state->worker =
      std::jthread([completion, source = std::move(source), destination = std::move(destination),
                    expected_revision](const std::stop_token& stop) {
        auto result = application::collect_and_save_copy(
            source, destination, stop, expected_revision,
            [completion](application::CollectionProgress progress) {
              completion->total_sources.store(progress.total_sources, std::memory_order_relaxed);
              completion->completed_sources.store(progress.completed_sources,
                                                  std::memory_order_relaxed);
            });
        std::lock_guard lock(completion->mutex);
        completion->result.emplace(std::move(result));
      });
  state_ = std::move(state);
  status_ = QStringLiteral("Collecting the last saved project and verifying all media");
  timer_.start();
  emit changed();
}

void ProjectCollectionController::cancel() {
  if (!state_)
    return;
  state_->worker.request_stop();
  status_ = QStringLiteral("Cancelling collected project copy");
  emit changed();
}

void ProjectCollectionController::poll() {
  if (!state_)
    return;
  if (!state_->worker.get_stop_token().stop_requested()) {
    const auto total = state_->total_sources.load(std::memory_order_relaxed);
    const auto completed = state_->completed_sources.load(std::memory_order_relaxed);
    if (total > 0) {
      const auto next = completed == total
                            ? QStringLiteral("Saving collected project and named revisions")
                            : QStringLiteral("Collecting media %1 of %2")
                                  .arg(static_cast<qulonglong>(completed))
                                  .arg(static_cast<qulonglong>(total));
      if (next != status_) {
        status_ = next;
        emit changed();
      }
    }
  }
  std::optional<foundation::Result<void>> completed;
  {
    std::lock_guard lock(state_->mutex);
    if (!state_->result)
      return;
    completed.emplace(std::move(*state_->result));
  }
  const auto destination = state_->destination;
  const auto captured_revision = state_->captured_revision;
  state_.reset();
  timer_.stop();
  const bool success = completed->has_value();
  status_ = success ? QStringLiteral("Collected project ready: %1 (from saved revision %2)")
                          .arg(QString::fromStdString(destination.filename().string()))
                          .arg(static_cast<qulonglong>(captured_revision))
                    : QStringLiteral("Collection failed: %1")
                          .arg(QString::fromStdString(completed->error().message));
  emit changed();
  emit finished(success);
}

} // namespace oscillate::quick
