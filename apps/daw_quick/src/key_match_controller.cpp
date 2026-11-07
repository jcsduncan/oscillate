#include "key_match_controller.hpp"

#include "key_review_controller.hpp"

#include "oscillate/application/browser_preview.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace oscillate::quick {
namespace {

// Analysis that ran across another edit is retried this many times.
constexpr int max_retries = 2;

[[nodiscard]] const project::Clip* find_clip(const project::Snapshot& snapshot,
                                             project::ClipId id) {
  for (const auto& track : snapshot.tracks)
    for (const auto& clip : track.clips)
      if (clip.id == id)
        return &clip;
  return nullptr;
}

[[nodiscard]] const project::SourceKeyReview* find_review(const project::Snapshot& snapshot,
                                                          project::SourceId source) {
  const auto found =
      std::ranges::find(snapshot.source_key_reviews, source, &project::SourceKeyReview::source);
  return found == snapshot.source_key_reviews.end() ? nullptr : &*found;
}

[[nodiscard]] QString signed_semitones(int semitones) {
  return (semitones > 0 ? QStringLiteral("+") : QStringLiteral("−")) +
         QString::number(std::abs(semitones));
}

} // namespace

KeyMatchController::KeyMatchController(Session session, Root root, CanEdit can_edit, Report report,
                                       Commit commit, Analyzer analyzer, QObject* parent)
    : QObject(parent), session_(std::move(session)), root_(std::move(root)),
      can_edit_(std::move(can_edit)), report_(std::move(report)), commit_(std::move(commit)),
      worker_([analyzer = std::move(analyzer)](
                  const std::filesystem::path& root, const project::SourceRecord& source,
                  std::uint64_t generation, const std::function<bool()>& cancelled,
                  bool& cache_degraded, const std::stop_token& stop) {
        if (analyzer)
          return analyzer(root, source, generation, cancelled);
        return load_source_key(root, source, generation, cancelled, cache_degraded, stop);
      }),
      timer_(this) {
  timer_.setInterval(50);
  connect(&timer_, &QTimer::timeout, this, &KeyMatchController::poll);
}

KeyMatchController::~KeyMatchController() = default;

void KeyMatchController::dropped(project::ClipId clip) {
  auto* session = session_();
  if (session == nullptr || !session->snapshot().key)
    return; // No song key yet: the toolbar suggests one from the clips.
  consider(clip);
}

void KeyMatchController::consider(project::ClipId clip_id) {
  auto* session = session_();
  if (session == nullptr)
    return;
  const auto& snapshot = session->snapshot();
  const auto* clip = find_clip(snapshot, clip_id);
  if (clip == nullptr || !snapshot.key)
    return;
  const auto* review = find_review(snapshot, clip->source);
  if (review == nullptr || (!review->detected && !review->correction)) {
    if (analysing_ != clip_id && std::ranges::find(waiting_, clip_id) == waiting_.end())
      waiting_.push_back(clip_id);
    start_next();
    return;
  }
  const auto match =
      application::key_match(*review, clip->processing.pitch_semitones, *snapshot.key);
  if (!match)
    return;
  offer_ = Offer{clip->id, clip->settings_generation, *snapshot.key, match->semitones};
  QString name = QString::fromStdString(clip->name);
  if (name.isEmpty())
    name = QStringLiteral("The clip");
  hint_ = QVariantMap{
      {QStringLiteral("clipId"), QString::fromStdString(clip->id.to_string())},
      {QStringLiteral("clipName"), name},
      {QStringLiteral("from"), key_name(match->from)},
      {QStringLiteral("to"), key_name(match->to)},
      {QStringLiteral("semitones"), match->semitones},
      {QStringLiteral("text"), QStringLiteral("%1 is in %2. Move it %3 to %4?")
                                   .arg(name, key_name(match->from),
                                        signed_semitones(match->semitones), key_name(match->to))}};
  emit changed();
}

void KeyMatchController::start_next() {
  if (analysing_ || waiting_.empty())
    return;
  auto* session = session_();
  if (session == nullptr) {
    waiting_.clear();
    return;
  }
  while (!waiting_.empty()) {
    const auto clip_id = waiting_.front();
    waiting_.pop_front();
    const auto& snapshot = session->snapshot();
    const auto* clip = find_clip(snapshot, clip_id);
    if (clip == nullptr)
      continue;
    const auto source =
        std::ranges::find(snapshot.sources, clip->source, &project::SourceRecord::id);
    if (source == snapshot.sources.end())
      continue;
    analysing_ = clip_id;
    source_ = *source;
    origin_revision_ = snapshot.revision;
    worker_.submit(root_(), source_, ++generation_);
    timer_.start();
    return;
  }
}

void KeyMatchController::poll() {
  auto outcome = worker_.collect();
  if (!outcome || outcome->generation != generation_ || !analysing_ || !source_)
    return;
  timer_.stop();
  const auto clip = *std::exchange(analysing_, std::nullopt);
  auto* session = session_();
  if (session == nullptr || !outcome->result) {
    retries_ = 0;
    start_next();
    return;
  }
  if (session->snapshot().revision != origin_revision_) {
    // Edited meanwhile: the result cannot be stored for the old revision.
    // The cache makes the retry quick.
    if (retries_++ < max_retries)
      waiting_.push_front(clip);
    else
      retries_ = 0;
    start_next();
    return;
  }
  retries_ = 0;
  const auto accepted = session->source_analysis().accept_source_key_analysis(
      source_->id, origin_revision_, outcome->result.value().estimate);
  if (accepted) {
    if (commit_)
      commit_();
    consider(clip);
  }
  start_next();
}

void KeyMatchController::refresh() {
  if (!offer_)
    return;
  auto* session = session_();
  if (session == nullptr) {
    clear_hint();
    return;
  }
  const auto& snapshot = session->snapshot();
  const auto* clip = find_clip(snapshot, offer_->clip);
  if (clip == nullptr || clip->settings_generation != offer_->settings_generation ||
      !snapshot.key || *snapshot.key != offer_->song)
    clear_hint();
}

void KeyMatchController::apply() {
  auto* session = session_();
  if (!offer_ || session == nullptr)
    return;
  if (can_edit_ && !can_edit_()) {
    if (report_)
      report_(QStringLiteral("Finish the current project operation first"));
    return;
  }
  const auto offer = *offer_;
  const auto* clip = find_clip(session->snapshot(), offer.clip);
  if (clip == nullptr || clip->settings_generation != offer.settings_generation) {
    clear_hint();
    return;
  }
  auto processing = clip->processing;
  processing.pitch_semitones += offer.semitones;
  // Re-Pitch ignores pitch: a transposed clip keeps its tempo in Complex.
  if (processing.warp.mode == project::WarpMode::repitch)
    processing.warp.mode = project::WarpMode::complex;
  const auto applied =
      session->audio_clips().set_audio_clip_processing(offer.clip, std::move(processing));
  const auto text = hint_.value(QStringLiteral("to")).toString();
  clear_hint();
  if (!applied) {
    if (report_)
      report_(QStringLiteral("Cannot transpose the clip: %1")
                  .arg(QString::fromStdString(applied.error().message)));
    return;
  }
  if (report_)
    report_(QStringLiteral("Moved to %1. Undo puts it back").arg(text));
  if (commit_)
    commit_();
}

void KeyMatchController::dismiss() { clear_hint(); }

void KeyMatchController::clear_hint() {
  if (!offer_ && hint_.isEmpty())
    return;
  offer_.reset();
  hint_.clear();
  emit changed();
}

} // namespace oscillate::quick
