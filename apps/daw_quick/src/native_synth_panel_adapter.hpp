#pragma once

#include "oscillate/foundation/error.hpp"
#include "oscillate/native_dsp/synth/runtime.hpp"
#include "oscillate/project/model.hpp"
#include "oscillate/timeline/timing.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oscillate::quick {

// How a submitted edit relates to its neighbours, so the project side can
// coalesce a drag into one undo step and only save on commit.
enum class NativeSynthGesture : std::uint8_t {
  single, // One complete edit: a click, reset, preset or keyboard step.
  begin,  // A drag starts; values are unchanged from the last state.
  update, // A drag moved; live preview, not yet an undo step.
  commit, // The drag ended with these values.
  cancel, // The drag was abandoned; restore the values from before begin.
};

struct NativeSynthPanelStatus {
  std::uint64_t saved_revision = 0;   // Model revision of the project's patch values.
  std::uint64_t audible_revision = 0; // Revision the running synth has applied.
  bool persistence_pending = false;   // CURRENT has not committed this model revision.
  bool deferred_changes = false;      // Sounding notes still use next-note values.
  bool editable = true;
  std::string message{}; // Explains a failure or why editing is unavailable.
};

// A document media slot's audio as a panel shows and cuts it (Crate's
// sample, ADR-0063).
struct NativeSynthMedia {
  // The decoded managed audio; null for built-in media (the engine's own),
  // while decoding, or when it cannot decode (`message` says why).
  std::shared_ptr<const native_dsp::synth::SynthMediaAsset> audio;
  // The managed source's sha256, or the built-in name; empty when the slot
  // holds nothing.
  std::string identity;
  bool builtin = false;
  // A decode is in flight; the media listener runs when it lands.
  bool decoding = false;
  // The managed source's reviewed tempo (corrected, else detected), if any.
  std::optional<double> reviewed_bpm;
  std::string message;
};

// A note of a MIDI clip on this synth's track where the song plays it, in
// quarter notes, as an engine meets it when the note starts: for panels
// that preview what the synth will make of the clip (Fret's groove lane).
struct NativeSynthClipNote {
  double beat = 0.0;         // From the song start.
  double bar_beat = 0.0;     // From its bar's start.
  std::int64_t bar = 0;      // Its bar, zero-based.
  double length_beats = 0.0; // Until its note-off.
  int key = 0;
  int velocity = 0;
};

// A window of bars from the start of this track's earliest MIDI clip.
struct NativeSynthClipWindow {
  double start_beat = 0.0;                // The window's first bar line, from the song start.
  double beats_per_bar = 0.0;             // Of that first bar.
  std::vector<NativeSynthClipNote> notes; // By start; loops repeat.
  // The tempo at the window's first bar line, in quarter notes a minute
  // (0 when unknown), and the project's key, as an engine is told them
  // (Wren's phrase lane: its vibrato delay is in time, its graces in key).
  double bpm = 0.0;
  std::optional<project::MusicalKey> key;
};

// What a media import changes besides the slot, decided on the import's
// worker from the decoded audio (a new sample's first chops, tempo and root).
// Each part left empty keeps the current one.
struct NativeSynthMediaPlan {
  // Replace the document's regions in the imported slot.
  std::optional<std::vector<project::NativeRegion>> regions;
  // Dense values in the engine's table order, replacing the patch's.
  std::optional<std::vector<double>> values;
  // The document's labels, replaced whole.
  std::optional<std::vector<project::NativeLabel>> labels;
};
// Runs on the import's worker: no project or UI access, only what it
// captured. `tempo` is the sound's tempo (a reviewed source's, else a
// confident analysis), when known.
using NativeSynthMediaPlanner = std::function<foundation::Result<NativeSynthMediaPlan>(
    const native_dsp::synth::SynthMediaAsset& audio, std::optional<double> tempo)>;
// Runs on the UI thread when the import finished: true when it landed;
// otherwise status() says why.
using NativeSynthMediaDone = std::function<void(bool imported)>;

// The narrow seam between a built-in synth panel and the project, for any
// engine. Root uses SessionNativeSynthAdapter; tests use an in-memory double.
// All calls happen on the UI thread. Values are dense, in the engine's table
// order.
class NativeSynthPanelAdapter {
public:
  virtual ~NativeSynthPanelAdapter() = default;
  [[nodiscard]] virtual std::vector<double> values() const = 0;
  virtual void submit(std::span<const double> values, NativeSynthGesture gesture) = 0;
  [[nodiscard]] virtual NativeSynthPanelStatus status() const = 0;
  // The engine's structured state (ADR-0057): routes, curves, chains,
  // sequences, media slots and labels. Empty for engines without one.
  [[nodiscard]] virtual project::NativeSynthDocument document() const { return {}; }
  // Replaces the document. A drag (begin, update..., commit) is one undo
  // step heard as it moves; cancel restores the document from before begin.
  virtual void submit_document(project::NativeSynthDocument document, NativeSynthGesture gesture) {
    (void)document;
    (void)gesture;
  }
  // Loads factory preset `preset` whole (values and document) as one edit.
  // False when this adapter cannot, and the panel submits the values.
  virtual bool submit_preset(std::size_t preset) {
    (void)preset;
    return false;
  }
  // User presets (ADR-0058), for built-in effects and synths: the current
  // values and document as a preset file's text, and a preset's text loaded whole as
  // one edit (false when it is not a preset for this engine). Adapters
  // without them return nullopt and false.
  [[nodiscard]] virtual std::optional<std::string> preset_text() const { return std::nullopt; }
  virtual bool submit_preset_text(std::string_view text) {
    (void)text;
    return false;
  }
  // Linked built-in effects (ADR-0058) share their document: how many share
  // this one's (1 alone), and leaving the group.
  [[nodiscard]] virtual std::size_t linked_count() const { return 1; }
  virtual void unlink() {}
  // A user's audio file (a WAV at `path`) in media slot `slot` of the
  // document, as one undo step. False when this adapter cannot, or the file
  // is refused; status() then says why.
  virtual bool import_media(std::string_view slot, std::string_view path) {
    (void)slot;
    (void)path;
    return false;
  }
  // The patch's values and document together as one edit (a re-chop:
  // regions and their per-chop parameters, ADR-0063). Gestures as
  // submit_document: a drag is one undo step heard as it moves.
  virtual void submit_state(std::span<const double> values, project::NativeSynthDocument document,
                            NativeSynthGesture gesture) {
    (void)values;
    (void)document;
    (void)gesture;
  }
  // Media slot `slot`'s audio, decoded and cached by identity. A managed
  // source not yet decoded starts decoding on a worker and reports
  // `decoding`; the media listener runs on the UI thread when it lands.
  [[nodiscard]] virtual NativeSynthMedia media(std::string_view slot) {
    (void)slot;
    return {};
  }
  virtual void set_media_listener(std::function<void()> listener) { (void)listener; }
  // A user's audio file at `path` into media slot `slot`, asynchronously:
  // staged and decoded on a worker, where `plan` decides the rest of the
  // edit, then finished on the UI thread as one undo step, and `done` runs.
  // False (and `done` never runs) when it is refused at once; status() says
  // why.
  virtual bool begin_media_import(std::string_view slot, std::string_view path,
                                  NativeSynthMediaPlanner plan, NativeSynthMediaDone done) {
    (void)slot;
    (void)path;
    (void)plan;
    (void)done;
    return false;
  }
  [[nodiscard]] virtual bool media_import_busy() const { return false; }
  // A MIDI clip on this synth's track from zero-based bar `bar`, lasting
  // `length`, with `notes` in clip-local ticks (their ids are replaced), as
  // one undo step (Chop to MIDI). False when it is refused; status() says why.
  virtual bool add_midi_clip(std::int64_t bar, timeline::TickPosition length,
                             std::vector<project::MidiNote> notes) {
    (void)bar;
    (void)length;
    (void)notes;
    return false;
  }
  // This synth track's MIDI clips, by id (as the arrangement's selection
  // names them); empty for adapters without a track.
  [[nodiscard]] virtual std::vector<std::string> midi_clip_ids() const { return {}; }
  // The notes this track's earliest MIDI clip plays in its first `bars`
  // bars (from the bar its start falls in), looped patterns repeated and
  // notes cut at the clip's end; nullopt without a track or a MIDI clip.
  [[nodiscard]] virtual std::optional<NativeSynthClipWindow>
  first_clip_window(std::int64_t bars) const {
    (void)bars;
    return std::nullopt;
  }
  // MIDI clip `clip` on this track: its notes become `notes` (clip-local
  // ticks; their ids are replaced) and its pattern `length`, as one undo step
  // (a generated pattern written to the clip). False when it is refused;
  // status() says why.
  virtual bool replace_midi_clip(std::string_view clip, timeline::TickPosition length,
                                 std::vector<project::MidiNote> notes) {
    (void)clip;
    (void)length;
    (void)notes;
    return false;
  }
  // The running synth's newest telemetry frame (ADR-0037), in its engine's
  // documented layout, or false when nothing is playing or it publishes none.
  [[nodiscard]] virtual bool telemetry(std::vector<float>& frame) {
    (void)frame;
    return false;
  }
  // Values ((id, value) pairs) written to the built-in effect `engine` right
  // after this effect on its chain, or to a new one inserted right after it
  // when the next is not one, as one undo step: Lens writing its match EQ
  // into Shape (ADR-0120). False when it is refused; status() says why.
  virtual bool write_next_effect(std::string_view engine,
                                 std::span<const std::pair<std::string_view, double>> values) {
    (void)engine;
    (void)values;
    return false;
  }
  // Macros (ADR-0059): the index of the track macro driving `parameter_id`,
  // mapping it to macro `index` over a plain range (moving it off another
  // macro first), and unmapping it. Adapters without a track's macros (tests,
  // the master's effects) return nullopt and false.
  [[nodiscard]] virtual std::optional<std::uint32_t> macro_of(std::string_view parameter_id) const {
    (void)parameter_id;
    return std::nullopt;
  }
  virtual bool map_to_macro(std::string_view parameter_id, std::uint32_t index, double minimum,
                            double maximum) {
    (void)parameter_id;
    (void)index;
    (void)minimum;
    (void)maximum;
    return false;
  }
  virtual bool unmap_macro(std::string_view parameter_id) {
    (void)parameter_id;
    return false;
  }
  // Every parameter of this device a macro drives, with that macro's index.
  [[nodiscard]] virtual std::vector<std::pair<std::string, std::uint32_t>> macro_bindings() const {
    return {};
  }
};

} // namespace oscillate::quick
