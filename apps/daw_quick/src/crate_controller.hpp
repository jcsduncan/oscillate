#pragma once

#include "native_synth_panel_adapter.hpp"

#include "oscillate/application/crate_chops.hpp"
#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/native_dsp/synth/runtime.hpp"
#include "oscillate/project/model.hpp"

#include <QObject>
#include <QString>
#include <QVariantList>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oscillate::quick {

// Crate's panel model (ADR-0063, spec 09 §5.1), beside the generic device
// controller that owns it: the sample, its chops and the chop-mode bar.
// Everything it shows derives from the adapter's document, values and
// decoded sample; it keeps only the panel's own state (selection, bank and
// the chop settings). Chopping is the application's pure crate functions;
// this class orchestrates them into edits, each one undo step.
class CrateController final : public QObject {
  Q_OBJECT
  // The `sample` slot holds managed or built-in audio that decoded.
  Q_PROPERTY(bool has_sample READ has_sample NOTIFY changed)
  // The loaded file's name (the crate.sample label), or the built-in's.
  Q_PROPERTY(QString sample_name READ sample_name NOTIFY changed)
  Q_PROPERTY(double sample_seconds READ sample_seconds NOTIFY changed)
  Q_PROPERTY(int sample_rate READ sample_rate NOTIFY changed)
  // The crate.bpm label, else 0.
  Q_PROPERTY(double bpm READ bpm NOTIFY changed)
  // The sample's length in 4/4 bars at that tempo, else 0.
  Q_PROPERTY(double sample_bars READ sample_bars NOTIFY changed)
  // crate.root as a note name ("F2", MIDI 60 = C3), empty without a sample.
  Q_PROPERTY(QString key_name READ key_name NOTIFY changed)
  // Regions in time order: [{index, slot, start, end (0..1 of the sample),
  // start_seconds, length_seconds, tag, name, key, key_name, bank}]; key is
  // the chop's pad key (36 + index) and bank index / 16.
  Q_PROPERTY(QVariantList chops READ chops NOTIFY changed)
  // Panel state: the selected chop (clamped), the pad bank (0 A, 1 B), the
  // chop mode ("transient", "grid", "equal", "manual"; setting it does not
  // re-chop) and its settings.
  Q_PROPERTY(int selected READ selected WRITE set_selected NOTIFY changed)
  Q_PROPERTY(int bank READ bank WRITE set_bank NOTIFY changed)
  Q_PROPERTY(QString chop_mode READ chop_mode WRITE set_chop_mode NOTIFY changed)
  Q_PROPERTY(double sensitivity READ sensitivity WRITE set_sensitivity NOTIFY changed)
  Q_PROPERTY(int grid_division READ grid_division WRITE set_grid_division NOTIFY changed)
  Q_PROPERTY(int equal_count READ equal_count WRITE set_equal_count NOTIFY changed)
  // An import or decode in flight; the last refusal or progress text.
  Q_PROPERTY(bool busy READ busy NOTIFY changed)
  Q_PROPERTY(QString message READ message NOTIFY changed)

public:
  static constexpr double default_sensitivity = application::crate::default_chop_sensitivity;
  static constexpr int default_grid_division = 16;
  static constexpr int default_equal_count = 8;
  // Waveform columns peaks() computes at most.
  static constexpr int max_peak_columns = 8192;
  // move_chop's phases.
  enum class DragPhase : std::uint8_t { begin = 0, update = 1, commit = 2, cancel = 3 };

  // `engine` must outlive the controller (registered engines are static).
  // `after_edit` runs after every edit and when the sample decodes, so the
  // owner refreshes every view (and this one) from the adapter.
  CrateController(const native_dsp::EngineDescriptor& engine, NativeSynthPanelAdapter& adapter,
                  std::function<void()> after_edit, QObject* parent = nullptr);
  ~CrateController() override;
  CrateController(const CrateController&) = delete;
  CrateController& operator=(const CrateController&) = delete;

  [[nodiscard]] bool has_sample() const noexcept { return audio_ != nullptr; }
  [[nodiscard]] QString sample_name() const;
  [[nodiscard]] double sample_seconds() const noexcept;
  [[nodiscard]] int sample_rate() const noexcept;
  [[nodiscard]] double bpm() const noexcept;
  [[nodiscard]] double sample_bars() const noexcept;
  [[nodiscard]] QString key_name() const;
  [[nodiscard]] QVariantList chops() const { return chops_; }
  [[nodiscard]] int selected() const noexcept { return selected_; }
  void set_selected(int index);
  [[nodiscard]] int bank() const noexcept { return bank_; }
  void set_bank(int bank);
  [[nodiscard]] QString chop_mode() const { return chop_mode_; }
  void set_chop_mode(const QString& mode);
  [[nodiscard]] double sensitivity() const noexcept { return sensitivity_; }
  void set_sensitivity(double sensitivity);
  [[nodiscard]] int grid_division() const noexcept { return grid_division_; }
  void set_grid_division(int division);
  [[nodiscard]] int equal_count() const noexcept { return equal_count_; }
  void set_equal_count(int count);
  [[nodiscard]] bool busy() const noexcept { return busy_; }
  [[nodiscard]] QString message() const { return message_; }

  // [max |sample|] per column over the whole sample, the loudest column 1;
  // empty without a sample.
  Q_INVOKABLE QVariantList peaks(int columns) const;
  // "crate.chop.NN.<control>" for chop `index`'s slot; empty out of range.
  Q_INVOKABLE QString chop_parameter(int index, const QString& control) const;
  // Re-chops with chop_mode and its setting (manual keeps the starts). The
  // others set the mode and its setting, then re-chop. One undo step each,
  // keeping slots per spec 09 §3 and the selection on its start.
  Q_INVOKABLE bool chop();
  Q_INVOKABLE bool chop_transients(double sensitivity);
  Q_INVOKABLE bool chop_grid(int division);
  Q_INVOKABLE bool chop_equal(int count);
  // Manual edits at a fraction (0..1) of the sample; index 0 cannot go.
  Q_INVOKABLE bool add_chop(double fraction);
  Q_INVOKABLE bool remove_chop(int index);
  Q_INVOKABLE bool rename_chop(int index, const QString& name);
  // A marker drag: phase 0 begin, 1 update, 2 commit, 3 cancel; one undo
  // step heard as it moves. The chop keeps its slot.
  Q_INVOKABLE bool move_chop(int index, double fraction, int phase);
  // crate.era and the era's bits, rate and drive as one edit.
  Q_INVOKABLE bool set_era(int index);
  // Every chop's reverse as one edit.
  Q_INVOKABLE bool reverse_all(bool on);
  // Imports a file (path or file URL) into `sample` asynchronously (busy
  // meanwhile), chopped by transients, with its tempo and root, as one undo
  // step. False when it is refused at once.
  Q_INVOKABLE bool load_sample(const QString& path_or_url);
  // Asks for an audio file, then load_sample; false when none is chosen or
  // there is no widget application to ask with.
  Q_INVOKABLE bool choose_sample();
  // Spec 09 §4's clip at zero-based bar `start_bar`, one undo step; false
  // without a sample, chops or tempo.
  Q_INVOKABLE bool chop_to_midi(int start_bar);
  // The key that plays chop `index` in the current layout: 36 + index on
  // pads, -1 on keys or out of range.
  Q_INVOKABLE int pad_key(int index) const;

  // The owner calls this after the project changes.
  void refresh();

signals:
  void changed();

private:
  [[nodiscard]] double value_of(std::string_view id) const;
  // Re-chops to `starts` (normalised) as one edit.
  bool rechop(const std::vector<std::uint64_t>& starts);
  [[nodiscard]] std::uint64_t frame_at(double fraction) const;
  [[nodiscard]] project::NativeSynthDocument
  with_regions(std::vector<project::NativeRegion> regions) const;
  void refuse(QString message);
  void rebuild_chops();
  void edited();

  const native_dsp::EngineDescriptor& engine_;
  NativeSynthPanelAdapter& adapter_;
  std::function<void()> after_edit_;

  // Derived from the adapter on refresh().
  project::NativeSynthDocument document_;
  std::vector<double> values_;
  NativeSynthMedia media_;
  std::shared_ptr<const native_dsp::synth::SynthMediaAsset> audio_;
  // The sample's regions in time order.
  std::vector<project::NativeRegion> regions_;
  QVariantList chops_;
  // A built-in sample as stereo audio, kept while the slot names it.
  std::string builtin_identity_;
  std::shared_ptr<const native_dsp::synth::SynthMediaAsset> builtin_audio_;
  // A marker drag's regions from before it began.
  std::optional<std::vector<project::NativeRegion>> drag_regions_;
  bool importing_ = false;

  // Panel state.
  int selected_ = 0;
  int bank_ = 0;
  QString chop_mode_ = QStringLiteral("transient");
  double sensitivity_ = default_sensitivity;
  int grid_division_ = default_grid_division;
  int equal_count_ = default_equal_count;
  bool busy_ = false;
  QString message_;
};

} // namespace oscillate::quick
