# Clay: note editing of audio (ADR-0081). Reuses the warp engine's FFT,
# windows and transient detector.
oscillate_add_library(clay_dsp
  SOURCES take.cpp spectrum.cpp synth.cpp melodic.cpp polyphonic.cpp note_sets.cpp
          percussive.cpp analysis.cpp render.cpp macros.cpp
  PUBLIC oscillate::foundation
  PRIVATE oscillate::warp_dsp
  OPTIMIZE)
