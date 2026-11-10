#include "master_assistant_controller.hpp"

#include "oscillate/application/mix_check.hpp"
#include "oscillate/application/project_session.hpp"
#include "oscillate/native_dsp/tonal_targets.hpp"
#include "preview_pipewire_output.hpp"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>

#include <algorithm>
#include <cmath>
#include <exception>
#include <filesystem>
#include <string>
#include <utility>

namespace oscillate::quick {
namespace {

namespace assistant = application::master_assistant;
namespace core = native_dsp::assistant;
namespace tonal = native_dsp::tonal;
using foundation::ErrorCode;

constexpr std::string_view custom_id = "custom";
constexpr double custom_default_lufs = -12.0;
constexpr double custom_ceiling_dbtp = -1.0;
// Readouts' precision.
constexpr int level_digits = 1;
constexpr int peak_digits = 2;

QString text(const std::string& s) { return QString::fromStdString(s); }
QString fixed(double value, int digits) { return QString::number(value, 'f', digits); }

// A device's few settings that say what it does, for its card.
QString settings_of(const core::Device& device) {
  const auto value = [&](std::string_view id) {
    const auto found = std::ranges::find(device.values, id, &std::pair<std::string, double>::first);
    return found == device.values.end() ? 0.0 : found->second;
  };
  const auto db = [](double v) {
    return (v >= 0.0 ? QStringLiteral("+") : QString()) + fixed(v, 1) + " dB";
  };
  if (device.engine == "ceiling")
    return QStringLiteral("Gain %1 · Ceiling %2 dBTP · Transients %3% · Clip %4")
        .arg(db(value("ceiling.gain")), fixed(value("ceiling.ceiling"), 1),
             fixed(value("ceiling.transients") * 100.0, 0), db(value("ceiling.clip")));
  if (device.engine == "press")
    return QStringLiteral("Threshold %1 dB · %2:1 · Attack %3 ms · Release %4 ms")
        .arg(fixed(value("press.threshold"), 1), fixed(value("press.ratio"), 1),
             fixed(value("press.attack"), 0), fixed(value("press.release"), 0));
  if (device.engine == "trim")
    return QStringLiteral("Gain %1 · Width %2%")
        .arg(db(value("trim.gain")), fixed(value("trim.width") * 100.0, 0));
  if (device.engine == "vista")
    return QStringLiteral("Mono under %1 Hz").arg(fixed(value("vista.mono_freq"), 0));
  if (device.engine == "keel")
    return QStringLiteral("Amount %1% · ±%2 dB · %3 s")
        .arg(fixed(value("keel.amount") * 100.0, 0), fixed(value("keel.max_boost"), 0),
             fixed(value("keel.speed"), 0));
  if (device.engine == "shape") {
    int bands = 0;
    for (int b = 1; b <= 8; ++b)
      bands += value("shape.on_" + std::to_string(b)) != 0.0 ? 1 : 0;
    return QStringLiteral("%1 band%2").arg(bands).arg(bands == 1 ? "" : "s");
  }
  if (device.engine == "strata")
    return QStringLiteral("Three bands · linear phase · makeup %1").arg(db(value("strata.output")));
  if (device.engine == "strike")
    return QStringLiteral("Lows' attack %1 · mids' %2")
        .arg(db(value("strike.band_1.attack")), db(value("strike.band_2.attack")));
  if (device.engine == "gild")
    return QStringLiteral("Level-matched harmonics");
  return {};
}

QStringList lines(const std::vector<std::string>& from) {
  QStringList out;
  for (const auto& line : from)
    out.push_back(text(line));
  return out;
}

} // namespace

MasterAssistantController::MasterAssistantController(Hooks hooks, QObject* parent)
    : QObject(parent), hooks_(std::move(hooks)),
      loudness_(
          QString::fromUtf8(core::loudness_presets.front().id.data(),
                            static_cast<qsizetype>(core::loudness_presets.front().id.size()))),
      custom_lufs_(custom_default_lufs) {
  const auto targets = tonal::factory_targets();
  if (!targets.empty())
    genre_ = text(targets.front().id);
  timer_.setInterval(poll_ms);
  connect(&timer_, &QTimer::timeout, this, &MasterAssistantController::poll);
}

MasterAssistantController::~MasterAssistantController() {
  stopAudition();
  if (progress_)
    progress_->cancel.store(true, std::memory_order_release);
}

// ---- Goals ---------------------------------------------------------------------------

core::Goals MasterAssistantController::goals() const noexcept {
  core::Goals out;
  out.character = character_;
  for (const auto& preset : core::loudness_presets) {
    if (loudness_ == QLatin1String(preset.id.data(), static_cast<qsizetype>(preset.id.size()))) {
      out.lufs = preset.lufs;
      out.ceiling_dbtp = preset.ceiling_dbtp;
      return out;
    }
  }
  out.lufs = custom_lufs_;
  out.ceiling_dbtp = custom_ceiling_dbtp;
  return out;
}

QVariantList MasterAssistantController::loudness_targets() const {
  QVariantList out;
  for (const auto& preset : core::loudness_presets)
    out.push_back(QVariantMap{
        {QStringLiteral("id"),
         QString::fromUtf8(preset.id.data(), static_cast<qsizetype>(preset.id.size()))},
        {QStringLiteral("name"),
         QString::fromUtf8(preset.name.data(), static_cast<qsizetype>(preset.name.size()))},
        {QStringLiteral("lufs"), preset.lufs},
        {QStringLiteral("ceiling"), preset.ceiling_dbtp}});
  out.push_back(
      QVariantMap{{QStringLiteral("id"),
                   QString::fromLatin1(custom_id.data(), static_cast<qsizetype>(custom_id.size()))},
                  {QStringLiteral("name"), QStringLiteral("Custom")},
                  {QStringLiteral("lufs"), custom_lufs_},
                  {QStringLiteral("ceiling"), custom_ceiling_dbtp}});
  return out;
}

void MasterAssistantController::set_loudness(const QString& id) {
  if (id == loudness_)
    return;
  loudness_ = id;
  goals_changed();
}

void MasterAssistantController::set_custom_lufs(double lufs) {
  const double clamped = std::clamp(lufs, core::custom_lufs_min, core::custom_lufs_max);
  if (clamped == custom_lufs_)
    return;
  custom_lufs_ = clamped;
  goals_changed();
}

QStringList MasterAssistantController::characters() const {
  QStringList out;
  for (const auto name : core::character_names)
    out.push_back(QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size())));
  return out;
}

QString MasterAssistantController::character() const {
  const auto name = core::character_names[static_cast<std::size_t>(character_)];
  return QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size()));
}

void MasterAssistantController::set_character(const QString& name) {
  for (std::size_t c = 0; c < core::character_count; ++c) {
    const auto candidate = core::character_names[c];
    if (name == QLatin1String(candidate.data(), static_cast<qsizetype>(candidate.size())) &&
        static_cast<core::Character>(c) != character_) {
      character_ = static_cast<core::Character>(c);
      goals_changed();
      return;
    }
  }
}

QVariantList MasterAssistantController::genres() const {
  QVariantList out;
  for (const auto& target : tonal::factory_targets())
    out.push_back(QVariantMap{{QStringLiteral("id"), text(target.id)},
                              {QStringLiteral("name"), text(target.name)}});
  return out;
}

void MasterAssistantController::set_genre(const QString& id) {
  if (id == genre_ || tonal::find_factory_target(id.toStdString()) == nullptr)
    return;
  genre_ = id;
  goals_changed();
}

QVariantList MasterAssistantController::references() const {
  QVariantList out;
  for (const auto& path : reference_paths_)
    out.push_back(QVariantMap{{QStringLiteral("name"), QFileInfo(text(path)).completeBaseName()},
                              {QStringLiteral("path"), text(path)},
                              {QStringLiteral("measured"), reference_cache_.contains(path)}});
  return out;
}

QString MasterAssistantController::reference_name() const {
  if (!reference_paths_.empty())
    return reference_paths_.size() == 1
               ? QFileInfo(text(reference_paths_.front())).completeBaseName()
               : QStringLiteral("%1 reference tracks").arg(reference_paths_.size());
  const auto* target = tonal::find_factory_target(genre_.toStdString());
  return target == nullptr ? QString() : text(target->name);
}

void MasterAssistantController::chooseReferences() {
  const auto chosen = QFileDialog::getOpenFileNames(
      nullptr, QStringLiteral("Choose finished tracks to master toward"), QDir::homePath(),
      QStringLiteral("Audio files (*.wav *.wave *.aiff *.aif *.flac *.mp3);;All files (*)"));
  for (const auto& path : chosen)
    addReference(path);
}

void MasterAssistantController::addReference(const QString& path) {
  const QFileInfo file(path);
  if (!file.isFile()) {
    set_status(QStringLiteral("Choose an audio file to master toward"));
    return;
  }
  const auto absolute = file.absoluteFilePath().toStdString();
  if (std::ranges::find(reference_paths_, absolute) != reference_paths_.end())
    return;
  reference_paths_.push_back(absolute);
  goals_changed();
}

void MasterAssistantController::removeReference(int index) {
  if (index < 0 || static_cast<std::size_t>(index) >= reference_paths_.size())
    return;
  reference_paths_.erase(reference_paths_.begin() + index);
  goals_changed();
}

bool MasterAssistantController::loop_available() const {
  return hooks_.loop && hooks_.loop().has_value();
}

void MasterAssistantController::set_use_loop(bool on) {
  if (on == use_loop_)
    return;
  use_loop_ = on;
  // Another range is another measurement.
  measured_.reset();
  goals_changed();
}

// A goal moved: the proposal, or the verdict on an accepted one, no longer
// answers it. Analyse plans again.
void MasterAssistantController::goals_changed() {
  // A run for the old goals no longer answers them: its result is dropped.
  if (job_.running()) {
    if (progress_)
      progress_->cancel.store(true, std::memory_order_release);
    ++generation_;
    status_.set(QStringLiteral("The goals changed: Analyse again"));
  }
  // Nothing proposed or verified for the old goals stands for the new ones.
  stopAudition();
  proposed_.reset();
  accepted_ = false;
  verification_.clear();
  verified_on_target_ = false;
  emit changed();
  // After an accept the mix has changed: the measurement is stale.
  project_changed();
}

// ---- Running -------------------------------------------------------------------------

void MasterAssistantController::analyse() { start(false); }

void MasterAssistantController::start(bool verify) {
  auto* session = hooks_.session ? hooks_.session() : nullptr;
  if (session == nullptr) {
    set_status(QStringLiteral("Open a project first"));
    return;
  }
  if (const auto why = hooks_.blocked ? hooks_.blocked() : QString(); !why.isEmpty()) {
    set_status(why);
    return;
  }
  if (job_.running())
    return;
  stopAudition();
  std::optional<assistant::Range> range;
  if (use_loop_ && hooks_.loop)
    range = hooks_.loop();
  // The measurement stands while the mix does.
  auto measured = measured_;
  if (measured && (stale_ || !application::mix_check::same_mix(session->snapshot(),
                                                               *measured->pinned->snapshot)))
    measured.reset();
  std::optional<application::PinnedSnapshot> pinned;
  if (!measured || verify) {
    auto pin = session->exports().pin_snapshot();
    if (!pin) {
      set_status(QStringLiteral("The assistant cannot start: %1").arg(text(pin.error().message)));
      return;
    }
    pinned = std::move(pin).value();
  }
  auto progress = std::make_shared<Progress>();
  const auto report = [progress](double fraction, const std::string& stage) {
    if (fraction >= 0.0)
      progress->fraction.store(fraction, std::memory_order_release);
    {
      const std::lock_guard lock(progress->mutex);
      progress->stage = stage;
    }
    return !progress->cancel.load(std::memory_order_acquire);
  };
  const auto goals = this->goals();
  const auto genre = genre_.toStdString();
  auto cache = reference_cache_;
  auto paths = reference_paths_;
  auto work = [=, pinned = std::move(pinned)]() mutable -> Done {
    Done done;
    const auto fail = [&](foundation::Error error) {
      done.result = foundation::Result<void>::failure(std::move(error));
      return std::move(done);
    };
    if (!pinned && (verify || !measured))
      return fail({ErrorCode::internal, "No snapshot to render"});
    if (verify) {
      auto after = assistant::verify(*pinned, range, report);
      if (!after)
        return fail(after.error());
      done.verified = std::move(after).value();
      return done;
    }
    // The reference: the tracks given, measured once each, or the genre.
    core::Reference reference;
    if (!paths.empty()) {
      std::vector<core::Analysis> tracks;
      for (const auto& path : paths) {
        if (!cache.contains(path)) {
          (void)report(0.0, "Measuring " + std::filesystem::path(path).filename().string());
          auto analysis = assistant::measure_reference(
              path, [&] { return progress->cancel.load(std::memory_order_acquire); });
          if (!analysis)
            return fail(analysis.error());
          cache.emplace(path, std::move(analysis).value());
        }
        tracks.push_back(cache.at(path));
      }
      auto from = core::reference_from(paths.size() == 1
                                           ? std::filesystem::path(paths.front()).stem().string()
                                           : std::to_string(paths.size()) + " reference tracks",
                                       tracks);
      if (!from)
        return fail(from.error());
      reference = std::move(from).value();
    } else {
      const auto* target = tonal::find_factory_target(genre);
      if (target == nullptr)
        return fail({ErrorCode::not_found, "No genre target " + genre});
      reference = core::reference_from(*target);
    }
    done.references = cache;
    if (!measured && pinned) {
      auto fresh = assistant::measure(std::move(*pinned), range, report);
      if (!fresh)
        return fail(fresh.error());
      done.measured = std::move(fresh).value();
    }
    const auto& mix = done.measured ? *done.measured : *measured;
    auto proposed = assistant::propose(mix, reference, goals, report);
    if (!proposed)
      return fail(proposed.error());
    done.proposed = std::move(proposed).value();
    return done;
  };
  const auto failed = [](const std::string& what) {
    Done done;
    done.result = foundation::Result<void>::failure({ErrorCode::internal, what});
    return done;
  };
  if (const auto refused = job_.start(++generation_, std::move(work), failed)) {
    set_status(QStringLiteral("Cannot start the assistant: %1").arg(text(*refused)));
    return;
  }
  progress_ = std::move(progress);
  if (!verify) {
    accepted_ = false;
    verification_.clear();
    verified_on_target_ = false;
  }
  status_.set(verify ? QStringLiteral("Measuring the master as it now renders…")
                     : QStringLiteral("Measuring the mix and planning a master…"));
  timer_.start();
  emit changed();
  emit progressChanged();
}

void MasterAssistantController::cancel() {
  if (progress_)
    progress_->cancel.store(true, std::memory_order_release);
}

double MasterAssistantController::progress() const noexcept {
  return progress_ ? progress_->fraction.load(std::memory_order_acquire) : 0.0;
}

QString MasterAssistantController::stage() const {
  if (!progress_)
    return {};
  const std::lock_guard lock(progress_->mutex);
  return text(progress_->stage);
}

void MasterAssistantController::poll() {
  emit progressChanged();
  if (job_.finished())
    finish();
}

void MasterAssistantController::finish() {
  timer_.stop();
  auto taken = job_.take();
  if (!taken || taken->generation != generation_) {
    emit changed();
    return;
  }
  auto& done = taken->result;
  for (auto& [path, analysis] : done.references)
    reference_cache_.insert_or_assign(path, std::move(analysis));
  if (!done.result) {
    set_status(done.result.error().code == ErrorCode::cancelled
                   ? QStringLiteral("Stopped")
                   : text(done.result.error().message));
    emit changed();
    project_changed();
    return;
  }
  if (done.verified) {
    const auto& goals = proposed_ ? proposed_->proposal.goals : this->goals();
    const core::Reference reference = reference_ ? *reference_ : core::Reference{};
    const auto after = core::outcome(*done.verified, reference);
    const auto before = proposed_ ? proposed_->before : after;
    verification_ = lines(core::outcome_lines(before, after, goals));
    verified_on_target_ = core::meets(after, goals);
    status_.set(verified_on_target_ ? QStringLiteral("The master meets the target")
                                    : QStringLiteral("The master misses the target: see why"));
    emit changed();
    project_changed();
    return;
  }
  if (done.measured)
    measured_ = std::make_shared<const assistant::Measured>(std::move(*done.measured));
  stale_ = false;
  proposed_ = std::move(done.proposed);
  // The reference the proposal was planned toward, for the verification.
  if (!reference_paths_.empty()) {
    std::vector<core::Analysis> tracks;
    for (const auto& path : reference_paths_)
      if (const auto found = reference_cache_.find(path); found != reference_cache_.end())
        tracks.push_back(found->second);
    auto from = core::reference_from(reference_name().toStdString(), tracks);
    reference_ = from ? std::optional(std::move(from).value()) : std::nullopt;
  } else if (const auto* target = tonal::find_factory_target(genre_.toStdString())) {
    reference_ = core::reference_from(*target);
  }
  device_kept_.assign(proposed_ ? proposed_->proposal.devices.size() : 0, true);
  nudge_kept_.assign(proposed_ ? proposed_->proposal.nudges.size() : 0, true);
  status_.set(QStringLiteral("Proposed: listen, then accept or reject"));
  emit changed();
  // The mix may have changed, or another project opened, meanwhile.
  project_changed();
}

bool MasterAssistantController::wait() {
  if (!job_.running())
    return false;
  job_.wait();
  finish();
  return true;
}

void MasterAssistantController::set_status(QString text) {
  if (status_.set(std::move(text)))
    emit changed();
}

void MasterAssistantController::project_changed() {
  auto* session = hooks_.session ? hooks_.session() : nullptr;
  if (!measured_ || job_.running())
    return;
  if (session == nullptr || session->snapshot().id != measured_->pinned->snapshot->id) {
    stopAudition();
    measured_.reset();
    proposed_.reset();
    accepted_ = false;
    verification_.clear();
    stale_ = false;
    emit changed();
    return;
  }
  // Accepting changes the master on purpose; anything else makes the
  // measurement stale.
  if (accepted_)
    return;
  const bool stale =
      !application::mix_check::same_mix(session->snapshot(), *measured_->pinned->snapshot);
  if (stale != stale_) {
    stale_ = stale;
    emit changed();
  }
}

// ---- Proposal ------------------------------------------------------------------------

QString MasterAssistantController::phase() const {
  if (job_.running())
    return QStringLiteral("working");
  if (accepted_)
    return verification_.isEmpty() ? QStringLiteral("accepted") : QStringLiteral("verified");
  if (proposed_)
    return QStringLiteral("proposed");
  return QStringLiteral("idle");
}

assistant::Selection MasterAssistantController::selection() const {
  return {device_kept_, nudge_kept_};
}

void MasterAssistantController::setDeviceAccepted(int index, bool accepted) {
  if (index < 0 || static_cast<std::size_t>(index) >= device_kept_.size() || accepted_)
    return;
  device_kept_[static_cast<std::size_t>(index)] = accepted;
  emit changed();
}

void MasterAssistantController::setNudgeAccepted(int index, bool accepted) {
  if (index < 0 || static_cast<std::size_t>(index) >= nudge_kept_.size() || accepted_)
    return;
  nudge_kept_[static_cast<std::size_t>(index)] = accepted;
  emit changed();
}

void MasterAssistantController::accept() {
  auto* session = hooks_.session ? hooks_.session() : nullptr;
  if (session == nullptr || !proposed_ || !measured_ || accepted_)
    return;
  if (hooks_.can_edit && !hooks_.can_edit())
    return;
  const auto& proposal = proposed_->proposal;
  const auto measured = measured_;
  stopAudition();
  auto done = assistant::accept(*session, proposal, selection(), *measured->pinned->snapshot);
  if (!done) {
    set_status(text(done.error().message));
    return;
  }
  accepted_ = true;
  if (hooks_.commit)
    hooks_.commit();
  status_.set(QStringLiteral("Accepted as one undo step: measuring the master…"));
  emit changed();
  start(true);
}

void MasterAssistantController::reject() {
  stopAudition();
  proposed_.reset();
  accepted_ = false;
  verification_.clear();
  device_kept_.clear();
  nudge_kept_.clear();
  status_.set(QStringLiteral("Rejected: the project is as it was"));
  emit changed();
}

QVariantMap MasterAssistantController::measured() const {
  if (!measured_)
    return {};
  const auto& mix = measured_->mix;
  QVariantMap out;
  out[QStringLiteral("integrated")] = fixed(mix.lufs(), level_digits);
  out[QStringLiteral("shortTerm")] = fixed(mix.max_short_term_lufs, level_digits);
  out[QStringLiteral("momentary")] = fixed(mix.max_momentary_lufs, level_digits);
  out[QStringLiteral("truePeak")] = fixed(mix.true_peak_dbtp, peak_digits);
  out[QStringLiteral("crest")] = fixed(mix.crest_db, level_digits);
  out[QStringLiteral("lowSide")] = fixed(mix.low_side_db, level_digits);
  out[QStringLiteral("spread")] = fixed(mix.tonal_spread_db, level_digits);
  out[QStringLiteral("seconds")] = mix.seconds();
  QVariantList regions;
  for (std::size_t r = 0; r < core::regions; ++r) {
    const auto& region = mix.region[r];
    regions.push_back(
        QVariantMap{{QStringLiteral("name"),
                     QString::fromUtf8(core::region_names[r].data(),
                                       static_cast<qsizetype>(core::region_names[r].size()))},
                    {QStringLiteral("crest"), fixed(region.crest_db, level_digits)},
                    {QStringLiteral("onsets"), fixed(region.onsets_per_second, level_digits)},
                    {QStringLiteral("width"), fixed(region.width_db, level_digits)}});
  }
  out[QStringLiteral("regions")] = regions;
  if (reference_) {
    const auto distance = core::tonal_distance(mix.tonal, reference_->target.median);
    out[QStringLiteral("tonalDistance")] = fixed(distance.rms_db, level_digits);
    const auto diff = core::tonal_difference(mix.tonal, reference_->target.median);
    QVariantList curve;
    for (std::size_t b = 0; b < tonal::bands; ++b) {
      const double low = reference_->target.low[b] - reference_->target.median[b];
      const double high = reference_->target.high[b] - reference_->target.median[b];
      curve.push_back(QVariantMap{{QStringLiteral("hz"), tonal::band_hz(b)},
                                  {QStringLiteral("mix"), diff[b]},
                                  {QStringLiteral("low"), low},
                                  {QStringLiteral("high"), high}});
    }
    out[QStringLiteral("curve")] = curve;
    out[QStringLiteral("curveRange")] = curve_range_db;
  }
  return out;
}

QVariantList MasterAssistantController::devices() const {
  QVariantList out;
  if (!proposed_)
    return out;
  const auto& devices = proposed_->proposal.devices;
  for (std::size_t d = 0; d < devices.size(); ++d)
    out.push_back(
        QVariantMap{{QStringLiteral("index"), static_cast<int>(d)},
                    {QStringLiteral("engine"), text(devices[d].engine)},
                    {QStringLiteral("name"), text(devices[d].name)},
                    {QStringLiteral("reason"), text(devices[d].reason)},
                    {QStringLiteral("settings"), settings_of(devices[d])},
                    {QStringLiteral("accepted"), d < device_kept_.size() && device_kept_[d]}});
  return out;
}

QVariantList MasterAssistantController::nudges() const {
  QVariantList out;
  if (!proposed_)
    return out;
  const auto& nudges = proposed_->proposal.nudges;
  for (std::size_t n = 0; n < nudges.size(); ++n) {
    const auto role = core::role_names[nudges[n].role];
    out.push_back(
        QVariantMap{{QStringLiteral("index"), static_cast<int>(n)},
                    {QStringLiteral("role"),
                     QString::fromUtf8(role.data(), static_cast<qsizetype>(role.size()))},
                    {QStringLiteral("gain"), nudges[n].gain_db},
                    {QStringLiteral("reason"), text(nudges[n].reason)},
                    {QStringLiteral("accepted"), n < nudge_kept_.size() && nudge_kept_[n]}});
  }
  return out;
}

QStringList MasterAssistantController::notes() const {
  return proposed_ ? lines(proposed_->proposal.notes) : QStringList{};
}

QStringList MasterAssistantController::prediction() const {
  if (!proposed_)
    return {};
  return lines(
      core::outcome_lines(proposed_->before, proposed_->predicted, proposed_->proposal.goals));
}

// ---- Audition ------------------------------------------------------------------------

void MasterAssistantController::startAudition() {
  if (!proposed_ || !measured_ || output_)
    return;
  const auto preview = hooks_.preview ? hooks_.preview() : std::optional<Preview>(Preview{});
  if (!preview) {
    set_status(QStringLiteral("The preview device is unavailable"));
    return;
  }
  const auto rate = measured_->pinned->snapshot->sample_rate;
  player_ = std::make_unique<assistant::Audition>(
      proposed_->current, core::matched_gain_db(proposed_->current_lufs, proposed_->proposed_lufs),
      proposed_->proposed, core::matched_gain_db(proposed_->proposed_lufs, proposed_->current_lufs),
      rate);
  cursor_ = std::make_unique<audio_io::TransportCursor>();
  const audio_io::StereoRenderSource source{player_.get(), &assistant::Audition::render_source,
                                            nullptr};
  output_ = preview->factory ? preview->factory(source, *cursor_)
                             : std::make_unique<PreviewPipeWireOutput>(source, *cursor_);
  if (!output_) {
    player_.reset();
    cursor_.reset();
    set_status(QStringLiteral("The preview device is unavailable"));
    return;
  }
  auto started = output_->start(rate, preview->buffer_frames, preview->target_serial);
  if (!started) {
    output_.reset();
    player_.reset();
    cursor_.reset();
    set_status(QStringLiteral("Cannot play the A/B: %1").arg(text(started.error().message)));
    return;
  }
  emit changed();
}

void MasterAssistantController::stopAudition() {
  if (output_)
    output_->stop();
  output_.reset();
  cursor_.reset();
  player_.reset();
  emit changed();
}

void MasterAssistantController::hearProposal(bool proposal) {
  if (player_)
    player_->choose_proposal(proposal);
  emit changed();
}

bool MasterAssistantController::hearing_proposal() const {
  return player_ && player_->proposal_chosen();
}

QVariantMap MasterAssistantController::audition() const {
  if (!proposed_)
    return {};
  const double current = core::matched_gain_db(proposed_->current_lufs, proposed_->proposed_lufs);
  const double proposed = core::matched_gain_db(proposed_->proposed_lufs, proposed_->current_lufs);
  const auto rate = measured_ ? measured_->pinned->snapshot->sample_rate : 1U;
  return {{QStringLiteral("currentLufs"), fixed(proposed_->current_lufs, level_digits)},
          {QStringLiteral("proposedLufs"), fixed(proposed_->proposed_lufs, level_digits)},
          {QStringLiteral("currentGain"), fixed(current, level_digits)},
          {QStringLiteral("proposedGain"), fixed(proposed, level_digits)},
          {QStringLiteral("seconds"),
           static_cast<double>(proposed_->window.frames) / static_cast<double>(rate)}};
}

} // namespace oscillate::quick
