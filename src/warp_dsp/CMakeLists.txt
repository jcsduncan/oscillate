oscillate_add_library(warp_dsp
  SOURCES time_map.cpp source_view.cpp rate_view.cpp
          core/bandlimited_kernel.cpp core/bandlimited_read.cpp real_fft.cpp
          core/source_window.cpp analysis/transients.cpp analysis/pitch.cpp
          modes/repitch.cpp modes/beats.cpp modes/complex.cpp modes/grains.cpp
  PUBLIC oscillate::foundation
  PRIVATE oscillate_pffft
  # The realtime FFT (ADR-0126) runs on the audio thread.
  OPTIMIZE)
