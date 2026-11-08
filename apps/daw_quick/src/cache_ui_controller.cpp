#include "cache_ui_controller.hpp"

#include "oscillate/application/source_analysis_cache_directory.hpp"

#include <QString>
#include <QVariantMap>

#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>

namespace oscillate::quick {
namespace {
constexpr std::uint64_t mib_bytes = 1024ULL * 1024;
constexpr int mib_decimals = 1;

QString project_status(application::CacheProjectStatus status) {
  switch (status) {
  case application::CacheProjectStatus::reachable:
    return QStringLiteral("Ready");
  case application::CacheProjectStatus::unreachable:
    return QStringLiteral("Project unavailable");
  case application::CacheProjectStatus::moved:
    return QStringLiteral("Project moved or replaced");
  case application::CacheProjectStatus::cache_unavailable:
    return QStringLiteral("Cache unavailable");
  case application::CacheProjectStatus::failed:
    return QStringLiteral("Inspection failed");
  }
  return QStringLiteral("Unknown");
}
} // namespace

class CacheUiWorker {
public:
  enum class Action : std::uint8_t {
    refresh,
    register_project,
    set_budget,
    clear_project,
    clear_all,
    forget_project
  };
  struct Request {
    Action action;
    std::filesystem::path root;
    std::uint64_t budget_bytes = 0;
  };
  struct Outcome {
    std::optional<application::CacheHealth> health;
    std::optional<media::CacheUsage> shared_usage;
    QString shared_status;
    QString status;
    bool success = false;
  };

  explicit CacheUiWorker(std::filesystem::path profile) {
    thread_ = std::jthread([shared = shared_,
                            profile = std::move(profile)](const std::stop_token& stop) {
      auto opened = application::CacheControl::open(profile);
      std::unique_ptr<application::CacheControl> control;
      QString open_error;
      if (opened) {
        control = std::move(opened).value();
      } else {
        open_error = QString::fromStdString(opened.error().message);
      }
      auto opened_shared = application::open_user_source_analysis_cache();
      std::optional<media::CacheStore> shared_cache;
      QString shared_open_error;
      if (opened_shared) {
        shared_cache.emplace(std::move(opened_shared).value());
      } else {
        shared_open_error = QString::fromStdString(opened_shared.error().message);
      }
      while (!stop.stop_requested()) {
        Request request;
        {
          std::unique_lock lock(shared->mutex);
          if (!shared->changed.wait(lock, stop, [&] { return !shared->pending.empty(); })) {
            break;
          }
          request = std::move(shared->pending.front());
          shared->pending.pop_front();
        }
        Outcome outcome;
        if (!control) {
          outcome.status = QStringLiteral("Cache controls unavailable: %1").arg(open_error);
        } else {
          try {
            foundation::Result<void> operation = foundation::Result<void>::success();
            std::uint64_t freed_bytes = 0;
            switch (request.action) {
            case Action::refresh:
              break;
            case Action::register_project:
              operation = control->register_project(request.root);
              break;
            case Action::set_budget:
              if (!shared_cache) {
                operation = foundation::Result<void>::failure(
                    {foundation::ErrorCode::unavailable,
                     "shared analysis cache unavailable; global budget cannot be applied"});
              } else {
                operation = control->set_budget(request.budget_bytes);
                if (operation) {
                  auto enforced = control->enforce_budget(&*shared_cache);
                  if (!enforced) {
                    operation = foundation::Result<void>::failure(enforced.error());
                  }
                }
              }
              break;
            case Action::clear_project:
              operation = control->clear_project(request.root);
              break;
            case Action::forget_project: {
              auto forgotten = control->forget_project_cache(request.root);
              if (forgotten) {
                freed_bytes = forgotten.value();
              } else {
                operation = foundation::Result<void>::failure(forgotten.error());
              }
              break;
            }
            case Action::clear_all: {
              auto cleared = control->clear_reachable();
              if (!cleared) {
                operation = foundation::Result<void>::failure(cleared.error());
              }
              break;
            }
            }
            if (request.action == Action::clear_all && shared_cache) {
              auto cleared = shared_cache->clear();
              if (!cleared && operation)
                operation = foundation::Result<void>::failure(cleared.error());
            }
            auto health = control->health(shared_cache ? &*shared_cache : nullptr);
            if (health) {
              outcome.health.emplace(std::move(health).value());
            } else if (operation) {
              operation = foundation::Result<void>::failure(health.error());
            }
            outcome.success = static_cast<bool>(operation);
            if (!operation) {
              outcome.status = QStringLiteral("Cache maintenance failed: %1")
                                   .arg(QString::fromStdString(operation.error().message));
            } else {
              switch (request.action) {
              case Action::refresh:
              case Action::register_project:
                outcome.status = QStringLiteral("Cache status is current");
                break;
              case Action::set_budget:
                outcome.status = QStringLiteral("Cache budget saved and applied");
                break;
              case Action::clear_project:
                outcome.status = QStringLiteral("Current project cache cleared");
                break;
              case Action::clear_all:
                outcome.status =
                    QStringLiteral("Reachable project and shared analysis caches cleared");
                break;
              case Action::forget_project:
                outcome.status =
                    QStringLiteral("Forgot %1 · freed %2 MiB")
                        .arg(QString::fromStdString(request.root.filename().string()))
                        .arg(static_cast<double>(freed_bytes) / static_cast<double>(mib_bytes), 0,
                             'f', mib_decimals);
                break;
              }
            }
          } catch (const std::exception& error) {
            outcome.status =
                QStringLiteral("Cache maintenance failed: %1").arg(QString::fromUtf8(error.what()));
          } catch (...) {
            outcome.status = QStringLiteral("Cache maintenance failed unexpectedly");
          }
        }
        if (shared_cache) {
          auto usage = shared_cache->usage();
          if (usage) {
            outcome.shared_usage.emplace(usage.value());
            outcome.shared_status = QStringLiteral("Shared analysis cache ready");
          } else {
            outcome.shared_status = QStringLiteral("Shared analysis cache unavailable: %1")
                                        .arg(QString::fromStdString(usage.error().message));
          }
        } else {
          outcome.shared_status =
              QStringLiteral("Shared analysis cache unavailable: %1").arg(shared_open_error);
        }
        if (request.action == Action::clear_all && !shared_cache && outcome.success)
          outcome.status =
              QStringLiteral("Project caches cleared; shared analysis cache unavailable");
        std::lock_guard lock(shared->mutex);
        shared->ready.push_back(std::move(outcome));
      }
    });
  }

  ~CacheUiWorker() {
    thread_.request_stop();
    shared_->changed.notify_all();
    thread_.detach();
  }

  void submit(Request request) {
    {
      std::lock_guard lock(shared_->mutex);
      shared_->pending.push_back(std::move(request));
    }
    shared_->changed.notify_one();
  }

  [[nodiscard]] std::deque<Outcome> collect() {
    std::lock_guard lock(shared_->mutex);
    std::deque<Outcome> ready;
    ready.swap(shared_->ready);
    return ready;
  }

private:
  struct Shared {
    std::mutex mutex;
    std::condition_variable_any changed;
    std::deque<Request> pending;
    std::deque<Outcome> ready;
  };
  std::shared_ptr<Shared> shared_ = std::make_shared<Shared>();
  std::jthread thread_;
};

CacheUiController::CacheUiController(std::filesystem::path profile, QObject* parent)
    : QObject(parent), worker_(std::make_unique<CacheUiWorker>(std::move(profile))), timer_(this) {
  timer_.setInterval(50);
  connect(&timer_, &QTimer::timeout, this, &CacheUiController::poll);
  queue_refresh();
}

CacheUiController::~CacheUiController() = default;

void CacheUiController::queue_refresh() {
  worker_->submit({CacheUiWorker::Action::refresh, {}, 0});
  ++outstanding_;
  timer_.start();
  emit changed();
}

void CacheUiController::register_project(const std::filesystem::path& root) {
  current_root_ = root.lexically_normal();
  worker_->submit({CacheUiWorker::Action::register_project, current_root_, 0});
  ++outstanding_;
  timer_.start();
  emit changed();
}

void CacheUiController::refresh() {
  if (!busy()) {
    queue_refresh();
  }
}

void CacheUiController::set_budget_mib(int mib) {
  if (busy() || mib < 64 || mib > 1024 * 1024) {
    status_ = QStringLiteral("Choose a cache budget from 64 to 1048576 MiB");
    emit changed();
    return;
  }
  worker_->submit(
      {CacheUiWorker::Action::set_budget, {}, static_cast<std::uint64_t>(mib) * mib_bytes});
  ++outstanding_;
  status_ = QStringLiteral("Applying cache budget…");
  timer_.start();
  emit changed();
}

void CacheUiController::clear_current() {
  if (busy() || current_root_.empty()) {
    return;
  }
  worker_->submit({CacheUiWorker::Action::clear_project, current_root_, 0});
  ++outstanding_;
  status_ = QStringLiteral("Clearing reconstructable project cache…");
  timer_.start();
  emit changed();
}

void CacheUiController::forget_project(const QString& path) {
  const auto root = std::filesystem::path(path.toStdString()).lexically_normal();
  // The open project registers itself again; Clear project covers it.
  if (busy() || root.empty() || root == current_root_) {
    return;
  }
  worker_->submit({CacheUiWorker::Action::forget_project, root, 0});
  ++outstanding_;
  status_ = QStringLiteral("Clearing and forgetting %1…")
                .arg(QString::fromStdString(root.filename().string()));
  timer_.start();
  emit changed();
}

void CacheUiController::clear_all() {
  if (busy()) {
    return;
  }
  worker_->submit({CacheUiWorker::Action::clear_all, {}, 0});
  ++outstanding_;
  status_ = QStringLiteral("Clearing reachable reconstructable caches…");
  timer_.start();
  emit changed();
}

void CacheUiController::poll() {
  const auto ready = worker_->collect();
  for (const auto& outcome : ready) {
    if (outstanding_ > 0) {
      --outstanding_;
    }
    status_ = outcome.status;
    shared_status_ = outcome.shared_status;
    if (outcome.shared_usage) {
      shared_usage_bytes_ = outcome.shared_usage->bytes;
      shared_entry_count_ = outcome.shared_usage->entries;
    }
    if (outcome.health) {
      budget_mib_ = outcome.health->budget_bytes / mib_bytes;
      usage_bytes_ = outcome.health->global.bytes;
      entry_count_ = outcome.health->global.entries;
      QVariantList projects;
      for (const auto& project : outcome.health->projects) {
        projects.append(QVariantMap{
            {QStringLiteral("name"), QString::fromStdString(project.root.filename().string())},
            {QStringLiteral("path"), QString::fromStdString(project.root.string())},
            {QStringLiteral("current"), project.root == current_root_},
            {QStringLiteral("status"), project_status(project.status)},
            {QStringLiteral("detail"), QString::fromStdString(project.detail)},
            {QStringLiteral("bytes"), QVariant::fromValue<qulonglong>(project.usage.bytes)},
            {QStringLiteral("entries"), QVariant::fromValue<qulonglong>(project.usage.entries)},
        });
      }
      projects_ = std::move(projects);
    }
  }
  if (!busy()) {
    timer_.stop();
  }
  if (!ready.empty()) {
    emit changed();
  }
}

} // namespace oscillate::quick
