#pragma once

#include "oscillate/application/cache_control.hpp"

#include <QObject>
#include <QTimer>
#include <QVariantList>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace oscillate::quick {

class CacheUiWorker;

// UI projection of worker-only cache maintenance. No registry or cache I/O runs
// in an invokable, QML binding, or the audio callback.
class CacheUiController final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool busy READ busy NOTIFY changed)
  Q_PROPERTY(QString status READ status NOTIFY changed)
  Q_PROPERTY(qulonglong budgetMiB READ budget_mib NOTIFY changed)
  Q_PROPERTY(qulonglong usageBytes READ usage_bytes NOTIFY changed)
  Q_PROPERTY(qulonglong entryCount READ entry_count NOTIFY changed)
  Q_PROPERTY(qulonglong sharedUsageBytes READ shared_usage_bytes NOTIFY changed)
  Q_PROPERTY(qulonglong sharedEntryCount READ shared_entry_count NOTIFY changed)
  Q_PROPERTY(QString sharedStatus READ shared_status NOTIFY changed)
  Q_PROPERTY(QVariantList projects READ projects NOTIFY changed)

public:
  explicit CacheUiController(std::filesystem::path profile, QObject* parent = nullptr);
  ~CacheUiController() override;

  [[nodiscard]] bool busy() const { return outstanding_ != 0; }
  [[nodiscard]] QString status() const { return status_; }
  [[nodiscard]] qulonglong budget_mib() const { return budget_mib_; }
  [[nodiscard]] qulonglong usage_bytes() const { return usage_bytes_; }
  [[nodiscard]] qulonglong entry_count() const { return entry_count_; }
  [[nodiscard]] qulonglong shared_usage_bytes() const { return shared_usage_bytes_; }
  [[nodiscard]] qulonglong shared_entry_count() const { return shared_entry_count_; }
  [[nodiscard]] QString shared_status() const { return shared_status_; }
  [[nodiscard]] QVariantList projects() const { return projects_; }

  void register_project(const std::filesystem::path& root);
  Q_INVOKABLE void refresh();
  Q_INVOKABLE void set_budget_mib(int mib);
  Q_INVOKABLE void clear_current();
  Q_INVOKABLE void clear_all();

signals:
  void changed();

private:
  void poll();
  void queue_refresh();
  std::unique_ptr<CacheUiWorker> worker_;
  QTimer timer_;
  std::filesystem::path current_root_;
  QString status_ = QStringLiteral("Inspecting cache…");
  QVariantList projects_;
  qulonglong budget_mib_ = 0;
  qulonglong usage_bytes_ = 0;
  qulonglong entry_count_ = 0;
  qulonglong shared_usage_bytes_ = 0;
  qulonglong shared_entry_count_ = 0;
  QString shared_status_;
  std::size_t outstanding_ = 0;
};

} // namespace oscillate::quick
