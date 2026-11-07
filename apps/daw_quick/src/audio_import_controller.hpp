#pragma once

#include "oscillate/application/project_session.hpp"

#include <QObject>
#include <QString>
#include <QTimer>
#include <QUrl>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <thread>
#include <variant>

namespace oscillate::quick {

// Owns one asynchronous import. All project commits and signals run on the UI thread.
// The song's name from a file name: a store's catalogue-number prefix
// dropped, underscores as spaces.
[[nodiscard]] QString song_name(const QString& file_stem);

class AudioImportController final : public QObject {
  Q_OBJECT

public:
  explicit AudioImportController(QObject* parent = nullptr);

  [[nodiscard]] bool busy() const noexcept { return completion_ != nullptr; }
  [[nodiscard]] bool closing() const noexcept { return close_after_import_; }
  [[nodiscard]] QString status() const { return status_; }

  void set_project(application::ProjectSession* session);
  void reject(QString message);
  [[nodiscard]] bool start(const QUrl& source, std::optional<std::size_t> insert_index);
  void cancel_for_close();

signals:
  void changed();
  // created_index is -1 on failure. refresh_project is true after a finish attempt.
  void completed(int created_index, bool refresh_project);
  void readyToClose();

private:
  void poll();

  struct Completion {
    std::atomic<bool> done{false};
    std::uint64_t revision = 0;
    std::optional<std::size_t> insert_index;
    // The song's name, from its file, for the new clip.
    std::string clip_name;
    using Prepared =
        std::variant<application::PreparedWavAudioImport, application::PreparedEncodedAudioImport>;
    std::optional<foundation::Result<Prepared>> result;
  };

  application::ProjectSession* session_ = nullptr;
  QTimer timer_;
  std::shared_ptr<Completion> completion_;
  std::jthread worker_;
  QString status_;
  bool close_after_import_ = false;
};

} // namespace oscillate::quick
