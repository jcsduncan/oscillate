# Private, header-only DSP primitives shared by the native DSP, synths,
# effects and analysis: one ramp, one biquad, one FFT (src/native_dsp/dsp).
add_library(oscillate_dsp_primitives INTERFACE)
add_library(oscillate::dsp_primitives ALIAS oscillate_dsp_primitives)
target_include_directories(oscillate_dsp_primitives INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}>
  # The shared numeric helpers (oscillate/native_dsp/dsp/math.hpp).
  $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/../../include>
)

add_library(oscillate_native_dsp eq.cpp compressor.cpp limiter.cpp meter.cpp loudness.cpp spectrum.cpp true_peak.cpp
  correlation.cpp)
add_library(oscillate::native_dsp ALIAS oscillate_native_dsp)
target_include_directories(oscillate_native_dsp PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/../../include>
)
target_link_libraries(oscillate_native_dsp PUBLIC oscillate::engine PRIVATE oscillate::dsp_primitives)
oscillate_target_defaults(oscillate_native_dsp)

# The neutral layer every table-driven native engine (synth, effect, note
# effect) is built on: parameter tables, parameter control and lanes,
# telemetry, the musical transport and compiled-document state.
add_library(oscillate_native_control parameters.cpp parameter_control.cpp telemetry.cpp)
add_library(oscillate::native_control ALIAS oscillate_native_control)
target_include_directories(oscillate_native_control PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/../../include>
)
target_link_libraries(oscillate_native_control PUBLIC oscillate::foundation)
oscillate_target_defaults(oscillate_native_control)

# Reusable building blocks for native synth engines. Engine-agnostic: no
# project model, persistence or Qt types.
add_library(oscillate_native_synth synth/oscillator.cpp synth/oversampler.cpp synth/chorus.cpp
                                          synth/vintage_effects.cpp
                                          synth/wavetable.cpp synth/drum_dsp.cpp
                                          synth/fm.cpp synth/bass_dsp.cpp
                                          synth/grain_dsp.cpp
                                          synth/modal_dsp.cpp)
add_library(oscillate::native_synth ALIAS oscillate_native_synth)
target_include_directories(oscillate_native_synth PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/../../include>
)
target_link_libraries(oscillate_native_synth PUBLIC oscillate::foundation
  PRIVATE oscillate::dsp_primitives)
oscillate_target_defaults(oscillate_native_synth)

# Note effects (ADR-0059): MIDI effects that run before a built-in synth.
add_library(oscillate_note_effects note_effects/note_effect.cpp note_effects/arp.cpp
                                          note_effects/chord.cpp note_effects/echo.cpp
                                          note_effects/random.cpp note_effects/scale.cpp
                                          # Comp plays held notes in a rhythm (ADR-0083, ADR-0084).
                                          note_effects/comp.cpp)
add_library(oscillate::note_effects ALIAS oscillate_note_effects)
target_include_directories(oscillate_note_effects PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/../../include>
)
target_link_libraries(oscillate_note_effects PUBLIC oscillate::native_control oscillate::engine
  oscillate::foundation)
oscillate_target_defaults(oscillate_note_effects)

# The generic synth runtime plus the built-in engines (Prism first).
# Concert streams its samples from disk on a thread of its own (ADR-0071).
add_library(oscillate_synth_engines synth/runtime.cpp synth/prism.cpp synth/engines.cpp
                                           synth/ember.cpp
                                           synth/pulse.cpp
                                           synth/halo.cpp synth/torque.cpp
                                           synth/drift_engine.cpp synth/concert.cpp
                                           synth/concert_library.cpp synth/sample_stream.cpp
                                           synth/anvil.cpp
                                           synth/knock.cpp
                                           synth/fret_feel.cpp
                                           synth/fret.cpp
                                           synth/fret_library.cpp
                                           synth/wren_breath.cpp
                                           synth/wren.cpp
                                           synth/wren_library.cpp
                                           synth/wren_played.cpp
                                           synth/bow.cpp
                                           synth/bow_library.cpp
                                           synth/crate/table.cpp synth/crate/document.cpp
                                           synth/crate/kernel.cpp
                                           synth/nova/parameters.cpp synth/nova/presets.cpp
                                           synth/nova/modulation.cpp synth/nova/oscillator.cpp
                                           synth/nova/document.cpp synth/nova/voice.cpp
                                           synth/nova/analogue.cpp
                                           synth/nova/engine.cpp synth/nova/tables.cpp
                                           synth/nova/tables_factory.cpp synth/nova/tables_noise.cpp
                                           synth/nova/filter.cpp synth/nova/filter_ladder.cpp
                                           synth/nova/filter_svf.cpp synth/nova/filter_character.cpp
                                           synth/nova/filter_comb.cpp synth/nova/arp.cpp
                                           synth/nova/fx.cpp synth/nova/fx_wrapped.cpp
                                           synth/nova/fx_tone.cpp synth/nova/fx_dynamics.cpp
                                           synth/nova/fx_motion.cpp synth/nova/fx_convolve.cpp
                                           synth/nova/fx_shift.cpp)
add_library(oscillate::synth_engines ALIAS oscillate_synth_engines)
target_include_directories(oscillate_synth_engines PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/../../include>
)
target_link_libraries(oscillate_synth_engines PUBLIC oscillate::native_control oscillate::native_synth
  oscillate::engine
  PRIVATE oscillate::dsp_primitives oscillate::native_effects oscillate::native_dsp
  # Crate reads its samples through the warp engine's band-limited reader.
  oscillate::warp_dsp Threads::Threads)
oscillate_target_defaults(oscillate_synth_engines)

# The interactive Quick development executable is a Debug build by default.
# Keep symbols and assertions, while compiling the callback's audio kernels at
# production optimization so auditioning (and the DSP oracle tests) do not
# depend on -O0 throughput. See oscillate_optimize_in_debug.
oscillate_optimize_in_debug(oscillate_native_dsp)
oscillate_optimize_in_debug(oscillate_native_control)
oscillate_optimize_in_debug(oscillate_native_synth)
oscillate_optimize_in_debug(oscillate_synth_engines)
oscillate_optimize_in_debug(oscillate_note_effects)

# The native effect platform (ADR-0040): the generic runtime, the registry
# and the built-in effects. Shape, Press and Ceiling run the classic EQ,
# compressor and limiter (ADR-0047), so the platform links their DSP.
add_library(oscillate_native_effects effects/runtime.cpp effects/registry.cpp
                                            effects/contour.cpp effects/forge.cpp
                                            effects/orbit.cpp effects/relay.cpp
                                            effects/expanse.cpp effects/vista.cpp
                                            effects/deck.cpp
                                            effects/pocket.cpp effects/pump.cpp
                                            effects/temper.cpp effects/flyby.cpp
                                            effects/dust.cpp effects/chop.cpp
                                            # Dust's layers, age and duck (ADR-0069).
                                            effects/dust/parts.cpp effects/dust/vinyl.cpp
                                            effects/dust/tape.cpp effects/dust/hum.cpp
                                            effects/dust/radio.cpp effects/dust/rain.cpp
                                            effects/dust/beds.cpp effects/dust/age.cpp
                                            effects/dust/duck.cpp
                                            effects/lens.cpp
                                            effects/loudness.cpp effects/scope.cpp
                                            effects/tune.cpp effects/trim.cpp
                                            effects/signal.cpp effects/shape.cpp
                                            effects/press.cpp effects/ceiling.cpp
                                            effects/strata.cpp
                                            # Edge's oversampler and guard (ADR-0108).
                                            effects/edge.cpp effects/edge_dsp.cpp
                                            # Hush, the gate and expander (ADR-0116).
                                            effects/hush.cpp
                                            # Cast, the convolution reverb (ADR-0115).
                                            effects/cast.cpp effects/cast_convolver.cpp
                                            effects/cast_ir.cpp)
add_library(oscillate::native_effects ALIAS oscillate_native_effects)
target_include_directories(oscillate_native_effects PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/../../include>
)
target_link_libraries(oscillate_native_effects PUBLIC oscillate::native_control oscillate::native_synth
  oscillate::engine oscillate::native_dsp PRIVATE oscillate::dsp_primitives oscillate_pffft)
oscillate_target_defaults(oscillate_native_effects)
oscillate_optimize_in_debug(oscillate_native_effects)

# GCC 15 at -O3 inlines std::sort over a counted prefix of a fixed std::array
# (the chord's tones, Comp's keys) and reports libstdc++'s heap code reading
# past the array, on paths the count rules out (a known -Warray-bounds false
# positive). Only these two translation units are affected.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  set_source_files_properties(note_effects/chord.cpp note_effects/comp.cpp
    PROPERTIES COMPILE_OPTIONS -Wno-array-bounds)
endif()
