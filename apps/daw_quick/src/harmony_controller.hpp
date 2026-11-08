#pragma once

#include "keyed_list_model.hpp"
#include "oscillate/harmony/clip.hpp"
#include "oscillate/harmony/harmony.hpp"
#include "oscillate/native_dsp/note_effects/note_effect.hpp"
#include "oscillate/project/model.hpp"
#include "status_text.hpp"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace oscillate::application {
class ProjectSession;
}

namespace oscillate::quick {

// Chord clips (ADR-0084, docs/ui/chord-clips-mockup.html): the chords a MIDI
// clip's notes make, for the piano roll's chord strip, and the Write, Chord
// and Rhythm tools beside it. The chords are read from the notes
// (harmony::recognise), so editing a note renames its chord. Every tool
// computes the clip's notes after it (harmony::rewrite, harmony::write, or
// Comp offline) and commits them as one undo step; a mood-pad drag is one.
class HarmonyController final : public QObject {
  Q_OBJECT
  // The clip the piano roll shows; empty for none.
  Q_PROPERTY(QString clipId READ clip_id WRITE set_clip_id NOTIFY clipChanged)
  // The strip: {index, name, roman, role, tension, startTicks, endTicks, drone}.
  Q_PROPERTY(QObject* chords READ chords CONSTANT)
  Q_PROPERTY(int selected READ selected NOTIFY changed)
  Q_PROPERTY(int count READ count NOTIFY changed)
  Q_PROPERTY(QString keyName READ key_name NOTIFY changed)
  // {name, roman, role, tension, inKey, root, quality, bass (-1 for the root),
  //  beats, startTicks, endTicks, voices: [name]}
  Q_PROPERTY(QVariantMap selection READ selection NOTIFY changed)
  // The twelve roots spelt for the key, and the qualities: [{value, suffix}].
  Q_PROPERTY(QStringList rootNames READ root_names NOTIFY changed)
  Q_PROPERTY(QVariantList qualities READ qualities CONSTANT)
  // Write: the Progression Lab.
  Q_PROPERTY(double brightness READ brightness NOTIFY labChanged)
  Q_PROPERTY(double restlessness READ restlessness NOTIFY labChanged)
  Q_PROPERTY(int modeIndex READ mode_index NOTIFY labChanged)
  Q_PROPERTY(QString modeName READ mode_name NOTIFY labChanged)
  Q_PROPERTY(QString feel READ feel NOTIFY labChanged)
  Q_PROPERTY(QString motion READ motion NOTIFY labChanged)
  Q_PROPERTY(QStringList modeNames READ mode_names NOTIFY labChanged)
  // Write's Complexity: plain triads changing slowly up to lush chords
  // changing every half bar with devices; it sets colour, pace and devices.
  Q_PROPERTY(double harmonyComplexity READ harmony_complexity NOTIFY labChanged)
  // With a song key: the pad offers the key's three modes (on), or all six.
  Q_PROPERTY(bool hasKey READ has_key NOTIFY labChanged)
  Q_PROPERTY(bool inKey READ key_modes_only WRITE set_key_modes_only NOTIFY labChanged)
  Q_PROPERTY(int shape READ shape NOTIFY labChanged)
  Q_PROPERTY(int colour READ colour NOTIFY labChanged)
  Q_PROPERTY(int paceBeats READ pace_beats NOTIFY labChanged)
  Q_PROPERTY(int suggestion READ suggestion NOTIFY labChanged)
  // [{index, name, summary, degrees: [int]}]
  Q_PROPERTY(QVariantList shapes READ shapes CONSTANT)
  // [{index, name, hint, on}]
  Q_PROPERTY(QVariantList devices READ devices NOTIFY labChanged)
  // [{index, title, label, chords}]
  Q_PROPERTY(QVariantList suggestions READ suggestions NOTIFY labChanged)
  // Chord: what comes next from the selected chord:
  // [{index, name, roman, likelihood, tension, shared}].
  Q_PROPERTY(QVariantList next READ next NOTIFY changed)
  Q_PROPERTY(bool insertNext READ insert_next WRITE set_insert_next NOTIFY viewChanged)
  // [{index, title, preview}]
  Q_PROPERTY(QVariantList substitutions READ substitutions NOTIFY changed)
  // [{index, title, why, preview, already}]
  Q_PROPERTY(QVariantList hints READ hints NOTIFY changed)
  Q_PROPERTY(int voicingStyle READ voicing_style NOTIFY viewChanged)
  // The bass under the chords (on), or the chords alone.
  Q_PROPERTY(bool bass READ bass NOTIFY viewChanged)
  // [{value, name}], Open first.
  Q_PROPERTY(QVariantList voicingStyles READ voicing_styles CONSTANT)
  Q_PROPERTY(QString status READ status NOTIFY changed)
  // Rhythm: Held, Push and land, then Comp's re-struck patterns:
  // [{index, name, summary, restruck}].
  Q_PROPERTY(QVariantList rhythmPatterns READ rhythm_patterns CONSTANT)
  Q_PROPERTY(int rhythmPattern READ rhythm_pattern NOTIFY rhythmChanged)
  Q_PROPERTY(double pushChance READ push_chance NOTIFY rhythmChanged)
  Q_PROPERTY(int pushBy READ push_by NOTIFY rhythmChanged)
  Q_PROPERTY(int groove READ groove NOTIFY rhythmChanged)
  Q_PROPERTY(QStringList pushNames READ push_names CONSTANT)
  Q_PROPERTY(QStringList grooveNames READ groove_names CONSTANT)
  // Apply acts on the selected chord only, else the whole clip.
  Q_PROPERTY(bool rhythmSelectedOnly READ rhythm_selected_only WRITE set_rhythm_selected_only NOTIFY
                 rhythmChanged)
  // A Comp on the clip's track plays the rhythm live over the held chords.
  Q_PROPERTY(bool liveRhythm READ live_rhythm NOTIFY changed)
  // Motion: small changes that keep the loop moving, measured in phrases.
  Q_PROPERTY(double variation READ variation NOTIFY rhythmChanged)
  Q_PROPERTY(int phrase READ phrase NOTIFY rhythmChanged)
  Q_PROPERTY(QStringList phraseNames READ phrase_names CONSTANT)
  Q_PROPERTY(bool turnaround READ turnaround NOTIFY rhythmChanged)
  Q_PROPERTY(bool drift READ drift NOTIFY rhythmChanged)
  Q_PROPERTY(double swell READ swell NOTIFY rhythmChanged)
  Q_PROPERTY(double density READ density NOTIFY rhythmChanged)
  Q_PROPERTY(bool lift READ lift NOTIFY rhythmChanged)
  // How early held chords change: none, a 16th, an 8th, a dotted 8th, varied.
  Q_PROPERTY(int anticipation READ anticipation NOTIFY rhythmChanged)
  Q_PROPERTY(QStringList anticipationNames READ anticipation_names CONSTANT)
  // Movement on the clip's track: [{index, name, summary}].
  Q_PROPERTY(QVariantList movements READ movements CONSTANT)
  // Articulation: Comp's hits from staccato (0.1) to as written (1).
  Q_PROPERTY(double articulation READ articulation NOTIFY rhythmChanged)
  // Layers: parts on their own tracks that follow the clip's chords.
  // [{index, name, summary}]
  Q_PROPERTY(QVariantList layers READ layers CONSTANT)
  Q_PROPERTY(int tonicPulse READ tonic_pulse NOTIFY rhythmChanged)
  Q_PROPERTY(QStringList tonicPulses READ tonic_pulses CONSTANT)
  Q_PROPERTY(int bassStyle READ bass_style NOTIFY rhythmChanged)
  Q_PROPERTY(QStringList bassStyles READ bass_styles CONSTANT)
  // Rhythm's Complexity: one knob over density and Motion.
  Q_PROPERTY(double rhythmComplexity READ rhythm_complexity NOTIFY rhythmChanged)
  // The top voice's cycle against the bar: [16, 12, 7] steps.
  Q_PROPERTY(int topCycle READ top_cycle NOTIFY rhythmChanged)
  Q_PROPERTY(QVariantList topCycles READ top_cycles CONSTANT)

public:
  struct Hooks {
    std::function<application::ProjectSession*()> session;
    // Whether the project may change now (reports why not).
    std::function<bool()> can_edit;
    // After the project changed here: refresh the views and playback.
    std::function<void()> commit;
  };

  // Lab pace choices, in beats per chord.
  static constexpr std::array<int, 3> pace_choices{8, 4, 2};
  // The key the Lab and names use when the song has none.
  static constexpr project::MusicalKey fallback_key{9, project::KeyMode::minor};
  // Rhythm 0 holds the chords (how melodic house plays them), 1 is push and
  // land; Comp's re-struck patterns follow.
  static constexpr int held_rhythm = 0;
  static constexpr int push_land_rhythm = 1;
  static constexpr int first_comp_rhythm = 2;
  // Motion's starting point: subtle, so a loop breathes without busying.
  static constexpr double default_variation = 0.3;
  static constexpr double default_swell = 0.3;
  static constexpr double default_density = 0.5;
  // The knobs start where the defaults above (and the Lab's) sit.
  static constexpr double default_rhythm_complexity = 0.5;
  static constexpr double default_harmony_complexity = 0.4;
  // The top voice's cycles, in 16ths: the bar, then two polymeters.
  static constexpr std::array<int, 3> top_cycle_steps{native_dsp::note_effects::comp_steps_per_bar,
                                                      12, 7};

  explicit HarmonyController(Hooks hooks, QObject* parent = nullptr);

  // The strip.
  Q_INVOKABLE void select(int index);
  // The chord under a clip tick, or -1.
  int index_at_tick(double ticks) const;
  // The chord's sound: its notes at its start, lowest first, to audition.
  QVariantList audition_keys(int index) const;
  // Removes the chord's notes.
  Q_INVOKABLE void remove(int index);

  // Chord: each re-voices the selected chord's notes in their rhythm.
  Q_INVOKABLE void set_root(int pitch_class);
  Q_INVOKABLE void set_quality(int quality);
  // -1 puts the root back in the bass.
  Q_INVOKABLE void set_bass(int pitch_class);
  Q_INVOKABLE void set_voicing_style(int style);
  // Turns the clip's bass notes on or off, keeping the rhythm; one undo step.
  Q_INVOKABLE void set_bass(bool on);
  Q_INVOKABLE void apply_substitution(int index);
  Q_INVOKABLE void apply_hint(int index);
  Q_INVOKABLE void use_next(int index);

  // Write: each choice writes the current suggestion over the clip as held
  // chords, tiled to the clip's length.
  Q_INVOKABLE void set_mood(double brightness, double restlessness);
  // The pad was released: ends the drag's undo step.
  Q_INVOKABLE void finish_mood();
  Q_INVOKABLE void set_shape(int shape);
  Q_INVOKABLE void set_colour(int colour);
  Q_INVOKABLE void set_pace_beats(int beats);
  Q_INVOKABLE void toggle_device(int device);
  Q_INVOKABLE void choose_suggestion(int index);
  Q_INVOKABLE void more();

  // Rhythm. Choices update a live Comp at once.
  Q_INVOKABLE void set_rhythm_pattern(int index);
  Q_INVOKABLE void set_push_chance(double chance);
  Q_INVOKABLE void set_push_by(int choice);
  Q_INVOKABLE void set_groove(int choice);
  // Rewrites the notes as the rhythm plays them (Held: as held chords).
  Q_INVOKABLE void apply_rhythm();
  // On: a Comp on the clip's track plays the rhythm over the held chords.
  Q_INVOKABLE void set_live_rhythm(bool live);
  Q_INVOKABLE void set_variation(double amount);
  Q_INVOKABLE void set_phrase(int choice);
  Q_INVOKABLE void set_turnaround(bool on);
  Q_INVOKABLE void set_drift(bool on);
  Q_INVOKABLE void set_swell(double amount);
  Q_INVOKABLE void set_top_cycle(int choice);
  Q_INVOKABLE void set_density(double amount);
  Q_INVOKABLE void set_lift(bool on);
  Q_INVOKABLE void set_anticipation(int choice);
  // Adds movement to the clip's track as an effect at the end of its
  // chain: a Pump with the kick, a filter breathing on a dotted 8th, or a
  // dotted-8th delay.
  Q_INVOKABLE void add_movement(int kind);
  Q_INVOKABLE void set_articulation(double amount);
  Q_INVOKABLE void set_tonic_pulse(int choice);
  Q_INVOKABLE void set_bass_style(int choice);
  // Adds a layer that follows the clip's chords on a new track under it:
  // a pad, a pluck rhythmized by a Chord Comp, a running tonic or a bass.
  Q_INVOKABLE void add_layer(int kind);
  // The selected chord in root position (0), first inversion (its third in
  // the bass, 1) or second (its fifth, 2).
  Q_INVOKABLE void set_inversion(int inversion);
  // Sets density and Motion together, from simple (0) to intricate (1).
  Q_INVOKABLE void set_rhythm_complexity(double amount);
  // Writes the suggestion at the new colour, pace and devices as it crosses
  // each; a drag is one undo step, ended by finish_harmony_complexity.
  Q_INVOKABLE void set_harmony_complexity(double amount);
  Q_INVOKABLE void finish_harmony_complexity();
  // A drag of a rhythm slider (push chance, articulation, complexity,
  // density, variation, swell) changes a live Comp in one undo step, ended
  // by finish_rhythm_drag.
  Q_INVOKABLE void finish_rhythm_drag();

  [[nodiscard]] QString clip_id() const { return clip_id_; }
  void set_clip_id(const QString& clip_id);
  [[nodiscard]] QObject* chords() noexcept { return &chords_; }
  [[nodiscard]] int selected() const noexcept { return selected_; }
  [[nodiscard]] int count() const noexcept { return static_cast<int>(lane_.size()); }
  [[nodiscard]] QString key_name() const;
  [[nodiscard]] QVariantMap selection() const;
  [[nodiscard]] QStringList root_names() const;
  [[nodiscard]] QVariantList qualities() const;
  [[nodiscard]] double brightness() const noexcept { return lab_.mood.brightness; }
  [[nodiscard]] double restlessness() const noexcept { return lab_.mood.restlessness; }
  [[nodiscard]] int mode_index() const noexcept;
  [[nodiscard]] QString mode_name() const;
  [[nodiscard]] QString feel() const;
  [[nodiscard]] QString motion() const;
  [[nodiscard]] QStringList mode_names() const;
  [[nodiscard]] bool has_key() const noexcept { return song_key_.has_value(); }
  [[nodiscard]] bool key_modes_only() const noexcept { return in_key_; }
  void set_key_modes_only(bool in_key);
  [[nodiscard]] int shape() const noexcept { return static_cast<int>(lab_.shape); }
  [[nodiscard]] int colour() const noexcept { return static_cast<int>(lab_.colour); }
  [[nodiscard]] int pace_beats() const noexcept;
  [[nodiscard]] int suggestion() const noexcept { return suggestion_; }
  [[nodiscard]] QVariantList shapes() const;
  [[nodiscard]] QVariantList devices() const;
  [[nodiscard]] QVariantList suggestions() const;
  [[nodiscard]] QVariantList next() const;
  [[nodiscard]] bool insert_next() const noexcept { return insert_next_; }
  void set_insert_next(bool insert);
  [[nodiscard]] QVariantList substitutions() const;
  [[nodiscard]] QVariantList hints() const;
  [[nodiscard]] int voicing_style() const noexcept { return static_cast<int>(voicing_.style); }
  [[nodiscard]] bool bass() const noexcept { return voicing_.bass; }
  [[nodiscard]] QString status() const { return status_.text(); }
  [[nodiscard]] QVariantList rhythm_patterns() const;
  [[nodiscard]] int rhythm_pattern() const noexcept { return rhythm_pattern_; }
  [[nodiscard]] double push_chance() const noexcept { return push_chance_; }
  [[nodiscard]] int push_by() const noexcept { return push_by_; }
  [[nodiscard]] int groove() const noexcept { return groove_; }
  [[nodiscard]] QStringList push_names() const;
  [[nodiscard]] QStringList groove_names() const;
  [[nodiscard]] bool rhythm_selected_only() const noexcept { return rhythm_selected_only_; }
  void set_rhythm_selected_only(bool selected_only);
  [[nodiscard]] bool live_rhythm() const noexcept { return live_rhythm_; }
  [[nodiscard]] double variation() const noexcept { return variation_; }
  [[nodiscard]] int phrase() const noexcept { return phrase_; }
  [[nodiscard]] QStringList phrase_names() const;
  [[nodiscard]] bool turnaround() const noexcept { return turnaround_; }
  [[nodiscard]] bool drift() const noexcept { return drift_; }
  [[nodiscard]] double swell() const noexcept { return swell_; }
  [[nodiscard]] double density() const noexcept { return density_; }
  [[nodiscard]] bool lift() const noexcept { return lift_; }
  [[nodiscard]] int anticipation() const noexcept { return anticipation_; }
  [[nodiscard]] QStringList anticipation_names() const;
  [[nodiscard]] QVariantList movements() const;
  [[nodiscard]] double articulation() const noexcept { return articulation_; }
  [[nodiscard]] QVariantList layers() const;
  [[nodiscard]] int tonic_pulse() const noexcept { return tonic_pulse_; }
  [[nodiscard]] QStringList tonic_pulses() const;
  [[nodiscard]] int bass_style() const noexcept { return bass_style_; }
  [[nodiscard]] QStringList bass_styles() const;
  [[nodiscard]] QVariantList voicing_styles() const;
  [[nodiscard]] double rhythm_complexity() const noexcept { return rhythm_complexity_; }
  [[nodiscard]] double harmony_complexity() const noexcept { return harmony_complexity_; }
  [[nodiscard]] int top_cycle() const noexcept { return top_cycle_; }
  [[nodiscard]] QVariantList top_cycles() const;

  // The project's revision moved (or another project opened).
  void project_changed();
  [[nodiscard]] const std::vector<harmony::ChordRegion>& lane() const noexcept { return lane_; }

signals:
  void changed();
  void clipChanged();
  void labChanged();
  void viewChanged();
  void rhythmChanged();
  // A chord to hear on the clip's track: MIDI keys, lowest first.
  void auditionRequested(QString trackId, QVariantList keys);

private:
  struct ClipView {
    project::ClipId clip;
    project::TrackId track;
    timeline::TickPosition length;
    std::vector<project::MidiNote> notes;
  };
  [[nodiscard]] application::ProjectSession* session() const;
  [[nodiscard]] std::optional<ClipView> clip_view() const;
  [[nodiscard]] project::MusicalKey key() const;
  // Commits `notes` as the clip's notes (one undo step, or part of the
  // mood drag's), with the pattern `length` (the clip's own when none), then
  // selects the chord under `select_tick`.
  [[nodiscard]] std::vector<project::MidiNote>
  kept_around(std::span<const project::MidiNote> notes, std::int64_t start, std::int64_t end) const;
  bool commit(std::vector<project::MidiNote> notes, std::int64_t select_tick, bool audition,
              std::optional<timeline::TickPosition> length = std::nullopt);
  // Whether Motion changes anything, and the phrase's length in ticks.
  [[nodiscard]] bool motion_on() const noexcept;
  [[nodiscard]] std::int64_t phrase_ticks() const noexcept;
  // The clip's notes with `lane`; `revoice` re-voices that region even when
  // its notes already make its chord.
  bool commit_lane(const std::vector<harmony::ChordRegion>& lane, std::size_t select,
                   std::optional<std::size_t> revoice = std::nullopt);
  // The selected chord changed to `chord`, re-voiced in its rhythm.
  void change_selected(const harmony::ChordSymbol& chord);
  void write_suggestion(bool audition);
  void regenerate();
  // The Lab follows the song key's family when there is one and in_key_.
  void apply_key_mode();
  void project_lane();
  // Comp's saved parameters for the rhythm chosen (pattern 1 and up).
  [[nodiscard]] std::optional<std::vector<project::NativeInstrumentParameter>>
  rhythm_parameters(std::span<const project::NativeInstrumentParameter> current) const;
  // The Comp on the clip's track, if any: its slot and whether it plays.
  [[nodiscard]] std::optional<std::pair<project::InsertSlotId, bool>> track_comp() const;
  // A live Comp takes the rhythm chosen now; within a slider drag, its
  // changes share one gesture.
  void update_live_rhythm(std::optional<std::uint64_t> gesture = {});
  // The open rhythm drag's gesture, opened by the first move.
  [[nodiscard]] std::uint64_t rhythm_drag();
  void apply_live_rhythm(bool live, std::optional<std::uint64_t> gesture);
  void set_status(QString status);

  Hooks hooks_;
  KeyedListModel chords_;
  QString clip_id_;
  std::optional<project::ClipId> clip_;
  // What the lane was read from, to skip unchanged revisions.
  std::vector<project::MidiNote> read_notes_;
  std::vector<harmony::ChordRegion> lane_;
  std::optional<project::MusicalKey> song_key_;
  // Melodic house's Open voicing in its register (ADR-0084).
  harmony::VoicingOptions voicing_ = harmony::open_voicing();
  harmony::LabOptions lab_;
  std::array<harmony::Suggestion, harmony::variant_count> suggestions_;
  int suggestion_ = 0;
  // While the pad is dragged: the last mood bucket written.
  int mood_bucket_ = -1;
  // One mood-pad drag is one undo step: its writes share a gesture.
  std::optional<std::uint64_t> mood_gesture_;
  // One rhythm-slider drag is one undo step on a live Comp.
  std::optional<std::uint64_t> rhythm_gesture_;
  std::uint64_t next_gesture_ = 0x4841'0000'0000'0001ULL;
  int selected_ = 0;
  bool insert_next_ = false;
  bool in_key_ = true;
  bool busy_ = false;
  StatusText status_;
  // Comp's first pattern, with its push and groove defaults (the constructor
  // reads them from the engine).
  int rhythm_pattern_ = held_rhythm;
  int anticipation_ = static_cast<int>(harmony::Anticipation::varied);
  double push_chance_ = 0.0;
  int push_by_ = 0;
  int groove_ = 0;
  bool rhythm_selected_only_ = false;
  bool live_rhythm_ = false;
  double variation_ = default_variation;
  int phrase_ = 0;
  bool turnaround_ = true;
  bool drift_ = true;
  double swell_ = default_swell;
  double density_ = default_density;
  bool lift_ = false;
  double articulation_ = 1.0;
  int tonic_pulse_ = static_cast<int>(harmony::Pulse::eighths);
  int bass_style_ = static_cast<int>(harmony::BassStyle::offbeat);
  int top_cycle_ = 0;
  double rhythm_complexity_ = default_rhythm_complexity;
  double harmony_complexity_ = default_harmony_complexity;
  // The chosen pattern's own push chance, which Complexity adds to.
  double pattern_push_ = 0.0;
};

} // namespace oscillate::quick
