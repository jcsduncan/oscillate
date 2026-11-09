# One native insert path (ADR-0043): the factory every chain builder uses to
# prepare native insert stages, and the built-in instruments those chains
# start with.
oscillate_add_library(native_inserts
  SOURCES native_stage.cpp effect_binding.cpp effect_stage.cpp
          # The built-in instruments' project binding and player (ADR-0104).
          instrument_binding.cpp synth_player.cpp tempo_ticks.cpp
  PUBLIC oscillate::project oscillate::engine oscillate::native_dsp oscillate::native_effects
         oscillate::synth_engines oscillate::note_effects oscillate::automation_project
         oscillate::timeline
  PRIVATE oscillate::routing)
