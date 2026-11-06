#pragma once

#include "oscillate/native_dsp/synth/nova/filter.hpp"
#include "synth/nova/filter_parts.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace oscillate::native_dsp::synth::nova {

// Nova's filter models, by family. Each family derives its coefficients
// and its small-signal response in its own file (filter_ladder.cpp ..
// filter_comb.cpp); its per-sample processing is inline in its header, so
// VoiceFilter::process (filter.cpp) runs it without a call. Every member is
// static.
struct VoiceFilter::Impl {
  using Memory = std::array<double, memory_size>;
  using Complex = filter_detail::Complex;
  using Pair = filter_detail::Pair;
  using ResponsePoint = filter_detail::ResponsePoint;

  // ---- Every model (filter.cpp) ------------------------------------------------------------

  static void derive(FilterModel model, const Params& p, double rate, Coefficients& c) noexcept;
  // The small-signal response at `hz`.
  [[nodiscard]] static Complex response(FilterModel model, const Coefficients& c, double hz,
                                        double rate) noexcept;
  [[nodiscard]] static double run(Slot& slot, std::size_t channel, double x, float* line,
                                  std::size_t mask) noexcept;

  // ---- Ladders: Ladder 24 and 12, MG Low, OTA, Diode (filter_ladder) -------------------------

  static void derive_ladder(FilterModel model, const Params& p, double rate,
                            Coefficients& c) noexcept;
  [[nodiscard]] static Complex response_ladder(FilterModel model, const Coefficients& c,
                                               const ResponsePoint& at) noexcept;
  [[nodiscard]] static inline double ladder(FilterModel model, const Coefficients& c, Memory& m,
                                            std::uint32_t& noise, double x) noexcept;
  static inline void ladder_pair(FilterModel model, Slot& slot, double& left,
                                 double& right) noexcept;
  [[nodiscard]] static inline double diode(const Coefficients& c, Memory& m, std::uint32_t& noise,
                                           double x) noexcept;

  // ---- Clean SVFs: LP, BP and HP 12 and 24, Notch, Peak (filter_svf) -----------------------

  static void derive_svf(FilterModel model, const Params& p, double rate, Coefficients& c) noexcept;
  [[nodiscard]] static Complex response_svf(FilterModel model, const Coefficients& c,
                                            const ResponsePoint& at) noexcept;
  [[nodiscard]] static inline double clean(FilterModel model, const Coefficients& c, Memory& m,
                                           double x) noexcept;

  // ---- Character: Formant, Vowel, Phaser, LP→BP→HP, Dual Peak, Ring (filter_character) -----

  // The pair of vowels a Formant or Vowel model sits between (vowel_at()'s
  // first): the bands move smoothly within a pair and turn at its ends, so
  // a blended ramp stays within one. 0 for the other models.
  [[nodiscard]] static std::size_t vowel_pair(FilterModel model, const Params& p) noexcept;
  // The Vowel model's place on A-E-I-O-U for a cutoff: 100 Hz to 10 kHz.
  [[nodiscard]] static double vowel_position(double cutoff_hz) noexcept;
  static void derive_character(FilterModel model, const Params& p, double rate,
                               Coefficients& c) noexcept;
  [[nodiscard]] static Complex response_character(FilterModel model, const Coefficients& c,
                                                  const ResponsePoint& at) noexcept;
  [[nodiscard]] static inline double character(FilterModel model, const Coefficients& c, Memory& m,
                                               double x) noexcept;
  [[nodiscard]] static inline Pair svf_band_pair(Pair x, double g, double k, double& left1,
                                                 double& right1, double& left2,
                                                 double& right2) noexcept;
  static inline void character_pair(Slot& slot, double& left, double& right) noexcept;

  // ---- Delay lines: Comb +, Comb −, Flanger (filter_comb) -----------------------------------

  static void derive_comb(FilterModel model, const Params& p, double rate,
                          Coefficients& c) noexcept;
  [[nodiscard]] static Complex response_comb(const Coefficients& c,
                                             const ResponsePoint& at) noexcept;
  [[nodiscard]] static inline double comb(const Coefficients& c, Memory& m, double x, float* line,
                                          std::size_t mask, std::size_t write) noexcept;
};

} // namespace oscillate::native_dsp::synth::nova
