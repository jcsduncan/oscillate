#include "key_review_controller.hpp"

#include "oscillate/application/source_analysis_cache_directory.hpp"
#include "oscillate/persistence/store.hpp"

#include <QStringList>
#include <QVariantList>

#include <algorithm>
#include <ranges>
#include <utility>

namespace oscillate::quick {
namespace {
using foundation::ErrorCode;
template <typename T> using Result = foundation::Result<T>;
} // namespace

foundation::Result<application::KeyCacheResult>
load_source_key(const std::filesystem::path& root, const project::SourceRecord& source,
                std::uint64_t generation, const std::function<bool()>& cancelled,
                bool& cache_degraded, const std::stop_token& stop) {
  auto store = persistence::ProjectStore::open_reader(root);
  if (!store) {
    return Result<application::KeyCacheResult>::failure(store.error());
  }
  std::optional<media::CacheStore> cache;
  auto opened = application::open_user_source_analysis_cache();
  if (opened) {
    cache.emplace(std::move(opened.value()));
  } else {
    cache_degraded = true;
  }
  auto result = application::load_key_suggestion(store.value(), cache ? &*cache : nullptr, source,
                                                 generation, cancelled, stop);
  if (result && !result.value().cache_warning.empty()) {
    cache_degraded = true;
  }
  if (cache && (!result || !result.value().cache_hit) &&
      !application::enforce_source_analysis_cache_budget(*cache)) {
    cache_degraded = true;
  }
  return result;
}

namespace {

QString relation_name(project::KeyRelation relation) {
  switch (relation) {
  case project::KeyRelation::relative:
    return QStringLiteral("Relative");
  case project::KeyRelation::dominant:
    return QStringLiteral("Dominant");
  case project::KeyRelation::subdominant:
    return QStringLiteral("Subdominant");
  }
  return QStringLiteral("Unknown");
}
} // namespace

QString key_name(project::MusicalKey key, bool abbreviated) {
  static const QStringList names{QStringLiteral("C"),  QStringLiteral("C#"), QStringLiteral("D"),
                                 QStringLiteral("D#"), QStringLiteral("E"),  QStringLiteral("F"),
                                 QStringLiteral("F#"), QStringLiteral("G"),  QStringLiteral("G#"),
                                 QStringLiteral("A"),  QStringLiteral("A#"), QStringLiteral("B")};
  if (key.tonic >= 12) {
    return QStringLiteral("Unknown");
  }
  const bool major = key.mode == project::KeyMode::major;
  return names[static_cast<int>(key.tonic)] +
         (abbreviated ? (major ? QStringLiteral(" maj") : QStringLiteral(" min"))
                      : (major ? QStringLiteral(" major") : QStringLiteral(" minor")));
}

KeyReviewController::KeyReviewController(Analyzer analyzer, QObject* parent)
    : QObject(parent),
      worker_([analyzer = std::move(analyzer)](
                  const std::filesystem::path& root, const project::SourceRecord& source,
                  std::uint64_t generation, const std::function<bool()>& cancelled,
                  bool& cache_degraded, const std::stop_token& stop) {
        if (analyzer) {
          return analyzer(root, source, generation, cancelled);
        }
        return load_source_key(root, source, generation, cancelled, cache_degraded, stop);
      }),
      timer_(this) {
  timer_.setInterval(50);
  connect(&timer_, &QTimer::timeout, this, &KeyReviewController::poll);
  publish();
}

KeyReviewController::~KeyReviewController() = default;

void KeyReviewController::set_project(application::ProjectSession* session,
                                      std::filesystem::path root) {
  worker_.submit({}, std::nullopt, ++generation_);
  timer_.stop();
  session_ = session;
  root_ = std::move(root);
  source_.reset();
  clip_id_.clear();
  origin_revision_ = 0;
  state_ = QStringLiteral("idle");
  message_.clear();
  cache_hit_ = false;
  cache_degraded_ = false;
  publish();
}

void KeyReviewController::select_clip(QString clip_id,
                                      std::optional<project::SourceRecord> source) {
  worker_.submit({}, std::nullopt, ++generation_);
  timer_.stop();
  source_ = std::move(source);
  clip_id_ = source_ ? std::move(clip_id) : QString();
  state_ = QStringLiteral("idle");
  message_.clear();
  cache_hit_ = false;
  cache_degraded_ = false;
  if (source_ && session_ && selection_matches_snapshot()) {
    const auto& reviews = session_->snapshot().source_key_reviews;
    const bool reviewed = std::ranges::any_of(
        reviews, [this](const auto& review) { return review.source == source_->id; });
    if (reviewed) {
      state_ = QStringLiteral("ready");
      publish();
    } else {
      reanalyze();
    }
  } else {
    source_.reset();
    clip_id_.clear();
    publish();
  }
}

bool KeyReviewController::selection_matches_snapshot() const {
  if (!session_ || !source_ || clip_id_.isEmpty()) {
    return false;
  }
  const auto& snapshot = session_->snapshot();
  const bool source_matches = std::ranges::any_of(
      snapshot.sources, [this](const auto& source) { return source == *source_; });
  return source_matches && std::ranges::any_of(snapshot.tracks, [this](const auto& track) {
           return std::ranges::any_of(track.clips, [this](const auto& clip) {
             return QString::fromStdString(clip.id.to_string()) == clip_id_ &&
                    clip.source == source_->id;
           });
         });
}

void KeyReviewController::cancel_selection() {
  worker_.submit({}, std::nullopt, ++generation_);
  timer_.stop();
  source_.reset();
  clip_id_.clear();
  origin_revision_ = 0;
  state_ = QStringLiteral("idle");
  message_.clear();
  publish();
}

void KeyReviewController::refresh_snapshot() {
  if (!source_) {
    return;
  }
  if (!selection_matches_snapshot()) {
    cancel_selection();
    return;
  }
  if (state_ == QStringLiteral("loading") && session_->snapshot().revision != origin_revision_) {
    worker_.submit({}, std::nullopt, ++generation_);
    timer_.stop();
    state_ = QStringLiteral("stale");
    message_ = QStringLiteral("Project changed during analysis. Retry to refresh suggestions.");
  }
  publish();
}

void KeyReviewController::reanalyze() {
  if (!session_ || !source_ || !selection_matches_snapshot()) {
    return;
  }
  origin_revision_ = session_->snapshot().revision;
  worker_.submit(root_, source_, ++generation_);
  state_ = QStringLiteral("loading");
  message_.clear();
  cache_hit_ = false;
  cache_degraded_ = false;
  timer_.start();
  publish();
}

void KeyReviewController::poll() {
  auto outcome = worker_.collect();
  if (!outcome || outcome->generation != generation_ || !source_ || outcome->source != *source_ ||
      !session_) {
    return;
  }
  timer_.stop();
  if (!selection_matches_snapshot()) {
    cancel_selection();
    return;
  }
  if (session_->snapshot().revision != origin_revision_) {
    state_ = QStringLiteral("stale");
    message_ = QStringLiteral("Project changed during analysis. Retry to refresh suggestions.");
    publish();
    return;
  }
  cache_degraded_ = outcome->cache_degraded;
  if (!outcome->result) {
    state_ = QStringLiteral("unavailable");
    message_ = QString::fromStdString(outcome->result.error().message);
    publish();
    return;
  }
  auto& result = outcome->result.value();
  if (result.estimate.identity.generation != generation_) {
    state_ = QStringLiteral("stale");
    message_ = QStringLiteral("Analysis generation changed. Retry to refresh suggestions.");
    publish();
    return;
  }
  auto applied = session_->source_analysis().accept_source_key_analysis(
      source_->id, origin_revision_, result.estimate);
  if (!applied) {
    state_ = QStringLiteral("unavailable");
    message_ = QString::fromStdString(applied.error().message);
    publish();
    return;
  }
  cache_hit_ = result.cache_hit;
  cache_degraded_ = cache_degraded_ || !result.cache_warning.empty();
  state_ = QStringLiteral("ready");
  message_ = cache_degraded_ ? QStringLiteral("Suggestions ready; cache unavailable")
                             : QStringLiteral("Suggestion stored in project. Save to keep it.");
  publish();
  emit projectRevisionChanged();
}

void KeyReviewController::set_correction(int tonic, int mode) {
  if (!session_ || !source_ || !selection_matches_snapshot() || tonic < 0 || tonic >= 12 ||
      (mode != 0 && mode != 1)) {
    message_ = QStringLiteral("Choose a valid tonic and major or minor mode");
    publish();
    return;
  }
  worker_.submit({}, std::nullopt, ++generation_);
  timer_.stop();
  auto applied = session_->source_analysis().set_source_key_correction(
      source_->id,
      project::MusicalKey{static_cast<std::uint8_t>(tonic),
                          mode == 0 ? project::KeyMode::major : project::KeyMode::minor});
  if (!applied) {
    message_ = QString::fromStdString(applied.error().message);
    publish();
    return;
  }
  state_ = QStringLiteral("ready");
  message_ = QStringLiteral("Key correction stored in project. Save to keep it.");
  publish();
  emit projectRevisionChanged();
}

void KeyReviewController::clear_correction() {
  if (!session_ || !source_ || !selection_matches_snapshot()) {
    return;
  }
  worker_.submit({}, std::nullopt, ++generation_);
  timer_.stop();
  auto applied = session_->source_analysis().set_source_key_correction(source_->id, std::nullopt);
  if (!applied) {
    message_ = QString::fromStdString(applied.error().message);
    publish();
    return;
  }
  state_ = QStringLiteral("ready");
  message_ = QStringLiteral("Key correction cleared. Save to keep it.");
  publish();
  emit projectRevisionChanged();
}

void KeyReviewController::publish() {
  const project::SourceKeyReview* review = nullptr;
  if (session_ && source_) {
    const auto& reviews = session_->snapshot().source_key_reviews;
    const auto found = std::ranges::find_if(
        reviews, [this](const auto& candidate) { return candidate.source == source_->id; });
    if (found != reviews.end()) {
      review = &*found;
    }
  }
  QVariantList compatible;
  if (review) {
    for (const auto& suggestion : review->compatible) {
      compatible.push_back(QVariantMap{{"key", key_name(suggestion.key)},
                                       {"relation", relation_name(suggestion.relation)}});
    }
  }
  const auto detected = review && review->detected
                            ? key_name(review->detected.value_or(project::MusicalKey{}))
                            : QStringLiteral("No reliable key");
  const auto corrected = review && review->correction
                             ? key_name(review->correction.value_or(project::MusicalKey{}))
                             : QString();
  preview_ = QVariantMap{
      {"state", state_},
      {"selectedClipId", clip_id_},
      {"sourceId", source_ ? QString::fromStdString(source_->id.to_string()) : QString()},
      {"detected", detected},
      {"confidence", review ? review->confidence : 0.0},
      {"compatible", compatible},
      {"corrected", corrected},
      {"correctedTonic",
       review && review->correction ? static_cast<int>(review->correction->tonic) : -1},
      {"correctedMode", review && review->correction
                            ? (review->correction->mode == project::KeyMode::major ? 0 : 1)
                            : -1},
      {"detectedTonic",
       review && review->detected ? static_cast<int>(review->detected->tonic) : -1},
      {"detectedMode", review && review->detected
                           ? (review->detected->mode == project::KeyMode::major ? 0 : 1)
                           : -1},
      {"cacheHit", cache_hit_},
      {"cacheDegraded", cache_degraded_},
      {"message", message_}};
  emit changed();
}

} // namespace oscillate::quick
