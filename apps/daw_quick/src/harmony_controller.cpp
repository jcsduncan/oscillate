#include "harmony_controller.hpp"

#include "oscillate/application/comp_patterns.hpp"
#include "oscillate/application/project_session.hpp"
#include "oscillate/harmony/clip.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/note_effects/note_effect.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <numeric>
#include <string>
#include <string_view>
#include <utility>

namespace oscillate::quick {
namespace {

namespace fx = native_dsp::note_effects;

constexpr std::int64_t quarter = timeline::ticks_per_quarter;
// Mood buckets while dragging the pad: one write per mode and tenth of restlessness.
constexpr int restless_buckets = 10;
constexpr int semitones = 12;
constexpr std::uint8_t max_midi_value = 127;
constexpr int midi_octave_offset = 1;
constexpr std::array<int, 7> major_scale{0, 2, 4, 5, 7, 9, 11};
constexpr std::array<int, 7> minor_scale{0, 2, 3, 5, 7, 8, 10};
constexpr std::array<std::string_view, harmony::variant_count> variant_titles{"Suggestion", "Swap",
                                                                              "Rotate", "Borrow"};
constexpr std::string_view held_name = "Held";
constexpr std::string_view held_summary =
    "Struck once and held, changes arriving early: how melodic house plays chords";
constexpr std::string_view push_land_name = "Push and land";
constexpr std::string_view push_land_summary =
    "Held; the next chord touched once, short, before it lands";
constexpr std::array<std::string_view, 5> anticipation_labels{"None", "16th", "8th", "Dotted 8th",
                                                              "Varied"};
enum class Movement : std::uint8_t { pump, filter, delay };
// Each movement: its effect engine, the factory preset, and the panel's words.
struct MovementRow {
  std::string_view engine;
  std::string_view preset;
  std::string_view name;
  std::string_view summary;
};
constexpr std::array<MovementRow, 3> movement_rows{{
    {"pump", "Classic Pump", "Pump", "The chords duck with every kick"},
    {"contour", "Dotted Breath", "Filter", "The filter breathes on a dotted 8th"},
    {"relay", "Dotted Eighth", "Delay", "A dotted-8th echo"},
}};
constexpr std::string_view push_chance_id = "comp.push_chance";
constexpr std::string_view push_by_id = "comp.push_amount";
constexpr std::string_view groove_id = "comp.groove";
constexpr std::string_view variation_id = "comp.variation";
constexpr std::string_view phrase_id = "comp.phrase";
constexpr std::string_view turnaround_id = "comp.turnaround";
constexpr std::string_view drift_id = "comp.drift";
constexpr std::string_view swell_id = "comp.swell";
constexpr std::string_view top_length_id = "comp.top_length";
constexpr std::string_view density_id = "comp.density";
constexpr std::string_view lift_id = "comp.lift";
constexpr std::string_view articulation_id = "comp.articulation";
// Articulation's shortest setting, a tenth of each written length.
constexpr double articulation_least = 0.1;
enum class Layer : std::uint8_t { pad, pluck, tonic, bass };
constexpr std::array<std::pair<std::string_view, std::string_view>, 4> layer_names{{
    {"Pad", "The chords held, open and an octave up"},
    {"Pluck", "The chords high, played in the rhythm by a Chord Comp"},
    {"Running tonic", "The key's tonic pulsing through every chord"},
    {"Bass", "The chords' basses, held, off the beat or rolling"},
}};
constexpr std::array<std::string_view, 3> pulse_names{"8ths", "16ths", "Dotted 8ths"};
constexpr std::array<std::string_view, 3> bass_style_names{"Held", "Off-beat", "Rolling"};
// Rhythm Complexity turns Lift on from here.
constexpr double lift_from = 0.55;
// The voicing styles the Chord tab offers, Open first.
constexpr std::array<std::pair<harmony::VoicingStyle, std::string_view>, 5> voicing_style_names{
    {{harmony::VoicingStyle::open, "Open"},
     {harmony::VoicingStyle::close, "Close"},
     {harmony::VoicingStyle::drop2, "Drop-2"},
     {harmony::VoicingStyle::spread, "Spread"},
     {harmony::VoicingStyle::rootless, "Rootless"}}};
// A progression that does not fill the clip evenly grows it to both
// lengths' common multiple (three bars against four make twelve), or, when
// that is more than this, to whole passes of the progression.
constexpr std::int64_t grown_clip_bars_most = 16;
constexpr std::int64_t beats_per_bar = 4;
// Rhythm Complexity: Variation and Swell rise with it to these at the top;
// Turnarounds join from the first, Top drift from the second threshold; the
// top note cycles 12 then 7 16ths past the last two; pushes grow past
// push_from, adding up to push_most to the pattern's own.
constexpr double variation_at_top = 0.6;
constexpr double swell_at_top = 0.6;
constexpr double turnarounds_from = 0.25;
constexpr double drift_from = 0.4;
constexpr std::array<double, 2> polymeter_from{0.7, 0.9};
constexpr double push_from = 0.6;
constexpr double push_most = 0.5;
// Harmony Complexity: colours by band (Triads, Natural, 7ths, Lush), the
// pace (2 bars, a bar, half a bar) and the devices it turns on, each from
// its threshold. Pedal tone, Pull home and Slide stay the musician's. The
// Top drone comes late: one of the records' 52 progressions holds its top
// note through.
constexpr std::array<double, 3> colour_from{0.2, 0.65, 0.85};
constexpr std::array<harmony::Colour, 4> colour_bands{
    harmony::Colour::triads, harmony::Colour::natural, harmony::Colour::sevenths,
    harmony::Colour::lush};
constexpr std::array<double, 2> pace_from{0.3, 0.75};
constexpr std::array<std::pair<harmony::Device, double>, 5> devices_from{
    {{harmony::Device::suspension, 0.55},
     {harmony::Device::drone, 0.65},
     {harmony::Device::walk, 0.7},
     {harmony::Device::passing, 0.8},
     {harmony::Device::borrowed, 0.9}}};

template <std::size_t N>
[[nodiscard]] std::size_t band_of(double value, const std::array<double, N>& from) {
  std::size_t band = 0;
  while (band < N && value >= from[band])
    ++band;
  return band;
}

// The phrases Comp measures Motion in, in bars, by choice.
constexpr auto phrase_bars = native_dsp::note_effects::comp_phrase_bars;

[[nodiscard]] QString text(std::string_view value) {
  return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}
[[nodiscard]] QString text(const std::string& value) { return QString::fromStdString(value); }

[[nodiscard]] std::int64_t end_of(const harmony::ChordRegion& region) {
  return region.position.value + region.length_ticks;
}

[[nodiscard]] QString role_name(harmony::Role role) {
  switch (role) {
  case harmony::Role::home:
    return QStringLiteral("Home");
  case harmony::Role::lift:
    return QStringLiteral("Lift");
  case harmony::Role::pull:
    return QStringLiteral("Pull");
  case harmony::Role::colour:
    return QStringLiteral("Colour");
  }
  return {};
}

[[nodiscard]] QString note_name(std::uint8_t midi, const project::MusicalKey& key) {
  return text(harmony::pitch_name(static_cast<std::uint8_t>(midi % semitones), key)) +
         QString::number(midi / semitones - midi_octave_offset);
}

[[nodiscard]] bool in_key(const harmony::ChordSymbol& chord, const project::MusicalKey& key) {
  const auto& scale = key.mode == project::KeyMode::major ? major_scale : minor_scale;
  std::uint16_t mask = 0;
  for (const int step : scale)
    mask |= static_cast<std::uint16_t>(1U << static_cast<unsigned>((key.tonic + step) % semitones));
  return (harmony::pitch_class_set(chord) & ~mask) == 0;
}

[[nodiscard]] const native_dsp::synth::ParameterDescriptor* comp_parameter(std::string_view id) {
  const auto* engine = fx::find_engine(application::comp::engine_id);
  if (engine == nullptr)
    return nullptr;
  const auto& parameters = engine->descriptor->parameters;
  const auto found = std::ranges::find(parameters, id, &native_dsp::synth::ParameterDescriptor::id);
  return found != parameters.end() ? &*found : nullptr;
}

[[nodiscard]] QStringList choice_names(std::string_view id) {
  QStringList names;
  if (const auto* parameter = comp_parameter(id))
    for (const auto choice : parameter->choices)
      names.push_back(text(choice));
  return names;
}

[[nodiscard]] double default_of(std::string_view id) {
  const auto* parameter = comp_parameter(id);
  return parameter != nullptr ? parameter->default_value : 0.0;
}

void set_parameter(std::vector<project::NativeInstrumentParameter>& parameters, std::string_view id,
                   double value) {
  const auto found = std::ranges::find(parameters, id, &project::NativeInstrumentParameter::id);
  if (found != parameters.end())
    found->value = value;
  else
    parameters.push_back({std::string(id), value});
}

// The notes starting outside [start, end), those held across `start` cut there.
[[nodiscard]] std::vector<project::MidiNote> outside(std::span<const project::MidiNote> notes,
                                                     std::int64_t start, std::int64_t end) {
  std::vector<project::MidiNote> kept;
  for (auto note : notes) {
    if (note.start.value >= start && note.start.value < end)
      continue;
    if (note.start.value < start && note.end.value > start)
      note.end = {start};
    kept.push_back(note);
  }
  return kept;
}

} // namespace

HarmonyController::HarmonyController(Hooks hooks, QObject* parent)
    : QObject(parent), hooks_(std::move(hooks)), chords_({}, this),
      push_chance_(default_of(push_chance_id)),
      push_by_(static_cast<int>(std::lround(default_of(push_by_id)))),
      groove_(static_cast<int>(std::lround(default_of(groove_id)))),
      phrase_(static_cast<int>(std::lround(default_of(phrase_id)))) {
  regenerate();
  // Start with the first pattern's own feel.
  const int first = rhythm_pattern_;
  rhythm_pattern_ = held_rhythm;
  set_rhythm_pattern(first);
}

application::ProjectSession* HarmonyController::session() const {
  return hooks_.session ? hooks_.session() : nullptr;
}

project::MusicalKey HarmonyController::key() const { return song_key_.value_or(fallback_key); }

void HarmonyController::set_status(QString status) {
  // `changed` also refreshes the rest of the view, so it is sent either way.
  status_.set(std::move(status));
  emit changed();
}

std::optional<HarmonyController::ClipView> HarmonyController::clip_view() const {
  const auto* current = session();
  if (current == nullptr || !clip_)
    return std::nullopt;
  for (const auto& track : current->snapshot().tracks)
    for (const auto& clip : track.midi_clips)
      if (clip.id == *clip_)
        return ClipView{clip.id, track.id, clip.pattern_length, clip.notes};
  return std::nullopt;
}

// ---- Projection -----------------------------------------------------------------------

void HarmonyController::set_clip_id(const QString& clip_id) {
  if (clip_id == clip_id_)
    return;
  // A drag on another clip's Comp is not this one's undo step.
  rhythm_gesture_.reset();
  clip_id_ = clip_id;
  const auto parsed = project::ClipId::parse(clip_id.toStdString());
  clip_ = parsed ? std::optional{parsed.value()} : std::nullopt;
  read_notes_.clear();
  lane_.clear();
  selected_ = 0;
  status_.set({});
  emit clipChanged();
  project_changed();
  // The switch starts where the clip is: off when its chords have no bass.
  const auto view = clip_view();
  const auto split = voicing_.bass_lowest + semitones;
  voicing_.bass = !view || view->notes.empty() ||
                  std::ranges::any_of(view->notes, [&](const auto& n) { return n.key < split; });
  emit viewChanged();
  project_lane();
}

void HarmonyController::project_changed() {
  const auto* current = session();
  const auto song_key = current != nullptr ? current->snapshot().key : std::nullopt;
  const auto comp = track_comp();
  const bool live = comp && comp->second;
  const bool live_moved = live != live_rhythm_;
  live_rhythm_ = live;
  const auto view = clip_view();
  auto notes = view ? view->notes : std::vector<project::MidiNote>{};
  const bool key_moved = song_key != song_key_;
  song_key_ = song_key;
  if (key_moved) {
    lab_.tonic = key().tonic;
    // The pad starts on the key's own mode.
    apply_key_mode();
    regenerate();
    emit labChanged();
  }
  if (!key_moved && notes == read_notes_ && chords_.count() == static_cast<int>(lane_.size())) {
    if (live_moved)
      emit changed();
    return;
  }
  lane_ = view ? harmony::recognise(notes, view->length) : std::vector<harmony::ChordRegion>{};
  read_notes_ = std::move(notes);
  project_lane();
}

void HarmonyController::project_lane() {
  selected_ = lane_.empty() ? 0 : std::clamp(selected_, 0, static_cast<int>(lane_.size()) - 1);
  const auto song = key();
  std::vector<KeyedListModel::Row> rows;
  rows.reserve(lane_.size());
  for (std::size_t i = 0; i < lane_.size(); ++i) {
    const auto& region = lane_[i];
    rows.push_back(
        {QStringLiteral("c%1").arg(region.position.value),
         QVariantMap{{QStringLiteral("index"), static_cast<int>(i)},
                     {QStringLiteral("name"), text(harmony::name(region.chord, song))},
                     {QStringLiteral("roman"), text(harmony::roman(region.chord, song))},
                     {QStringLiteral("startTicks"), static_cast<double>(region.position.value)},
                     {QStringLiteral("endTicks"), static_cast<double>(end_of(region))},
                     {QStringLiteral("drone"), region.top.has_value()},
                     {QStringLiteral("tension"), harmony::tension(region.chord, song)},
                     {QStringLiteral("role"), role_name(harmony::role(region.chord, song))}}});
  }
  chords_.update(std::move(rows));
  emit changed();
}

QString HarmonyController::key_name() const {
  const auto song = key();
  return text(harmony::pitch_name(song.tonic, song)) + (song.mode == project::KeyMode::major
                                                            ? QStringLiteral(" major")
                                                            : QStringLiteral(" minor"));
}

QStringList HarmonyController::root_names() const {
  const auto song = key();
  QStringList names;
  for (std::uint8_t pitch = 0; pitch < harmony::pitch_classes; ++pitch)
    names.push_back(text(harmony::pitch_name(pitch, song)));
  return names;
}

QVariantList HarmonyController::qualities() const {
  QVariantList out;
  for (auto value = static_cast<int>(harmony::ChordQuality::major);
       value <= static_cast<int>(harmony::last_chord_quality); ++value)
    out.push_back(QVariantMap{{QStringLiteral("value"), value},
                              {QStringLiteral("suffix"),
                               text(harmony::suffix(static_cast<harmony::ChordQuality>(value)))}});
  return out;
}

QVariantMap HarmonyController::selection() const {
  if (lane_.empty())
    return {};
  const auto& region = lane_[static_cast<std::size_t>(selected_)];
  const auto song = key();
  QVariantList voices;
  for (const auto& entry : audition_keys(selected_))
    voices.push_back(note_name(static_cast<std::uint8_t>(entry.toInt()), song));
  return {{QStringLiteral("name"), text(harmony::name(region.chord, song))},
          {QStringLiteral("roman"), text(harmony::roman(region.chord, song))},
          {QStringLiteral("role"), role_name(harmony::role(region.chord, song))},
          {QStringLiteral("tension"), harmony::tension(region.chord, song)},
          {QStringLiteral("inKey"), in_key(region.chord, song)},
          {QStringLiteral("root"), static_cast<int>(region.chord.root)},
          {QStringLiteral("quality"), static_cast<int>(region.chord.quality)},
          {QStringLiteral("bass"), region.chord.bass ? static_cast<int>(*region.chord.bass) : -1},
          {QStringLiteral("voices"), voices},
          {QStringLiteral("beats"), static_cast<double>(region.length_ticks) / quarter},
          {QStringLiteral("startTicks"), static_cast<double>(region.position.value)},
          {QStringLiteral("endTicks"), static_cast<double>(end_of(region))}};
}

QVariantList HarmonyController::audition_keys(int index) const {
  if (index < 0 || static_cast<std::size_t>(index) >= lane_.size())
    return {};
  const auto& region = lane_[static_cast<std::size_t>(index)];
  // The notes sounding at the chord's first onset inside it.
  std::int64_t onset = end_of(region);
  for (const auto& note : read_notes_)
    if (note.start.value >= region.position.value && note.start.value < onset)
      onset = note.start.value;
  std::vector<int> keys;
  for (const auto& note : read_notes_)
    if (note.start.value <= onset && note.end.value > onset &&
        note.end.value > region.position.value)
      keys.push_back(note.key);
  std::ranges::sort(keys);
  const auto [first, last] = std::ranges::unique(keys);
  keys.erase(first, last);
  QVariantList out;
  for (const int key_number : keys)
    out.push_back(key_number);
  return out;
}

int HarmonyController::index_at_tick(double ticks) const {
  const auto tick = static_cast<std::int64_t>(std::floor(ticks));
  for (std::size_t i = 0; i < lane_.size(); ++i)
    if (tick >= lane_[i].position.value && tick < end_of(lane_[i]))
      return static_cast<int>(i);
  return -1;
}

void HarmonyController::set_insert_next(bool insert) {
  if (insert == insert_next_)
    return;
  insert_next_ = insert;
  emit viewChanged();
}

// ---- Commit ---------------------------------------------------------------------------

// A rhythm for the whole clip rebuilds it from its chords; for the selected
// chord, the notes around it stay.
std::vector<project::MidiNote>
HarmonyController::kept_around(std::span<const project::MidiNote> notes, std::int64_t start,
                               std::int64_t end) const {
  if (!rhythm_selected_only_)
    return {};
  return outside(notes, start, end);
}

bool HarmonyController::commit(std::vector<project::MidiNote> notes, std::int64_t select_tick,
                               bool audition, std::optional<timeline::TickPosition> length) {
  auto* current = session();
  const auto view = clip_view();
  if (current == nullptr || !view) {
    set_status(QStringLiteral("Open a MIDI clip first"));
    return false;
  }
  if (busy_ || (hooks_.can_edit && !hooks_.can_edit()))
    return false;
  // Every write fits the clip: notes pushed before its start or running past
  // its end are trimmed to it, and nothing left over can refuse the edit.
  const auto pattern = length.value_or(view->length);
  for (auto& note : notes) {
    note.start.value = std::max<std::int64_t>(note.start.value, 0);
    note.end.value = std::min(note.end.value, pattern.value);
    note.velocity = std::clamp<std::uint8_t>(note.velocity, 1, max_midi_value);
  }
  std::erase_if(notes, [](const project::MidiNote& note) {
    return note.end <= note.start || note.key > max_midi_value;
  });
  busy_ = true;
  const auto applied = current->midi_clips().replace_midi_clip_notes(
      view->clip, pattern, std::move(notes), mood_gesture_);
  busy_ = false;
  if (!applied) {
    set_status(text(applied.error().message));
    return false;
  }
  status_.set({});
  if (hooks_.commit)
    hooks_.commit();
  project_changed();
  if (const int index = index_at_tick(static_cast<double>(select_tick)); index >= 0)
    selected_ = index;
  project_lane();
  if (audition)
    emit auditionRequested(QString::fromStdString(view->track.to_string()),
                           audition_keys(selected_));
  return true;
}

bool HarmonyController::commit_lane(const std::vector<harmony::ChordRegion>& lane,
                                    std::size_t select, std::optional<std::size_t> revoice) {
  const auto view = clip_view();
  if (!view || select >= lane.size())
    return false;
  auto notes = harmony::rewrite(view->notes, lane, view->length, voicing_, revoice);
  return commit(std::move(notes), lane[select].position.value, true);
}

// ---- The strip ------------------------------------------------------------------------

void HarmonyController::select(int index) {
  if (index < 0 || static_cast<std::size_t>(index) >= lane_.size())
    return;
  if (index != selected_) {
    selected_ = index;
    emit changed();
  }
  if (const auto view = clip_view())
    emit auditionRequested(QString::fromStdString(view->track.to_string()), audition_keys(index));
}

void HarmonyController::remove(int index) {
  const auto view = clip_view();
  if (!view || index < 0 || static_cast<std::size_t>(index) >= lane_.size())
    return;
  const auto& region = lane_[static_cast<std::size_t>(index)];
  static_cast<void>(commit(outside(view->notes, region.position.value, end_of(region)),
                           region.position.value, false));
}

// ---- Chord ----------------------------------------------------------------------------

void HarmonyController::change_selected(const harmony::ChordSymbol& chord) {
  if (lane_.empty())
    return;
  auto lane = lane_;
  auto& region = lane[static_cast<std::size_t>(selected_)];
  region.chord = chord;
  region.device = harmony::ChordDevice::none;
  static_cast<void>(commit_lane(lane, static_cast<std::size_t>(selected_)));
}

void HarmonyController::set_root(int pitch_class) {
  if (lane_.empty() || pitch_class < 0 || pitch_class >= harmony::pitch_classes)
    return;
  auto chord = lane_[static_cast<std::size_t>(selected_)].chord;
  chord.root = static_cast<std::uint8_t>(pitch_class);
  if (chord.bass && *chord.bass == chord.root)
    chord.bass.reset();
  change_selected(chord);
}

void HarmonyController::set_quality(int quality) {
  if (lane_.empty() || quality < static_cast<int>(harmony::ChordQuality::major) ||
      quality > static_cast<int>(harmony::last_chord_quality))
    return;
  auto chord = lane_[static_cast<std::size_t>(selected_)].chord;
  chord.quality = static_cast<harmony::ChordQuality>(quality);
  change_selected(chord);
}

void HarmonyController::set_bass(int pitch_class) {
  if (lane_.empty() || pitch_class >= harmony::pitch_classes)
    return;
  auto chord = lane_[static_cast<std::size_t>(selected_)].chord;
  if (pitch_class < 0 || pitch_class == chord.root)
    chord.bass.reset();
  else
    chord.bass = static_cast<std::uint8_t>(pitch_class);
  change_selected(chord);
}

void HarmonyController::set_voicing_style(int style) {
  if (style < 0 || style > static_cast<int>(harmony::VoicingStyle::open))
    return;
  const auto chosen = static_cast<harmony::VoicingStyle>(style);
  // Open has its own register, low and wide; the keyboard styles sit higher.
  const auto offset = voicing_.register_offset;
  const auto bass = voicing_.bass;
  voicing_ = chosen == harmony::VoicingStyle::open ? harmony::open_voicing()
                                                   : harmony::VoicingOptions{.style = chosen};
  voicing_.register_offset = offset;
  voicing_.bass = bass;
  emit viewChanged();
  if (!lane_.empty())
    static_cast<void>(commit_lane(lane_, static_cast<std::size_t>(selected_),
                                  static_cast<std::size_t>(selected_)));
}

void HarmonyController::set_bass(bool on) {
  if (on == voicing_.bass)
    return;
  voicing_.bass = on;
  emit viewChanged();
  if (!lane_.empty())
    static_cast<void>(commit_lane(lane_, static_cast<std::size_t>(selected_)));
}

void HarmonyController::apply_substitution(int index) {
  if (lane_.empty())
    return;
  auto edits = harmony::substitutions(lane_, static_cast<std::size_t>(selected_), key());
  if (index < 0 || static_cast<std::size_t>(index) >= edits.size())
    return;
  const auto& edit = edits[static_cast<std::size_t>(index)];
  static_cast<void>(commit_lane(edit.lane, edit.selected));
}

void HarmonyController::apply_hint(int index) {
  if (lane_.empty())
    return;
  auto edits = harmony::hints(lane_, static_cast<std::size_t>(selected_), key());
  if (index < 0 || static_cast<std::size_t>(index) >= edits.size() ||
      edits[static_cast<std::size_t>(index)].already)
    return;
  const auto& edit = edits[static_cast<std::size_t>(index)];
  static_cast<void>(commit_lane(edit.lane, edit.selected));
}

void HarmonyController::use_next(int index) {
  if (lane_.empty())
    return;
  const auto at = static_cast<std::size_t>(selected_);
  const auto candidates = harmony::next_candidates(lane_[at].chord, key());
  if (index < 0 || static_cast<std::size_t>(index) >= candidates.size())
    return;
  const auto& chord = candidates[static_cast<std::size_t>(index)].chord;
  auto lane = lane_;
  if (!insert_next_ && at + 1 < lane.size()) {
    lane[at + 1].chord = chord;
    lane[at + 1].device = harmony::ChordDevice::none;
  } else {
    // Split in: the chord's second half becomes the next chord.
    auto& region = lane[at];
    const auto half = region.length_ticks / 2 / quarter * quarter;
    if (half < quarter) {
      set_status(QStringLiteral("Too short to split: a chord needs a beat at least"));
      return;
    }
    harmony::ChordRegion added{.position = {region.position.value + half},
                               .length_ticks = region.length_ticks - half,
                               .chord = chord};
    region.length_ticks = half;
    lane.insert(lane.begin() + static_cast<std::ptrdiff_t>(at) + 1, added);
  }
  static_cast<void>(commit_lane(lane, at + 1));
}

QVariantList HarmonyController::next() const {
  if (lane_.empty())
    return {};
  const auto song = key();
  QVariantList out;
  const auto candidates =
      harmony::next_candidates(lane_[static_cast<std::size_t>(selected_)].chord, song);
  for (std::size_t i = 0; i < candidates.size(); ++i) {
    const auto& candidate = candidates[i];
    out.push_back(
        QVariantMap{{QStringLiteral("index"), static_cast<int>(i)},
                    {QStringLiteral("name"), text(harmony::name(candidate.chord, song))},
                    {QStringLiteral("roman"), text(harmony::roman(candidate.chord, song))},
                    {QStringLiteral("likelihood"), candidate.likelihood},
                    {QStringLiteral("tension"), candidate.tension},
                    {QStringLiteral("shared"), candidate.shared_tones}});
  }
  return out;
}

QVariantList HarmonyController::substitutions() const {
  if (lane_.empty())
    return {};
  QVariantList out;
  const auto edits = harmony::substitutions(lane_, static_cast<std::size_t>(selected_), key());
  for (std::size_t i = 0; i < edits.size(); ++i)
    out.push_back(QVariantMap{{QStringLiteral("index"), static_cast<int>(i)},
                              {QStringLiteral("title"), text(edits[i].title)},
                              {QStringLiteral("preview"), text(edits[i].preview)}});
  return out;
}

QVariantList HarmonyController::hints() const {
  if (lane_.empty())
    return {};
  QVariantList out;
  const auto edits = harmony::hints(lane_, static_cast<std::size_t>(selected_), key());
  for (std::size_t i = 0; i < edits.size(); ++i)
    out.push_back(QVariantMap{{QStringLiteral("index"), static_cast<int>(i)},
                              {QStringLiteral("title"), text(edits[i].title)},
                              {QStringLiteral("why"), text(edits[i].why)},
                              {QStringLiteral("preview"), text(edits[i].preview)},
                              {QStringLiteral("already"), edits[i].already}});
  return out;
}

// ---- Write: the Lab -------------------------------------------------------------------

void HarmonyController::regenerate() { suggestions_ = harmony::suggest(lab_); }

void HarmonyController::apply_key_mode() {
  lab_.key_mode = song_key_ && in_key_ ? std::optional{song_key_->mode} : std::nullopt;
  if (song_key_ && in_key_)
    lab_.mood.brightness = harmony::home_brightness(song_key_->mode);
}

void HarmonyController::set_key_modes_only(bool in_key) {
  if (in_key == in_key_)
    return;
  in_key_ = in_key;
  apply_key_mode();
  regenerate();
  emit labChanged();
}

void HarmonyController::write_suggestion(bool audition) {
  const auto view = clip_view();
  if (!view) {
    set_status(QStringLiteral("Open a MIDI clip first"));
    return;
  }
  if (suggestion_ < 0)
    suggestion_ = 0;
  const auto& progression = suggestions_[static_cast<std::size_t>(suggestion_)].chords;
  if (progression.empty())
    return;
  auto length = view->length;
  const auto progression_ticks =
      progression.back().position.value + progression.back().length_ticks;
  if (progression_ticks > 0 && length.value % progression_ticks != 0) {
    const auto most = grown_clip_bars_most * beats_per_bar * quarter;
    const auto common = std::lcm(length.value, progression_ticks);
    const auto passes = (length.value + progression_ticks - 1) / progression_ticks;
    length = timeline::TickPosition{common <= most ? common : passes * progression_ticks};
  }
  const auto lane = harmony::tile(progression, {0}, length);
  if (lane.empty())
    return;
  // Chords already played in a rhythm keep it; an empty clip gets them held.
  auto notes = harmony::rewrite(view->notes, lane, length, voicing_);
  static_cast<void>(commit(std::move(notes), 0, audition,
                           length != view->length ? std::optional{length} : std::nullopt));
}

int HarmonyController::mode_index() const noexcept {
  const auto modes = harmony::pad_modes(lab_.key_mode);
  const auto mode = harmony::mode_at(lab_.mood.brightness, lab_.key_mode);
  return static_cast<int>(std::ranges::find(modes, mode) - modes.begin());
}
QString HarmonyController::mode_name() const {
  return text(harmony::mode_name(harmony::mode_at(lab_.mood.brightness, lab_.key_mode)));
}
QString HarmonyController::feel() const { return text(harmony::feel_at(lab_.mood)); }
QString HarmonyController::motion() const {
  constexpr double still = 1.0 / 3.0;
  constexpr double moving = 2.0 / 3.0;
  return lab_.mood.restlessness < still    ? QStringLiteral("still")
         : lab_.mood.restlessness < moving ? QStringLiteral("moving")
                                           : QStringLiteral("restless");
}
QStringList HarmonyController::mode_names() const {
  QStringList names;
  for (const auto mode : harmony::pad_modes(lab_.key_mode))
    names.push_back(text(harmony::mode_name(mode)));
  return names;
}
int HarmonyController::pace_beats() const noexcept {
  return static_cast<int>(lab_.chord_ticks / quarter);
}

void HarmonyController::set_mood(double brightness, double restlessness) {
  lab_.mood.brightness = std::clamp(brightness, 0.0, 1.0);
  lab_.mood.restlessness = std::clamp(restlessness, 0.0, 1.0);
  regenerate();
  emit labChanged();
  const int bucket = mode_index() * (restless_buckets + 1) +
                     static_cast<int>(std::lround(lab_.mood.restlessness * restless_buckets));
  if (bucket == mood_bucket_)
    return;
  mood_bucket_ = bucket;
  if (!mood_gesture_)
    mood_gesture_ = next_gesture_++;
  write_suggestion(false);
}

void HarmonyController::finish_mood() {
  mood_bucket_ = -1;
  mood_gesture_.reset();
  if (const auto view = clip_view())
    emit auditionRequested(QString::fromStdString(view->track.to_string()),
                           audition_keys(selected_));
}

void HarmonyController::set_shape(int shape) {
  if (shape < 0 || static_cast<std::size_t>(shape) >= harmony::shape_count)
    return;
  lab_.shape = static_cast<harmony::Shape>(shape);
  regenerate();
  emit labChanged();
  write_suggestion(true);
}

void HarmonyController::set_colour(int colour) {
  if (colour < 0 || colour > static_cast<int>(harmony::Colour::lush))
    return;
  lab_.colour = static_cast<harmony::Colour>(colour);
  regenerate();
  emit labChanged();
  write_suggestion(true);
}

void HarmonyController::set_pace_beats(int beats) {
  if (std::ranges::find(pace_choices, beats) == pace_choices.end())
    return;
  lab_.chord_ticks = beats * quarter;
  regenerate();
  emit labChanged();
  write_suggestion(false);
}

void HarmonyController::toggle_device(int device) {
  if (device < 0 || static_cast<std::size_t>(device) >= harmony::device_count)
    return;
  lab_.devices ^= harmony::device_bit(static_cast<harmony::Device>(device));
  regenerate();
  emit labChanged();
  write_suggestion(true);
}

void HarmonyController::choose_suggestion(int index) {
  if (index < 0 || static_cast<std::size_t>(index) >= harmony::variant_count)
    return;
  suggestion_ = index;
  emit labChanged();
  write_suggestion(true);
}

void HarmonyController::more() {
  ++lab_.seed;
  regenerate();
  emit labChanged();
  write_suggestion(true);
}

QVariantList HarmonyController::shapes() const {
  QVariantList out;
  for (std::size_t i = 0; i < harmony::shape_count; ++i) {
    const auto shape = static_cast<harmony::Shape>(i);
    QVariantList degrees;
    for (const auto degree : harmony::shape_degrees(shape))
      degrees.push_back(static_cast<int>(degree));
    out.push_back(QVariantMap{{QStringLiteral("index"), static_cast<int>(i)},
                              {QStringLiteral("name"), text(harmony::shape_name(shape))},
                              {QStringLiteral("summary"), text(harmony::shape_summary(shape))},
                              {QStringLiteral("degrees"), degrees}});
  }
  return out;
}

QVariantList HarmonyController::devices() const {
  QVariantList out;
  for (std::size_t i = 0; i < harmony::device_count; ++i) {
    const auto device = static_cast<harmony::Device>(i);
    out.push_back(
        QVariantMap{{QStringLiteral("index"), static_cast<int>(i)},
                    {QStringLiteral("name"), text(harmony::device_name(device))},
                    {QStringLiteral("hint"), text(harmony::device_hint(device))},
                    {QStringLiteral("on"), (lab_.devices & harmony::device_bit(device)) != 0}});
  }
  return out;
}

QVariantList HarmonyController::suggestions() const {
  QVariantList out;
  const auto song = key();
  for (std::size_t i = 0; i < suggestions_.size(); ++i) {
    QStringList names;
    for (const auto& region : suggestions_[i].chords)
      names.push_back(text(harmony::name(region.chord, song)));
    out.push_back(QVariantMap{{QStringLiteral("index"), static_cast<int>(i)},
                              {QStringLiteral("title"), text(variant_titles[i])},
                              {QStringLiteral("label"), text(suggestions_[i].label)},
                              {QStringLiteral("chords"), names.join(QStringLiteral(" · "))}});
  }
  return out;
}

// ---- Rhythm ---------------------------------------------------------------------------

QVariantList HarmonyController::rhythm_patterns() const {
  QVariantList out{QVariantMap{{QStringLiteral("index"), held_rhythm},
                               {QStringLiteral("name"), text(held_name)},
                               {QStringLiteral("summary"), text(held_summary)},
                               {QStringLiteral("restruck"), false}},
                   QVariantMap{{QStringLiteral("index"), push_land_rhythm},
                               {QStringLiteral("name"), text(push_land_name)},
                               {QStringLiteral("summary"), text(push_land_summary)},
                               {QStringLiteral("restruck"), false}}};
  const auto patterns = application::comp::patterns();
  for (std::size_t i = 0; i < patterns.size(); ++i)
    out.push_back(QVariantMap{{QStringLiteral("index"), first_comp_rhythm + static_cast<int>(i)},
                              {QStringLiteral("name"), text(patterns[i].name)},
                              {QStringLiteral("summary"), text(patterns[i].summary)},
                              {QStringLiteral("restruck"), true}});
  return out;
}

QStringList HarmonyController::anticipation_names() const {
  QStringList out;
  for (const auto label : anticipation_labels)
    out.push_back(text(label));
  return out;
}

void HarmonyController::set_anticipation(int choice) {
  if (choice < 0 || choice >= static_cast<int>(anticipation_labels.size()) ||
      choice == anticipation_)
    return;
  anticipation_ = choice;
  emit rhythmChanged();
}

QVariantList HarmonyController::movements() const {
  QVariantList out;
  for (std::size_t i = 0; i < movement_rows.size(); ++i)
    out.push_back(QVariantMap{{QStringLiteral("index"), static_cast<int>(i)},
                              {QStringLiteral("name"), text(movement_rows[i].name)},
                              {QStringLiteral("summary"), text(movement_rows[i].summary)}});
  return out;
}

void HarmonyController::add_movement(int kind) {
  auto* current = session();
  const auto view = clip_view();
  if (current == nullptr || !view || kind < 0 || kind >= static_cast<int>(movement_rows.size()) ||
      (hooks_.can_edit && !hooks_.can_edit()))
    return;
  const auto& row = movement_rows[static_cast<std::size_t>(kind)];
  const auto* engine = native_dsp::effects::find_engine(row.engine);
  if (engine == nullptr)
    return;
  const auto& presets = engine->descriptor->presets;
  const auto preset = std::ranges::find(presets, row.preset, &native_dsp::synth::SynthPreset::name);
  const auto& tracks = current->snapshot().tracks;
  const auto track = std::ranges::find(tracks, view->track, &project::Track::id);
  const auto added = current->effects().add_effect_engine(
      project::SignalPathId{view->track}, track->effects.size(), row.engine,
      preset != presets.end() ? std::optional{static_cast<std::size_t>(preset - presets.begin())}
                              : std::nullopt);
  if (!added) {
    set_status(text(added.error().message));
    return;
  }
  set_status(QStringLiteral("Added %1 to the track").arg(text(row.name)));
  if (hooks_.commit)
    hooks_.commit();
}

QStringList HarmonyController::push_names() const { return choice_names(push_by_id); }
QStringList HarmonyController::groove_names() const { return choice_names(groove_id); }

void HarmonyController::set_rhythm_pattern(int index) {
  if (index < held_rhythm ||
      index >= first_comp_rhythm + static_cast<int>(application::comp::patterns().size()) ||
      index == rhythm_pattern_)
    return;
  rhythm_pattern_ = index;
  // A pattern brings its own feel: its push and groove.
  if (index >= first_comp_rhythm) {
    if (const auto pattern = application::comp::pattern_parameters(
            static_cast<std::size_t>(index - first_comp_rhythm), {})) {
      const auto value = [&](std::string_view id) {
        const auto found =
            std::ranges::find(pattern.value(), id, &project::NativeInstrumentParameter::id);
        return found != pattern.value().end() ? found->value : default_of(id);
      };
      push_chance_ = value(push_chance_id);
      pattern_push_ = push_chance_;
      push_by_ = static_cast<int>(std::lround(value(push_by_id)));
      groove_ = static_cast<int>(std::lround(value(groove_id)));
    }
  }
  emit rhythmChanged();
  update_live_rhythm();
}

void HarmonyController::set_push_chance(double chance) {
  const double clamped = std::clamp(chance, 0.0, 1.0);
  if (clamped == push_chance_)
    return;
  push_chance_ = clamped;
  emit rhythmChanged();
  update_live_rhythm(rhythm_drag());
}

void HarmonyController::set_push_by(int choice) {
  if (choice < 0 || choice >= push_names().size() || choice == push_by_)
    return;
  push_by_ = choice;
  emit rhythmChanged();
  update_live_rhythm();
}

void HarmonyController::set_groove(int choice) {
  if (choice < 0 || choice >= groove_names().size() || choice == groove_)
    return;
  groove_ = choice;
  emit rhythmChanged();
  update_live_rhythm();
}

void HarmonyController::set_rhythm_selected_only(bool selected_only) {
  if (selected_only == rhythm_selected_only_)
    return;
  rhythm_selected_only_ = selected_only;
  emit rhythmChanged();
}

std::optional<std::vector<project::NativeInstrumentParameter>> HarmonyController::rhythm_parameters(
    std::span<const project::NativeInstrumentParameter> current) const {
  if (rhythm_pattern_ < first_comp_rhythm)
    return std::nullopt;
  auto parameters = application::comp::pattern_parameters(
      static_cast<std::size_t>(rhythm_pattern_ - first_comp_rhythm), current);
  if (!parameters)
    return std::nullopt;
  auto values = std::move(parameters).value();
  set_parameter(values, push_chance_id, push_chance_);
  set_parameter(values, push_by_id, push_by_);
  set_parameter(values, groove_id, groove_);
  set_parameter(values, variation_id, variation_);
  set_parameter(values, phrase_id, phrase_);
  set_parameter(values, turnaround_id, turnaround_ ? 1.0 : 0.0);
  set_parameter(values, drift_id, drift_ ? 1.0 : 0.0);
  set_parameter(values, swell_id, swell_);
  set_parameter(values, density_id, density_);
  set_parameter(values, lift_id, lift_ ? 1.0 : 0.0);
  set_parameter(values, articulation_id, articulation_);
  set_parameter(values, top_length_id, top_cycle_steps[static_cast<std::size_t>(top_cycle_)]);
  return values;
}

void HarmonyController::apply_rhythm() {
  auto* current = session();
  const auto view = clip_view();
  if (current == nullptr || !view || lane_.empty()) {
    set_status(QStringLiteral("Write some chords first"));
    return;
  }
  // The range: the selected chord, or the whole clip.
  std::int64_t start = 0;
  std::int64_t end = view->length.value;
  std::vector<harmony::ChordRegion> lane = lane_;
  if (rhythm_selected_only_) {
    const auto& region = lane_[static_cast<std::size_t>(selected_)];
    start = region.position.value;
    end = end_of(region);
    lane = {region};
  }
  const auto held = harmony::write(lane, voicing_);
  if (rhythm_pattern_ < first_comp_rhythm) {
    // Held, the changes anticipated; or pushed and landed.
    const auto how = static_cast<harmony::Anticipation>(anticipation_);
    const auto played = rhythm_pattern_ == push_land_rhythm
                            ? harmony::push_and_land(lane, voicing_, how)
                            : harmony::write(harmony::anticipate(lane, how), voicing_);
    auto notes = kept_around(view->notes, start, end);
    notes.insert(notes.end(), played.begin(), played.end());
    static_cast<void>(commit(std::move(notes), start, true));
    return;
  }
  const auto parameters = rhythm_parameters({});
  if (!parameters)
    return;
  // Motion varies over a phrase: a shorter clip grows to hold one, its
  // chords repeated, so the variations are there to edit.
  std::optional<timeline::TickPosition> grown;
  auto played_held = held;
  if (!rhythm_selected_only_ && motion_on() && view->length.value < phrase_ticks()) {
    grown = timeline::TickPosition{phrase_ticks()};
    end = grown->value;
    played_held = harmony::write(harmony::tile(lane_, {0}, *grown), voicing_);
  }
  auto played =
      current->notes().comp_rhythm_notes(view->clip, *parameters, played_held, {start}, {end});
  if (!played) {
    set_status(text(played.error().message));
    return;
  }
  auto notes = kept_around(view->notes, start, end);
  notes.insert(notes.end(), played.value().begin(), played.value().end());
  // Where the pattern plays no bass (house leaves it to the bass line) the
  // chords' bass stays held under it, so the clip still names its chords.
  // The bass role is the voicing's own: below its bass's octave.
  const auto is_bass = [split = harmony::bass_split(voicing_)](const project::MidiNote& note) {
    return note.key < split;
  };
  std::vector<project::MidiNote> played_bass;
  std::ranges::copy_if(played.value(), std::back_inserter(played_bass), is_bass);
  std::ranges::sort(played_bass, {}, &project::MidiNote::start);
  for (const auto& bass : played_held) {
    if (!voicing_.bass || !is_bass(bass))
      continue;
    // Held in the gaps between the pattern's own bass notes.
    auto piece = bass;
    for (const auto& note : played_bass) {
      if (note.end <= piece.start || note.start >= bass.end)
        continue;
      if (note.start > piece.start) {
        auto before = piece;
        before.end = note.start;
        notes.push_back(before);
      }
      piece.start = std::max(piece.start, note.end);
    }
    if (piece.start < bass.end)
      notes.push_back(piece);
  }
  if (commit(std::move(notes), start, true, grown) && grown)
    set_status(QStringLiteral("The clip grew to %1 bars so the phrase can vary")
                   .arg(phrase_bars[static_cast<std::size_t>(phrase_)]));
}

std::optional<std::pair<project::InsertSlotId, bool>> HarmonyController::track_comp() const {
  const auto* current = session();
  const auto view = clip_view();
  if (current == nullptr || !view)
    return std::nullopt;
  const auto& tracks = current->snapshot().tracks;
  const auto track = std::ranges::find(tracks, view->track, &project::Track::id);
  if (track == tracks.end())
    return std::nullopt;
  for (const auto& insert : track->note_effects)
    if (insert.engine == application::comp::engine_id)
      return std::pair{insert.id, insert.enabled};
  return std::nullopt;
}

void HarmonyController::set_live_rhythm(bool live) { apply_live_rhythm(live, std::nullopt); }

void HarmonyController::apply_live_rhythm(bool live, std::optional<std::uint64_t> gesture) {
  auto* current = session();
  const auto view = clip_view();
  if (current == nullptr || !view || (hooks_.can_edit && !hooks_.can_edit()))
    return;
  const auto comp = track_comp();
  foundation::Result<void> applied = foundation::Result<void>::success();
  if (!live) {
    if (!comp || !comp->second)
      return;
    applied = current->track_devices().set_note_effect_enabled(view->track, comp->first, false);
  } else {
    if (rhythm_pattern_ < first_comp_rhythm)
      rhythm_pattern_ = first_comp_rhythm;
    std::vector<project::NativeInstrumentParameter> saved;
    if (comp) {
      const auto& tracks = current->snapshot().tracks;
      const auto track = std::ranges::find(tracks, view->track, &project::Track::id);
      const auto insert =
          std::ranges::find(track->note_effects, comp->first, &project::NoteEffectInsert::id);
      saved = insert->parameters;
    }
    auto parameters = rhythm_parameters(saved);
    if (!parameters)
      return;
    if (comp) {
      // Switched on with its settings in one undo step.
      applied = current->track_devices().enable_note_effect_with_parameters(
          view->track, comp->first, std::move(*parameters), gesture);
    } else {
      // First in the chain, so it plays what the clip holds.
      const auto added = current->track_devices().add_note_effect(
          view->track, 0, application::comp::engine_id, std::move(*parameters));
      if (!added)
        applied = foundation::Result<void>::failure(added.error());
    }
    emit rhythmChanged();
  }
  if (!applied) {
    set_status(text(applied.error().message));
    return;
  }
  if (hooks_.commit)
    hooks_.commit();
  project_changed();
}

QStringList HarmonyController::phrase_names() const { return choice_names(phrase_id); }

QVariantList HarmonyController::top_cycles() const {
  QVariantList out;
  for (const int cycle : top_cycle_steps)
    out.push_back(cycle);
  return out;
}

bool HarmonyController::motion_on() const noexcept {
  return variation_ > 0.0 || turnaround_ || drift_ || swell_ > 0.0 ||
         top_cycle_steps[static_cast<std::size_t>(top_cycle_)] !=
             native_dsp::note_effects::comp_steps_per_bar;
}

std::int64_t HarmonyController::phrase_ticks() const noexcept {
  return phrase_bars[static_cast<std::size_t>(phrase_)] * beats_per_bar * quarter;
}

void HarmonyController::set_variation(double amount) {
  const double clamped = std::clamp(amount, 0.0, 1.0);
  if (clamped == variation_)
    return;
  variation_ = clamped;
  emit rhythmChanged();
  update_live_rhythm(rhythm_drag());
}

void HarmonyController::set_phrase(int choice) {
  if (choice < 0 || choice >= static_cast<int>(phrase_bars.size()) || choice == phrase_)
    return;
  phrase_ = choice;
  emit rhythmChanged();
  update_live_rhythm();
}

void HarmonyController::set_turnaround(bool on) {
  if (on == turnaround_)
    return;
  turnaround_ = on;
  emit rhythmChanged();
  update_live_rhythm();
}

void HarmonyController::set_drift(bool on) {
  if (on == drift_)
    return;
  drift_ = on;
  emit rhythmChanged();
  update_live_rhythm();
}

void HarmonyController::set_swell(double amount) {
  const double clamped = std::clamp(amount, 0.0, 1.0);
  if (clamped == swell_)
    return;
  swell_ = clamped;
  emit rhythmChanged();
  update_live_rhythm(rhythm_drag());
}

void HarmonyController::set_top_cycle(int choice) {
  if (choice < 0 || choice >= static_cast<int>(top_cycle_steps.size()) || choice == top_cycle_)
    return;
  top_cycle_ = choice;
  emit rhythmChanged();
  update_live_rhythm();
}

void HarmonyController::set_density(double amount) {
  const double clamped = std::clamp(amount, 0.0, 1.0);
  if (clamped == density_)
    return;
  density_ = clamped;
  emit rhythmChanged();
  update_live_rhythm(rhythm_drag());
}

void HarmonyController::set_rhythm_complexity(double amount) {
  const double c = std::clamp(amount, 0.0, 1.0);
  rhythm_complexity_ = c;
  density_ = c;
  variation_ = c * variation_at_top;
  swell_ = c * swell_at_top;
  turnaround_ = c >= turnarounds_from;
  lift_ = c >= lift_from;
  drift_ = c >= drift_from;
  top_cycle_ = static_cast<int>(band_of(c, polymeter_from));
  push_chance_ =
      std::min(1.0, pattern_push_ + std::max(0.0, c - push_from) / (1.0 - push_from) * push_most);
  emit rhythmChanged();
  update_live_rhythm(rhythm_drag());
}

void HarmonyController::set_harmony_complexity(double amount) {
  const double c = std::clamp(amount, 0.0, 1.0);
  harmony_complexity_ = c;
  const auto colour = colour_bands[band_of(c, colour_from)];
  const auto pace = pace_choices[band_of(c, pace_from)] * quarter;
  auto devices = static_cast<harmony::DeviceSet>(lab_.devices &
                                                 (harmony::device_bit(harmony::Device::pedal) |
                                                  harmony::device_bit(harmony::Device::pull_home) |
                                                  harmony::device_bit(harmony::Device::slide)));
  for (const auto& [device, from] : devices_from)
    if (c >= from)
      devices = static_cast<harmony::DeviceSet>(devices | harmony::device_bit(device));
  const bool moved = colour != lab_.colour || pace != lab_.chord_ticks || devices != lab_.devices;
  lab_.colour = colour;
  lab_.chord_ticks = pace;
  lab_.devices = devices;
  if (moved)
    regenerate();
  emit labChanged();
  if (!moved)
    return;
  if (!mood_gesture_)
    mood_gesture_ = next_gesture_++;
  write_suggestion(false);
}

void HarmonyController::finish_harmony_complexity() { finish_mood(); }

void HarmonyController::set_articulation(double amount) {
  const double clamped = std::clamp(amount, articulation_least, 1.0);
  if (clamped == articulation_)
    return;
  articulation_ = clamped;
  emit rhythmChanged();
  update_live_rhythm(rhythm_drag());
}

void HarmonyController::set_tonic_pulse(int choice) {
  if (choice < 0 || choice >= static_cast<int>(pulse_names.size()) || choice == tonic_pulse_)
    return;
  tonic_pulse_ = choice;
  emit rhythmChanged();
}

void HarmonyController::set_bass_style(int choice) {
  if (choice < 0 || choice >= static_cast<int>(bass_style_names.size()) || choice == bass_style_)
    return;
  bass_style_ = choice;
  emit rhythmChanged();
}

QVariantList HarmonyController::layers() const {
  QVariantList out;
  for (std::size_t i = 0; i < layer_names.size(); ++i)
    out.push_back(QVariantMap{{QStringLiteral("index"), static_cast<int>(i)},
                              {QStringLiteral("name"), text(layer_names[i].first)},
                              {QStringLiteral("summary"), text(layer_names[i].second)}});
  return out;
}

QStringList HarmonyController::tonic_pulses() const {
  QStringList out;
  for (const auto name : pulse_names)
    out.push_back(text(name));
  return out;
}

QStringList HarmonyController::bass_styles() const {
  QStringList out;
  for (const auto name : bass_style_names)
    out.push_back(text(name));
  return out;
}

void HarmonyController::add_layer(int kind) {
  auto* current = session();
  const auto view = clip_view();
  if (current == nullptr || !view || kind < 0 || kind >= static_cast<int>(layer_names.size()) ||
      (hooks_.can_edit && !hooks_.can_edit()))
    return;
  if (lane_.empty()) {
    set_status(QStringLiteral("Write some chords first: a layer follows them"));
    return;
  }
  // Where the chord clip sits in the song, and its track's place and name.
  const auto& tracks = current->snapshot().tracks;
  const auto source = std::ranges::find(tracks, view->track, &project::Track::id);
  const project::MidiClip* clip = nullptr;
  for (const auto& candidate : source->midi_clips)
    if (candidate.id == view->clip)
      clip = &candidate;
  const auto* musical =
      clip != nullptr ? std::get_if<project::MusicalInterval>(&clip->placement) : nullptr;
  if (musical == nullptr) {
    set_status(
        QStringLiteral("Layers follow clips on the bar grid: switch the clip to bars first"));
    return;
  }
  const auto start = musical->start;
  const auto index = static_cast<std::size_t>(source - tracks.begin()) + 1;
  const auto source_name = source->name;
  const auto layer = static_cast<Layer>(kind);
  std::vector<project::MidiNote> notes;
  switch (layer) {
  case Layer::pad:
    notes = harmony::pad_layer(lane_);
    break;
  case Layer::pluck:
    notes = harmony::pluck_layer(lane_);
    break;
  case Layer::tonic:
    notes = harmony::running_tonic(view->length, key().tonic,
                                   static_cast<harmony::Pulse>(tonic_pulse_));
    break;
  case Layer::bass:
    notes = harmony::bass_line(lane_, static_cast<harmony::BassStyle>(bass_style_));
    break;
  }
  const auto track = current->instruments().add_synth_track(index);
  if (!track) {
    set_status(text(track.error().message));
    return;
  }
  static_cast<void>(current->tracks().set_track_name(
      track.value(),
      source_name + " · " + std::string(layer_names[static_cast<std::size_t>(kind)].first)));
  const auto added = current->midi_clips().add_midi_clip_with_notes(track.value(), start,
                                                                    view->length, std::move(notes));
  if (!added) {
    set_status(text(added.error().message));
    return;
  }
  if (layer == Layer::pluck) {
    // The rhythmic supporter: the rhythm chosen here (Held plays Comp's first).
    const int pattern = rhythm_pattern_;
    if (rhythm_pattern_ < first_comp_rhythm)
      rhythm_pattern_ = first_comp_rhythm;
    auto parameters = rhythm_parameters({});
    rhythm_pattern_ = pattern;
    if (parameters)
      static_cast<void>(current->track_devices().add_note_effect(
          track.value(), 0, application::comp::engine_id, std::move(*parameters)));
  }
  set_status(QStringLiteral("Added %1 on a new track")
                 .arg(text(layer_names[static_cast<std::size_t>(kind)].first)));
  if (hooks_.commit)
    hooks_.commit();
}

void HarmonyController::set_inversion(int inversion) {
  if (lane_.empty() || inversion < 0)
    return;
  const auto& chord = lane_[static_cast<std::size_t>(selected_)].chord;
  // The chord's tones in order: the root, the third (or what stands for
  // it), the fifth; inversion n puts tone n in the bass.
  const auto iv = harmony::intervals(chord.quality);
  constexpr int second_inversion = 2;
  if (inversion > second_inversion || static_cast<std::size_t>(inversion) >= iv.count)
    return;
  set_bass(inversion == 0 ? -1
                          : (chord.root + iv.semitones[static_cast<std::size_t>(inversion)]) %
                                harmony::pitch_classes);
}

void HarmonyController::set_lift(bool on) {
  if (on == lift_)
    return;
  lift_ = on;
  emit rhythmChanged();
  update_live_rhythm();
}

QVariantList HarmonyController::voicing_styles() const {
  QVariantList out;
  for (const auto& [style, label] : voicing_style_names)
    out.push_back(QVariantMap{{QStringLiteral("value"), static_cast<int>(style)},
                              {QStringLiteral("name"), text(label)}});
  return out;
}

void HarmonyController::update_live_rhythm(std::optional<std::uint64_t> gesture) {
  if (live_rhythm_)
    apply_live_rhythm(true, gesture);
}

std::uint64_t HarmonyController::rhythm_drag() {
  if (!rhythm_gesture_)
    rhythm_gesture_ = next_gesture_++;
  return *rhythm_gesture_;
}

void HarmonyController::finish_rhythm_drag() { rhythm_gesture_.reset(); }

} // namespace oscillate::quick
