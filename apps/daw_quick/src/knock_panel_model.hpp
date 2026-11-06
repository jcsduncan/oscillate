#pragma once

#include "native_synth_device_controller.hpp"
#include "native_synth_panel_adapter.hpp"

#include "oscillate/native_dsp/synth/parameters.hpp"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace oscillate::quick {

class KnockPatternsModel;

// Pad `pad`'s (from 0) parameter `control` in Knock's table:
// "knock.pad<pad + 1>.<control>", such as "knock.pad3.res.pitch".
[[nodiscard]] std::string knock_pad_parameter(std::size_t pad, std::string_view control);

// A pad named by its sound, as the pads and the piano roll show it: the
// resonator's material and the striker, "Wood · Stick". Empty when `values`
// are not Knock's (a different table, or a pad out of range).
[[nodiscard]] std::string knock_pad_label(const native_dsp::synth::EngineDescriptor& engine,
                                          std::span<const double> values, std::size_t pad);

// A key's name with C1 at MIDI 36, as Knock's pads are named: "C♯1".
[[nodiscard]] QString knock_key_name(int key);

// Knock's panel beside the generic device controller (ADR-0068): each pad's
// label from its sound and its designed ringing modes (knock::design_pad),
// which the panel draws and animates from the moment the pad is hit. Both
// follow the controller's values (the drag's own while dragging).
class KnockPanelModel final : public QObject {
  Q_OBJECT
  Q_PROPERTY(int pad_count READ pad_count CONSTANT)
  Q_PROPERTY(int first_key READ first_key CONSTANT)
  // The ringing-modes view's frequency range, Hz (log axis).
  Q_PROPERTY(double view_min_hz READ view_min_hz CONSTANT)
  Q_PROPERTY(double view_max_hz READ view_max_hz CONSTANT)
  // Each pad's key name ("C1", "C♯1" ...) and its label from its sound.
  Q_PROPERTY(QStringList key_names READ key_names CONSTANT)
  Q_PROPERTY(QStringList pad_labels READ pad_labels NOTIFY changed)
  // Increments whenever the values (so the modes or labels) change.
  Q_PROPERTY(int revision READ revision NOTIFY changed)
  // The Patterns area: loops, the generator and Write to clip
  // (KnockPatternsModel), a child.
  Q_PROPERTY(QObject* patterns READ patterns CONSTANT)
  // The stereo strip's range: where a hit lands, -1 (left) .. 1 (right).
  Q_PROPERTY(double place_min READ place_min CONSTANT)
  Q_PROPERTY(double place_max READ place_max CONSTANT)

public:
  static constexpr double view_min_hz_value = 30.0;
  static constexpr double view_max_hz_value = 16'000.0;

  static constexpr double place_min_value = -1.0;
  static constexpr double place_max_value = 1.0;

  // `adapter` (the controller's) lets the patterns write clips; null leaves
  // them a preview.
  explicit KnockPanelModel(NativeSynthDeviceController& controller,
                           std::uint32_t sample_rate = 48'000,
                           NativeSynthPanelAdapter* adapter = nullptr, QObject* parent = nullptr);

  [[nodiscard]] static int pad_count() noexcept;
  [[nodiscard]] static int first_key() noexcept;
  [[nodiscard]] static double view_min_hz() noexcept { return view_min_hz_value; }
  [[nodiscard]] static double view_max_hz() noexcept { return view_max_hz_value; }
  [[nodiscard]] QStringList key_names() const;
  [[nodiscard]] QStringList pad_labels() const { return labels_; }
  [[nodiscard]] int revision() const noexcept { return revision_; }
  [[nodiscard]] QObject* patterns() const;
  [[nodiscard]] KnockPatternsModel* pattern_model() const noexcept { return patterns_; }
  [[nodiscard]] static double place_min() noexcept { return place_min_value; }
  [[nodiscard]] static double place_max() noexcept { return place_max_value; }

  // Pad `pad`'s parameter `control` ("res.pitch" gives "knock.pad1.res.pitch"
  // for pad 0); empty when the pad is out of range or Knock has no such
  // parameter.
  Q_INVOKABLE QString pad_parameter(int pad, const QString& control) const;
  // Pad `pad`'s modes as its next hit plays them, [{hz, level (0..1, the
  // strongest 1), t60 (s, global Decay applied), part ("resonator",
  // "coupled" or "body")}]; empty for a pad out of range.
  Q_INVOKABLE QVariantList modes(int pad) const;
  // Where `hz` sits across the view, 0..1 on its log axis (clamped).
  Q_INVOKABLE double view_fraction(double hz) const;

signals:
  void changed();

private:
  void update();

  NativeSynthDeviceController& controller_;
  std::uint32_t sample_rate_;
  QStringList labels_;
  int revision_ = 0;
  KnockPatternsModel* patterns_ = nullptr; // A child.
};

} // namespace oscillate::quick
