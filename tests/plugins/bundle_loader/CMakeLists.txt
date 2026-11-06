set(_bundle_root "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
add_library(oscillate_vst3_loaded_gain_fixture MODULE gain_plugin.cpp)
set_target_properties(oscillate_vst3_loaded_gain_fixture PROPERTIES
  PREFIX ""
  OUTPUT_NAME "LoadedGain"
  LIBRARY_OUTPUT_DIRECTORY "${_bundle_root}/LoadedGain.vst3/Contents/x86_64-linux")
target_include_directories(oscillate_vst3_loaded_gain_fixture SYSTEM PRIVATE
  "${OSCILLATE_VST3_SDK_ROOT}")
oscillate_target_defaults(oscillate_vst3_loaded_gain_fixture)
if(TARGET oscillate_quick_routed_playback_tests)
  add_dependencies(oscillate_quick_routed_playback_tests oscillate_vst3_loaded_gain_fixture)
endif()
if(TARGET oscillate)
  add_dependencies(oscillate oscillate_vst3_loaded_gain_fixture)
endif()

oscillate_add_test_executable(oscillate_vst3_bundle_loader_tests SOURCES loader_tests.cpp
  LIBS oscillate::vst3_bundle_loader
  DEFINITIONS VST3_HELPER_PATH="$<TARGET_FILE:oscillate_vst3_scan_helper>"
    VST3_GAIN_BUNDLE="${_bundle_root}/LoadedGain.vst3"
  DEPENDS oscillate_vst3_loaded_gain_fixture oscillate_vst3_scan_helper
  TEST plugins_vst3_bundle_loader)

add_library(oscillate_vst3_connected_fixture MODULE connected_plugin.cpp)
set_target_properties(oscillate_vst3_connected_fixture PROPERTIES
  PREFIX ""
  OUTPUT_NAME "ConnectedGain"
  LIBRARY_OUTPUT_DIRECTORY "${_bundle_root}/ConnectedGain.vst3/Contents/x86_64-linux")
target_include_directories(oscillate_vst3_connected_fixture SYSTEM PRIVATE
  "${OSCILLATE_VST3_SDK_ROOT}")
oscillate_target_defaults(oscillate_vst3_connected_fixture)

# Same verified VST3 effect with one inactive stereo auxiliary detector bus.
add_library(oscillate_vst3_sidechain_fixture MODULE connected_plugin.cpp)
set_target_properties(oscillate_vst3_sidechain_fixture PROPERTIES
  PREFIX ""
  OUTPUT_NAME "SidechainGain"
  LIBRARY_OUTPUT_DIRECTORY "${_bundle_root}/SidechainGain.vst3/Contents/x86_64-linux")
target_compile_definitions(oscillate_vst3_sidechain_fixture PRIVATE
  OSCILLATE_FIXTURE_SIDECHAIN=1)
target_include_directories(oscillate_vst3_sidechain_fixture SYSTEM PRIVATE
  "${OSCILLATE_VST3_SDK_ROOT}")
oscillate_target_defaults(oscillate_vst3_sidechain_fixture)

# An independent offline oracle: this variant emits its declared seven-frame
# latency and a three-frame post-latency effect tail.
add_library(oscillate_vst3_sidechain_tail_fixture MODULE connected_plugin.cpp)
set_target_properties(oscillate_vst3_sidechain_tail_fixture PROPERTIES
  PREFIX ""
  OUTPUT_NAME "SidechainTail"
  LIBRARY_OUTPUT_DIRECTORY "${_bundle_root}/SidechainTail.vst3/Contents/x86_64-linux")
target_compile_definitions(oscillate_vst3_sidechain_tail_fixture PRIVATE
  OSCILLATE_FIXTURE_SIDECHAIN=1 OSCILLATE_FIXTURE_TRUE_LATENCY=1)
target_include_directories(oscillate_vst3_sidechain_tail_fixture SYSTEM PRIVATE
  "${OSCILLATE_VST3_SDK_ROOT}")
oscillate_target_defaults(oscillate_vst3_sidechain_tail_fixture)

oscillate_add_test_executable(oscillate_vst3_connected_bundle_tests SOURCES connected_bundle_tests.cpp
  LIBS oscillate::vst3_project_binding
  DEFINITIONS VST3_HELPER_PATH="$<TARGET_FILE:oscillate_vst3_scan_helper>"
    VST3_CONNECTED_BUNDLE="${_bundle_root}/ConnectedGain.vst3"
  DEPENDS oscillate_vst3_connected_fixture oscillate_vst3_scan_helper
  TEST plugins_vst3_connected_bundle)

add_library(oscillate_vst3_instrument_fixture MODULE instrument_plugin.cpp)
set_target_properties(oscillate_vst3_instrument_fixture PROPERTIES
  PREFIX ""
  OUTPUT_NAME "TimedInstrument"
  LIBRARY_OUTPUT_DIRECTORY "${_bundle_root}/TimedInstrument.vst3/Contents/x86_64-linux")
target_include_directories(oscillate_vst3_instrument_fixture SYSTEM PRIVATE
  "${OSCILLATE_VST3_SDK_ROOT}")
oscillate_target_defaults(oscillate_vst3_instrument_fixture)

# Same synthetic source with continuous parameters 2..66 for event-capacity tests.
add_library(oscillate_vst3_lane_instrument_fixture MODULE instrument_plugin.cpp)
set_target_properties(oscillate_vst3_lane_instrument_fixture PROPERTIES
  PREFIX ""
  OUTPUT_NAME "LaneInstrument"
  LIBRARY_OUTPUT_DIRECTORY "${_bundle_root}/LaneInstrument.vst3/Contents/x86_64-linux")
target_compile_definitions(oscillate_vst3_lane_instrument_fixture PRIVATE
  OSCILLATE_FIXTURE_PARAMETERS=66)
target_include_directories(oscillate_vst3_lane_instrument_fixture SYSTEM PRIVATE
  "${OSCILLATE_VST3_SDK_ROOT}")
oscillate_target_defaults(oscillate_vst3_lane_instrument_fixture)

oscillate_add_test_executable(oscillate_vst3_instrument_bundle_tests SOURCES instrument_bundle_tests.cpp
  LIBS oscillate::vst3_bundle_loader ${CMAKE_DL_LIBS}
  DEFINITIONS VST3_HELPER_PATH="$<TARGET_FILE:oscillate_vst3_scan_helper>"
    VST3_INSTRUMENT_BUNDLE="${_bundle_root}/TimedInstrument.vst3"
  DEPENDS oscillate_vst3_instrument_fixture oscillate_vst3_scan_helper
  TEST plugins_vst3_instrument_bundle)
