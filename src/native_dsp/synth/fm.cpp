#include "oscillate/native_dsp/synth/fm.hpp"

#include <cmath>
#include <cstddef>
#include <numbers>

namespace oscillate::native_dsp::synth {

FmWaveTables::FmWaveTables() {
  for (std::size_t wave = 0; wave < 4; ++wave) {
    float* table = &tables_[wave * (fm_table_size + 1)];
    for (std::size_t i = 0; i <= fm_table_size; ++i) {
      const double phase = static_cast<double>(i % fm_table_size) / fm_table_size;
      const double s = std::sin(2.0 * std::numbers::pi * phase);
      double value = s;
      switch (static_cast<FmWave>(wave)) {
      case FmWave::sine:
        break;
      case FmWave::half_sine:
        value = phase < 0.5 ? s : 0.0;
        break;
      case FmWave::abs_sine:
        value = std::fabs(s);
        break;
      case FmWave::quarter_sine:
        value = (phase < 0.25 || (phase >= 0.5 && phase < 0.75)) ? std::fabs(s) : 0.0;
        break;
      }
      table[i] = static_cast<float>(value);
    }
  }
}

const FmWaveTables& FmWaveTables::instance() {
  static const FmWaveTables tables;
  return tables;
}

} // namespace oscillate::native_dsp::synth
