# One native insert path (ADR-0043): the factory every chain builder uses to
# prepare native insert stages.
oscillate_add_library(native_inserts
  SOURCES native_stage.cpp effect_binding.cpp effect_stage.cpp
  PUBLIC oscillate::project oscillate::engine oscillate::native_dsp oscillate::native_effects
         oscillate::synth_engines oscillate::automation_project oscillate::timeline
  PRIVATE oscillate::routing)
