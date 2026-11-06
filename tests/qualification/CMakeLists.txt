cmake_minimum_required(VERSION 3.28)
project(oscillate_live_qualification LANGUAGES CXX)

# This project is intentionally separate from ordinary CTest. The live Python
# harness needs PipeWire and a named disposable target; the standalone 32-track
# rehearsals default to an engine or fake callback. The managed-WAV rehearsal
# also has an explicit-target live mode, excluded from ordinary CTest.
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../.. oscillate)

oscillate_add_executable(oscillate_desktop_playback_fixture SOURCES desktop_playback_fixture.cpp
  LIBS oscillate::application)

oscillate_add_executable(oscillate_routed_live_fixture SOURCES routed_live_fixture.cpp
  LIBS oscillate::application)

# Headless routed PipeWire player driven by the routed live qualification
# scripts over stdin.
oscillate_add_executable(oscillate_routed_player SOURCES routed_player.cpp
  LIBS oscillate::application oscillate::audio_io oscillate::persistence)

# Explicit local performance rehearsal, never an ordinary CTest gate.
oscillate_add_executable(oscillate_routed_32track_rehearsal SOURCES routed_32track_rehearsal.cpp allocation_probe.cpp
  LIBS oscillate::engine oscillate::native_dsp)

oscillate_add_executable(oscillate_streaming_32track_rehearsal SOURCES streaming_32track_rehearsal.cpp allocation_probe.cpp
  LIBS oscillate::application oscillate::audio_io)

if(OSCILLATE_BUILD_QUICK_UI AND TARGET oscillate_jack_realtime_export)
  # Uses the production Quick real-time export object with a named physical
  # JACK connection. Explicit opt-in; never built by the default target or CTest.
  oscillate_add_executable(oscillate_physical_jack_export EXCLUDE_FROM_ALL SOURCES physical_jack_export.cpp
    LIBS oscillate_quick_core PkgConfig::JACK)
endif()

if(OSCILLATE_BUILD_CODEC_ADAPTER AND OSCILLATE_BUILD_SOURCE_RESAMPLER)
  # Explicit local measurement; generated long media is never part of Git or CTest.
  oscillate_add_executable(oscillate_compressed_seek_rehearsal SOURCES compressed_seek_rehearsal.cpp
    LIBS oscillate::codec oscillate::media)
endif()

if(OSCILLATE_BUILD_VST3_SCANNER)
  # Opt-in installed-plugin graph probe. It is never part of default CTest.
  oscillate_add_executable(oscillate_surge_live_graph EXCLUDE_FROM_ALL SOURCES surge_live_graph.cpp
    LIBS oscillate::application oscillate::vst3_instrument_provider
      oscillate::vst3_manual_effect_edit oscillate::vst3_bundle_loader oscillate::plugins
    DEFINITIONS VST3_QUALIFICATION_HELPER="$<TARGET_FILE:oscillate_vst3_scan_helper>"
    DEPENDS oscillate_vst3_scan_helper)

  set(_qual_vst3_root "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
  add_library(oscillate_qualification_gain_fixture MODULE
    ${CMAKE_CURRENT_LIST_DIR}/../plugins/bundle_loader/gain_plugin.cpp)
  set_target_properties(oscillate_qualification_gain_fixture PROPERTIES
    PREFIX "" OUTPUT_NAME "LoadedGain"
    LIBRARY_OUTPUT_DIRECTORY "${_qual_vst3_root}/LoadedGain.vst3/Contents/x86_64-linux")
  target_include_directories(oscillate_qualification_gain_fixture SYSTEM PRIVATE
    "${OSCILLATE_VST3_SDK_ROOT}")
  oscillate_target_defaults(oscillate_qualification_gain_fixture)
  add_library(oscillate_qualification_instrument_fixture MODULE
    ${CMAKE_CURRENT_LIST_DIR}/../plugins/bundle_loader/instrument_plugin.cpp)
  set_target_properties(oscillate_qualification_instrument_fixture PROPERTIES
    PREFIX "" OUTPUT_NAME "TimedInstrument"
    LIBRARY_OUTPUT_DIRECTORY "${_qual_vst3_root}/TimedInstrument.vst3/Contents/x86_64-linux")
  target_include_directories(oscillate_qualification_instrument_fixture SYSTEM PRIVATE
    "${OSCILLATE_VST3_SDK_ROOT}")
  oscillate_target_defaults(oscillate_qualification_instrument_fixture)
  target_link_libraries(oscillate_streaming_32track_rehearsal PRIVATE
    oscillate::vst3_track_insert oscillate::vst3_instrument_provider)
  target_compile_definitions(oscillate_streaming_32track_rehearsal PRIVATE
    OSCILLATE_QUALIFICATION_VST3=1
    VST3_QUALIFICATION_HELPER="$<TARGET_FILE:oscillate_vst3_scan_helper>"
    VST3_QUALIFICATION_GAIN_BUNDLE="${_qual_vst3_root}/LoadedGain.vst3"
    VST3_QUALIFICATION_INSTRUMENT_BUNDLE="${_qual_vst3_root}/TimedInstrument.vst3")
  add_dependencies(oscillate_streaming_32track_rehearsal
    oscillate_qualification_gain_fixture oscillate_qualification_instrument_fixture
    oscillate_vst3_scan_helper)
endif()
