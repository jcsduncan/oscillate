#pragma once

#include "oscillate/foundation/error.hpp"

#include <QObject>
#include <QString>
#include <QTimer>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace oscillate::application {
class ProjectSession;
}

namespace oscillate::quick {

// Owns the cancellable worker for a copy of CURRENT and named revisions.
class ProjectCollectionController final : public QObject {
  Q_OBJECT

public:
  explicit ProjectCollectionController(QObject* parent = nullptr);
  ~ProjectCollectionController() override;

  [[nodiscard]] bool busy() const { return state_ != nullptr; }
  [[nodiscard]] QString status() const { return status_; }
  // UI-thread collection command. The caller owns cross-feature admission;
  // this owner saves the current project, names its copy and starts the worker.
  [[nodiscard]] std::optional<QString> collect(application::ProjectSession& session,
                                               const std::filesystem::path& source,
                                               const std::filesystem::path& library);
  // Save a copy as: the same collection into `destination`, which the caller
  // has checked is free; copySaved reports it when done, for opening.
  [[nodiscard]] std::optional<QString> save_copy(application::ProjectSession& session,
                                                 const std::filesystem::path& source,
                                                 const std::filesystem::path& destination);
  void cancel();

signals:
  void changed();
  void finished(bool success);
  // A save_copy finished: the copy at `destination` was made from `source`.
  void copySaved(const QString& destination, const QString& source);

private:
  void start(std::filesystem::path source, std::filesystem::path destination,
             std::uint64_t expected_revision, bool save_as);
  void poll();

  struct State {
    std::mutex mutex;
    std::optional<foundation::Result<void>> result;
    std::atomic<std::size_t> completed_sources{0};
    std::atomic<std::size_t> total_sources{0};
    std::jthread worker;
    std::filesystem::path source;
    std::filesystem::path destination;
    std::uint64_t captured_revision = 0;
    bool save_as = false;
  };
  QTimer timer_;
  std::unique_ptr<State> state_;
  QString status_;
};

} // namespace oscillate::quick
