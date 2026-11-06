# Tests of the application-owned VST3 adapters (src/application/vst3). The
# plugin fixtures are built by tests/plugins; the export oracles are shared
# with tests/export.
set(_plugin_fixtures "${PROJECT_BINARY_DIR}/tests/plugins")
set(_bundle_fixtures "${_plugin_fixtures}/bundle_loader/fixtures")
set(_export_test_support "${PROJECT_SOURCE_DIR}/tests/export")

oscillate_add_test_executable(oscillate_application_mixed_instruments_tests SOURCES mixed_instruments_tests.cpp
  LIBS oscillate::application oscillate::vst3_instrument_provider oscillate::persistence
  DEFINITIONS VST3_HELPER_PATH="$<TARGET_FILE:oscillate_vst3_scan_helper>"
    VST3_INSTRUMENT_BUNDLE="${_bundle_fixtures}/TimedInstrument.vst3"
  DEPENDS oscillate_vst3_instrument_fixture oscillate_vst3_scan_helper
  TEST application_mixed_instruments)
oscillate_add_test_executable(oscillate_application_two_vst3_instruments_tests SOURCES two_vst3_instruments_tests.cpp
  LIBS oscillate::application oscillate::vst3_instrument_provider
    oscillate::vst3_manual_effect_edit oscillate::persistence
  DEFINITIONS VST3_HELPER_PATH="$<TARGET_FILE:oscillate_vst3_scan_helper>"
    VST3_INSTRUMENT_BUNDLE="${_bundle_fixtures}/TimedInstrument.vst3"
  DEPENDS oscillate_vst3_instrument_fixture oscillate_vst3_scan_helper
  TEST application_two_vst3_instruments)
oscillate_add_test_executable(oscillate_application_vst3_manual_effect_tests SOURCES vst3_manual_effect_edit_tests.cpp
  LIBS oscillate::vst3_manual_effect_edit
  TEST application_vst3_manual_effect)
oscillate_add_test_executable(oscillate_application_vst3_manual_adoption_tests SOURCES vst3_manual_adoption_tests.cpp
  LIBS oscillate::vst3_manual_effect_edit oscillate::vst3_track_insert
    oscillate::vst3_instrument_provider oscillate::persistence
  DEFINITIONS VST3_HELPER_PATH="$<TARGET_FILE:oscillate_vst3_scan_helper>"
    VST3_CONNECTED_BUNDLE="${_bundle_fixtures}/ConnectedGain.vst3"
    VST3_INSTRUMENT_BUNDLE="${_bundle_fixtures}/TimedInstrument.vst3"
  DEPENDS oscillate_vst3_connected_fixture oscillate_vst3_instrument_fixture
    oscillate_vst3_scan_helper
  TEST application_vst3_manual_adoption)

oscillate_add_test_executable(oscillate_vst3_routed_insert_tests SOURCES routed_track_insert_tests.cpp
  LIBS oscillate::vst3_track_insert oscillate::persistence
  DEFINITIONS VST3_HELPER_PATH="$<TARGET_FILE:oscillate_vst3_scan_helper>"
    VST3_GAIN_BUNDLE="$<TARGET_FILE_DIR:oscillate_vst3_loaded_gain_fixture>/../.."
    VST3_CONNECTED_BUNDLE="$<TARGET_FILE_DIR:oscillate_vst3_connected_fixture>/../.."
    VST3_SIDECHAIN_BUNDLE="$<TARGET_FILE_DIR:oscillate_vst3_sidechain_fixture>/../.."
  DEPENDS oscillate_vst3_loaded_gain_fixture oscillate_vst3_connected_fixture
    oscillate_vst3_sidechain_fixture oscillate_vst3_scan_helper
  TEST application_vst3_routed_insert)

oscillate_add_test_executable(oscillate_vst3_insert_service_tests SOURCES insert_service_tests.cpp
  LIBS oscillate::vst3_insert_service
  DEFINITIONS VST3_HELPER_PATH="$<TARGET_FILE:oscillate_vst3_scan_helper>"
    VST3_FIXTURE_ROOT="${_plugin_fixtures}/fixtures"
    VST3_INSTRUMENT_BUNDLE="${_bundle_fixtures}/TimedInstrument.vst3"
  DEPENDS oscillate_vst3_scan_helper oscillate_vst3_fixture_Valid oscillate_vst3_instrument_fixture
  TEST application_vst3_insert_service)

oscillate_add_test_executable(oscillate_vst3_instrument_provider_tests SOURCES instrument_provider_tests.cpp
  LIBS oscillate::vst3_instrument_provider oscillate::persistence
  DEFINITIONS VST3_HELPER_PATH="$<TARGET_FILE:oscillate_vst3_scan_helper>"
    VST3_INSTRUMENT_BUNDLE="${_bundle_fixtures}/TimedInstrument.vst3"
    VST3_LANE_INSTRUMENT_BUNDLE="${_bundle_fixtures}/LaneInstrument.vst3"
  DEPENDS oscillate_vst3_instrument_fixture oscillate_vst3_lane_instrument_fixture
    oscillate_vst3_scan_helper
  TEST application_vst3_instrument_provider)

oscillate_add_test_executable(oscillate_vst3_instrument_export_tests SOURCES vst3_instrument_export_tests.cpp
  LIBS oscillate::vst3_instrument_export oscillate::persistence oscillate::native_inserts
  INCLUDES ${_export_test_support}
  DEFINITIONS VST3_HELPER_PATH="$<TARGET_FILE:oscillate_vst3_scan_helper>"
    VST3_INSTRUMENT_BUNDLE="${_bundle_fixtures}/TimedInstrument.vst3"
  DEPENDS oscillate_vst3_instrument_fixture oscillate_vst3_scan_helper
  TEST application_vst3_instrument_export)

oscillate_add_test_executable(oscillate_vst3_mixed_export_tests SOURCES vst3_mixed_export_tests.cpp
  LIBS oscillate::vst3_instrument_export oscillate::persistence oscillate::native_inserts
  INCLUDES ${_export_test_support}
  DEFINITIONS VST3_HELPER_PATH="$<TARGET_FILE:oscillate_vst3_scan_helper>"
    VST3_INSTRUMENT_BUNDLE="${_bundle_fixtures}/TimedInstrument.vst3"
    VST3_GAIN_BUNDLE="${_bundle_fixtures}/LoadedGain.vst3"
    VST3_TAIL_BUNDLE="${CMAKE_CURRENT_BINARY_DIR}/fixtures/TailGain.vst3"
  DEPENDS oscillate_vst3_instrument_fixture oscillate_vst3_loaded_gain_fixture
    oscillate_vst3_export_tail_fixture oscillate_vst3_scan_helper
  TEST application_vst3_mixed_export)

add_library(oscillate_vst3_export_tail_fixture MODULE tail_gain_plugin.cpp)
set_target_properties(oscillate_vst3_export_tail_fixture PROPERTIES
  PREFIX ""
  OUTPUT_NAME "TailGain"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixtures/TailGain.vst3/Contents/x86_64-linux")
target_include_directories(oscillate_vst3_export_tail_fixture SYSTEM PRIVATE
  "${OSCILLATE_VST3_SDK_ROOT}")
oscillate_target_defaults(oscillate_vst3_export_tail_fixture)

oscillate_add_test_executable(oscillate_vst3_effect_export_tests SOURCES vst3_effect_export_tests.cpp
  LIBS oscillate::vst3_effect_export oscillate::persistence oscillate::native_inserts
  INCLUDES ${_export_test_support}
  DEFINITIONS VST3_HELPER_PATH="$<TARGET_FILE:oscillate_vst3_scan_helper>"
    VST3_GAIN_BUNDLE="${_bundle_fixtures}/LoadedGain.vst3"
    VST3_CONNECTED_BUNDLE="${_bundle_fixtures}/ConnectedGain.vst3"
    VST3_SIDECHAIN_BUNDLE="${_bundle_fixtures}/SidechainGain.vst3"
    VST3_SIDECHAIN_TAIL_BUNDLE="${_bundle_fixtures}/SidechainTail.vst3"
    VST3_TAIL_BUNDLE="${CMAKE_CURRENT_BINARY_DIR}/fixtures/TailGain.vst3"
  DEPENDS oscillate_vst3_loaded_gain_fixture oscillate_vst3_connected_fixture
    oscillate_vst3_sidechain_fixture oscillate_vst3_sidechain_tail_fixture
    oscillate_vst3_export_tail_fixture oscillate_vst3_scan_helper
  TEST application_vst3_effect_export)
# Both VST3 export suites decode the shared master output stage's formats/rates.
foreach(format_test oscillate_vst3_effect_export_tests oscillate_vst3_instrument_export_tests)
  target_compile_definitions(${format_test} PRIVATE
    OSCILLATE_HAVE_CODEC=$<BOOL:${OSCILLATE_BUILD_CODEC_ADAPTER}>
    OSCILLATE_HAVE_EXPORT_RESAMPLER=$<BOOL:${OSCILLATE_BUILD_EXPORT_RESAMPLER}>)
  if(OSCILLATE_BUILD_CODEC_ADAPTER)
    target_include_directories(${format_test} SYSTEM PRIVATE ${OSCILLATE_SNDFILE_INCLUDE_DIR})
    target_link_libraries(${format_test} PRIVATE ${OSCILLATE_SNDFILE_LIBRARY})
  endif()
endforeach()
