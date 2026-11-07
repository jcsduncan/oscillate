#include "vst3_browser_controller.hpp"
#include "saved_processing.hpp"
#include "vst3_helper_path.hpp"

#include <QFileInfo>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace oscillate::quick {
namespace {
// How often the UI thread looks for a finished scan or verification.
constexpr int poll_interval_ms = 50;
// Bounds on what a user may add and what one scan may return.
constexpr qsizetype max_search_path_chars = 4096;
constexpr std::size_t max_explicit_paths = 16;
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
constexpr plugins::Vst3CatalogueLimits catalogue_limits{
    .max_bundles = 128, .max_entries = 20000, .max_search_paths = 32};
#endif
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
[[nodiscard]] QString scan_state(plugins::ScanStatus status) {
  switch (status) {
  case plugins::ScanStatus::ok:
    return QStringLiteral("ready");
  case plugins::ScanStatus::invalid_bundle:
    return QStringLiteral("invalid bundle");
  case plugins::ScanStatus::invalid_metadata:
    return QStringLiteral("invalid metadata");
  case plugins::ScanStatus::scanner_error:
    return QStringLiteral("scan error");
  case plugins::ScanStatus::crashed:
    return QStringLiteral("scanner crashed");
  case plugins::ScanStatus::timed_out:
    return QStringLiteral("scan timed out");
  case plugins::ScanStatus::oversized_response:
    return QStringLiteral("response too large");
  case plugins::ScanStatus::quarantined:
    return QStringLiteral("quarantined");
  }
  return QStringLiteral("unknown");
}

[[nodiscard]] bool is_effect_class(const plugins::Vst3Class& klass) {
  if (klass.category != "Audio Module Class")
    return false;
  std::string_view remaining = klass.subcategories;
  while (!remaining.empty()) {
    const auto separator = remaining.find('|');
    if (remaining.substr(0, separator) == "Fx")
      return true;
    if (separator == std::string_view::npos)
      break;
    remaining.remove_prefix(separator + 1);
  }
  return false;
}

[[nodiscard]] bool is_instrument_class(const plugins::Vst3Class& klass) {
  if (klass.category != "Audio Module Class")
    return false;
  std::string_view remaining = klass.subcategories;
  while (!remaining.empty()) {
    const auto separator = remaining.find('|');
    if (remaining.substr(0, separator) == "Instrument")
      return true;
    if (separator == std::string_view::npos)
      break;
    remaining.remove_prefix(separator + 1);
  }
  return false;
}

#endif
} // namespace

Vst3BrowserController::Vst3BrowserController(Host host, QObject* parent)
    : QObject(parent), host_(std::move(host)), timer_(this) {
  timer_.setInterval(poll_interval_ms);
  connect(&timer_, &QTimer::timeout, this, &Vst3BrowserController::poll);
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  status_ = QStringLiteral("Select an audio track, then scan for effects");
#else
  status_ = QStringLiteral("VST3 browser unavailable in this build");
#endif
}

Vst3BrowserController::~Vst3BrowserController() = default;

bool Vst3BrowserController::vst3_browser_available() {
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  return true;
#else
  return false;
#endif
}

bool Vst3BrowserController::busy() const {
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  return completion_ != nullptr;
#else
  return false;
#endif
}

void Vst3BrowserController::refresh_vst3_catalogue() {
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  start_refresh(std::nullopt);
#else
  status_ = QStringLiteral("VST3 browser unavailable in this build");
  emit changed();
#endif
}

void Vst3BrowserController::add_vst3_search_path(const QString& path) {
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  const auto trimmed = path.trimmed();
  if (trimmed.isEmpty() || trimmed.size() > max_search_path_chars ||
      !QFileInfo(trimmed).isAbsolute()) {
    status_ = QStringLiteral("Enter an absolute VST3 bundle or folder path");
    emit changed();
    return;
  }
  if (busy()) {
    status_ = QStringLiteral("Wait for the current VST3 scan");
    emit changed();
    return;
  }
  const auto requested = std::filesystem::path(trimmed.toStdString());
  if (std::ranges::find(explicit_paths_, requested) == explicit_paths_.end()) {
    if (explicit_paths_.size() == max_explicit_paths) {
      status_ = QStringLiteral("VST3 browser supports %1 additional paths").arg(max_explicit_paths);
      emit changed();
      return;
    }
    explicit_paths_.push_back(requested);
  }
  start_refresh(std::nullopt);
#else
  (void)path;
  refresh_vst3_catalogue();
#endif
}

void Vst3BrowserController::retry_vst3_bundle(const QString& bundle) {
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  const auto selected = std::ranges::find_if(catalogue_.entries, [&bundle](const auto& entry) {
    return QString::fromStdString(entry.canonical_bundle.string()) == bundle;
  });
  if (selected == catalogue_.entries.end()) {
    status_ = QStringLiteral("Select a discovered VST3 bundle to retry");
    emit changed();
    return;
  }
  start_refresh(selected->canonical_bundle);
#else
  (void)bundle;
  refresh_vst3_catalogue();
#endif
}

#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
void Vst3BrowserController::start_refresh(std::optional<std::filesystem::path> retry) {
  if (busy()) {
    status_ = QStringLiteral("Wait for the current VST3 task");
    emit changed();
    return;
  }
  if (!scanner_)
    scanner_ = std::make_unique<plugins::Vst3Scanner>(detail::vst3_helper_path());
  auto completion = std::make_shared<Completion>();
  auto* scanner = scanner_.get();
  auto explicit_paths = explicit_paths_;
  const bool retrying = retry.has_value();
  try {
    worker_ = std::jthread([completion, scanner, explicit_paths = std::move(explicit_paths),
                            retry = std::move(retry)](const std::stop_token& stop) {
      try {
        plugins::Vst3Catalogue catalogue(
            [scanner, &stop](const auto& bundle) {
              if (stop.stop_requested()) {
                plugins::ScanResult cancelled;
                cancelled.status = plugins::ScanStatus::scanner_error;
                cancelled.diagnostic = "Scan cancelled";
                return cancelled;
              }
              return scanner->scan(bundle);
            },
            [scanner](const auto& bundle) { scanner->retry(bundle); },
            plugins::standard_linux_vst3_roots(), catalogue_limits);
        if (retry)
          scanner->retry(*retry);
        completion->catalogue.emplace(catalogue.refresh(explicit_paths));
      } catch (const std::exception& error) {
        completion->failure = QString::fromUtf8(error.what());
      } catch (...) {
        completion->failure = QStringLiteral("VST3 scan failed unexpectedly");
      }
      completion->done.store(true, std::memory_order_release);
    });
  } catch (const std::exception& error) {
    status_ = QStringLiteral("Could not start VST3 scan: %1").arg(error.what());
    emit changed();
    return;
  }
  completion_ = std::move(completion);
  status_ = retrying ? QStringLiteral("Retrying VST3 bundle in scanner helper")
                     : QStringLiteral("Scanning VST3 bundles in helper process");
  emit changed();
  timer_.start();
}
#endif

// QML invokable signature is positional; class and track IDs remain distinct at the call site.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void Vst3BrowserController::insert_vst3_effect(int entry_index, const QString& class_id,
                                               const QString& track_id) {
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  if (!session() || busy() || host_.jobs_busy()) {
    status_ = QStringLiteral("Open a project and finish active tasks before insertion");
    emit changed();
    return;
  }
  const auto parsed_track = project::TrackId::parse(track_id.toStdString());
  const auto& snapshot = session()->snapshot();
  const auto selected_track =
      parsed_track ? std::ranges::find(snapshot.tracks, parsed_track.value(), &project::Track::id)
                   : snapshot.tracks.end();
  // Any track's chain takes a VST3 effect (ADR-0050).
  if (selected_track == snapshot.tracks.end()) {
    status_ = QStringLiteral("Select a track for the effect");
    emit changed();
    return;
  }
  if (entry_index < 0 || static_cast<std::size_t>(entry_index) >= catalogue_.entries.size()) {
    status_ = QStringLiteral("Choose a scanned VST3 effect");
    emit changed();
    return;
  }
  const auto& entry = catalogue_.entries[static_cast<std::size_t>(entry_index)];
  if (!entry.scan.ok() ||
      std::ranges::none_of(entry.scan.metadata.classes, [&class_id](const auto& klass) {
        return QString::fromStdString(klass.id) == class_id && is_effect_class(klass);
      })) {
    status_ = QStringLiteral("Selected effect is unavailable; retry its bundle");
    emit changed();
    return;
  }
  auto completion = std::make_shared<Completion>();
  completion->action = Completion::Action::verify_effect;
  completion->project_generation = host_.project_generation();
  completion->revision = snapshot.revision;
  completion->track = parsed_track.value();
  auto* scanner = scanner_.get();
  auto chosen = entry;
  const auto chosen_class = class_id.toStdString();
  try {
    worker_ = std::jthread(
        [completion, scanner, chosen = std::move(chosen), chosen_class](const std::stop_token&) {
          try {
            completion->verified.emplace(
                plugins::verify_vst3_effect_selection(chosen, chosen_class, *scanner));
          } catch (const std::exception& error) {
            completion->failure = QString::fromUtf8(error.what());
          } catch (...) {
            completion->failure = QStringLiteral("VST3 verification failed unexpectedly");
          }
          completion->done.store(true, std::memory_order_release);
        });
  } catch (const std::exception& error) {
    status_ = QStringLiteral("Could not start VST3 verification: %1").arg(error.what());
    emit changed();
    return;
  }
  completion_ = std::move(completion);
  status_ = QStringLiteral("Verifying selected effect outside the UI process");
  emit changed();
  timer_.start();
#else
  (void)entry_index;
  (void)class_id;
  (void)track_id;
  refresh_vst3_catalogue();
#endif
}

// QML invokable signature is positional; class and track IDs remain distinct at the call site.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void Vst3BrowserController::insert_vst3_instrument(int entry_index, const QString& class_id,
                                                   const QString& track_id) {
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  if (!session() || busy() || host_.jobs_busy()) {
    status_ = QStringLiteral("Open a project and finish active tasks before insertion");
    emit changed();
    return;
  }
  const bool creating_track = track_id.isEmpty();
  std::optional<project::TrackId> parsed_track;
  if (!creating_track) {
    auto parsed = project::TrackId::parse(track_id.toStdString());
    if (parsed)
      parsed_track = parsed.value();
  }
  const auto& snapshot = session()->snapshot();
  const auto selected_track =
      parsed_track ? std::ranges::find(snapshot.tracks, *parsed_track, &project::Track::id)
                   : snapshot.tracks.end();
  // An instrument starts its track's chain; any other tracks play beside it
  // (ADR-0050).
  if (!creating_track &&
      (selected_track == snapshot.tracks.end() ||
       selected_track->kind != project::TrackKind::instrument ||
       !std::holds_alternative<std::monostate>(selected_track->instrument_source))) {
    status_ =
        QStringLiteral("Choose an instrument track without a source, or drop into the arrangement");
    emit changed();
    return;
  }
  if (entry_index < 0 || static_cast<std::size_t>(entry_index) >= catalogue_.entries.size()) {
    status_ = QStringLiteral("Choose a scanned VST3 instrument");
    emit changed();
    return;
  }
  const auto& entry = catalogue_.entries[static_cast<std::size_t>(entry_index)];
  if (!entry.scan.ok() ||
      std::ranges::none_of(entry.scan.metadata.classes, [&class_id](const auto& klass) {
        return QString::fromStdString(klass.id) == class_id && is_instrument_class(klass);
      })) {
    status_ = QStringLiteral("Selected instrument is unavailable; retry its bundle");
    emit changed();
    return;
  }
  auto completion = std::make_shared<Completion>();
  completion->action = creating_track ? Completion::Action::verify_new_instrument
                                      : Completion::Action::verify_instrument;
  completion->project_generation = host_.project_generation();
  completion->revision = snapshot.revision;
  if (parsed_track)
    completion->track = *parsed_track;
  auto* scanner = scanner_.get();
  auto chosen = entry;
  const auto chosen_class = class_id.toStdString();
  try {
    worker_ = std::jthread(
        [completion, scanner, chosen = std::move(chosen), chosen_class](const std::stop_token&) {
          try {
            completion->verified_instrument.emplace(
                plugins::verify_vst3_instrument_selection(chosen, chosen_class, *scanner));
          } catch (const std::exception& error) {
            completion->failure = QString::fromUtf8(error.what());
          } catch (...) {
            completion->failure = QStringLiteral("VST3 verification failed unexpectedly");
          }
          completion->done.store(true, std::memory_order_release);
        });
  } catch (const std::exception& error) {
    status_ = QStringLiteral("Could not start VST3 verification: %1").arg(error.what());
    emit changed();
    return;
  }
  completion_ = std::move(completion);
  status_ = QStringLiteral("Verifying selected instrument outside the UI process");
  emit changed();
  timer_.start();
#else
  (void)entry_index;
  (void)class_id;
  (void)track_id;
  refresh_vst3_catalogue();
#endif
}

void Vst3BrowserController::create_vst3_instrument_track(int entry_index, const QString& class_id) {
  insert_vst3_instrument(entry_index, class_id, {});
}

// The selected catalogue entry is a discovery hint. A worker rescans it before
// ProjectSession compares its receipt against the exact saved class and binary.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void Vst3BrowserController::relink_vst3_plugin(const QString& track_id, const QString& slot_id,
                                               int entry_index) {
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  if (!session() || busy() || host_.playback_busy() || host_.jobs_busy()) {
    status_ =
        QStringLiteral("Stop playback and finish current work before relinking a VST3 plugin");
    emit changed();
    return;
  }
  const auto parsed_track = project::TrackId::parse(track_id.toStdString());
  const auto& snapshot = session()->snapshot();
  const auto track =
      parsed_track ? std::ranges::find(snapshot.tracks, parsed_track.value(), &project::Track::id)
                   : snapshot.tracks.end();
  if (track == snapshot.tracks.end()) {
    status_ = QStringLiteral("Saved VST3 relink track is unavailable");
    emit changed();
    return;
  }
  std::optional<project::InsertSlotId> slot;
  const project::Vst3PluginInstance* saved = nullptr;
  if (slot_id.isEmpty()) {
    if (track->kind == project::TrackKind::instrument)
      saved = detail::vst3_instrument_source(*track);
  } else {
    auto parsed = project::InsertSlotId::parse(slot_id.toStdString());
    if (parsed) {
      slot = parsed.value();
      const auto found = std::ranges::find(track->effects, *slot, &project::EffectInsert::id);
      if (found != track->effects.end())
        saved = detail::vst3_effect(*found);
    }
  }
  if (!saved || !scanner_ || entry_index < 0 ||
      static_cast<std::size_t>(entry_index) >= catalogue_.entries.size()) {
    status_ = QStringLiteral("Select a saved VST3 and a scanned bundle to relink");
    emit changed();
    return;
  }
  const auto& entry = catalogue_.entries[static_cast<std::size_t>(entry_index)];
  const bool instrument = !slot;
  const auto matching = std::ranges::find_if(entry.scan.metadata.classes, [&](const auto& klass) {
    return klass.id == saved->class_id && klass.vendor == saved->vendor &&
           klass.name == saved->name && klass.version == saved->version &&
           (instrument ? is_instrument_class(klass) : is_effect_class(klass));
  });
  if (!entry.scan.ok() || entry.scan.binary.sha256 != saved->binary_sha256 ||
      matching == entry.scan.metadata.classes.end()) {
    status_ =
        QStringLiteral("Relink requires the exact saved VST3 class and binary; state retained");
    emit changed();
    return;
  }
  auto completion = std::make_shared<Completion>();
  completion->action =
      instrument ? Completion::Action::relink_instrument : Completion::Action::relink_effect;
  completion->project_generation = host_.project_generation();
  completion->revision = snapshot.revision;
  completion->track = parsed_track.value();
  completion->slot = slot;
  completion->original_plugin = *saved;
  auto* scanner = scanner_.get();
  auto chosen = entry;
  const auto chosen_class = saved->class_id;
  try {
    worker_ = std::jthread([completion, scanner, chosen = std::move(chosen), chosen_class,
                            instrument](const std::stop_token&) {
      try {
        if (instrument)
          completion->verified_instrument.emplace(
              plugins::verify_vst3_instrument_selection(chosen, chosen_class, *scanner));
        else
          completion->verified.emplace(
              plugins::verify_vst3_effect_selection(chosen, chosen_class, *scanner));
      } catch (const std::exception& error) {
        completion->failure = QString::fromUtf8(error.what());
      } catch (...) {
        completion->failure = QStringLiteral("VST3 relink verification failed unexpectedly");
      }
      completion->done.store(true, std::memory_order_release);
    });
  } catch (const std::exception& error) {
    status_ = QStringLiteral("Could not start VST3 relink: %1").arg(error.what());
    emit changed();
    return;
  }
  completion_ = std::move(completion);
  status_ = QStringLiteral("Verifying saved VST3 binary for relink");
  emit changed();
  timer_.start();
#else
  (void)track_id;
  (void)slot_id;
  (void)entry_index;
  refresh_vst3_catalogue();
#endif
}

void Vst3BrowserController::publish() {
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  entries_.clear();
  for (std::size_t index = 0; index < catalogue_.entries.size(); ++index) {
    const auto& entry = catalogue_.entries[index];
    const auto bundle = QString::fromStdString(entry.canonical_bundle.string());
    const auto status = scan_state(entry.scan.status);
    if (!entry.scan.ok() || entry.scan.metadata.classes.empty()) {
      entries_.push_back(
          QVariantMap{{"index", static_cast<int>(index)},
                      {"bundle", bundle},
                      {"name", QString::fromStdString(entry.canonical_bundle.filename().string())},
                      {"vendor", QString()},
                      {"classId", QString()},
                      {"state", status},
                      {"diagnostic", QString::fromStdString(entry.scan.diagnostic)},
                      {"kind", QStringLiteral("other")},
                      {"insertable", false}});
      continue;
    }
    for (const auto& klass : entry.scan.metadata.classes) {
      const bool effect = is_effect_class(klass);
      const bool instrument = is_instrument_class(klass);
      entries_.push_back(QVariantMap{
          {"index", static_cast<int>(index)},
          {"bundle", bundle},
          {"name", QString::fromStdString(klass.name)},
          {"vendor", QString::fromStdString(klass.vendor)},
          {"version", QString::fromStdString(klass.version)},
          {"classId", QString::fromStdString(klass.id)},
          {"sha256", QString::fromStdString(entry.scan.binary.sha256)},
          {"state", status},
          {"diagnostic", effect || instrument ? QString::fromStdString(entry.scan.diagnostic)
                                              : QStringLiteral("Unsupported VST3 class")},
          {"kind", effect       ? QStringLiteral("effect")
                   : instrument ? QStringLiteral("instrument")
                                : QStringLiteral("other")},
          {"insertable", effect || instrument}});
    }
  }
#endif
  emit changed();
}

void Vst3BrowserController::poll() {
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  if (!completion_ || !completion_->done.load(std::memory_order_acquire))
    return;
  timer_.stop();
  if (worker_.joinable())
    worker_.join();
  auto completion = std::move(completion_);
  if (close_after_scan_) {
    close_after_scan_ = false;
    emit scanStoppedForClose();
    return;
  }
  if (!completion->failure.isEmpty()) {
    status_ = QStringLiteral("VST3 task failed: %1").arg(completion->failure);
  } else if (completion->action == Completion::Action::refresh) {
    if (completion->catalogue) {
      catalogue_ = std::move(*completion->catalogue);
      publish();
      status_ = QStringLiteral("Scanned %1 VST3 bundles%2")
                    .arg(catalogue_.entries.size())
                    .arg(catalogue_.truncated ? QStringLiteral(" · limit reached") : QString());
    } else {
      status_ = QStringLiteral("VST3 scan returned no result");
    }
  } else if (!session() || completion->project_generation != host_.project_generation() ||
             completion->revision != session()->snapshot().revision ||
             (completion->action != Completion::Action::verify_new_instrument &&
              !completion->track)) {
    status_ = QStringLiteral("Project changed during verification; action cancelled");
  } else {
    const auto report_insert = [this](const auto& inserted, std::uint64_t before) {
      if (session()->snapshot().revision != before) {
        host_.commit();
      }
      status_ = inserted ? QStringLiteral("Latest VST3 insert verified and saved")
                         : QStringLiteral("VST3 insert: %1")
                               .arg(QString::fromStdString(inserted.error().message));
    };
    if (completion->action == Completion::Action::relink_effect ||
        completion->action == Completion::Action::relink_instrument) {
      const auto relink = [&](const plugins::Vst3Class& klass,
                              const plugins::Vst3BinaryIdentity& binary) {
        const auto before = session()->snapshot().revision;
        const auto applied = session()->effects().relink_vst3_plugin(
            completion->revision, *completion->track, completion->slot,
            *completion->original_plugin,
            {.class_id = klass.id,
             .vendor = klass.vendor,
             .name = klass.name,
             .version = klass.version,
             .canonical_bundle_path = binary.canonical_bundle.string(),
             .binary_sha256 = binary.sha256});
        const bool accepted = session()->snapshot().revision != before;
        if (accepted) {
          host_.refresh();
          host_.playback_status(
              QStringLiteral("VST3 relink revision %1 is %2 and not active; "
                             "Play will verify the current graph")
                  .arg(host_.revision())
                  .arg(applied ? QStringLiteral("saved") : QStringLiteral("unsaved")));
        }
        if (applied) {
          status_ = QStringLiteral("Exact VST3 binary relinked and saved; "
                                   "project state retained");
        } else if (accepted) {
          status_ = QStringLiteral("VST3 relink accepted in project but save failed; "
                                   "use Save to retry. State retained: %1")
                        .arg(QString::fromStdString(applied.error().message));
        } else {
          status_ = QStringLiteral("VST3 relink refused; state retained: %1")
                        .arg(QString::fromStdString(applied.error().message));
        }
      };
      if (!completion->track || !completion->original_plugin) {
        status_ = QStringLiteral("VST3 relink target was lost; state retained");
      } else if (completion->action == Completion::Action::relink_effect) {
        if (!completion->verified)
          status_ = QStringLiteral("VST3 effect relink returned no receipt");
        else if (!completion->verified.value())
          status_ = QStringLiteral("VST3 effect relink refused; state retained: %1")
                        .arg(QString::fromStdString(completion->verified.value().error().message));
        else
          relink(completion->verified.value().value().effect_class(),
                 completion->verified.value().value().binary());
      } else if (!completion->verified_instrument) {
        status_ = QStringLiteral("VST3 instrument relink returned no receipt");
      } else if (!completion->verified_instrument.value()) {
        status_ = QStringLiteral("VST3 instrument relink refused; state retained: %1")
                      .arg(QString::fromStdString(
                          completion->verified_instrument.value().error().message));
      } else {
        relink(completion->verified_instrument.value().value().instrument_class(),
               completion->verified_instrument.value().value().binary());
      }
    } else if (completion->action == Completion::Action::verify_effect) {
      if (!completion->verified || !completion->track) {
        status_ = QStringLiteral("VST3 effect verification returned no result");
      } else if (!completion->verified.value()) {
        status_ = QStringLiteral("VST3 insert refused: %1")
                      .arg(QString::fromStdString(completion->verified.value().error().message));
      } else {
        const auto before = session()->snapshot().revision;
        const auto selected = std::ranges::find(session()->snapshot().tracks, *completion->track,
                                                &project::Track::id);
        const auto position =
            selected == session()->snapshot().tracks.end() ? 0U : selected->effects.size();
        report_insert(plugins::insert_verified_vst3_effect(*session(),
                                                           completion->verified.value().value(),
                                                           *completion->track, position),
                      before);
      }
    } else if (completion->action == Completion::Action::verify_instrument ||
               completion->action == Completion::Action::verify_new_instrument) {
      if (!completion->verified_instrument) {
        status_ = QStringLiteral("VST3 instrument verification returned no result");
      } else if (!completion->verified_instrument.value()) {
        status_ = QStringLiteral("VST3 instrument insert refused: %1")
                      .arg(QString::fromStdString(
                          completion->verified_instrument.value().error().message));
      } else {
        const auto before = session()->snapshot().revision;
        if (completion->action == Completion::Action::verify_new_instrument) {
          const auto created = plugins::create_verified_vst3_instrument_track(
              *session(), completion->verified_instrument.value().value());
          report_insert(created, before);
          if (created)
            host_.track_created(
                static_cast<int>(session()->tracks().track_index(created.value()).value_or(0)));
        } else if (completion->track) {
          report_insert(
              plugins::insert_verified_vst3_instrument(
                  *session(), completion->verified_instrument.value().value(), *completion->track),
              before);
        } else {
          status_ = QStringLiteral("VST3 instrument target is unavailable");
        }
      }
    }
  }
  host_.status(status_);
  emit changed();
#endif
}

void Vst3BrowserController::scan_for_project(const project::Snapshot& snapshot) {
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  if (busy())
    return;
  const auto roots = plugins::standard_linux_vst3_roots();
  const auto saved_in_default_root = [&roots](const project::Track& track) {
    for (const auto* plugin : detail::saved_vst3_plugins(track)) {
      if (plugin->missing)
        continue;
      const auto bundle = std::filesystem::path(plugin->bundle_path).lexically_normal();
      if (!bundle.is_absolute())
        continue;
      if (std::ranges::any_of(roots, [&bundle](const std::filesystem::path& root) {
            const auto relative = bundle.lexically_relative(root.lexically_normal());
            return !relative.empty() && *relative.begin() != "..";
          }))
        return true;
    }
    return false;
  };
  if (std::ranges::any_of(snapshot.tracks, saved_in_default_root))
    refresh_vst3_catalogue();
#else
  (void)snapshot;
#endif
}

bool Vst3BrowserController::close_when_done() {
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  if (!completion_)
    return false;
  close_after_scan_ = true;
  worker_.request_stop();
  status_ = QStringLiteral("Finishing VST3 scan before closing");
  emit changed();
  timer_.start();
  return true;
#else
  return false;
#endif
}

} // namespace oscillate::quick
