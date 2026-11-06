#include "oscillate/application/key_cache.hpp"
#include "oscillate/application/project_session.hpp"
#include "oscillate/application/source_analysis_cache_directory.hpp"
#include "oscillate/application/tempo_cache.hpp"
#include "oscillate/application/waveform_cache.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

namespace {
using namespace oscillate;
namespace fs = std::filesystem;

using oscillate::test::require;

using oscillate::test::take;

class Directory : public oscillate::test::TempDir {
public:
  Directory() : TempDir("oscillate-shared-analysis") {}
};

class EnvironmentValue {
public:
  EnvironmentValue(const char* name, const char* value) : name_(name) {
    if (const char* previous = ::getenv(name))
      previous_ = previous;
    if (value == nullptr) {
      REQUIRE_EQ(::unsetenv(name), 0, "cannot unset test environment");
    } else {
      REQUIRE_EQ(::setenv(name, value, 1), 0, "cannot set test environment");
    }
  }
  EnvironmentValue(const EnvironmentValue&) = delete;
  EnvironmentValue& operator=(const EnvironmentValue&) = delete;
  ~EnvironmentValue() {
    if (previous_) {
      (void)::setenv(name_, previous_->c_str(), 1);
    } else {
      (void)::unsetenv(name_);
    }
  }

private:
  const char* name_;
  std::optional<std::string> previous_;
};

project::SourceRecord import_source(persistence::ProjectStore& store, std::uint64_t id_number) {
  const int fd = ::open(OSCILLATE_STARTER_PLUCK, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  REQUIRE_GE(fd, 0, "starter source missing");
  auto id = take(project::SourceId::from_bits({0, id_number}));
  auto staged = store.stage_wav(id, fd);
  ::close(fd);
  return take(std::move(staged)).record();
}

void source_analysis_is_shared_but_media_is_verified() {
  Directory temporary;
  auto first_store = take(persistence::ProjectStore::open_writer(temporary.path() / "first", true));
  auto second_store =
      take(persistence::ProjectStore::open_writer(temporary.path() / "second", true));
  const auto first_source = import_source(first_store, 91);
  const auto second_source = import_source(second_store, 92);
  REQUIRE_NE(first_source.id, second_source.id,
             "projects do not contain independent IDs for identical media");
  REQUIRE_EQ(first_source.sha256, second_source.sha256,
             "projects do not contain independent IDs for identical media");

  auto first_cache = take(application::open_user_source_analysis_cache(temporary.path() / "xdg"));
  auto first_wave =
      take(application::load_waveform_summary(first_store, &first_cache, first_source, 1));
  auto first_tempo =
      take(application::load_tempo_suggestions(first_store, &first_cache, first_source, 2));
  auto first_key =
      take(application::load_key_suggestion(first_store, &first_cache, first_source, 3));
  REQUIRE_TRUE(!first_wave.cache_hit, "fresh shared cache unexpectedly contained analysis");
  REQUIRE_TRUE(!first_tempo.cache_hit, "fresh shared cache unexpectedly contained analysis");
  REQUIRE_TRUE(!first_key.cache_hit, "fresh shared cache unexpectedly contained analysis");
  REQUIRE_OK(application::enforce_source_analysis_cache_budget(first_cache),
             "small shared entries exceeded the interim source-analysis budget");

  auto second_cache = take(application::open_user_source_analysis_cache(temporary.path() / "xdg"));
  auto second_wave =
      take(application::load_waveform_summary(second_store, &second_cache, second_source, 11));
  auto second_tempo =
      take(application::load_tempo_suggestions(second_store, &second_cache, second_source, 12));
  auto second_key =
      take(application::load_key_suggestion(second_store, &second_cache, second_source, 13));
  REQUIRE_TRUE(second_wave.cache_hit, "second project recomputed identical source analysis");
  REQUIRE_TRUE(second_tempo.cache_hit, "second project recomputed identical source analysis");
  REQUIRE_TRUE(second_key.cache_hit, "second project recomputed identical source analysis");
  REQUIRE_EQ(first_wave.summary.identity.source_key, second_wave.summary.identity.source_key,
             "shared payload did not rebind generation or preserve onset suggestions");
  REQUIRE_EQ(second_wave.summary.identity.generation, 11,
             "shared payload did not rebind generation or preserve onset suggestions");
  REQUIRE_EQ(first_tempo.estimate.identity.source_key, second_tempo.estimate.identity.source_key,
             "shared payload did not rebind generation or preserve onset suggestions");
  REQUIRE_EQ(second_tempo.estimate.identity.generation, 12,
             "shared payload did not rebind generation or preserve onset suggestions");
  REQUIRE_EQ(first_tempo.estimate.suggested_onset_frames,
             second_tempo.estimate.suggested_onset_frames,
             "shared payload did not rebind generation or preserve onset suggestions");
  REQUIRE_EQ(first_key.estimate.identity.source_key, second_key.estimate.identity.source_key,
             "shared payload did not rebind generation or preserve onset suggestions");
  REQUIRE_EQ(second_key.estimate.identity.generation, 13,
             "shared payload did not rebind generation or preserve onset suggestions");

  const auto cache_leaf = temporary.path() / "xdg/oscillate/source-analysis/v1";
  const auto tempo_key = take(application::tempo_cache_key(first_source)).hex();
  {
    std::ofstream corrupt(cache_leaf / (tempo_key + ".cache"), std::ios::binary | std::ios::trunc);
    corrupt << "damaged";
  }
  auto rebuilt =
      take(application::load_tempo_suggestions(second_store, &second_cache, second_source, 14));
  require(!rebuilt.cache_hit, "corrupt shared tempo payload was not rebuilt");
  require(take(application::load_tempo_suggestions(first_store, &first_cache, first_source, 15))
              .cache_hit,
          "rebuilt shared tempo payload was not reusable");
  REQUIRE_OK(second_cache.clear(), "cannot clear shared cache");
  require(!take(application::load_key_suggestion(first_store, &first_cache, first_source, 16))
               .cache_hit,
          "missing shared entry was not rebuilt");

  std::stop_source cancelled;
  cancelled.request_stop();
  require(!application::load_waveform_summary(first_store, &first_cache, first_source, 17, {},
                                              cancelled.get_token()),
          "cancelled source verification returned a cache hit");
  int cancellation_checks = 0;
  auto superseded = application::load_waveform_summary(first_store, &first_cache, first_source, 18,
                                                       [&] { return ++cancellation_checks >= 2; });
  REQUIRE_TRUE(!superseded, "superseded generation returned a shared cache hit");
  REQUIRE_GE(cancellation_checks, 2, "superseded generation returned a shared cache hit");
  const auto asset = fs::directory_iterator(temporary.path() / "second/assets")->path();
  fs::remove(asset);
  REQUIRE_TRUE(!application::load_waveform_summary(second_store, &second_cache, second_source, 19),
               "missing source was masked by a shared cache hit");
  REQUIRE_TRUE(!application::load_tempo_suggestions(second_store, &second_cache, second_source, 20),
               "missing source was masked by a shared cache hit");
  REQUIRE_TRUE(!application::load_key_suggestion(second_store, &second_cache, second_source, 21),
               "missing source was masked by a shared cache hit");
}

void linked_source_must_match_saved_bytes_before_shared_hit() {
  Directory temporary;
  auto seed_store = take(persistence::ProjectStore::open_writer(temporary.path() / "seed", true));
  const auto managed = import_source(seed_store, 93);
  auto cache = take(application::open_user_source_analysis_cache(temporary.path() / "xdg"));
  require(!take(application::load_waveform_summary(seed_store, &cache, managed, 1)).cache_hit,
          "seed waveform should populate shared cache");

  const auto external_path = temporary.path() / "linked.wav";
  fs::copy_file(OSCILLATE_STARTER_PLUCK, external_path);
  auto linked = managed;
  linked.id = take(project::SourceId::from_bits({0, 94}));
  linked.external_link = project::ExternalSourceLink{external_path.string()};
  const auto linked_root = temporary.path() / "linked-project";
  auto session = take(application::ProjectSession::create(linked_root));
  auto snapshot = session->snapshot();
  session.reset();
  snapshot.revision += 1;
  snapshot.sources.push_back(linked);
  auto linked_store = take(persistence::ProjectStore::open_writer(linked_root, false));
  REQUIRE_OK(linked_store.save(snapshot, persistence::SaveKind::explicit_save),
             "linked source fixture did not save");
  require(take(application::load_waveform_summary(linked_store, &cache, linked, 2)).cache_hit,
          "verified external source did not reuse shared waveform");

  {
    std::fstream damage(external_path, std::ios::in | std::ios::out | std::ios::binary);
    damage.seekg(100);
    const auto original = damage.get();
    REQUIRE_NE(original, std::char_traits<char>::eof(), "linked fixture is unexpectedly short");
    damage.seekp(100);
    damage.put(static_cast<char>(original ^ 0x7f));
  }
  require(!application::load_waveform_summary(linked_store, &cache, linked, 3),
          "same-size damaged external source was hidden by a shared cache hit");
  fs::remove(external_path);
  require(!application::load_waveform_summary(linked_store, &cache, linked, 4),
          "missing external source was hidden by a shared cache hit");
}

void cache_directory_requires_private_nofollow_path() {
  Directory temporary;
  const auto home = temporary.path() / "xdg";
  auto cache = take(application::open_user_source_analysis_cache(home));
  REQUIRE_OK(cache.usage(), "private shared cache did not open");
  require(!application::open_user_source_analysis_cache(fs::path("relative-cache")),
          "relative XDG cache home was accepted");
  const auto link = temporary.path() / "cache-link";
  fs::create_directory_symlink(home, link);
  require(!application::open_user_source_analysis_cache(link),
          "symlinked XDG cache home was followed");
  const auto app = home / "oscillate";
  REQUIRE_EQ(::chmod(app.c_str(), 0755), 0, "cannot change fixture permissions");
  REQUIRE_OK(application::open_user_source_analysis_cache(home),
             "safe application cache parent was rejected");
  REQUIRE_EQ(::chmod(home.c_str(), 0775), 0, "cannot set protected fixture parent permissions");
  REQUIRE_EQ(::chmod(app.c_str(), 0775), 0, "cannot set protected fixture parent permissions");
  REQUIRE_OK(application::open_user_source_analysis_cache(home),
             "group-writable cache parents beneath a private ancestor were rejected");
  REQUIRE_EQ(::chmod(home.c_str(), 0700), 0, "cannot restore fixture parent permissions");
  REQUIRE_EQ(::chmod(app.c_str(), 0755), 0, "cannot restore fixture parent permissions");
  const auto analysis = app / "source-analysis";
  REQUIRE_EQ(::chmod(analysis.c_str(), 0755), 0, "cannot change fixture permissions");
  require(!application::open_user_source_analysis_cache(home),
          "nonprivate analysis-cache directory was accepted");
  REQUIRE_EQ(::chmod(analysis.c_str(), 0700), 0, "cannot restore fixture permissions");
  REQUIRE_EQ(::chmod(app.c_str(), 0777), 0, "cannot change fixture permissions");
  require(!application::open_user_source_analysis_cache(home),
          "world-writable application cache parent was accepted");
  REQUIRE_EQ(::chmod(app.c_str(), 0700), 0, "cannot change fixture cache-home permissions");
  REQUIRE_EQ(::chmod(home.c_str(), 0777), 0, "cannot change fixture cache-home permissions");
  require(!application::open_user_source_analysis_cache(home),
          "world-writable XDG cache home was accepted");
}

void cache_home_uses_xdg_then_home_fallback() {
  Directory temporary;
  const auto xdg = (temporary.path() / "explicit-cache").string();
  const auto home = (temporary.path() / "home").string();
  fs::create_directory(home);
  REQUIRE_EQ(::chmod(home.c_str(), 0700), 0, "cannot secure test HOME directory");
  EnvironmentValue scoped_home("HOME", home.c_str());
  {
    EnvironmentValue scoped_xdg("XDG_CACHE_HOME", xdg.c_str());
    REQUIRE_OK(application::open_user_source_analysis_cache(),
               "absolute XDG cache home was ignored");
    REQUIRE_TRUE(fs::is_directory(fs::path(xdg) / "oscillate/source-analysis/v1"),
                 "absolute XDG cache home was ignored");
  }
  {
    EnvironmentValue scoped_xdg("XDG_CACHE_HOME", "relative-cache");
    auto fallback = application::open_user_source_analysis_cache();
    if (!fallback)
      throw std::runtime_error(fallback.error().message);
    require(fs::is_directory(fs::path(home) / ".cache/oscillate/source-analysis/v1"),
            "relative XDG cache home did not fall back to HOME");
  }
  {
    EnvironmentValue scoped_xdg("XDG_CACHE_HOME", nullptr);
    EnvironmentValue missing_home("HOME", nullptr);
    require(!application::open_user_source_analysis_cache(),
            "missing user cache home was accepted");
  }
}
} // namespace

int main() {
  try {
    source_analysis_is_shared_but_media_is_verified();
    linked_source_must_match_saved_bytes_before_shared_hit();
    cache_directory_requires_private_nofollow_path();
    cache_home_uses_xdg_then_home_fallback();
    std::cout << "Shared source analysis cache contract passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
