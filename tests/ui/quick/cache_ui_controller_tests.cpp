#include "../../../apps/daw_quick/src/cache_ui_controller.hpp"

#include "oscillate/application/project_session.hpp"
#include "oscillate/application/source_analysis_cache_directory.hpp"
#include "oscillate/media/cache_identity.hpp"
#include "oscillate/media/cache_store.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/wait.hpp"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <QVariant>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace {
namespace app = oscillate::application;
namespace media = oscillate::media;
namespace project = oscillate::project;
namespace quick = oscillate::quick;
namespace persistence = oscillate::persistence;
namespace foundation = oscillate::foundation;

using oscillate::test::require;

template <typename T> T take(foundation::Result<T> result) {
  if (!result) {
    throw std::runtime_error(result.error().message);
  }
  return std::move(result).value();
}

void await_idle(quick::CacheUiController& controller) {
  oscillate::test::wait_until(
      [&] {
        QCoreApplication::processEvents();
        return !controller.busy();
      },
      "the cache worker completed");
  require(!controller.busy(), "cache worker did not complete");
}

constexpr std::size_t project_entry_bytes = 128;

// A cache key for one fixture source; the bytes are what the entry holds.
media::CacheKey fixture_key() {
  const auto id = take(project::SourceId::from_bits({0, 1}));
  // A named source record: GCC misreads the optional link of an inline
  // temporary as uninitialised when the descriptor copies it.
  project::SourceRecord source{.id = id,
                               .encoding = project::SourceEncoding::wav,
                               .sha256 = std::string(64, 'a'),
                               .byte_length = 60,
                               .sample_rate = 48000,
                               .channels = 2,
                               .frames = 8,
                               .external_link = std::nullopt};
  const media::CacheDescriptor descriptor{media::CacheArtifact::source_analysis,
                                          std::move(source),
                                          {},
                                          {},
                                          "quick-cache-test",
                                          1,
                                          1,
                                          {std::byte{0x01}},
                                          {}};
  return take(media::make_cache_key(descriptor));
}

void put_project_entry(const std::filesystem::path& root) {
  auto store = take(persistence::ProjectStore::open_reader(root));
  auto cache = take(media::CacheStore::adopt_directory_fd(take(store.open_cache_directory())));
  require(cache.put(fixture_key(), std::vector<std::byte>(project_entry_bytes, std::byte{0x7a}))
              .has_value(),
          "cache entry could not be written");
}

// Forget clears another registered project's cache, drops it from the list
// and says how much it freed; the open project cannot be forgotten.
void forgetting_a_project_frees_its_cache() {
  QTemporaryDir temporary;
  require(temporary.isValid(), "temporary directory unavailable");
  const auto profile = std::filesystem::path(temporary.path().toStdString());
  const auto cache_home = profile / "xdg-cache";
  require(::setenv("XDG_CACHE_HOME", cache_home.c_str(), 1) == 0,
          "temporary shared cache home could not be selected");
  const auto open = profile / "open";
  const auto other = profile / "other";
  take(app::ProjectSession::create(open)).reset();
  take(app::ProjectSession::create(other)).reset();
  put_project_entry(other);
  {
    auto control = take(app::CacheControl::open(profile));
    require(control->register_project(other).has_value(), "fixture project was not registered");
  }

  quick::CacheUiController controller(profile);
  controller.register_project(open);
  await_idle(controller);
  const auto listed = controller.projects();
  require(listed.size() == 2, "both registered projects are listed");
  const auto listed_other = std::ranges::find_if(listed, [](const QVariant& row) {
    return !row.toMap().value(QStringLiteral("current")).toBool();
  });
  require(listed_other != listed.end() &&
              listed_other->toMap().value(QStringLiteral("path")).toString() ==
                  QString::fromStdString(other.string()),
          "a listed project carries its path");

  controller.forget_project(QString::fromStdString(open.string()));
  require(!controller.busy() && controller.projects().size() == 2,
          "the open project was forgotten");
  controller.forget_project(QString::fromStdString(other.string()));
  await_idle(controller);
  require(controller.projects().size() == 1 &&
              controller.projects().front().toMap().value(QStringLiteral("current")).toBool(),
          "the forgotten project is still listed");
  require(controller.status().startsWith(QStringLiteral("Forgot other · freed ")) &&
              controller.status().endsWith(QStringLiteral(" MiB")),
          "the status does not report the space freed: " + controller.status().toStdString());
  require(!std::filesystem::exists(other / "cache" / (fixture_key().hex() + ".cache")),
          "the forgotten project's cache remains");
  require(std::filesystem::exists(other / "state" / "CURRENT"),
          "forgetting touched the project itself");
}

void cache_controls_preserve_project_assets() {
  QTemporaryDir temporary;
  require(temporary.isValid(), "temporary directory unavailable");
  const auto profile = std::filesystem::path(temporary.path().toStdString());
  const auto cache_home = profile / "xdg-cache";
  require(::setenv("XDG_CACHE_HOME", cache_home.c_str(), 1) == 0,
          "temporary shared cache home could not be selected");
  const auto root = profile / "session";
  auto session = take(app::ProjectSession::create(root));
  session.reset();

  auto store = take(persistence::ProjectStore::open_reader(root));
  auto cache = take(media::CacheStore::adopt_directory_fd(take(store.open_cache_directory())));
  const auto key = fixture_key();
  require(cache.put(key, std::vector<std::byte>(project_entry_bytes, std::byte{0x7a})).has_value(),
          "cache entry could not be written");
  auto shared_cache = take(app::open_user_source_analysis_cache(cache_home));
  require(shared_cache.put(key, std::vector<std::byte>(96, std::byte{0x5a})).has_value(),
          "shared analysis entry could not be written");

  quick::CacheUiController controller(profile);
  controller.register_project(root);
  await_idle(controller);
  require(controller.entry_count() == 2 && controller.usage_bytes() >= 224,
          "global usage did not count project and shared analysis caches");
  require(controller.shared_entry_count() == 1 && controller.shared_usage_bytes() >= 96,
          "shared analysis cache was not counted");
  require(controller.projects().size() == 1 &&
              controller.projects().front().toMap().value(QStringLiteral("current")).toBool(),
          "current project status was not projected");

  controller.set_budget_mib(63);
  require(!controller.busy() && controller.status().contains(QStringLiteral("64")),
          "invalid cache budget was not rejected synchronously");
  controller.set_budget_mib(64);
  await_idle(controller);
  require(controller.budget_mib() == 64, "global budget was not persisted");
  controller.clear_current();
  await_idle(controller);
  require(controller.entry_count() == 1 && std::filesystem::exists(root / "state" / "CURRENT"),
          "clearing derived cache damaged project state");
  require(controller.shared_entry_count() == 1,
          "clearing one project also removed the shared analysis cache");

  require(cache.put(key, std::vector<std::byte>(64, std::byte{0x7a})).has_value(),
          "cache entry could not be regenerated");
  controller.clear_all();
  await_idle(controller);
  require(controller.entry_count() == 0 && controller.shared_entry_count() == 0,
          "clear all did not clear project and shared analysis caches");
}
} // namespace

int main(int argc, char** argv) {
  QCoreApplication application(argc, argv);
  try {
    cache_controls_preserve_project_assets();
    forgetting_a_project_frees_its_cache();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
