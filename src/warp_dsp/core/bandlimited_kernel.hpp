#pragma once

#include "oscillate/warp_dsp/bandlimited.hpp"

// The kernel is public (oscillate/warp_dsp/bandlimited.hpp); the warp modes
// keep reaching it by its detail names.
namespace oscillate::warp_dsp::detail {

using KernelDesign = warp_dsp::KernelDesign;
using BandlimitedKernel = warp_dsp::BandlimitedKernel;
using warp_dsp::reference_kernel;

} // namespace oscillate::warp_dsp::detail
