#include "mix_check_controller.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileDialog>
#include <QFileInfo>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <utility>

namespace oscillate::quick {
namespace {
namespace check = application::mix_check;

// Worker progress is polled this often; "checked N s ago" re-read this often.
constexpr int poll_ms = 50;
constexpr int clock_ms = 5'000;
// Under a minute the age reads in seconds, under an hour in minutes, and
// then as the time of day.
constexpr qint64 seconds_per_minute = 60;
constexpr qint64 seconds_per_hour = 3'600;

[[nodiscard]] QVariantList numbers(const std::vector<double>& values) {
  QVariantList list;
  list.reserve(static_cast<qsizetype>(values.size()));
  for (const double value : values)
    list.append(std::isfinite(value) ? value : analysis::mix::silence_db);
  return list;
}

[[nodiscard]] QString text(const std::string& value) { return QString::fromStdString(value); }

[[nodiscard]] QString colour(const std::optional<std::uint32_t>& rgb) {
  if (!rgb)
    return {};
  return QStringLiteral("#%1").arg(*rgb, 6, 16, QLatin1Char('0'));
}

[[nodiscard]] QVariantMap to_map(const check::Finding& finding) {
  QVariantMap map;
  map.insert(QStringLiteral("id"), text(finding.id));
  map.insert(QStringLiteral("kind"),
             QString::fromUtf8(check::kind_name(finding.kind).data(),
                               static_cast<qsizetype>(check::kind_name(finding.kind).size())));
  map.insert(QStringLiteral("severity"),
             finding.bad ? QStringLiteral("bad") : QStringLiteral("warn"));
  map.insert(QStringLiteral("title"), text(finding.title));
  map.insert(QStringLiteral("text"), text(finding.text));
  QVariantList tracks;
  for (const auto& track : finding.tracks)
    tracks.append(QVariantMap{{QStringLiteral("id"), text(track.id.to_string())},
                              {QStringLiteral("name"), text(track.name)},
                              {QStringLiteral("color"), colour(track.color)}});
  map.insert(QStringLiteral("tracks"), tracks);
  if (finding.band)
    map.insert(QStringLiteral("band"),
               QVariantMap{{QStringLiteral("low"), finding.band->low},
                           {QStringLiteral("high"), finding.band->high},
                           {QStringLiteral("label"), text(finding.band->label)}});
  else
    map.insert(QStringLiteral("band"), QVariant());
  map.insert(QStringLiteral("evidence"),
             QVariantMap{{QStringLiteral("kind"), text(finding.evidence.kind)},
                         {QStringLiteral("caption"), text(finding.evidence.caption)},
                         {QStringLiteral("x"), numbers(finding.evidence.x)},
                         {QStringLiteral("before"), numbers(finding.evidence.before)},
                         {QStringLiteral("after"), numbers(finding.evidence.after)},
                         {QStringLiteral("markers"), numbers(finding.evidence.markers)}});
  if (finding.fix) {
    QVariantList params;
    for (const auto& [label, value] : finding.fix->params)
      params.append(QVariant(QVariantList{text(label), text(value)}));
    map.insert(QStringLiteral("fix"),
               QVariantMap{{QStringLiteral("device"), text(finding.fix->device)},
                           {QStringLiteral("where"), text(finding.fix->where)},
                           {QStringLiteral("params"), params}});
  } else {
    map.insert(QStringLiteral("fix"), QVariant());
  }
  map.insert(QStringLiteral("expected"), text(finding.expected));
  map.insert(QStringLiteral("applied"), finding.applied);
  map.insert(QStringLiteral("result"), text(finding.result));
  return map;
}

} // namespace

MixCheckController::MixCheckController(Hooks hooks, QObject* parent)
    : QObject(parent), hooks_(std::move(hooks)) {
  timer_.setInterval(poll_ms);
  connect(&timer_, &QTimer::timeout, this, &MixCheckController::poll);
  clock_.setInterval(clock_ms);
  connect(&clock_, &QTimer::timeout, this, &MixCheckController::checkedAtChanged);
}

MixCheckController::~MixCheckController() {
  if (job_)
    job_->cancel.store(true, std::memory_order_release);
  if (reference_job_)
    reference_job_->cancel.store(true, std::memory_order_release);
  if (worker_.joinable())
    worker_.join();
  if (reference_worker_.joinable())
    reference_worker_.join();
}

void MixCheckController::set_status(QString status) {
  // `changed` also refreshes the rest of the view, so it is sent either way.
  status_.set(std::move(status));
  emit changed();
}

double MixCheckController::progress() const noexcept {
  return job_ ? job_->progress.load(std::memory_order_acquire) : 0.0;
}

QString MixCheckController::stage() const {
  if (!job_)
    return {};
  const std::lock_guard lock(job_->mutex);
  return text(job_->stage);
}

QString MixCheckController::checked_at() const {
  if (!checked_time_.isValid())
    return {};
  const auto seconds = checked_time_.secsTo(QDateTime::currentDateTime());
  if (seconds < clock_ms / 1'000)
    return QStringLiteral("checked just now");
  if (seconds < seconds_per_minute)
    return QStringLiteral("checked %1 s ago").arg(seconds);
  if (seconds < seconds_per_hour)
    return QStringLiteral("checked %1 min ago").arg(seconds / seconds_per_minute);
  return QStringLiteral("checked at %1").arg(checked_time_.toString(QStringLiteral("HH:mm")));
}

QString MixCheckController::last_checked_at() const {
  if (!previous_history_)
    return {};
  const auto when = QDateTime::fromSecsSinceEpoch(previous_history_->checked_at);
  const auto today = QDate::currentDate();
  if (when.date() == today)
    return QStringLiteral("today %1").arg(when.toString(QStringLiteral("HH:mm")));
  if (when.date() == today.addDays(-1))
    return QStringLiteral("yesterday %1").arg(when.toString(QStringLiteral("HH:mm")));
  return when.toString(QStringLiteral("d MMM HH:mm"));
}

QVariantMap MixCheckController::summary() const {
  if (!report_)
    return {};
  return {{QStringLiteral("projectName"), hooks_.project_name ? hooks_.project_name() : QString()},
          {QStringLiteral("trackCount"), static_cast<int>(report_->track_count)},
          {QStringLiteral("durationSeconds"), report_->duration_seconds}};
}

QVariantMap MixCheckController::spectrum() const {
  if (!report_)
    return {{QStringLiteral("freqs"), QVariantList{}},
            {QStringLiteral("before"), QVariantList{}},
            {QStringLiteral("after"), QVariantList{}},
            {QStringLiteral("mono"), QVariantList{}}};
  return {{QStringLiteral("freqs"), numbers(report_->freqs)},
          {QStringLiteral("before"), numbers(baseline_spectrum_)},
          {QStringLiteral("after"), after_ ? numbers(report_->spectrum) : QVariantList{}},
          {QStringLiteral("mono"), numbers(report_->mono)}};
}

QVariantList MixCheckController::meters() const {
  QVariantList list;
  if (!report_)
    return list;
  for (const auto& meter : report_->meters)
    list.append(QVariantMap{{QStringLiteral("id"), text(meter.id)},
                            {QStringLiteral("label"), text(meter.label)},
                            {QStringLiteral("value"), text(meter.value)},
                            {QStringLiteral("note"), text(meter.note)},
                            {QStringLiteral("state"), text(meter.state)}});
  return list;
}

void MixCheckController::set_target(const QString& id) {
  const auto wanted = id.toStdString();
  if (wanted == target_)
    return;
  target_ = std::string(check::target(wanted).id);
  rebuild();
  emit changed();
}

QVariantList MixCheckController::targets() const {
  QVariantList list;
  for (const auto& goal : check::targets)
    list.append(
        QVariantMap{{QStringLiteral("id"),
                     QString::fromUtf8(goal.id.data(), static_cast<qsizetype>(goal.id.size()))},
                    {QStringLiteral("name"),
                     QString::fromUtf8(goal.name.data(), static_cast<qsizetype>(goal.name.size()))},
                    {QStringLiteral("note"),
                     QString::fromUtf8(goal.note.data(), static_cast<qsizetype>(goal.note.size()))},
                    {QStringLiteral("lufsLow"), goal.lufs_low},
                    {QStringLiteral("lufsHigh"), goal.lufs_high},
                    {QStringLiteral("truePeak"), goal.true_peak},
                    {QStringLiteral("lraLow"), goal.lra_low},
                    {QStringLiteral("lraHigh"), goal.lra_high},
                    {QStringLiteral("crestLow"), goal.crest_low},
                    {QStringLiteral("crestHigh"), goal.crest_high}});
  return list;
}

QVariantMap MixCheckController::loudness() const {
  const auto value = report_ ? report_->loudness : check::LoudnessSummary{};
  return {{QStringLiteral("integrated"), value.integrated},
          {QStringLiteral("truePeak"), value.true_peak},
          {QStringLiteral("range"), value.range},
          {QStringLiteral("crest"), value.crest}};
}

QVariantList MixCheckController::energy() const {
  QVariantList list;
  if (report_)
    for (const auto& bar : report_->energy)
      list.append(QVariantMap{{QStringLiteral("bar"), static_cast<int>(bar.bar)},
                              {QStringLiteral("lufs"), bar.lufs}});
  return list;
}

QVariantList MixCheckController::sections() const {
  QVariantList list;
  if (report_)
    for (const auto& section : report_->sections)
      list.append(QVariantMap{{QStringLiteral("name"), text(section.name)},
                              {QStringLiteral("firstBar"), static_cast<int>(section.first_bar)},
                              {QStringLiteral("lastBar"), static_cast<int>(section.last_bar)},
                              {QStringLiteral("lufs"), section.lufs}});
  return list;
}

QString MixCheckController::energy_note() const {
  return report_ ? text(report_->energy_note) : QString();
}

QVariantList MixCheckController::findings() const {
  QVariantList list;
  if (report_)
    for (const auto& finding : report_->findings)
      list.append(to_map(finding));
  return list;
}

QVariantList MixCheckController::fine() const {
  QVariantList list;
  if (report_)
    for (const auto& line : report_->fine)
      list.append(text(line));
  return list;
}

QVariantList MixCheckController::since_last() const {
  QVariantList list;
  if (!previous_history_ || !current_history_)
    return list;
  for (const auto& change : check::since_last(*previous_history_, *current_history_, target_))
    list.append(QVariantMap{{QStringLiteral("direction"), text(change.direction)},
                            {QStringLiteral("text"), text(change.text)}});
  return list;
}

QVariantList MixCheckController::reference() const {
  QVariantList list;
  if (!reference_track_ || !measured_ || !report_)
    return list;
  for (const auto& band :
       analysis::mix::compare_reference(measured_->master, report_->loudness.integrated,
                                        reference_track_->profile, reference_track_->lufs))
    list.append(QVariantMap{{QStringLiteral("band"), text(band.band)},
                            {QStringLiteral("diffDb"), band.diff_db}});
  return list;
}

const check::Finding* MixCheckController::find(const QString& id) const {
  if (!report_)
    return nullptr;
  const auto wanted = id.toStdString();
  const auto found = std::ranges::find(report_->findings, wanted, &check::Finding::id);
  return found == report_->findings.end() ? nullptr : &*found;
}

// ---- Runs ----------------------------------------------------------------------------

void MixCheckController::run() {
  if (job_) {
    set_status(QStringLiteral("Mix Check is already running"));
    return;
  }
  // A fresh run starts a fresh "before".
  applied_.clear();
  after_ = false;
  start(std::nullopt);
}

void MixCheckController::start(std::optional<check::Finding> applied) {
  auto* session = hooks_.session ? hooks_.session() : nullptr;
  if (session == nullptr) {
    set_status(QStringLiteral("Open a project first"));
    return;
  }
  if (const auto why = hooks_.blocked ? hooks_.blocked() : QString(); !why.isEmpty()) {
    set_status(why);
    return;
  }
  stopListening();
  auto pinned = session->exports().pin_snapshot();
  if (!pinned) {
    set_status(QStringLiteral("Mix Check cannot start: %1").arg(text(pinned.error().message)));
    return;
  }
  auto job = std::make_shared<Job>();
  job->stage = "Rendering and measuring the song";
  job->target = target_;
  job->applied = std::move(applied);
  job->session = session->session_generation();
  try {
    worker_ = std::jthread([job, pinned = std::move(pinned).value()]() mutable {
      try {
        auto measured = check::measure(pinned, [&](double fraction, const std::string& stage) {
          job->progress.store(fraction, std::memory_order_release);
          {
            const std::lock_guard lock(job->mutex);
            job->stage = stage;
          }
          return !job->cancel.load(std::memory_order_acquire);
        });
        if (measured) {
          job->report = check::report(measured.value(), job->target);
          job->previous = check::load_history(pinned.reader);
          job->checked_at = QDateTime::currentSecsSinceEpoch();
          // Losing the history only loses the "since last" list.
          (void)check::save_history(pinned.reader, check::summarise(*job->report, job->checked_at));
        }
        job->result.emplace(std::move(measured));
      } catch (const std::exception& error) {
        job->result.emplace(foundation::Result<check::Measurements>::failure(
            {foundation::ErrorCode::internal, error.what()}));
      }
      job->done.store(true, std::memory_order_release);
    });
  } catch (const std::exception& error) {
    set_status(QStringLiteral("Cannot start Mix Check: %1").arg(QString::fromUtf8(error.what())));
    return;
  }
  job_ = std::move(job);
  timer_.start();
  status_.set(job_->applied ? QStringLiteral("Re-checking after the fix…")
                            : QStringLiteral("Checking the mix…"));
  emit changed();
}

void MixCheckController::cancel() {
  if (job_)
    job_->cancel.store(true, std::memory_order_release);
  if (reference_job_)
    reference_job_->cancel.store(true, std::memory_order_release);
}

void MixCheckController::poll() {
  if (reference_job_ && reference_job_->done.load(std::memory_order_acquire)) {
    if (reference_worker_.joinable())
      reference_worker_.join();
    finish_reference();
  }
  if (job_ && job_->done.load(std::memory_order_acquire)) {
    if (worker_.joinable())
      worker_.join();
    finish();
  } else if (job_) {
    emit changed();
  }
  if (!job_ && !reference_job_)
    timer_.stop();
}

void MixCheckController::finish() {
  auto job = std::move(job_);
  job_.reset();
  // The worker fills the result, and with a measurement the report, before it reports done.
  if (!job->result) {
    set_status(QStringLiteral("Mix Check failed"));
    return;
  }
  auto& result = *job->result;
  if (job->cancel.load(std::memory_order_acquire)) {
    set_status(QStringLiteral("Mix Check cancelled"));
    return;
  }
  if (auto* session = hooks_.session ? hooks_.session() : nullptr;
      session == nullptr || session->session_generation() != job->session) {
    set_status(QStringLiteral("Mix Check dropped: the project changed"));
    return;
  }
  if (!result) {
    set_status(QStringLiteral("Mix Check failed: %1").arg(text(result.error().message)));
    return;
  }
  if (!job->report) {
    set_status(QStringLiteral("Mix Check failed"));
    return;
  }
  const auto before = report_;
  measured_ = std::make_shared<const check::Measurements>(std::move(result).value());
  const auto& report = report_.emplace(std::move(*job->report));
  if (job->previous)
    previous_history_ = std::move(job->previous);
  else
    previous_history_.reset();
  current_history_ = check::summarise(report, job->checked_at);
  checked_time_ = QDateTime::fromSecsSinceEpoch(job->checked_at);
  clock_.start();
  QString fixed;
  if (auto& applied = job->applied; applied && before) {
    auto finding = std::move(*applied);
    check::judge(finding, *before, report, *measured_);
    fixed = text(finding.result);
    applied_[finding.id] = std::move(finding);
    after_ = true;
  } else {
    baseline_spectrum_ = report.spectrum;
  }
  rebuild();
  stale_ = false;
  const auto open =
      std::ranges::count_if(report.findings, [](const check::Finding& f) { return !f.applied; });
  status_.set(!fixed.isEmpty() ? QStringLiteral("Re-checked: %1").arg(fixed)
                               : QStringLiteral("%1 to look at \u00b7 %2 tracks measured")
                                     .arg(open)
                                     .arg(measured_->tracks.size()));
  emit changed();
  emit checkedAtChanged();
}

// The report for the chosen target, with the fixes applied since the last
// Run kept in the list as fixed.
void MixCheckController::rebuild() {
  if (!measured_)
    return;
  report_ = check::report(*measured_, target_);
  for (const auto& [id, finding] : applied_) {
    const auto found = std::ranges::find(report_->findings, id, &check::Finding::id);
    if (found != report_->findings.end()) {
      found->applied = true;
      found->result = finding.result;
      found->evidence = finding.evidence;
    } else {
      report_->findings.insert(report_->findings.begin(), finding);
    }
  }
}

void MixCheckController::apply(const QString& finding_id) {
  if (job_) {
    set_status(QStringLiteral("Wait for the check to finish"));
    return;
  }
  const auto* finding = find(finding_id);
  auto* session = hooks_.session ? hooks_.session() : nullptr;
  if (finding == nullptr || session == nullptr || !measured_) {
    set_status(QStringLiteral("Run Mix Check first"));
    return;
  }
  if (finding->applied) {
    set_status(QStringLiteral("Already applied: undo it from the history"));
    return;
  }
  if (!finding->fix) {
    set_status(QStringLiteral("This one has no one-click fix"));
    return;
  }
  if (hooks_.can_edit && !hooks_.can_edit())
    return;
  stopListening();
  auto copy = *finding;
  if (auto done = check::apply(*session, copy, *measured_->snapshot); !done) {
    set_status(QStringLiteral("Apply failed: %1").arg(text(done.error().message)));
    return;
  }
  if (hooks_.commit)
    hooks_.commit();
  start(std::move(copy));
}

void MixCheckController::project_changed() {
  auto* session = hooks_.session ? hooks_.session() : nullptr;
  if (!measured_ || job_)
    return;
  if (session == nullptr || session->snapshot().id != measured_->snapshot->id) {
    reset();
    return;
  }
  const bool stale = !check::same_mix(session->snapshot(), *measured_->snapshot);
  if (stale != stale_) {
    stale_ = stale;
    emit changed();
  }
}

void MixCheckController::reset() {
  cancel();
  listening_.clear();
  solo_before_.clear();
  measured_.reset();
  report_.reset();
  applied_.clear();
  after_ = false;
  baseline_spectrum_.clear();
  previous_history_.reset();
  current_history_.reset();
  checked_time_ = {};
  clock_.stop();
  stale_ = false;
  status_.set({});
  emit changed();
  emit checkedAtChanged();
}

// ---- Listen --------------------------------------------------------------------------

void MixCheckController::listen(const QString& finding_id) {
  if (!listening_.isEmpty()) {
    const bool same = listening_ == finding_id;
    stopListening();
    if (same)
      return;
  }
  const auto* finding = find(finding_id);
  auto* session = hooks_.session ? hooks_.session() : nullptr;
  if (finding == nullptr || session == nullptr) {
    set_status(QStringLiteral("Run Mix Check first"));
    return;
  }
  if (!finding->tracks.empty() && hooks_.set_solo) {
    solo_before_.clear();
    for (const auto& track : session->snapshot().tracks)
      solo_before_[track.id] = track.soloed;
    for (const auto& track : session->snapshot().tracks) {
      const bool wanted = std::ranges::any_of(
          finding->tracks, [&](const check::TrackRef& ref) { return ref.id == track.id; });
      if (track.soloed != wanted)
        hooks_.set_solo(track.id, wanted);
    }
  }
  listening_ = finding_id;
  if (hooks_.play_from)
    hooks_.play_from(finding->worst_frame);
  status_.set(finding->band ? QStringLiteral("Soloing %1").arg(text(finding->band->label))
                            : QStringLiteral("Playing from where it is worst"));
  emit changed();
}

void MixCheckController::stopListening() {
  if (listening_.isEmpty())
    return;
  listening_.clear();
  if (hooks_.stop)
    hooks_.stop();
  auto* session = hooks_.session ? hooks_.session() : nullptr;
  if (session != nullptr && hooks_.set_solo)
    for (const auto& track : session->snapshot().tracks) {
      const auto was = solo_before_.find(track.id);
      if (was != solo_before_.end() && was->second != track.soloed)
        hooks_.set_solo(track.id, was->second);
    }
  solo_before_.clear();
  emit changed();
}

// ---- Reference -----------------------------------------------------------------------

void MixCheckController::chooseReference() {
  const auto chosen = QFileDialog::getOpenFileName(
      nullptr, QStringLiteral("Choose a finished track to compare with"), QDir::homePath(),
      QStringLiteral("Audio files (*.wav *.wave *.aiff *.aif *.flac *.mp3);;All files (*)"));
  if (!chosen.isEmpty())
    setReference(chosen);
}

void MixCheckController::setReference(const QString& path) {
  if (reference_job_) {
    set_status(QStringLiteral("Still reading the last reference"));
    return;
  }
  const QFileInfo file(path);
  if (!file.isFile()) {
    set_status(QStringLiteral("Choose an audio file to compare with"));
    return;
  }
  auto job = std::make_shared<ReferenceJob>();
  const auto target = file.absoluteFilePath().toStdString();
  try {
    reference_worker_ = std::jthread([job, target] {
      try {
        job->result.emplace(check::measure_reference(
            target, [&] { return job->cancel.load(std::memory_order_acquire); }));
      } catch (const std::exception& error) {
        job->result.emplace(foundation::Result<check::ReferenceTrack>::failure(
            {foundation::ErrorCode::internal, error.what()}));
      }
      job->done.store(true, std::memory_order_release);
    });
  } catch (const std::exception& error) {
    set_status(
        QStringLiteral("Cannot read the reference: %1").arg(QString::fromUtf8(error.what())));
    return;
  }
  reference_job_ = std::move(job);
  timer_.start();
  set_status(QStringLiteral("Reading %1…").arg(file.fileName()));
}

void MixCheckController::finish_reference() {
  auto job = std::move(reference_job_);
  reference_job_.reset();
  if (!job->result) {
    set_status(QStringLiteral("Reference: no result"));
    return;
  }
  auto& result = *job->result;
  if (!result) {
    set_status(QStringLiteral("Reference: %1").arg(text(result.error().message)));
    return;
  }
  reference_track_ = std::move(result).value();
  reference_name_ = text(reference_track_->name);
  set_status(report_ ? QStringLiteral("Compared with %1").arg(reference_name_)
                     : QStringLiteral("Run Mix Check to compare with %1").arg(reference_name_));
}

void MixCheckController::clearReference() {
  reference_track_.reset();
  reference_name_.clear();
  emit changed();
}

bool MixCheckController::wait(int timeout_ms) {
  QElapsedTimer elapsed;
  elapsed.start();
  while ((job_ || reference_job_) && elapsed.elapsed() < timeout_ms) {
    if ((job_ && job_->done.load(std::memory_order_acquire)) ||
        (reference_job_ && reference_job_->done.load(std::memory_order_acquire)))
      poll();
    else {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  }
  return !job_ && !reference_job_;
}

} // namespace oscillate::quick
