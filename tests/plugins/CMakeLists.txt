set(_fixture_root "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
foreach(_case IN ITEMS Valid Crash Hang Malformed NullFactory NoExports Large)
  add_library(oscillate_vst3_fixture_${_case} MODULE fixture_plugin.cpp)
  set_target_properties(oscillate_vst3_fixture_${_case} PROPERTIES
    PREFIX ""
    OUTPUT_NAME "${_case}"
    LIBRARY_OUTPUT_DIRECTORY "${_fixture_root}/${_case}.vst3/Contents/x86_64-linux")
  target_compile_definitions(oscillate_vst3_fixture_${_case} PRIVATE "FIXTURE_${_case}=1")
  target_include_directories(oscillate_vst3_fixture_${_case} PRIVATE "${OSCILLATE_VST3_SDK_ROOT}")
  oscillate_target_defaults(oscillate_vst3_fixture_${_case})
endforeach()

oscillate_add_test_executable(oscillate_scan_descendant_helper SOURCES scan_descendant_helper.cpp)

oscillate_add_test_executable(oscillate_plugins_tests SOURCES vst3_scan_tests.cpp
  LIBS oscillate::plugins
  INCLUDES "${CMAKE_CURRENT_LIST_DIR}/../../src/plugins"
  DEFINITIONS VST3_HELPER_PATH="$<TARGET_FILE:oscillate_vst3_scan_helper>"
    VST3_DESCENDANT_HELPER_PATH="$<TARGET_FILE:oscillate_scan_descendant_helper>"
    VST3_FIXTURE_ROOT="${_fixture_root}"
  DEPENDS oscillate_vst3_scan_helper oscillate_scan_descendant_helper oscillate_vst3_fixture_Valid
    oscillate_vst3_fixture_Crash oscillate_vst3_fixture_Hang oscillate_vst3_fixture_Malformed
    oscillate_vst3_fixture_NullFactory oscillate_vst3_fixture_NoExports oscillate_vst3_fixture_Large
  TEST plugins_vst3_scan)

oscillate_add_test_executable(oscillate_vst3_catalogue_tests SOURCES vst3_catalogue_tests.cpp
  LIBS oscillate::plugins
  DEFINITIONS CATALOGUE_FIXTURE_DIR="${CMAKE_CURRENT_BINARY_DIR}/catalogue-fixture"
    VST3_HELPER_PATH="$<TARGET_FILE:oscillate_vst3_scan_helper>"
    VST3_FIXTURE_ROOT="${_fixture_root}"
  DEPENDS oscillate_vst3_scan_helper oscillate_vst3_fixture_Valid
  TEST plugins_vst3_catalogue)
add_subdirectory(effect_host)
add_subdirectory(bundle_loader)
add_subdirectory(project_binding)
add_subdirectory(graph_node)

# Explicit local qualification tool. Real third-party bundles execute in this
# separate process and are never part of the deterministic CTest suite.
oscillate_add_executable(oscillate_vst3_compatibility_probe EXCLUDE_FROM_ALL SOURCES compatibility_probe.cpp
  LIBS oscillate::vst3_project_binding oscillate::persistence nlohmann_json::nlohmann_json
  DEFINITIONS VST3_HELPER_PATH="$<TARGET_FILE:oscillate_vst3_scan_helper>"
    VST3_CONNECTED_BUNDLE="${CMAKE_CURRENT_BINARY_DIR}/bundle_loader/fixtures/ConnectedGain.vst3"
    VST3_INSTRUMENT_BUNDLE="${CMAKE_CURRENT_BINARY_DIR}/bundle_loader/fixtures/TimedInstrument.vst3"
  DEPENDS oscillate_vst3_scan_helper oscillate_vst3_connected_fixture
    oscillate_vst3_instrument_fixture)

# Opt-in latency survey. This runs a real plug-in in-process and is kept out
# of the default build and deterministic CTest suite.
oscillate_add_executable(oscillate_vst3_live_latency_probe EXCLUDE_FROM_ALL SOURCES live_latency_probe.cpp
  LIBS oscillate::vst3_bundle_loader oscillate::plugins nlohmann_json::nlohmann_json
  DEFINITIONS VST3_HELPER_PATH="$<TARGET_FILE:oscillate_vst3_scan_helper>"
    VST3_CONNECTED_BUNDLE="${CMAKE_CURRENT_BINARY_DIR}/bundle_loader/fixtures/ConnectedGain.vst3"
    VST3_INSTRUMENT_BUNDLE="${CMAKE_CURRENT_BINARY_DIR}/bundle_loader/fixtures/TimedInstrument.vst3"
  DEPENDS oscillate_vst3_scan_helper oscillate_vst3_connected_fixture
    oscillate_vst3_instrument_fixture)

# Opt-in binary-specific fixed-tail screen. It executes arbitrary third-party
# effect code in-process, so only its synthetic self-test is deterministic.
oscillate_add_executable(oscillate_vst3_fixed_tail_probe EXCLUDE_FROM_ALL SOURCES fixed_tail_probe.cpp
  LIBS oscillate::vst3_bundle_loader oscillate::plugins nlohmann_json::nlohmann_json
  DEFINITIONS VST3_HELPER_PATH="$<TARGET_FILE:oscillate_vst3_scan_helper>"
    VST3_GAIN_BUNDLE="$<TARGET_FILE_DIR:oscillate_vst3_loaded_gain_fixture>/../.."
  DEPENDS oscillate_vst3_scan_helper oscillate_vst3_loaded_gain_fixture)

# Opt-in MIDI note-off/release screen for an exact instrument binary and state.
# Third-party code runs in this process; self-test alone is deterministic.
oscillate_add_executable(oscillate_vst3_instrument_tail_probe EXCLUDE_FROM_ALL SOURCES instrument_tail_probe.cpp
  LIBS oscillate::vst3_bundle_loader oscillate::plugins nlohmann_json::nlohmann_json
  DEFINITIONS VST3_HELPER_PATH="$<TARGET_FILE:oscillate_vst3_scan_helper>"
    VST3_INSTRUMENT_BUNDLE="${CMAKE_CURRENT_BINARY_DIR}/bundle_loader/fixtures/TimedInstrument.vst3"
  DEPENDS oscillate_vst3_scan_helper oscillate_vst3_instrument_fixture)
if(OSCILLATE_ENABLE_SANITIZERS)
  # The crash fixture needs to remain a real SIGSEGV so the supervisor can
  # distinguish it from a helper that exits with a normal error code.
  set_tests_properties(plugins_vst3_scan PROPERTIES ENVIRONMENT "ASAN_OPTIONS=handle_segv=0")
endif()
