#include "crate_controller.hpp"

#include "oscillate/application/crate_chops.hpp"
#include "oscillate/foundation/value_text.hpp"
#include "oscillate/native_dsp/synth/crate.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QPointer>
#include <QUrl>
#include <QVariantMap>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <iterator>
#include <limits>
#include <system_error>
#include <utility>

namespace oscillate::quick {
namespace {

namespace chops = application::crate;
namespace facts = native_dsp::synth::crate;

// A sample's bars are counted in 4/4: the label names a loop's tempo, not
// its meter.
constexpr double sample_beats_per_bar = 4.0;
constexpr double seconds_per_minute = 60.0;
using native_dsp::EngineDescriptor;
using native_dsp::synth::SynthMediaAsset;

// The global parameters the panel writes (spec 09 §2.1).
constexpr std::string_view layout_id = "crate.layout";
constexpr std::string_view root_id = chops::root_parameter;
constexpr std::string_view era_id = "crate.era";
constexpr std::string_view bits_id = "crate.bits";
constexpr std::string_view rate_id = "crate.rate";
constexpr std::string_view drive_id = "crate.drive";
constexpr std::string_view reverse_control = "reverse";

constexpr std::string_view mode_transient = "transient";
constexpr std::string_view mode_grid = "grid";
constexpr std::string_view mode_equal = "equal";
constexpr std::string_view mode_manual = "manual";

// Note names as the panel shows them: MIDI 60 is C3, so pad 1 (36) is C1.
constexpr std::array<std::string_view, 12> note_names{"C",  "C♯", "D",  "D♯", "E",  "F",
                                                      "F♯", "G",  "G♯", "A",  "A♯", "B"};
constexpr int semitones_per_octave = 12;
constexpr int octave_offset = 2;

QString text(std::string_view view) {
  return QString::fromUtf8(view.data(), static_cast<qsizetype>(view.size()));
}

std::string local_file(const QString& path_or_url) {
  const QUrl url(path_or_url);
  return (url.isLocalFile() ? url.toLocalFile() : path_or_url).toStdString();
}

const project::NativeLabel* find_label(const project::NativeSynthDocument& document,
                                       std::string_view id) {
  const auto found = std::ranges::find(document.labels, id, &project::NativeLabel::id);
  return found == document.labels.end() ? nullptr : &*found;
}

std::optional<std::size_t> parameter_index(const EngineDescriptor& engine, std::string_view id) {
  return native_dsp::find_parameter(engine, id);
}

void set_value(const EngineDescriptor& engine, std::vector<double>& values, std::string_view id,
               double value) {
  if (const auto index = parameter_index(engine, id); index && *index < values.size())
    values[*index] = value;
}

// A choice parameter's value for choice `choice`.
std::optional<double> choice_value(const EngineDescriptor& engine, std::string_view id,
                                   std::size_t choice) {
  const auto index = parameter_index(engine, id);
  if (!index)
    return std::nullopt;
  return engine.parameters[*index].minimum + static_cast<double>(choice);
}

std::shared_ptr<const SynthMediaAsset> builtin_asset(std::string_view name) {
  const auto mono = facts::builtin_sample(name);
  if (mono.empty())
    return nullptr;
  auto asset = std::make_shared<SynthMediaAsset>();
  asset->source_rate = facts::builtin_sample_rate;
  asset->frames.reserve(mono.size());
  for (const float sample : mono)
    asset->frames.push_back({sample, sample});
  return asset;
}

QString note_name(int key) {
  if (key < 0)
    return {};
  // A whole octave number; the formatter gives a negative one its minus sign.
  const int octave = key / semitones_per_octave - octave_offset;
  return text(note_names[static_cast<std::size_t>(key % semitones_per_octave)]) +
         QString::fromStdString(foundation::format_number(octave, 0));
}

} // namespace

CrateController::CrateController(const EngineDescriptor& engine, NativeSynthPanelAdapter& adapter,
                                 std::function<void()> after_edit, QObject* parent)
    : QObject(parent), engine_(engine), adapter_(adapter), after_edit_(std::move(after_edit)) {
  // A decode landing refreshes every view, as an edit does.
  adapter_.set_media_listener([this] { edited(); });
  refresh();
}

CrateController::~CrateController() { adapter_.set_media_listener({}); }

double CrateController::value_of(std::string_view id) const {
  const auto index = parameter_index(engine_, id);
  return index && *index < values_.size() ? values_[*index] : 0.0;
}

QString CrateController::sample_name() const {
  if (!has_sample())
    return {};
  if (const auto* label = find_label(document_, facts::sample_name_label))
    return QString::fromStdString(label->text);
  if (media_.builtin)
    return QString::fromStdString(media_.identity);
  return QStringLiteral("Sample");
}

double CrateController::sample_seconds() const noexcept {
  return audio_ && audio_->source_rate > 0
             ? static_cast<double>(audio_->frames.size()) / static_cast<double>(audio_->source_rate)
             : 0.0;
}

int CrateController::sample_rate() const noexcept {
  return audio_ ? static_cast<int>(audio_->source_rate) : 0;
}

double CrateController::sample_bars() const noexcept {
  const double tempo = bpm();
  return tempo > 0.0 ? sample_seconds() * tempo / seconds_per_minute / sample_beats_per_bar : 0.0;
}

double CrateController::bpm() const noexcept {
  const auto* label = find_label(document_, facts::bpm_label);
  if (label == nullptr)
    return 0.0;
  double value = 0.0;
  const auto* end = label->text.data() + label->text.size();
  const auto [last, error] = std::from_chars(label->text.data(), end, value);
  if (error != std::errc{} || last != end || !std::isfinite(value) ||
      value < facts::min_sample_bpm || value > facts::max_sample_bpm)
    return 0.0;
  return value;
}

QString CrateController::key_name() const {
  return has_sample() ? note_name(static_cast<int>(std::lround(value_of(root_id)))) : QString{};
}

void CrateController::set_selected(int index) {
  const int last = std::max(0, static_cast<int>(regions_.size()) - 1);
  const int clamped = std::clamp(index, 0, last);
  if (clamped == selected_)
    return;
  selected_ = clamped;
  emit changed();
}

void CrateController::set_bank(int bank) {
  const int clamped =
      std::clamp(bank, 0, static_cast<int>(facts::max_chops / facts::bank_size) - 1);
  if (clamped == bank_)
    return;
  bank_ = clamped;
  emit changed();
}

void CrateController::set_chop_mode(const QString& mode) {
  const auto id = mode.toStdString();
  if (id != mode_transient && id != mode_grid && id != mode_equal && id != mode_manual)
    return;
  if (mode == chop_mode_)
    return;
  chop_mode_ = mode;
  emit changed();
}

void CrateController::set_sensitivity(double sensitivity) {
  if (!std::isfinite(sensitivity))
    return;
  const double clamped = std::clamp(sensitivity, 0.0, 1.0);
  if (clamped == sensitivity_)
    return;
  sensitivity_ = clamped;
  emit changed();
}

void CrateController::set_grid_division(int division) {
  if (division != static_cast<int>(chops::grid_division_quarter) &&
      division != static_cast<int>(chops::grid_division_eighth) &&
      division != static_cast<int>(chops::grid_division_sixteenth))
    return;
  if (division == grid_division_)
    return;
  grid_division_ = division;
  emit changed();
}

void CrateController::set_equal_count(int count) {
  const int clamped = std::clamp(count, 1, static_cast<int>(chops::max_crate_chops));
  if (clamped == equal_count_)
    return;
  equal_count_ = clamped;
  emit changed();
}

void CrateController::refuse(QString message) {
  message_ = std::move(message);
  emit changed();
}

void CrateController::edited() {
  if (after_edit_)
    after_edit_();
  else
    refresh();
}

void CrateController::refresh() {
  auto document = adapter_.document();
  auto values = adapter_.values();
  auto media = adapter_.media(facts::sample_slot);
  auto audio = media.audio;
  if (media.builtin) {
    if (builtin_identity_ != media.identity) {
      builtin_identity_ = media.identity;
      builtin_audio_ = builtin_asset(media.identity);
    }
    audio = builtin_audio_;
  }
  const bool busy = importing_ || adapter_.media_import_busy() || media.decoding;
  const bool differs = document != document_ || values != values_ || audio != audio_ ||
                       busy != busy_ || media.message != media_.message ||
                       media.identity != media_.identity;
  if (!differs)
    return;
  if (!media.message.empty() && media.message != media_.message)
    message_ = QString::fromStdString(media.message);
  document_ = std::move(document);
  values_ = std::move(values);
  media_ = std::move(media);
  audio_ = std::move(audio);
  busy_ = busy;
  regions_.clear();
  for (const auto& region : document_.regions) {
    if (region.media == facts::sample_slot)
      regions_.push_back(region);
  }
  std::ranges::stable_sort(regions_, {}, &project::NativeRegion::start_frame);
  selected_ = std::clamp(selected_, 0, std::max(0, static_cast<int>(regions_.size()) - 1));
  rebuild_chops();
  emit changed();
}

void CrateController::rebuild_chops() {
  chops_.clear();
  if (!audio_ || audio_->frames.empty() || audio_->source_rate == 0)
    return;
  const auto frames = static_cast<double>(audio_->frames.size());
  const auto rate = static_cast<double>(audio_->source_rate);
  for (std::size_t index = 0; index < regions_.size(); ++index) {
    const auto& region = regions_[index];
    const auto start = static_cast<double>(region.start_frame);
    const auto end =
        index + 1 < regions_.size() ? static_cast<double>(regions_[index + 1].start_frame) : frames;
    const int key = static_cast<int>(facts::first_pad_key + index);
    chops_.push_back(
        QVariantMap{{QStringLiteral("index"), static_cast<int>(index)},
                    {QStringLiteral("slot"), static_cast<int>(region.slot)},
                    {QStringLiteral("start"), start / frames},
                    {QStringLiteral("end"), end / frames},
                    {QStringLiteral("start_seconds"), start / rate},
                    {QStringLiteral("length_seconds"), (end - start) / rate},
                    {QStringLiteral("tag"), QString::fromStdString(region.tag)},
                    {QStringLiteral("name"), QString::fromStdString(region.name)},
                    {QStringLiteral("key"), key},
                    {QStringLiteral("key_name"), note_name(key)},
                    {QStringLiteral("bank"), static_cast<int>(index / facts::bank_size)}});
  }
}

QVariantList CrateController::peaks(int columns) const {
  QVariantList result;
  if (!audio_ || audio_->frames.empty() || columns <= 0)
    return result;
  const auto count = static_cast<std::size_t>(std::min(columns, max_peak_columns));
  const auto& frames = audio_->frames;
  std::vector<float> peak(count, 0.0F);
  for (std::size_t column = 0; column < count; ++column) {
    const auto from = column * frames.size() / count;
    const auto to = std::max(from + 1, (column + 1) * frames.size() / count);
    for (std::size_t frame = from; frame < std::min(to, frames.size()); ++frame)
      peak[column] =
          std::max({peak[column], std::abs(frames[frame].left), std::abs(frames[frame].right)});
  }
  const float loudest = *std::ranges::max_element(peak);
  const float scale = loudest > 0.0F ? 1.0F / loudest : 0.0F;
  result.reserve(static_cast<qsizetype>(count));
  for (const float value : peak)
    result.push_back(static_cast<double>(value * scale));
  return result;
}

QString CrateController::chop_parameter(int index, const QString& control) const {
  if (index < 0 || static_cast<std::size_t>(index) >= regions_.size())
    return {};
  const auto id = control.toStdString();
  if (std::ranges::find(facts::chop_controls, id) == facts::chop_controls.end())
    return {};
  return QString::fromStdString(
      facts::chop_parameter_id(regions_[static_cast<std::size_t>(index)].slot, id));
}

int CrateController::pad_key(int index) const {
  if (index < 0 || static_cast<std::size_t>(index) >= facts::max_chops)
    return -1;
  const auto pads = choice_value(engine_, layout_id, static_cast<std::size_t>(facts::Layout::pads));
  if (!pads || value_of(layout_id) != *pads)
    return -1;
  return static_cast<int>(facts::first_pad_key) + index;
}

std::uint64_t CrateController::frame_at(double fraction) const {
  if (!audio_ || audio_->frames.empty())
    return 0;
  const auto frames = static_cast<double>(audio_->frames.size());
  const double clamped = std::isfinite(fraction) ? std::clamp(fraction, 0.0, 1.0) : 0.0;
  return std::min<std::uint64_t>(static_cast<std::uint64_t>(std::llround(clamped * frames)),
                                 audio_->frames.size() - 1);
}

project::NativeSynthDocument
CrateController::with_regions(std::vector<project::NativeRegion> regions) const {
  auto document = document_;
  std::erase_if(document.regions, [](const project::NativeRegion& region) {
    return region.media == facts::sample_slot;
  });
  document.regions.insert(document.regions.end(), std::make_move_iterator(regions.begin()),
                          std::make_move_iterator(regions.end()));
  return document;
}

bool CrateController::rechop(const std::vector<std::uint64_t>& starts) {
  if (!audio_) {
    refuse(QStringLiteral("Load a sample first"));
    return false;
  }
  auto made = chops::make_regions(regions_, starts, *audio_);
  if (!made) {
    refuse(QString::fromStdString(made.error().message));
    return false;
  }
  std::optional<std::uint64_t> selected_start;
  if (static_cast<std::size_t>(selected_) < regions_.size())
    selected_start = regions_[static_cast<std::size_t>(selected_)].start_frame;
  // Dropped slots and slots new to a chop start plain.
  auto reset = made.value().freed_slots;
  for (const auto& region : made.value().regions) {
    if (std::ranges::find(regions_, region.slot, &project::NativeRegion::slot) == regions_.end())
      reset.push_back(region.slot);
  }
  auto values = values_;
  chops::reset_chop_slots(engine_, values, reset);
  adapter_.submit_state(values, with_regions(std::move(made.value().regions)),
                        NativeSynthGesture::single);
  const auto status = adapter_.status();
  message_ = QString::fromStdString(status.message);
  edited();
  if (selected_start) {
    const auto kept =
        std::ranges::find(regions_, *selected_start, &project::NativeRegion::start_frame);
    if (kept != regions_.end())
      set_selected(static_cast<int>(kept - regions_.begin()));
  }
  emit changed();
  return status.message.empty();
}

bool CrateController::chop() {
  if (!audio_) {
    refuse(QStringLiteral("Load a sample first"));
    return false;
  }
  const auto mode = chop_mode_.toStdString();
  foundation::Result<chops::ChopStarts> starts =
      foundation::Result<chops::ChopStarts>::success(chops::region_starts(regions_));
  if (mode == mode_transient) {
    starts = chops::chop_transients(*audio_, static_cast<float>(sensitivity_));
  } else if (mode == mode_grid) {
    const double tempo = bpm();
    if (tempo <= 0.0) {
      refuse(QStringLiteral("The sample has no tempo for a grid"));
      return false;
    }
    starts = chops::chop_grid(*audio_, tempo, static_cast<std::uint32_t>(grid_division_), 0);
  } else if (mode == mode_equal) {
    starts = chops::chop_equal(*audio_, static_cast<std::size_t>(equal_count_));
  } else if (starts.value().empty()) {
    // Manual with nothing to keep: the whole sample is one chop.
    starts.value().push_back(0);
  }
  if (!starts) {
    refuse(QString::fromStdString(starts.error().message));
    return false;
  }
  return rechop(starts.value());
}

bool CrateController::chop_transients(double sensitivity) {
  set_chop_mode(text(mode_transient));
  set_sensitivity(sensitivity);
  return chop();
}

bool CrateController::chop_grid(int division) {
  set_chop_mode(text(mode_grid));
  set_grid_division(division);
  return chop();
}

bool CrateController::chop_equal(int count) {
  set_chop_mode(text(mode_equal));
  set_equal_count(count);
  return chop();
}

bool CrateController::add_chop(double fraction) {
  if (!audio_) {
    refuse(QStringLiteral("Load a sample first"));
    return false;
  }
  const auto starts = chops::region_starts(regions_);
  const auto added = chops::add_chop_start(starts, frame_at(fraction), *audio_);
  if (added == starts) {
    refuse(starts.size() >= chops::max_crate_chops ? QStringLiteral("Crate holds 32 chops")
                                                   : QStringLiteral("Too close to another chop"));
    return false;
  }
  const auto frame = frame_at(fraction);
  const bool done = rechop(added);
  if (done) {
    const auto made = std::ranges::find(regions_, frame, &project::NativeRegion::start_frame);
    if (made != regions_.end())
      set_selected(static_cast<int>(made - regions_.begin()));
  }
  return done;
}

bool CrateController::remove_chop(int index) {
  if (index <= 0 || static_cast<std::size_t>(index) >= regions_.size()) {
    refuse(index == 0 ? QStringLiteral("The first chop always plays from the start")
                      : QStringLiteral("No such chop"));
    return false;
  }
  return rechop(
      chops::remove_chop_start(chops::region_starts(regions_), static_cast<std::size_t>(index)));
}

bool CrateController::rename_chop(int index, const QString& name) {
  if (index < 0 || static_cast<std::size_t>(index) >= regions_.size()) {
    refuse(QStringLiteral("No such chop"));
    return false;
  }
  auto regions = regions_;
  regions[static_cast<std::size_t>(index)].name =
      chops::utf8_prefix(name.trimmed().toStdString(), project::max_native_region_name);
  adapter_.submit_document(with_regions(std::move(regions)), NativeSynthGesture::single);
  const auto status = adapter_.status();
  message_ = QString::fromStdString(status.message);
  edited();
  emit changed();
  return status.message.empty();
}

// The chop, its new place and its phase are named at every call (from QML).
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool CrateController::move_chop(int index, double fraction, int phase) {
  if (!audio_ || phase < static_cast<int>(DragPhase::begin) ||
      phase > static_cast<int>(DragPhase::cancel))
    return false;
  const auto drag = static_cast<DragPhase>(phase);
  if (drag == DragPhase::cancel) {
    if (!drag_regions_)
      return false;
    drag_regions_.reset();
    adapter_.submit_document(document_, NativeSynthGesture::cancel);
    edited();
    return true;
  }
  if (index <= 0 || static_cast<std::size_t>(index) >= regions_.size())
    return false;
  if (drag == DragPhase::begin || !drag_regions_) {
    drag_regions_ = regions_;
    adapter_.submit_document(document_, NativeSynthGesture::begin);
    if (drag == DragPhase::begin)
      return true;
  }
  auto moved = chops::move_region(*drag_regions_, static_cast<std::size_t>(index),
                                  frame_at(fraction), *audio_);
  const bool commit = drag == DragPhase::commit;
  if (commit)
    drag_regions_.reset();
  adapter_.submit_document(with_regions(std::move(moved)),
                           commit ? NativeSynthGesture::commit : NativeSynthGesture::update);
  const auto status = adapter_.status();
  message_ = QString::fromStdString(status.message);
  edited();
  return status.message.empty();
}

bool CrateController::set_era(int index) {
  if (index < 0 || static_cast<std::size_t>(index) >= facts::era_settings.size())
    return false;
  const auto era = choice_value(engine_, era_id, static_cast<std::size_t>(index));
  if (!era)
    return false;
  const auto& settings = facts::era_settings[static_cast<std::size_t>(index)];
  auto values = values_;
  set_value(engine_, values, era_id, *era);
  set_value(engine_, values, bits_id, settings.bits);
  set_value(engine_, values, rate_id, settings.rate);
  set_value(engine_, values, drive_id, settings.drive);
  adapter_.submit(values, NativeSynthGesture::single);
  edited();
  return true;
}

bool CrateController::reverse_all(bool on) {
  if (regions_.empty())
    return false;
  auto values = values_;
  for (const auto& region : regions_) {
    const auto id = facts::chop_parameter_id(region.slot, reverse_control);
    if (const auto value = choice_value(engine_, id, on ? 1 : 0))
      set_value(engine_, values, id, *value);
  }
  adapter_.submit(values, NativeSynthGesture::single);
  edited();
  return true;
}

bool CrateController::load_sample(const QString& path_or_url) {
  if (importing_ || adapter_.media_import_busy()) {
    refuse(QStringLiteral("Finish the current load first"));
    return false;
  }
  const auto path = local_file(path_or_url);
  if (path.empty()) {
    refuse(QStringLiteral("Choose an audio file"));
    return false;
  }
  const auto name = QFileInfo(QString::fromStdString(path)).fileName();
  // Everything the plan needs, copied: it runs on the import's worker.
  std::vector<std::uint32_t> previous_slots;
  previous_slots.reserve(regions_.size());
  for (const auto& region : regions_)
    previous_slots.push_back(region.slot);
  auto plan = [engine = &engine_, values = values_, previous_slots = std::move(previous_slots),
               file_name = name.toStdString(), sensitivity = sensitivity_](
                  const SynthMediaAsset& audio,
                  std::optional<double> tempo) -> foundation::Result<NativeSynthMediaPlan> {
    using Plan = foundation::Result<NativeSynthMediaPlan>;
    auto starts = chops::chop_transients(audio, static_cast<float>(sensitivity));
    if (!starts)
      return Plan::failure(starts.error());
    auto state = chops::new_sample_state(*engine, values, previous_slots, starts.value(), audio,
                                         tempo, file_name);
    if (!state)
      return Plan::failure(state.error());
    NativeSynthMediaPlan result;
    result.regions = std::move(state.value().regions);
    result.values = std::move(state.value().values);
    result.labels = std::move(state.value().labels);
    return Plan::success(std::move(result));
  };
  QPointer<CrateController> self(this);
  const bool begun = adapter_.begin_media_import(
      facts::sample_slot, path, std::move(plan), [self, name](bool imported) {
        if (!self)
          return;
        self->importing_ = false;
        if (imported) {
          self->selected_ = 0;
          self->bank_ = 0;
          self->chop_mode_ = text(mode_transient);
          self->message_ = QStringLiteral("Loaded %1").arg(name);
        } else {
          const auto status = self->adapter_.status();
          self->message_ = status.message.empty() ? QStringLiteral("Could not load %1").arg(name)
                                                  : QString::fromStdString(status.message);
        }
        self->edited();
        emit self->changed();
      });
  if (!begun) {
    const auto status = adapter_.status();
    refuse(status.message.empty() ? QStringLiteral("Could not load %1").arg(name)
                                  : QString::fromStdString(status.message));
    return false;
  }
  importing_ = true;
  busy_ = true;
  message_ = QStringLiteral("Loading %1").arg(name);
  emit changed();
  return true;
}

bool CrateController::choose_sample() {
  if (qobject_cast<QApplication*>(QCoreApplication::instance()) == nullptr) {
    refuse(QStringLiteral("Drop an audio file on the panel to load it"));
    return false;
  }
  // Formats the build cannot decode are refused by the import with a reason.
  const auto chosen = QFileDialog::getOpenFileName(
      nullptr, QStringLiteral("Load a sample"), QDir::homePath(),
      QStringLiteral("Audio (*.wav *.wave *.aiff *.aif *.flac *.mp3);;All files (*)"));
  return !chosen.isEmpty() && load_sample(chosen);
}

bool CrateController::chop_to_midi(int start_bar) {
  if (!audio_ || regions_.empty()) {
    refuse(QStringLiteral("Load and chop a sample first"));
    return false;
  }
  const double tempo = bpm();
  if (tempo <= 0.0) {
    refuse(QStringLiteral("The sample has no tempo"));
    return false;
  }
  if (start_bar < 0) {
    refuse(QStringLiteral("Choose a bar in the song"));
    return false;
  }
  auto clip = chops::chop_to_midi(regions_, audio_->frames.size(), audio_->source_rate, tempo);
  if (!clip) {
    refuse(QString::fromStdString(clip.error().message));
    return false;
  }
  if (!adapter_.add_midi_clip(start_bar, clip.value().length, std::move(clip.value().notes))) {
    const auto status = adapter_.status();
    refuse(status.message.empty() ? QStringLiteral("Could not write the clip")
                                  : QString::fromStdString(status.message));
    return false;
  }
  message_ = QStringLiteral("Chops written to a clip at bar %1").arg(start_bar + 1);
  edited();
  emit changed();
  return true;
}

} // namespace oscillate::quick
