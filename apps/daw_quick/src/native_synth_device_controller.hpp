#pragma once

#include "macro_map_state.hpp"
#include "native_synth_panel_adapter.hpp"
#include "native_synth_visuals.hpp"

#include "oscillate/native_dsp/synth/parameters.hpp"

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace oscillate::quick {

class CrateController;
class NativeSynthDocumentModel;
class NovaConstellation;
class NovaArp;

// Presentation model for a built-in synth device panel, for any engine.
// Everything it shows comes from the engine's parameter table, so new
// parameters and new engines need no controller change; engine-specific
// curves come from NativeSynthVisuals. Edits go to the adapter; the adapter's
// state is the truth.
class NativeSynthDeviceController final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QString engine_id READ engine_id CONSTANT)
  Q_PROPERTY(QString engine_name READ engine_name CONSTANT)
  Q_PROPERTY(bool has_filter_response READ has_filter_response CONSTANT)
  // The engine's designed panel (device_panels.hpp): its QML file (empty
  // when the engine has none), design size, the host inputs it binds by
  // name, and whether it reflows rather than scaling.
  Q_PROPERTY(QString panel_url READ panel_url CONSTANT)
  Q_PROPERTY(int panel_width READ panel_width CONSTANT)
  Q_PROPERTY(int panel_height READ panel_height CONSTANT)
  Q_PROPERTY(QStringList panel_inputs READ panel_inputs CONSTANT)
  Q_PROPERTY(bool panel_reflows READ panel_reflows CONSTANT)
  // Increments whenever any value changes; QML bindings read it to refresh.
  Q_PROPERTY(int revision READ revision NOTIFY values_changed)
  Q_PROPERTY(QVariantList presets READ presets CONSTANT)
  Q_PROPERTY(QStringList preset_categories READ preset_categories CONSTANT)
  // Every factory preset tag, in first-seen order.
  Q_PROPERTY(QStringList preset_tags READ preset_tags CONSTANT)
  Q_PROPERTY(int preset_index READ preset_index NOTIFY values_changed)
  Q_PROPERTY(QString preset_name READ preset_name NOTIFY values_changed)
  Q_PROPERTY(QString preset_category READ preset_category NOTIFY values_changed)
  Q_PROPERTY(bool preset_modified READ preset_modified NOTIFY values_changed)
  Q_PROPERTY(bool deferred READ deferred NOTIFY status_changed)
  Q_PROPERTY(bool editable READ editable NOTIFY status_changed)
  Q_PROPERTY(bool pending READ pending NOTIFY status_changed)
  Q_PROPERTY(QString status_text READ status_text NOTIFY status_changed)
  Q_PROPERTY(QString status_detail READ status_detail NOTIFY status_changed)
  Q_PROPERTY(QString gesture_parameter READ gesture_parameter NOTIFY gesture_changed)
  // The engine's structured document (NativeSynthDocumentModel): routes,
  // curves, chains, media, labels and sequences, each signalling alone.
  Q_PROPERTY(QObject* document READ document CONSTANT)
  // Crate's panel model (CrateController, spec 09 §5.1) when engine_id is
  // "crate"; null for every other engine.
  Q_PROPERTY(QObject* crate READ crate CONSTANT)
  // Nova's Constellation (NovaConstellation, ADR-0077) when engine_id is
  // "nova"; null for every other engine.
  Q_PROPERTY(QObject* constellation READ constellation CONSTANT)
  // Nova's arp pattern engine model (NovaArp, ADR-0078) when engine_id is
  // "nova"; null for every other engine.
  Q_PROPERTY(QObject* arp READ arp CONSTANT)
  // The user's own presets for this engine (ADR-0058), [{name}] sorted by
  // name, saved under $XDG_DATA_HOME/oscillate/presets/<engine>/.
  Q_PROPERTY(QVariantList user_presets READ user_presets NOTIFY user_presets_changed)
  // Favourite presets, as keys from preset_key: factory and user presets
  // alike, kept in favourites.txt beside the user presets.
  Q_PROPERTY(QStringList favourites READ favourites NOTIFY favourites_changed)
  // Bumps whenever a Nova FX module preset is saved or deleted.
  Q_PROPERTY(int fx_presets_revision READ fx_presets_revision NOTIFY fx_presets_changed)
  // How many built-in effects share this one's document (1 when unlinked).
  Q_PROPERTY(int linked_count READ linked_count NOTIFY link_changed)
  // Macros (ADR-0059): bumps whenever which of this device's knobs a track
  // macro drives changes; and the macro MAP mode is mapping (-1 when off).
  Q_PROPERTY(int macro_revision READ macro_revision NOTIFY macros_changed)
  Q_PROPERTY(int macro_map_index READ macro_map_index NOTIFY macro_map_changed)

public:
  // `engine` must outlive the controller (registered engines are static).
  NativeSynthDeviceController(const native_dsp::synth::EngineDescriptor& engine,
                              NativeSynthPanelAdapter& adapter, QObject* parent = nullptr);

  [[nodiscard]] QString engine_id() const;
  [[nodiscard]] QString engine_name() const;
  [[nodiscard]] QString panel_url() const;
  [[nodiscard]] int panel_width() const;
  [[nodiscard]] int panel_height() const;
  [[nodiscard]] QStringList panel_inputs() const;
  [[nodiscard]] bool panel_reflows() const;
  [[nodiscard]] bool has_filter_response() const noexcept { return visuals_ != nullptr; }

  [[nodiscard]] int revision() const noexcept { return revision_; }
  [[nodiscard]] QVariantList presets() const;
  [[nodiscard]] QStringList preset_categories() const;
  [[nodiscard]] QStringList preset_tags() const;
  [[nodiscard]] int preset_index() const noexcept { return preset_index_; }
  [[nodiscard]] QString preset_name() const;
  [[nodiscard]] QString preset_category() const;
  [[nodiscard]] bool preset_modified() const;
  [[nodiscard]] bool deferred() const noexcept { return status_.deferred_changes; }
  [[nodiscard]] bool editable() const noexcept { return status_.editable; }
  [[nodiscard]] bool pending() const noexcept {
    return status_.audible_revision < status_.saved_revision;
  }
  [[nodiscard]] QString status_text() const;
  [[nodiscard]] QString status_detail() const;
  [[nodiscard]] QString gesture_parameter() const { return gesture_parameter_; }
  [[nodiscard]] QObject* document() const;
  [[nodiscard]] QObject* crate() const;
  [[nodiscard]] QObject* constellation() const;
  [[nodiscard]] QObject* arp() const;
  [[nodiscard]] QVariantList user_presets() const;
  [[nodiscard]] QStringList favourites() const { return favourites_; }
  [[nodiscard]] int linked_count() const noexcept { return linked_count_; }

  // Static facts about a parameter: name, group, unit, kind, choices, timing.
  Q_INVOKABLE QVariantMap parameter(const QString& id) const;
  Q_INVOKABLE QStringList parameter_ids(const QString& group) const;
  // Every continuous, automatable live parameter in table order: what a
  // modulation route may target.
  Q_INVOKABLE QStringList modulatable_ids() const;
  Q_INVOKABLE double normalised(const QString& id) const;
  Q_INVOKABLE double value(const QString& id) const;
  Q_INVOKABLE int choice(const QString& id) const;
  Q_INVOKABLE QString display(const QString& id) const;
  // Text for a hypothetical knob position, for drag previews and tooltips.
  Q_INVOKABLE QString display_at(const QString& id, double normalised) const;
  Q_INVOKABLE bool is_default(const QString& id) const;

  Q_INVOKABLE void set_normalised(const QString& id, double normalised);
  Q_INVOKABLE void set_value(const QString& id, double value);
  Q_INVOKABLE void set_choice(const QString& id, int index);
  // Several plain values {id: value} as one edit (one undo step), such as a
  // new effect slot's defaults. Unknown ids are ignored.
  Q_INVOKABLE void set_values(const QVariantMap& values);
  // Keyboard and wheel: one step of a discrete parameter, or 1% (0.1% fine).
  Q_INVOKABLE void step(const QString& id, int direction, bool fine);
  Q_INVOKABLE void reset_to_default(const QString& id);
  Q_INVOKABLE void begin_gesture(const QString& id);
  Q_INVOKABLE void end_gesture(const QString& id);
  Q_INVOKABLE void cancel_gesture(const QString& id);

  Q_INVOKABLE void apply_preset(int index);
  Q_INVOKABLE void step_preset(int direction);

  // Analog-prototype magnitude in dB at `points` log-spaced frequencies from
  // 20 Hz to 20 kHz for the current patch; empty when the engine has none.
  Q_INVOKABLE QVariantList filter_response(int points) const;
  Q_INVOKABLE double frequency_at(double fraction) const;
  Q_INVOKABLE double fraction_of(double hz) const;
  // Pulse pad sketches (empty for other engines): amplitude (peak 1) and
  // swept-body pitch in Hz at `points` times over `seconds`, and the pad's
  // shaping filter in dB on the frequency_at scale.
  Q_INVOKABLE QVariantList pad_envelope(int pad, double seconds, int points) const;
  Q_INVOKABLE QVariantList pad_pitch(int pad, double seconds, int points) const;
  Q_INVOKABLE QVariantList pad_band(int pad, int points) const;
  // Halo sketches (empty for other engines): the routing diagram of
  // algorithm `index` (0-based) as {carriers, feedback, edges [{from, to}],
  // column, row, columns, rows} with 0-based operators, and operator `op`'s
  // envelope as {points, seconds, release}.
  Q_INVOKABLE QVariantMap fm_algorithm(int index) const;
  Q_INVOKABLE QVariantMap operator_envelope(int op, int points) const;
  // Torque sketches (empty for other engines): one cycle of the current
  // model's oscillator (peak 1) at `points` phases, and the Sub/808 pitch
  // drop in semitones above the note at `points` times over `seconds`.
  Q_INVOKABLE QVariantList oscillator_cycle(int points) const;
  Q_INVOKABLE QVariantList pitch_drop(double seconds, int points) const;
  // Drift sketches (empty for other engines): a deterministic preview of the
  // grain cloud over `seconds` as [{time, position, semitones, length,
  // reverse, pan}] (position in seconds behind the write head), the partial
  // bank as [{ratio, level}], grain window `shape` at `points` positions, and
  // the amp envelope as {points, seconds, release}.
  Q_INVOKABLE QVariantList grain_cloud(double seconds, int max_grains) const;
  Q_INVOKABLE QVariantList partial_spectrum() const;
  Q_INVOKABLE QVariantList grain_window(int shape, int points) const;
  Q_INVOKABLE QVariantMap amp_envelope(int points) const;
  // Concert's sample library (empty for other engines): {state ("missing",
  // "invalid" or "ready"), message, directory, name, attribution,
  // install_command, required (the library Concert plays, with its
  // attribution), resident_mb, ring_seconds [88, A0 up], first_key,
  // key_count, last_damped_key}. Looking again after an install is
  // concert_reload_library.
  Q_INVOKABLE QVariantMap concert_library() const;
  Q_INVOKABLE QVariantMap concert_reload_library();
  // Fret's sample library (empty for other engines), as concert_library:
  // {state, message, directory, name, attribution, install_command,
  // required, resident_mb, samples, first_key, last_key}; fret_reload_library
  // looks again after an install.
  Q_INVOKABLE QVariantMap fret_library() const;
  Q_INVOKABLE QVariantMap fret_reload_library();
  // Fret's groove lane (empty for other engines): what Feel plays of the
  // first fret_groove_bars bars of this track's earliest MIDI clip, decided
  // as the kernel decides (fret::feel::decide, then Articulation) with the
  // panel's values, or with the notes as drawn when `as_drawn`:
  // {bars, beats_per_bar, max_velocity, notes: [{position, length (0..1 across the
  // lane), key, name ("E1"), pitch ("E"), drawn (the clip's velocity),
  // velocity, articulation ("pluck", "staccato", "ghost" or "bridge"),
  // pushed, delay_ms}]}. Without a clip, notes is empty. Keys Fret does not
  // play are left out.
  Q_INVOKABLE QVariantMap fret_groove(bool as_drawn) const;
  static constexpr int fret_groove_bars = 2;
  // Wren's sample library (empty for other engines), as fret_library:
  // {state, message, directory, name, attribution, install_command,
  // required, resident_mb, samples, first_key, last_key}; wren_reload_library
  // looks again after an install.
  Q_INVOKABLE QVariantMap wren_library() const;
  Q_INVOKABLE QVariantMap wren_reload_library();
  // Wren's phrase lane (empty for other engines): how Breath plays the first
  // wren_phrase_bars bars of this track's earliest MIDI clip, decided as the
  // kernel decides (wren::breath::decide with the project key, then
  // wren::played) with the panel's values: {bars, beats_per_bar, bpm,
  // max_velocity, low_key, high_key (the keys drawn, graces included),
  // notes: [{position, length (0..1 across the lane, as held), key, name
  // ("D5"), drawn (the clip's velocity), velocity, tongue ("t", "k" or
  // "slur": how it starts, or its grace starts), staccato, vibrato (the
  // vibrato take's share at full bloom), vibrato_from, vibrato_full (0..1
  // across the lane: where the bloom starts and is complete), swell_db,
  // grace, grace_key, grace_length (0..1: the grace plays from `position`,
  // then the note slurred from it), cents}]}. Without a clip, notes is
  // empty. Keys Wren does not play are left out.
  Q_INVOKABLE QVariantMap wren_phrase() const;
  static constexpr int wren_phrase_bars = 2;
  // Contour sketch (empty for other engines): the filter's measured response
  // in dB at `points` frequencies on the frequency_at scale, at the panel
  // cutoff, or at `cutoff_hz` when it is positive (the live one).
  Q_INVOKABLE QVariantList contour_response(int points, double cutoff_hz) const;
  // Shape sketch (empty for other engines): the EQ's response in dB at
  // `points` frequencies on the frequency_at scale, of the bands that are on
  // together (`band` < 0) or of band `band` (from 0) alone, empty while off.
  Q_INVOKABLE QVariantList shape_response(int points, int band) const;
  // Press sketch (empty for other engines): the static transfer curve, output
  // dB (makeup included) at `points` input levels from `floor_db` to 0 dBFS.
  Q_INVOKABLE QVariantList press_transfer(double floor_db, int points) const;
  // Strata sketches (empty for other engines), at 48 kHz: band `band`'s
  // static curve as press_transfer's; the crossovers as they play; per
  // playing band its magnitude at `points` frequencies on the 20 Hz-20 kHz
  // log scale; and {id: value} that write OTT mode out as bands.
  Q_INVOKABLE QVariantList strata_transfer(int band, double floor_db, int points) const;
  Q_INVOKABLE QVariantList strata_crossovers() const;
  Q_INVOKABLE QVariantList strata_band_shapes(int points) const;
  Q_INVOKABLE QVariantMap strata_ott_values() const;

  // Forge sketches (empty or 0 for other engines): the static transfer curve
  // with auto gain at `points` inputs over -1..1, and the makeup in dB.
  Q_INVOKABLE QVariantList forge_curve(int points) const;
  Q_INVOKABLE double forge_makeup_db() const;

  // The running synth's live state (ADR-0037), decoded for this engine:
  // {live} plus Torque {running, step, key, sweep, cutoff, gate}, Pulse
  // {gain_reduction, pads [0..1 x 12]}, Knock {last_pad (-1 before any), hits (a count
  // that changes with every hit), levels [0..1 x 8], output_peak_db,
  // places [-1..1 x 8]: where each pad's latest hit landed}, Drift {grains [{key, behind,
  // semitones, reverse, phase, frozen}]} or Concert {sustain, sostenuto,
  // soft, energies [88 x log10 energy], dampers [88 x 0 lifted..1 resting],
  // library (concert_library's state), underruns (late disk reads)},
  // the Orbit effect {lfo_left, lfo_right (bipolar), sweep_left,
  // sweep_right (ms, Hz or gain by mode)}, Contour {cutoff_hz, envelope
  // (linear peak), lfo (semitones)} or Forge {input_rms, output_rms,
  // makeup_db, curve [65 over -1..1]}, Shape {input_peak, output_peak
  // ([left, right] dBFS, held 1 s)} or Press and Ceiling {input_peak,
  // output_peak, reduction (dB, positive, held 1 s)} or Pump {phase (0..1,
  // -1 idle), gain_db, cycle_seconds, keyed, kick [256 peaks of the last
  // cycle], applied [256 gains: the shape as heard]} or Fret {library
  // (fret_library's state), underruns, notes (a count that changes with every
  // note), last_key (-1 before any), last_name ("E1"), last_velocity, last_articulation
  // ("pluck", "staccato", "ghost" or "bridge"), last_pushed, output_peak_db}
  // or Wren {library (wren_library's state), notes (a count that changes with
  // every note), last_key (-1 before any), last_name ("D5"), last_velocity,
  // last_tongue ("t", "k" or "slur"), last_staccato, last_grace, vibrato,
  // output_peak_db}.
  // live is false when nothing plays. Any non-empty frame also passes
  // through raw as {frame}, for panels that decode their layout themselves.
  // Engines with an output spectrum (Shape, Strata, Temper) also give
  // {spectrum}: dBFS per log-spaced band, low to high.
  // Panels poll it while visible.
  Q_INVOKABLE QVariantMap telemetry();

  // User presets: the current values and document saved under a name (an
  // existing one is replaced), loaded whole as one undo step, deleted, and
  // written to or read from any file (an import is kept and loaded). False
  // when the name, file or preset is unusable; the status says why.
  Q_INVOKABLE bool save_user_preset(const QString& name);
  Q_INVOKABLE bool apply_user_preset(const QString& name);
  Q_INVOKABLE bool delete_user_preset(const QString& name);
  // Renames user preset `from` to `to`, keeping it loaded and a favourite;
  // false when either name is unusable, `from` is missing or `to` is taken.
  Q_INVOKABLE bool rename_user_preset(const QString& from, const QString& to);
  // Nova FX module presets: one module's slot values ({"p0".."p15", "mix"},
  // uid-free so they load into any slot) saved under a name in
  // <user presets>/fx/<module>/. Names sorted; an empty map when missing;
  // false when the module, name or file is unusable.
  [[nodiscard]] int fx_presets_revision() const noexcept { return fx_presets_revision_; }
  Q_INVOKABLE QStringList fx_presets(const QString& module) const;
  Q_INVOKABLE bool save_fx_preset(const QString& module, const QString& name,
                                  const QVariantMap& values);
  Q_INVOKABLE QVariantMap fx_preset(const QString& module, const QString& name) const;
  Q_INVOKABLE bool delete_fx_preset(const QString& module, const QString& name);
  // A preset's favourite key: "factory/<name>" or "user/<name>".
  Q_INVOKABLE QString preset_key(bool user, const QString& name) const;
  Q_INVOKABLE bool is_favourite(const QString& key) const { return favourites_.contains(key); }
  // Adds or removes a favourite and saves the list; false when it cannot
  // be saved (the list in memory still changes).
  Q_INVOKABLE bool set_favourite(const QString& key, bool favourite);
  Q_INVOKABLE bool export_preset(const QString& path);
  Q_INVOKABLE bool import_preset(const QString& path);
  // A user's audio file (WAV) in document media slot `slot`, as one undo
  // step. False when it is refused; the status says why.
  Q_INVOKABLE bool import_media(const QString& slot, const QString& path);
  // The system clipboard, for copying shapes between panels.
  Q_INVOKABLE void set_clipboard_text(const QString& text);
  Q_INVOKABLE QString clipboard_text() const;
  // Leaves this effect's linked group, keeping its document.
  Q_INVOKABLE void unlink();

  [[nodiscard]] int macro_revision() const noexcept { return macro_revision_; }
  [[nodiscard]] int macro_map_index() const;
  // MAP mode is shared with the macro strip; null leaves it off. Not owned.
  void set_macro_map_state(MacroMapState* state);
  // The track macro driving parameter `id`, or -1.
  Q_INVOKABLE int macro_of(const QString& id) const;
  // Maps `id` to macro `index` (0..7) from its current value to the top of
  // its range (to the bottom when it sits at the top), so a macro at zero
  // leaves it where it is. False when it cannot be mapped; the status says why.
  Q_INVOKABLE bool map_to_macro(const QString& id, int index);
  Q_INVOKABLE bool unmap_macro(const QString& id);
  // Turns MAP mode off (Escape, or the armed macro clicked again).
  Q_INVOKABLE void end_macro_map();

  // C++ panel models: the values the panel shows (table order, the drag's
  // own while dragging) and the document model.
  [[nodiscard]] const std::vector<double>& current_values() const noexcept { return values_; }
  [[nodiscard]] NativeSynthDocumentModel* document_model() const noexcept { return document_; }
  [[nodiscard]] const native_dsp::synth::EngineDescriptor& table() const noexcept {
    return engine_;
  }
  // The project's document now.
  [[nodiscard]] project::NativeSynthDocument current_document() const {
    return adapter_.document();
  }
  // A user preset file's text; nullopt when it is missing or unusable.
  [[nodiscard]] std::optional<QByteArray> user_preset_text(const QString& name) const;

  // Whole-sound edits (the Constellation's morph, ADR-0077). A drag is
  // begin_state_gesture, updates heard live as they come, then
  // end_state_gesture: one undo step. An update with a document also
  // replaces the document (pass one only when it changed); values alone
  // take the live path, so held notes glide. apply_state is one complete
  // edit of values and document.
  void begin_state_gesture();
  void update_state(std::span<const double> values, const project::NativeSynthDocument* document);
  void end_state_gesture();
  [[nodiscard]] bool state_gesture() const noexcept { return state_gesture_; }
  void apply_state(std::span<const double> values, project::NativeSynthDocument document);

  // Root calls this after the project or audible state changes.
  Q_INVOKABLE void refresh();

signals:
  void values_changed();
  void status_changed();
  void gesture_changed();
  void user_presets_changed();
  void favourites_changed();
  void fx_presets_changed();
  void link_changed();
  void macros_changed();
  void macro_map_changed();

private:
  [[nodiscard]] std::optional<std::size_t> index_of(const QString& id) const;
  void write(std::size_t index, double value);
  void submit(NativeSynthGesture gesture);
  void identify_preset();
  [[nodiscard]] QString user_preset_directory() const;
  [[nodiscard]] QString user_preset_file(const QString& name) const;
  [[nodiscard]] QString fx_preset_directory(const QString& module) const;
  bool load_preset_text(const QByteArray& text, const QString& name);
  [[nodiscard]] QString favourites_file() const;
  void load_favourites();
  bool save_favourites() const;

  NativeSynthPanelAdapter& adapter_;
  const native_dsp::synth::EngineDescriptor& engine_;
  const NativeSynthVisuals* visuals_;
  // Id -> table index. Nova binds hundreds of controls to a table of about a
  // thousand parameters, so lookups must not scan it.
  std::unordered_map<std::string_view, std::size_t> index_;
  std::vector<double> values_;
  std::vector<std::vector<double>> preset_values_;
  NativeSynthPanelStatus status_{};
  QString gesture_parameter_;
  NativeSynthDocumentModel* document_ = nullptr; // A child; created before the first refresh.
  CrateController* crate_ = nullptr;             // A child, for Crate only.
  NovaConstellation* constellation_ = nullptr;   // A child, for Nova only.
  NovaArp* arp_ = nullptr;                       // A child, for Nova only.
  bool state_gesture_ = false;
  int preset_index_ = -1;
  // The user preset last loaded, and its values, until something else is.
  QString user_preset_;
  std::vector<double> user_preset_values_;
  QStringList favourites_;
  int fx_presets_revision_ = 0;
  int linked_count_ = 1;
  int revision_ = 0;
  std::vector<std::pair<std::string, std::uint32_t>> macro_bindings_;
  int macro_revision_ = 0;
  QPointer<MacroMapState> macro_map_;
};

} // namespace oscillate::quick
