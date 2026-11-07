#pragma once

#include "background_job.hpp"
#include "oscillate/application/clay_edit.hpp"
#include "oscillate/application/project_session.hpp"
#include "oscillate/clay_dsp/take.hpp"
#include "oscillate/persistence/groove_file.hpp"
#include "oscillate/processing/clay_source.hpp"
#include "status_text.hpp"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>

namespace oscillate::quick {

// Clay, the Notes tab of the audio clip editor (ADR-0081 phase 2,
// docs/ui/clay-mockup.html). One clip at a time: detection runs on a worker
// (the analysis is cached on disk), every edit gesture is one undo step
// (a drag previews locally between begin_* and commit_*), and the clip plays
// its rendered take through its normal warp path; the render is warmed on a
// worker after each commit so playback finds it ready.
//
// Notes are addressed by key: "<note id>:<voice>", voice 0 for the note
// itself and 1.. for its harmonies. Times are song seconds; pitches are
// cents on the MIDI scale (6900 = A4).
class ClayController final : public QObject {
  Q_OBJECT
  // The editor shows a clip (open_clip); false after close().
  Q_PROPERTY(bool open READ is_open NOTIFY clipChanged)
  Q_PROPERTY(QString clipId READ clip_id NOTIFY clipChanged)
  Q_PROPERTY(QString clipName READ clip_name NOTIFY clipChanged)
  Q_PROPERTY(QString trackName READ track_name NOTIFY clipChanged)
  // The clip's song span, in seconds.
  Q_PROPERTY(double clipStart READ clip_start NOTIFY clipChanged)
  Q_PROPERTY(double clipEnd READ clip_end NOTIFY clipChanged)
  // Seconds per beat and per bar at the clip, for the ruler and grid.
  Q_PROPERTY(double beatSeconds READ beat_seconds NOTIFY clipChanged)
  Q_PROPERTY(int beatsPerBar READ beats_per_bar NOTIFY clipChanged)
  // The clip has a saved take (detected at least once).
  Q_PROPERTY(bool hasTake READ has_take NOTIFY takeChanged)
  // "melodic", "polyphonic" or "percussive" ("" before detection).
  Q_PROPERTY(QString mode READ mode NOTIFY takeChanged)
  Q_PROPERTY(QStringList modes READ modes CONSTANT)
  // Detection: running, a 0..1 progress and the stage's name.
  Q_PROPERTY(bool detecting READ detecting NOTIFY detectingChanged)
  Q_PROPERTY(double progress READ progress NOTIFY detectingChanged)
  Q_PROPERTY(QString stage READ stage NOTIFY detectingChanged)
  // Last outcome or problem, in plain words.
  Q_PROPERTY(QString status READ status NOTIFY statusChanged)
  // The notes as drawn: [{key, id, voice, harmony, muted, selected, start,
  // end, sourceStart, sourceEnd, centre, originalCentre, pitchCents, formant,
  // gainDb, level (0..1), modulation, drift, noise (0..1), lyric: "",
  // curve: [t, cents, t, cents, ...] as edited, originalCurve: [...],
  // levels: [t, level, ...]}], sorted by start then key.
  Q_PROPERTY(QVariantList notes READ notes NOTIFY notesChanged)
  Q_PROPERTY(QStringList selection READ selection NOTIFY notesChanged)
  // The pitch span worth showing: the notes' range with a margin, cents.
  Q_PROPERTY(double lowCents READ low_cents NOTIFY notesChanged)
  Q_PROPERTY(double highCents READ high_cents NOTIFY notesChanged)
  // The clip's waveform behind the notes: min/max pairs from clipStart to
  // clipEnd.
  Q_PROPERTY(QVariantList waveform READ waveform NOTIFY waveformChanged)
  // The take's key: tonic 0..11 (-1 chromatic), minor, and its name
  // ("A minor", "Chromatic"); keyFromAnalysis while it is still the one
  // detection proposed from the source's key review or the song's key.
  Q_PROPERTY(int keyTonic READ key_tonic NOTIFY takeChanged)
  Q_PROPERTY(bool keyMinor READ key_minor NOTIFY takeChanged)
  Q_PROPERTY(QString keyName READ key_name NOTIFY takeChanged)
  Q_PROPERTY(bool keyFromAnalysis READ key_from_analysis NOTIFY takeChanged)
  Q_PROPERTY(QStringList tonicNames READ tonic_names CONSTANT)
  // The song's key for "Fit to song key" (tonic -1 when the song has none).
  Q_PROPERTY(int songKeyTonic READ song_key_tonic NOTIFY clipChanged)
  Q_PROPERTY(bool songKeyMinor READ song_key_minor NOTIFY clipChanged)
  // Pitch snapping for Main and Pitch drags: "scale", "chromatic" or "off".
  Q_PROPERTY(QString snap MEMBER snap_ NOTIFY snapChanged)
  // "none" (no take), "rendering" (warming the playback render) or "ready".
  Q_PROPERTY(QString renderState READ render_state NOTIFY renderStateChanged)
  // A drag or macro is previewing (not yet an undo step).
  Q_PROPERTY(bool previewing READ previewing NOTIFY notesChanged)
  // Auditions play the original instead of the edit (A/B).
  Q_PROPERTY(bool abOriginal MEMBER ab_original_ NOTIFY abChanged)
  // Instrument tracks with built-in synths, for To MIDI and Follow the
  // bass: [{id, name, engine, hasNotes}].
  Q_PROPERTY(QVariantList instrumentTracks READ instrument_tracks NOTIFY clipChanged)
  // This editor's committed edits, oldest first: [{label}].
  Q_PROPERTY(QVariantList history READ history NOTIFY historyChanged)
  // Quantize divisions and groove names for the Quantize macro.
  Q_PROPERTY(QStringList divisions READ divisions CONSTANT)

public:
  using Session = std::function<application::ProjectSession*()>;
  using CanEdit = std::function<bool()>;
  using Report = std::function<void(QString)>;
  using Commit = std::function<void()>;
  using FindGroove = std::function<std::optional<persistence::GrooveTemplate>(const QString&)>;
  using Step = std::function<void()>;

  ClayController(Session session, CanEdit can_edit, Report report, Commit commit,
                 FindGroove find_groove, Step undo, Step redo, QObject* parent = nullptr);
  ~ClayController() override;

  [[nodiscard]] bool is_open() const noexcept { return clip_.has_value(); }
  [[nodiscard]] QString clip_id() const;
  [[nodiscard]] QString clip_name() const { return clip_name_; }
  [[nodiscard]] QString track_name() const { return track_name_; }
  [[nodiscard]] double clip_start() const noexcept { return clip_start_; }
  [[nodiscard]] double clip_end() const noexcept { return clip_end_; }
  [[nodiscard]] double beat_seconds() const noexcept { return beat_seconds_; }
  [[nodiscard]] int beats_per_bar() const noexcept { return beats_per_bar_; }
  [[nodiscard]] bool has_take() const noexcept { return take_.has_value(); }
  [[nodiscard]] QString mode() const;
  [[nodiscard]] static QStringList modes();
  [[nodiscard]] bool detecting() const noexcept { return job_ != nullptr; }
  [[nodiscard]] double progress() const;
  [[nodiscard]] QString stage() const;
  [[nodiscard]] QString status() const { return status_.text(); }
  [[nodiscard]] QVariantList notes() const { return notes_; }
  [[nodiscard]] QStringList selection() const;
  [[nodiscard]] double low_cents() const noexcept { return low_cents_; }
  [[nodiscard]] double high_cents() const noexcept { return high_cents_; }
  [[nodiscard]] QVariantList waveform() const { return waveform_; }
  // Columns in waveform(): it holds a min/max pair per column.
  [[nodiscard]] static int waveform_columns() noexcept;
  [[nodiscard]] int key_tonic() const;
  [[nodiscard]] bool key_minor() const;
  [[nodiscard]] QString key_name() const;
  [[nodiscard]] bool key_from_analysis() const noexcept { return key_from_analysis_; }
  [[nodiscard]] static QStringList tonic_names();
  [[nodiscard]] int song_key_tonic() const;
  [[nodiscard]] bool song_key_minor() const;
  [[nodiscard]] QString render_state() const { return render_state_; }
  [[nodiscard]] bool previewing() const noexcept { return preview_.has_value(); }
  [[nodiscard]] QString audition_path() const { return audition_path_; }
  [[nodiscard]] QVariantList instrument_tracks() const;
  [[nodiscard]] QVariantList history() const { return history_; }
  [[nodiscard]] static QStringList divisions();

  // Shows `clip_id` (an audio clip). A clip with a take loads its analysis
  // on a worker (from the cache when it is there); one without waits for
  // detect().
  Q_INVOKABLE void open_clip(const QString& clip_id);
  Q_INVOKABLE void close();
  // Detects the clip's notes in `mode` (one of modes). On a clip with no take,
  // or a take in another mode, finishing saves a fresh take (one undo step;
  // a different mode drops the old edits). Otherwise it only loads.
  Q_INVOKABLE void detect(const QString& mode);
  Q_INVOKABLE void cancel_detect();
  // Removes the take: the clip plays its source again (one undo step).
  Q_INVOKABLE void remove_take();

  // Selection, by key.
  Q_INVOKABLE void select(const QString& key, bool additive);
  Q_INVOKABLE void select_all();
  Q_INVOKABLE void clear_selection();
  // Notes overlapping a box of song seconds and cents.
  Q_INVOKABLE void select_box(double from_seconds, double to_seconds, double low_cents,
                              double high_cents, bool additive);

  // A tool drag on the selection, previewed until commit_edit (one undo
  // step) or cancel_edit. Tools: "move" (time and pitch), "pitch", "modulation",
  // "drift", "formant", "amplitude", "time_start", "time_end", "time_move".
  // update_edit's arguments are the drag's totals since begin_edit: song
  // seconds, cents, and `amount` (modulation/drift: a factor on the scale at
  // the start; formant: cents; amplitude: dB). Pitch snaps by `snap`.
  Q_INVOKABLE bool begin_edit(const QString& tool);
  Q_INVOKABLE void update_edit(double seconds, double cents, double amount);
  Q_INVOKABLE void commit_edit();
  Q_INVOKABLE void cancel_edit();
  // The selection's centres onto the nearest scale pitch (Pitch tool
  // double-click), one undo step.
  Q_INVOKABLE void snap_selection();

  // Structure, each one undo step. split cuts a note at song seconds; join
  // merges two touching notes into the first; delete_selection mutes notes
  // and removes harmonies.
  Q_INVOKABLE void split(const QString& key, double seconds);
  Q_INVOKABLE void join(const QString& first, const QString& second);
  Q_INVOKABLE void delete_selection();

  // Macros on the selection (all notes when none is selected), previewed
  // while a slider moves and committed on release as one undo step.
  Q_INVOKABLE void begin_macro();
  // Centre and drift amounts 0..1 toward the take's key.
  Q_INVOKABLE void preview_correct_pitch(double centre, double drift);
  // `grid` is one of divisions or a groove pool name; amount 0..1.
  Q_INVOKABLE void preview_quantize(const QString& grid, double amount);
  Q_INVOKABLE void preview_level(double amount);
  Q_INVOKABLE void commit_macro(const QString& label);

  // The take's key: tonic 0..11 and minor, or chromatic (one undo step).
  Q_INVOKABLE void set_key(int tonic, bool minor);
  Q_INVOKABLE void set_chromatic();

  // Creative tools, each one undo step.
  // To MIDI: the notes as a MIDI clip with bends on `track_id` (a
  // built-in synth track) or a new Nova track when empty.
  Q_INVOKABLE void to_midi(const QString& track_id);
  // Harmonise: a copy of each selected note `degrees` scale steps away
  // (negative below), with its own formant shift and level.
  Q_INVOKABLE void harmonise(int degrees, double formant_cents, double gain_db);
  // Fit to key: from the take's key to tonic/minor; with follow_quality a
  // minor third becomes major (and so on). The take's key becomes the new one.
  Q_INVOKABLE void fit_to_key(int tonic, bool minor, bool follow_quality);
  // Follow the bass: each note moves to the note of `track_id` it overlaps
  // most; time_amount 0..1 moves its timing too; stay_in_key snaps after.
  Q_INVOKABLE void follow_notes(const QString& track_id, double time_amount, bool stay_in_key);
  // Variation: small seeded changes of pitch, timing and level.
  Q_INVOKABLE void variation(int seed, double amount);

  // Auditions render a short WAV (auditionReady gives its path): one note,
  // or the selection's span (all notes when none), edited or, with
  // abOriginal, as recorded.
  Q_INVOKABLE void audition_note(const QString& key);
  Q_INVOKABLE void audition_selection();

  Q_INVOKABLE void undo();
  Q_INVOKABLE void redo();

  // Test hooks: wait for detection, the render warm-up or an audition.
  bool wait_for_detect(int timeout_ms);
  bool wait_for_render(int timeout_ms);
  // Test seam: how detection reads the clip's audio (a slow disk in tests);
  // empty decodes the managed source.
  using Decode = std::function<foundation::Result<processing::clay::DecodedSource>(
      const persistence::ProjectStore&, const project::SourceRecord&, const std::stop_token&)>;
  void set_decoder(Decode decode) { decode_ = std::move(decode); }
  // Detections given up (cancelled) whose worker has not finished yet.
  [[nodiscard]] std::size_t abandoned_detections() { return detect_.abandoned(); }

public slots:
  // The project changed (an edit, undo or redo): the take, notes and the
  // clip's placement follow; a clip that went away closes the editor.
  void project_changed();

signals:
  void clipChanged();
  void takeChanged();
  void detectingChanged();
  void statusChanged();
  void notesChanged();
  void waveformChanged();
  void snapChanged();
  void renderStateChanged();
  void abChanged();
  void historyChanged();
  void auditionReady(const QString& path);

private:
  // A clip's decoded audio and analysis, shared with workers.
  struct Loaded {
    explicit Loaded(project::SourceRecord record) : source(std::move(record)) {}
    project::SourceRecord source;
    project::ClayMode mode = project::ClayMode::melodic;
    processing::clay::DecodedSource audio;
    clay_dsp::Take analysed;
  };
  // A detection's progress, shared with its worker. Cancelling it stops
  // the worker's reading and analysis; the controller never waits for it.
  struct Job {
    std::stop_source cancel;
    std::atomic<double> progress{0.0};
    mutable std::mutex mutex;
    std::string stage;
    // The session the detection started in; a result for another is dropped.
    std::uint64_t session = 0;
  };
  using Detected = foundation::Result<std::shared_ptr<const Loaded>>;
  using Key = std::pair<std::uint32_t, std::uint32_t>;

  [[nodiscard]] application::ProjectSession* editable(const QString& action);
  [[nodiscard]] const project::ClayTake& shown_take() const;
  void start_detect(project::ClayMode mode, bool save);
  void poll();
  void finish_detect();
  void reload_from_snapshot();
  void rebuild_edited();
  void rebuild_notes();
  void rebuild_waveform();
  bool commit(project::ClayTake take, const QString& label);
  void warm_render();
  void set_status(QString status);
  void set_render_state(QString state);
  [[nodiscard]] std::vector<std::uint32_t> selected_notes() const;
  [[nodiscard]] std::vector<Key> scope() const;
  [[nodiscard]] project::ClayNoteEdit edit_for(const project::ClayTake& take, Key key) const;
  void put_edit(project::ClayTake& take, const project::ClayNoteEdit& edit) const;
  [[nodiscard]] const clay_dsp::Note* note_of(std::uint32_t id) const;
  [[nodiscard]] double source_frames_per_second() const;
  [[nodiscard]] std::optional<project::ClayTake> macro_result(
      const std::function<std::vector<clay_dsp::NoteEdit>(std::span<const clay_dsp::NoteEdit>,
                                                          std::span<const std::uint32_t>)>& run);
  void audition_span(double from_frame, double to_frame);

  Session session_;
  CanEdit can_edit_;
  Report report_;
  Commit commit_;
  FindGroove find_groove_;
  Step undo_;
  Step redo_;

  std::optional<project::ClipId> clip_;
  QString clip_name_;
  QString track_name_;
  double clip_start_ = 0.0;
  double clip_end_ = 0.0;
  double beat_seconds_ = 0.5;
  int beats_per_bar_ = 4;
  std::optional<application::clay::ClipTime> time_;
  std::optional<project::MusicalKey> song_key_;
  std::vector<project::TrackId> instrument_ids_;
  QVariantList instrument_tracks_;

  std::optional<project::ClayTake> take_;
  std::optional<project::ClayTake> preview_;
  std::optional<project::ClayTake> base_;
  QString tool_;
  bool key_from_analysis_ = false;
  std::shared_ptr<const Loaded> loaded_;
  std::optional<clay_dsp::Take> edited_;
  std::set<Key> selected_;

  QVariantList notes_;
  QVariantList waveform_;
  double low_cents_ = 4'800.0;
  double high_cents_ = 8'400.0;
  QString snap_ = QStringLiteral("scale");
  StatusText status_;
  QString render_state_ = QStringLiteral("none");
  bool ab_original_ = false;
  QString audition_path_;
  QVariantList history_;

  Decode decode_;
  std::shared_ptr<Job> job_;
  bool save_on_detect_ = false;
  BackgroundJob<Detected> detect_;
  // Stops the running warm render; empty when none runs.
  std::optional<std::stop_source> warm_stop_;
  BackgroundJob<std::optional<foundation::Error>> warm_;
  BackgroundJob<foundation::Result<QString>> audition_;
  QTimer timer_;
};

} // namespace oscillate::quick
