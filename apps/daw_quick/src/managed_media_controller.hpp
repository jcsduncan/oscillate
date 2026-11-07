#pragma once

#include "oscillate/application/project_session.hpp"

#include <QObject>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVariantList>

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace oscillate::quick {

// UI-thread owner of saved-source diagnostics and one media recovery operation
// (QML: projectBridge.managedMedia). ProjectSession owns managed repair;
// external relink verifies on this controller's worker.
class ManagedMediaController final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QVariantList managedMediaIssues READ issues NOTIFY changed)
  Q_PROPERTY(QString managedMediaStatus READ status NOTIFY changed)
  Q_PROPERTY(bool managedMediaBusy READ busy NOTIFY changed)
  // Whether a repair or relink may start now (the project's other jobs are idle).
  Q_PROPERTY(bool managedMediaRepairEnabled READ repair_enabled NOTIFY repairEnabledChanged)

public:
  using RepairAllowed = std::function<bool()>;
  explicit ManagedMediaController(RepairAllowed repair_allowed = {}, QObject* parent = nullptr);

  [[nodiscard]] QVariantList issues() const { return issues_; }
  [[nodiscard]] QString status() const { return status_; }
  [[nodiscard]] bool busy() const;
  [[nodiscard]] bool closing() const { return close_after_repair_; }
  [[nodiscard]] bool repair_enabled() const { return repair_allowed_ && repair_allowed_(); }

  void set_project(application::ProjectSession* session);
  Q_INVOKABLE void refresh();
  void reject_busy();
  Q_INVOKABLE void choose_replacement(const QString& source_id);
  Q_INVOKABLE void repair(const QString& source_id, const QUrl& replacement);
  Q_INVOKABLE void choose_external_relink(const QString& source_id);
  Q_INVOKABLE void relink(const QString& source_id, const QUrl& replacement);
  Q_INVOKABLE void cancel();
  void cancel_for_close();
  // The project's jobs changed: repairEnabled may differ.
  void notify_repair_enabled() { emit repairEnabledChanged(); }

signals:
  void changed();
  void repairEnabledChanged();
  void started();
  void finished(bool changed, bool external);
  void readyToClose();

private:
  void poll();

  application::ProjectSession* session_ = nullptr;
  RepairAllowed repair_allowed_;
  QTimer timer_;
  QVariantList issues_;
  QString status_;
  bool close_after_repair_ = false;
  struct ExternalRelinkState {
    std::mutex mutex;
    std::optional<foundation::Result<application::PreparedExternalSourceRelink>> result;
    std::jthread worker;
  };
  std::unique_ptr<ExternalRelinkState> external_relink_;
};

} // namespace oscillate::quick
