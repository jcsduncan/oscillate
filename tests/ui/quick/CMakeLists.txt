# The Qt Quick app's tests. apps/daw_quick includes this directory after
# defining its libraries; OSCILLATE_QUICK_SOURCE_DIR is that app directory.

oscillate_add_test(NAME quick_real_startup_contract
  COMMAND bash ${CMAKE_SOURCE_DIR}/tests/ui/quick/real_startup.sh
    $<TARGET_FILE:oscillate>)
# Eight fresh Qt starts can take over 20 s on a cold or instrumented local
# machine; keep the bound finite without misclassifying cache warm-up as a hang.

find_package(Qt6 6.10 REQUIRED COMPONENTS Test Widgets)
add_executable(oscillate_quick_track_routing_view_tests
  track_routing_view_tests.cpp
  ${OSCILLATE_QUICK_SOURCE_DIR}/src/track_routing_view.cpp)
target_link_libraries(oscillate_quick_track_routing_view_tests PRIVATE
  oscillate::project oscillate::native_effects Qt6::Core)
oscillate_target_defaults(oscillate_quick_track_routing_view_tests)
oscillate_add_test(NAME quick_track_routing_view_contract
  COMMAND oscillate_quick_track_routing_view_tests)
# Browser sample previews (ADR-0059): the folder scan and the decode,
# tempo and stretch, without a device. WAV only, so no codec is needed.
add_executable(oscillate_quick_sample_preview_tests
  sample_preview_tests.cpp
  ${OSCILLATE_QUICK_SOURCE_DIR}/src/sample_library.cpp ${OSCILLATE_QUICK_SOURCE_DIR}/src/sample_library.hpp
  ${OSCILLATE_QUICK_SOURCE_DIR}/src/sample_preview.cpp ${OSCILLATE_QUICK_SOURCE_DIR}/src/sample_preview.hpp)
target_compile_definitions(oscillate_quick_sample_preview_tests PRIVATE OSCILLATE_HAVE_CODEC=0)
target_link_libraries(oscillate_quick_sample_preview_tests PRIVATE
  oscillate::application oscillate::warp_dsp oscillate::export)
oscillate_target_defaults(oscillate_quick_sample_preview_tests)
oscillate_add_test(NAME quick_sample_preview_contract
  COMMAND oscillate_quick_sample_preview_tests)
add_executable(oscillate_quick_sidechain_route_commands_tests
  sidechain_route_commands_tests.cpp
  ${OSCILLATE_QUICK_SOURCE_DIR}/src/route_commands.cpp ${OSCILLATE_QUICK_SOURCE_DIR}/src/route_commands.hpp)
target_include_directories(oscillate_quick_sidechain_route_commands_tests PRIVATE src)
target_link_libraries(oscillate_quick_sidechain_route_commands_tests PRIVATE
  oscillate::application Qt6::Core)
oscillate_target_defaults(oscillate_quick_sidechain_route_commands_tests)
oscillate_add_test(NAME quick_sidechain_route_commands_contract
  COMMAND oscillate_quick_sidechain_route_commands_tests)
add_executable(oscillate_quick_shell_sparkle_tests
  shell_sparkle_tests.cpp
  ${OSCILLATE_QUICK_SOURCE_DIR}/src/command_palette.cpp ${OSCILLATE_QUICK_SOURCE_DIR}/src/command_palette.hpp
  ${OSCILLATE_QUICK_SOURCE_DIR}/src/history_controller.cpp
  ${OSCILLATE_QUICK_SOURCE_DIR}/src/history_controller.hpp
  ${OSCILLATE_QUICK_SOURCE_DIR}/src/master_section.cpp ${OSCILLATE_QUICK_SOURCE_DIR}/src/master_section.hpp
  ${OSCILLATE_QUICK_SOURCE_DIR}/src/true_peak_monitor.cpp)
target_link_libraries(oscillate_quick_shell_sparkle_tests PRIVATE
  oscillate::application oscillate::native_dsp Qt6::Core)
oscillate_target_defaults(oscillate_quick_shell_sparkle_tests)
oscillate_add_test(NAME quick_shell_sparkle_contract
  COMMAND oscillate_quick_shell_sparkle_tests)
add_executable(oscillate_quick_note_tools_controller_tests
  note_tools_controller_tests.cpp
  ${OSCILLATE_QUICK_SOURCE_DIR}/src/note_tools_controller.cpp
  ${OSCILLATE_QUICK_SOURCE_DIR}/src/note_tools_controller.hpp)
target_include_directories(oscillate_quick_note_tools_controller_tests PRIVATE src)
target_link_libraries(oscillate_quick_note_tools_controller_tests PRIVATE
  oscillate::application oscillate::persistence Qt6::Core)
oscillate_target_defaults(oscillate_quick_note_tools_controller_tests)
oscillate_add_test(NAME quick_note_tools_controller_contract
  COMMAND oscillate_quick_note_tools_controller_tests)
add_executable(oscillate_quick_macro_controller_tests
  macro_controller_tests.cpp
  ${OSCILLATE_QUICK_SOURCE_DIR}/src/macro_controller.cpp
  ${OSCILLATE_QUICK_SOURCE_DIR}/src/macro_controller.hpp
  ${OSCILLATE_QUICK_SOURCE_DIR}/src/rack_library_controller.cpp
  ${OSCILLATE_QUICK_SOURCE_DIR}/src/rack_library_controller.hpp)
target_include_directories(oscillate_quick_macro_controller_tests PRIVATE src)
target_link_libraries(oscillate_quick_macro_controller_tests PRIVATE
  oscillate_quick_native_synth oscillate_quick_native_synth_session oscillate::application
  oscillate::persistence Qt6::Core)
oscillate_target_defaults(oscillate_quick_macro_controller_tests)
oscillate_add_test(NAME quick_macro_controller_contract
  COMMAND oscillate_quick_macro_controller_tests)
add_executable(oscillate_quick_freeze_controller_tests
  freeze_controller_tests.cpp
  ${OSCILLATE_QUICK_SOURCE_DIR}/src/freeze_controller.cpp
  ${OSCILLATE_QUICK_SOURCE_DIR}/src/freeze_controller.hpp)
target_include_directories(oscillate_quick_freeze_controller_tests PRIVATE src)
target_link_libraries(oscillate_quick_freeze_controller_tests PRIVATE
  oscillate::application oscillate::persistence Qt6::Core)
oscillate_target_defaults(oscillate_quick_freeze_controller_tests)
oscillate_add_test(NAME quick_freeze_controller_contract
  COMMAND oscillate_quick_freeze_controller_tests TIER slow)
add_executable(oscillate_quick_mix_check_controller_tests
  mix_check_controller_tests.cpp
  ${OSCILLATE_QUICK_SOURCE_DIR}/src/mix_check_controller.cpp
  ${OSCILLATE_QUICK_SOURCE_DIR}/src/mix_check_controller.hpp)
target_include_directories(oscillate_quick_mix_check_controller_tests PRIVATE src)
target_link_libraries(oscillate_quick_mix_check_controller_tests PRIVATE
  oscillate::application oscillate::persistence Qt6::Core Qt6::Widgets)
oscillate_target_defaults(oscillate_quick_mix_check_controller_tests)
oscillate_add_test(NAME quick_mix_check_controller_contract
  COMMAND oscillate_quick_mix_check_controller_tests TIER slow)
add_executable(oscillate_quick_groove_capture_controller_tests
  groove_capture_controller_tests.cpp
  ${OSCILLATE_QUICK_SOURCE_DIR}/src/groove_capture_controller.cpp
  ${OSCILLATE_QUICK_SOURCE_DIR}/src/groove_capture_controller.hpp)
target_include_directories(oscillate_quick_groove_capture_controller_tests PRIVATE src)
target_link_libraries(oscillate_quick_groove_capture_controller_tests PRIVATE
  oscillate::application oscillate::persistence Qt6::Core Qt6::Widgets)
oscillate_target_defaults(oscillate_quick_groove_capture_controller_tests)
oscillate_add_test(NAME quick_groove_capture_controller_contract
  COMMAND oscillate_quick_groove_capture_controller_tests)
add_executable(oscillate_quick_clay_controller_tests
  clay_controller_tests.cpp
  ${OSCILLATE_QUICK_SOURCE_DIR}/src/clay_controller.cpp
  ${OSCILLATE_QUICK_SOURCE_DIR}/src/clay_controller.hpp)
target_include_directories(oscillate_quick_clay_controller_tests PRIVATE src)
target_link_libraries(oscillate_quick_clay_controller_tests PRIVATE
  oscillate::application oscillate::persistence oscillate::export oscillate::clay_source
  Qt6::Core)
oscillate_target_defaults(oscillate_quick_clay_controller_tests)
oscillate_add_test(NAME quick_clay_controller_contract
  COMMAND oscillate_quick_clay_controller_tests)
add_executable(oscillate_quick_harmony_controller_tests
  harmony_controller_tests.cpp
  ${OSCILLATE_QUICK_SOURCE_DIR}/src/harmony_controller.cpp
  ${OSCILLATE_QUICK_SOURCE_DIR}/src/harmony_controller.hpp
  ${OSCILLATE_QUICK_SOURCE_DIR}/src/keyed_list_model.cpp
  ${OSCILLATE_QUICK_SOURCE_DIR}/src/keyed_list_model.hpp)
target_include_directories(oscillate_quick_harmony_controller_tests PRIVATE src)
target_link_libraries(oscillate_quick_harmony_controller_tests PRIVATE
  oscillate::application oscillate::harmony oscillate::native_dsp Qt6::Core Qt6::Test)
oscillate_target_defaults(oscillate_quick_harmony_controller_tests)
oscillate_add_test(NAME quick_harmony_controller_contract
  COMMAND oscillate_quick_harmony_controller_tests)
# MIDI recording (ADR-0086): a take written where it was heard.
add_executable(oscillate_quick_recording_controller_tests
  recording_controller_tests.cpp
  ${OSCILLATE_QUICK_SOURCE_DIR}/src/recording_controller.cpp
  ${OSCILLATE_QUICK_SOURCE_DIR}/src/recording_controller.hpp)
target_include_directories(oscillate_quick_recording_controller_tests PRIVATE src)
target_link_libraries(oscillate_quick_recording_controller_tests PRIVATE
  oscillate::application Qt6::Core)
oscillate_target_defaults(oscillate_quick_recording_controller_tests)
oscillate_add_test(NAME quick_recording_controller_contract
  COMMAND oscillate_quick_recording_controller_tests)
get_filename_component(oscillate_quick_qml_import_dir
  "${Qt6_DIR}/../../qt6/qml" ABSOLUTE)
# Tests drive the app's own bridge library; the definition turns on their
# simulated keyboard and mouse input (QTest) in the test sources only.
add_library(oscillate_quick_test_core INTERFACE)
target_link_libraries(oscillate_quick_test_core INTERFACE oscillate_quick_core)
target_compile_definitions(oscillate_quick_test_core INTERFACE OSCILLATE_QUICK_TEST_INPUT=1)
add_executable(oscillate_quick_sidechain_shell_tests
  sidechain_shell_tests.cpp)
target_include_directories(oscillate_quick_sidechain_shell_tests PRIVATE src)
target_link_libraries(oscillate_quick_sidechain_shell_tests PRIVATE
  oscillate_quick_test_core Qt6::Test)
oscillate_target_defaults(oscillate_quick_sidechain_shell_tests)
oscillate_add_test(NAME quick_sidechain_shell_contract
  COMMAND oscillate_quick_sidechain_shell_tests)
set_tests_properties(quick_sidechain_shell_contract PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QML_IMPORT_PATH=${oscillate_quick_qml_import_dir}")
add_executable(oscillate_quick_bus_nesting_tests bus_nesting_tests.cpp)
target_link_libraries(oscillate_quick_bus_nesting_tests PRIVATE
  oscillate_quick_test_core Qt6::Test)
oscillate_target_defaults(oscillate_quick_bus_nesting_tests)
oscillate_add_test(NAME quick_bus_nesting_contract
  COMMAND oscillate_quick_bus_nesting_tests)
set_tests_properties(quick_bus_nesting_contract PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QML_IMPORT_PATH=${oscillate_quick_qml_import_dir}")
add_executable(oscillate_quick_live_chain_edit_tests live_chain_edit_tests.cpp)
target_include_directories(oscillate_quick_live_chain_edit_tests PRIVATE src)
target_link_libraries(oscillate_quick_live_chain_edit_tests PRIVATE
  oscillate_quick_test_core Qt6::Test)
oscillate_target_defaults(oscillate_quick_live_chain_edit_tests)
oscillate_add_test(NAME quick_live_chain_edit_contract
  COMMAND oscillate_quick_live_chain_edit_tests)
set_tests_properties(quick_live_chain_edit_contract PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")
add_executable(oscillate_quick_live_audio_edit_tests live_audio_edit_tests.cpp)
target_link_libraries(oscillate_quick_live_audio_edit_tests PRIVATE
  oscillate_quick_test_core Qt6::Test)
oscillate_target_defaults(oscillate_quick_live_audio_edit_tests)
oscillate_add_test(NAME quick_live_audio_edit_contract
  COMMAND oscillate_quick_live_audio_edit_tests)
set_tests_properties(quick_live_audio_edit_contract PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")
if(TARGET oscillate_jack_realtime_export)
  add_executable(oscillate_quick_realtime_export_tests
    realtime_export_run_tests.cpp)
  target_link_libraries(oscillate_quick_realtime_export_tests PRIVATE oscillate_quick_test_core)
  if(OSCILLATE_BUILD_VST3_SCANNER)
    target_compile_definitions(oscillate_quick_realtime_export_tests PRIVATE
      VST3_HELPER_PATH="$<TARGET_FILE:oscillate_vst3_scan_helper>"
      VST3_GAIN_BUNDLE="${CMAKE_BINARY_DIR}/tests/plugins/bundle_loader/fixtures/LoadedGain.vst3"
      VST3_INSTRUMENT_BUNDLE="${CMAKE_BINARY_DIR}/tests/plugins/bundle_loader/fixtures/TimedInstrument.vst3")
    add_dependencies(oscillate_quick_realtime_export_tests
      oscillate_quick_vst3_helper_copy oscillate_vst3_loaded_gain_fixture
      oscillate_vst3_instrument_fixture)
  endif()
  oscillate_target_defaults(oscillate_quick_realtime_export_tests)
  oscillate_add_test(NAME quick_realtime_export_contract COMMAND oscillate_quick_realtime_export_tests)
endif()
if(OSCILLATE_BUILD_CODEC_ADAPTER OR OSCILLATE_BUILD_EXPORT_RESAMPLER OR
   OSCILLATE_BUILD_VST3_SCANNER)
  oscillate_add_quick_core(oscillate_quick_minimal_core 0 0 0)
else()
  add_library(oscillate_quick_minimal_core ALIAS oscillate_quick_core)
endif()
add_executable(oscillate_quick_smoke
  smoke_main.cpp
  smoke_audio_backend_scenario.cpp
  smoke_audio_backend_scenario.hpp
  smoke_import_scenario.cpp
  smoke_import_scenario.hpp
  smoke_export_scenario.cpp
  smoke_export_scenario.hpp
  smoke_tempo_map_scenario.cpp
  smoke_tempo_map_scenario.hpp
  smoke_waveform_scenario.cpp
  smoke_waveform_scenario.hpp
  smoke_routing_scenario.cpp
  smoke_routing_scenario.hpp
  smoke_automation_scenario.cpp
  smoke_automation_scenario.hpp)
target_link_libraries(oscillate_quick_smoke PRIVATE
  oscillate_quick_test_core Qt6::Test)
if(OSCILLATE_BUILD_VST3_SCANNER)
  add_dependencies(oscillate_quick_smoke oscillate_quick_vst3_helper_copy
    oscillate_vst3_instrument_fixture oscillate_vst3_loaded_gain_fixture)
endif()
oscillate_target_defaults(oscillate_quick_smoke)
# Mockup-comparison screenshots of the real shell; a developer tool, not a test.
add_executable(oscillate_quick_screenshot screenshot_main.cpp screenshot_demo.cpp)
target_link_libraries(oscillate_quick_screenshot PRIVATE oscillate_quick_core)
oscillate_target_defaults(oscillate_quick_screenshot)
oscillate_add_test(NAME quick_shell_smoke
  COMMAND bash ${CMAKE_SOURCE_DIR}/tests/ui/quick/smoke.sh
    $<TARGET_FILE:oscillate_quick_smoke> ${OSCILLATE_BUILD_CODEC_ADAPTER}
    ${OSCILLATE_BUILD_EXPORT_RESAMPLER} ${OSCILLATE_BUILD_SOURCE_RESAMPLER}
 TIER slow)
oscillate_add_test(NAME quick_audio_backend_contract
  COMMAND bash ${CMAKE_SOURCE_DIR}/tests/ui/quick/smoke_audio_backend.sh
    $<TARGET_FILE:oscillate_quick_smoke>)
oscillate_add_test(NAME quick_tempo_map_shell_smoke
  COMMAND bash ${CMAKE_SOURCE_DIR}/tests/ui/quick/smoke_tempo_map.sh
    $<TARGET_FILE:oscillate_quick_smoke>)
oscillate_add_test(NAME quick_import_shell_smoke
  COMMAND bash ${CMAKE_SOURCE_DIR}/tests/ui/quick/smoke_import.sh
    $<TARGET_FILE:oscillate_quick_smoke> ${OSCILLATE_BUILD_CODEC_ADAPTER} TIER slow)
oscillate_add_test(NAME quick_waveform_stem_shell_smoke
  COMMAND bash ${CMAKE_SOURCE_DIR}/tests/ui/quick/smoke_waveform.sh
    $<TARGET_FILE:oscillate_quick_smoke> ${OSCILLATE_BUILD_EXPORT_RESAMPLER} TIER slow)
add_executable(oscillate_quick_project_library_recovery_tests
  project_library_recovery_tests.cpp)
target_link_libraries(oscillate_quick_project_library_recovery_tests PRIVATE
  oscillate_quick_minimal_core)
oscillate_target_defaults(oscillate_quick_project_library_recovery_tests)
oscillate_add_test(NAME quick_project_library_recovery_contract
  COMMAND oscillate_quick_project_library_recovery_tests)
oscillate_add_test(NAME quick_arrangement_grid_contract
  COMMAND bash ${CMAKE_SOURCE_DIR}/tests/ui/quick/arrangement_grid.sh
    $<TARGET_FILE:oscillate_quick_smoke>)
add_executable(oscillate_quick_timeline_extent_tests
  timeline_extent_tests.cpp)
target_link_libraries(oscillate_quick_timeline_extent_tests PRIVATE
  oscillate_quick_minimal_core Qt6::Test)
oscillate_target_defaults(oscillate_quick_timeline_extent_tests)
oscillate_add_test(NAME quick_timeline_extent_contract COMMAND oscillate_quick_timeline_extent_tests)
set_tests_properties(quick_timeline_extent_contract PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")
add_executable(oscillate_quick_arrangement_selection_tests arrangement_selection_tests.cpp)
target_link_libraries(oscillate_quick_arrangement_selection_tests PRIVATE
  oscillate_quick_test_core Qt6::Test)
oscillate_target_defaults(oscillate_quick_arrangement_selection_tests)
oscillate_add_test(NAME quick_arrangement_selection_contract
  COMMAND oscillate_quick_arrangement_selection_tests)
set_tests_properties(quick_arrangement_selection_contract PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QML_IMPORT_PATH=${oscillate_quick_qml_import_dir}")
add_executable(oscillate_quick_arrangement_clip_trim_tests arrangement_clip_trim_tests.cpp)
target_link_libraries(oscillate_quick_arrangement_clip_trim_tests PRIVATE
  oscillate_quick_test_core Qt6::Test)
oscillate_target_defaults(oscillate_quick_arrangement_clip_trim_tests)
oscillate_add_test(NAME quick_arrangement_clip_trim_contract
  COMMAND oscillate_quick_arrangement_clip_trim_tests)
set_tests_properties(quick_arrangement_clip_trim_contract PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QML_IMPORT_PATH=${oscillate_quick_qml_import_dir}")
add_executable(oscillate_quick_arrangement_warp_tempo_tests arrangement_warp_tempo_tests.cpp)
target_link_libraries(oscillate_quick_arrangement_warp_tempo_tests PRIVATE
  oscillate_quick_test_core Qt6::Test)
oscillate_target_defaults(oscillate_quick_arrangement_warp_tempo_tests)
oscillate_add_test(NAME quick_arrangement_warp_tempo_contract
  COMMAND oscillate_quick_arrangement_warp_tempo_tests)
set_tests_properties(quick_arrangement_warp_tempo_contract PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QML_IMPORT_PATH=${oscillate_quick_qml_import_dir}")
add_executable(oscillate_quick_shell_text_fit_tests shell_text_fit_tests.cpp screenshot_demo.cpp)
target_link_libraries(oscillate_quick_shell_text_fit_tests PRIVATE
  oscillate_quick_test_core Qt6::Test)
oscillate_target_defaults(oscillate_quick_shell_text_fit_tests)
oscillate_add_test(NAME quick_shell_text_fit_contract
  COMMAND oscillate_quick_shell_text_fit_tests)
set_tests_properties(quick_shell_text_fit_contract PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QML_IMPORT_PATH=${oscillate_quick_qml_import_dir}")
add_executable(oscillate_quick_project_summary_view_tests
  project_summary_view_tests.cpp)
target_link_libraries(oscillate_quick_project_summary_view_tests PRIVATE
  oscillate_quick_minimal_core Qt6::Test)
oscillate_target_defaults(oscillate_quick_project_summary_view_tests)
oscillate_add_test(NAME quick_project_summary_view_contract
  COMMAND oscillate_quick_project_summary_view_tests)
set_tests_properties(quick_project_summary_view_contract PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
oscillate_add_test(NAME quick_insert_chain_ui_contract
  COMMAND bash ${CMAKE_SOURCE_DIR}/tests/ui/quick/insert_chain_ui.sh
    $<TARGET_FILE:oscillate_quick_smoke> TIER slow)
if(OSCILLATE_BUILD_VST3_SCANNER)
  add_executable(oscillate_quick_track_creation_tests
    track_creation_tests.cpp)
  target_link_libraries(oscillate_quick_track_creation_tests PRIVATE
    oscillate_quick_test_core Qt6::Test)
  target_compile_definitions(oscillate_quick_track_creation_tests PRIVATE
    VST3_HELPER_PATH="$<TARGET_FILE:oscillate_vst3_scan_helper>"
    VST3_INSTRUMENT_BUNDLE="${CMAKE_BINARY_DIR}/tests/plugins/bundle_loader/fixtures/TimedInstrument.vst3"
    VST3_GAIN_BUNDLE="${CMAKE_BINARY_DIR}/tests/plugins/bundle_loader/fixtures/LoadedGain.vst3")
  add_dependencies(oscillate_quick_track_creation_tests
    oscillate_quick_vst3_helper_copy oscillate_vst3_instrument_fixture
    oscillate_vst3_loaded_gain_fixture)
  oscillate_target_defaults(oscillate_quick_track_creation_tests)
  oscillate_add_test(NAME quick_track_creation_contract COMMAND oscillate_quick_track_creation_tests)
  set_tests_properties(quick_track_creation_contract PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")
  add_executable(oscillate_quick_vst3_editor_tests
    vst3_editor_tests.cpp)
  target_link_libraries(oscillate_quick_vst3_editor_tests PRIVATE
    oscillate_quick_test_core Qt6::Test)
  target_compile_definitions(oscillate_quick_vst3_editor_tests PRIVATE
    VST3_HELPER_PATH="$<TARGET_FILE:oscillate_vst3_scan_helper>"
    VST3_GAIN_BUNDLE="${CMAKE_BINARY_DIR}/tests/plugins/bundle_loader/fixtures/LoadedGain.vst3"
    VST3_CONNECTED_BUNDLE="${CMAKE_BINARY_DIR}/tests/plugins/bundle_loader/fixtures/ConnectedGain.vst3"
    VST3_INSTRUMENT_BUNDLE="${CMAKE_BINARY_DIR}/tests/plugins/bundle_loader/fixtures/TimedInstrument.vst3")
  add_dependencies(oscillate_quick_vst3_editor_tests
    oscillate_quick_vst3_helper_copy oscillate_vst3_loaded_gain_fixture
    oscillate_vst3_connected_fixture
    oscillate_vst3_instrument_fixture)
  oscillate_target_defaults(oscillate_quick_vst3_editor_tests)
  oscillate_add_test(NAME quick_vst3_editor_contract COMMAND oscillate_quick_vst3_editor_tests)
  set_tests_properties(quick_vst3_editor_contract PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")
  # Explicit installed-plugin/editor smoke. Third-party code and XTest run
  # only on an isolated X11 display; neither default build nor CTest runs it.
  find_package(X11 QUIET)
  if(X11_FOUND)
    add_executable(oscillate_quick_surge_native_live_smoke EXCLUDE_FROM_ALL
      surge_native_live_smoke.cpp)
    target_link_libraries(oscillate_quick_surge_native_live_smoke PRIVATE
      oscillate_quick_test_core Qt6::Test X11::X11 ${CMAKE_DL_LIBS})
    target_compile_definitions(oscillate_quick_surge_native_live_smoke PRIVATE
      VST3_HELPER_PATH="$<TARGET_FILE:oscillate_vst3_scan_helper>")
    add_dependencies(oscillate_quick_surge_native_live_smoke
      oscillate_quick_vst3_helper_copy)
    oscillate_target_defaults(oscillate_quick_surge_native_live_smoke)
  endif()
  add_executable(oscillate_quick_plugin_automation_tests
    plugin_automation_tests.cpp)
  target_link_libraries(oscillate_quick_plugin_automation_tests PRIVATE
    oscillate_quick_test_core Qt6::Test)
  target_compile_definitions(oscillate_quick_plugin_automation_tests PRIVATE
    VST3_HELPER_PATH="$<TARGET_FILE:oscillate_vst3_scan_helper>"
    VST3_CONNECTED_BUNDLE="${CMAKE_BINARY_DIR}/tests/plugins/bundle_loader/fixtures/ConnectedGain.vst3"
    VST3_INSTRUMENT_BUNDLE="${CMAKE_BINARY_DIR}/tests/plugins/bundle_loader/fixtures/TimedInstrument.vst3"
    VST3_LANE_EFFECT_BUNDLE="${CMAKE_BINARY_DIR}/tests/plugins/graph_node/fixtures/LaneEffect.vst3"
    VST3_LANE_INSTRUMENT_BUNDLE="${CMAKE_BINARY_DIR}/tests/plugins/bundle_loader/fixtures/LaneInstrument.vst3")
  add_dependencies(oscillate_quick_plugin_automation_tests
    oscillate_quick_vst3_helper_copy oscillate_vst3_connected_fixture
    oscillate_vst3_instrument_fixture oscillate_vst3_lane_effect_fixture
    oscillate_vst3_lane_instrument_fixture)
  oscillate_target_defaults(oscillate_quick_plugin_automation_tests)
  oscillate_add_test(NAME quick_plugin_automation_contract
    COMMAND oscillate_quick_plugin_automation_tests)
  set_tests_properties(quick_plugin_automation_contract PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")
endif()
add_executable(oscillate_quick_waveform_tests
  waveform_worker_tests.cpp
  ${OSCILLATE_QUICK_SOURCE_DIR}/src/waveform_controller.cpp
  ${OSCILLATE_QUICK_SOURCE_DIR}/src/waveform_worker.cpp
  ${OSCILLATE_QUICK_SOURCE_DIR}/src/waveform_view.cpp
  ${OSCILLATE_QUICK_SOURCE_DIR}/src/waveform_detail.cpp
  ${OSCILLATE_QUICK_SOURCE_DIR}/src/clip_waveform_projection.cpp
)
target_link_libraries(oscillate_quick_waveform_tests PRIVATE
  oscillate::application Qt6::Core)
oscillate_target_defaults(oscillate_quick_waveform_tests)
oscillate_add_test(NAME quick_waveform_contract COMMAND oscillate_quick_waveform_tests)
add_executable(oscillate_quick_model_tests
  quick_model_tests.cpp
  ${OSCILLATE_QUICK_SOURCE_DIR}/src/keyed_list_model.cpp
  ${OSCILLATE_QUICK_SOURCE_DIR}/src/keyed_list_model.hpp
  ${OSCILLATE_QUICK_SOURCE_DIR}/src/meter_model.cpp
  ${OSCILLATE_QUICK_SOURCE_DIR}/src/meter_model.hpp
)
set_target_properties(oscillate_quick_model_tests PROPERTIES AUTOMOC ON)
target_link_libraries(oscillate_quick_model_tests PRIVATE oscillate::foundation Qt6::Core Qt6::Test)
oscillate_target_defaults(oscillate_quick_model_tests)
oscillate_add_test(NAME quick_model_contract COMMAND oscillate_quick_model_tests)
# The engine contracts panels draw with and the device panel catalogue.
add_executable(oscillate_quick_ui_contracts_tests ui_contracts_tests.cpp)
target_link_libraries(oscillate_quick_ui_contracts_tests PRIVATE oscillate_quick_core)
oscillate_target_defaults(oscillate_quick_ui_contracts_tests)
oscillate_add_test(NAME quick_ui_contracts_contract COMMAND oscillate_quick_ui_contracts_tests)
add_executable(oscillate_quick_tempo_tests
  tempo_worker_tests.cpp
  ${OSCILLATE_QUICK_SOURCE_DIR}/src/tempo_worker.cpp
)
target_link_libraries(oscillate_quick_tempo_tests PRIVATE
  oscillate::application Qt6::Core)
oscillate_target_defaults(oscillate_quick_tempo_tests)
oscillate_add_test(NAME quick_tempo_worker_contract COMMAND oscillate_quick_tempo_tests)
add_executable(oscillate_quick_key_review_tests
  key_review_ui_tests.cpp)
target_link_libraries(oscillate_quick_key_review_tests PRIVATE
  oscillate_quick_test_core Qt6::Test)
target_compile_definitions(oscillate_quick_key_review_tests PRIVATE
  OSCILLATE_STARTER_PLUCK="${PROJECT_SOURCE_DIR}/assets/starter/pluck.wav")
oscillate_target_defaults(oscillate_quick_key_review_tests)
oscillate_add_test(NAME quick_key_review_contract COMMAND oscillate_quick_key_review_tests)
set_tests_properties(quick_key_review_contract PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QML_IMPORT_PATH=${oscillate_quick_qml_import_dir}")
# The crash report notice offered at startup after a crash.
add_executable(oscillate_quick_crash_report_tests crash_report_notice_tests.cpp)
target_link_libraries(oscillate_quick_crash_report_tests PRIVATE oscillate_quick_test_core)
target_compile_definitions(oscillate_quick_crash_report_tests PRIVATE
  OSCILLATE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
oscillate_target_defaults(oscillate_quick_crash_report_tests)
oscillate_add_test(NAME quick_crash_report_contract COMMAND oscillate_quick_crash_report_tests)
set_tests_properties(quick_crash_report_contract PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QML_IMPORT_PATH=${oscillate_quick_qml_import_dir}")
# Save diagnostics: the controller, its options and the plug-in scan rows.
add_executable(oscillate_quick_diagnostics_tests diagnostics_controller_tests.cpp)
target_link_libraries(oscillate_quick_diagnostics_tests PRIVATE oscillate_quick_test_core)
oscillate_target_defaults(oscillate_quick_diagnostics_tests)
oscillate_add_test(NAME quick_diagnostics_contract COMMAND oscillate_quick_diagnostics_tests)
set_tests_properties(quick_diagnostics_contract PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_executable(oscillate_quick_cache_ui_tests
  cache_ui_controller_tests.cpp)
target_link_libraries(oscillate_quick_cache_ui_tests PRIVATE
  oscillate_quick_test_core)
oscillate_target_defaults(oscillate_quick_cache_ui_tests)
oscillate_add_test(NAME quick_cache_ui_contract COMMAND oscillate_quick_cache_ui_tests)
add_executable(oscillate_quick_routed_playback_tests
  routed_playback_tests.cpp
  ${OSCILLATE_QUICK_SOURCE_DIR}/src/routed_playback.cpp
)
target_link_libraries(oscillate_quick_routed_playback_tests PRIVATE oscillate::application)
target_compile_definitions(oscillate_quick_routed_playback_tests PRIVATE
  OSCILLATE_HAVE_VST3=$<BOOL:${OSCILLATE_BUILD_VST3_SCANNER}>)
if(OSCILLATE_BUILD_VST3_SCANNER)
  target_link_libraries(oscillate_quick_routed_playback_tests PRIVATE
    oscillate::vst3_track_insert oscillate::vst3_instrument_provider
    oscillate::vst3_manual_effect_edit)
  target_compile_definitions(oscillate_quick_routed_playback_tests PRIVATE
    OSCILLATE_VST3_HELPER_PATH="$<TARGET_FILE:oscillate_vst3_scan_helper>"
    VST3_GAIN_BUNDLE="${CMAKE_BINARY_DIR}/tests/plugins/bundle_loader/fixtures/LoadedGain.vst3"
    VST3_CONNECTED_BUNDLE="${CMAKE_BINARY_DIR}/tests/plugins/bundle_loader/fixtures/ConnectedGain.vst3"
    VST3_LANE_EFFECT_BUNDLE="${CMAKE_BINARY_DIR}/tests/plugins/graph_node/fixtures/LaneEffect.vst3"
    VST3_INSTRUMENT_BUNDLE="${CMAKE_BINARY_DIR}/tests/plugins/bundle_loader/fixtures/TimedInstrument.vst3")
  add_dependencies(oscillate_quick_routed_playback_tests oscillate_quick_vst3_helper_copy
    oscillate_vst3_connected_fixture oscillate_vst3_instrument_fixture
    oscillate_vst3_lane_effect_fixture)
endif()
oscillate_target_defaults(oscillate_quick_routed_playback_tests)
oscillate_add_test(NAME quick_routed_native_graph_contract
  COMMAND oscillate_quick_routed_playback_tests --native-graph TIER slow)
oscillate_add_test(NAME quick_routed_native_processors_contract
  COMMAND oscillate_quick_routed_playback_tests --native-processors)
oscillate_add_test(NAME quick_routed_vst3_chains_contract
  COMMAND oscillate_quick_routed_playback_tests --vst3-chains)
oscillate_add_test(NAME quick_routed_instrument_sources_contract
  COMMAND oscillate_quick_routed_playback_tests --instrument-sources)
add_executable(oscillate_quick_warp_editor_tests
  warp_editor_tests.cpp)
target_link_libraries(oscillate_quick_warp_editor_tests PRIVATE
  oscillate_quick_test_core Qt6::Test)
oscillate_target_defaults(oscillate_quick_warp_editor_tests)
oscillate_add_test(NAME quick_warp_editor_contract COMMAND oscillate_quick_warp_editor_tests)
set_tests_properties(quick_warp_editor_contract PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QML_IMPORT_PATH=${oscillate_quick_qml_import_dir}")
add_executable(oscillate_quick_routed_revision_status_tests
  routed_revision_status_tests.cpp)
target_link_libraries(oscillate_quick_routed_revision_status_tests PRIVATE
  oscillate_quick_minimal_core Qt6::Test)
oscillate_target_defaults(oscillate_quick_routed_revision_status_tests)
oscillate_add_test(NAME quick_routed_revision_status_contract
  COMMAND oscillate_quick_routed_revision_status_tests)
set_tests_properties(quick_routed_revision_status_contract PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QML_IMPORT_PATH=${oscillate_quick_qml_import_dir}")
oscillate_add_test(NAME quick_routed_solo_pause_contract
  COMMAND oscillate_quick_routed_revision_status_tests --solo-pause TIER slow)
set_tests_properties(quick_routed_solo_pause_contract PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QML_IMPORT_PATH=${oscillate_quick_qml_import_dir}")
oscillate_add_test(NAME quick_vst3_browser_contract
  COMMAND bash ${CMAKE_SOURCE_DIR}/tests/ui/quick/vst3_browser.sh
    $<TARGET_FILE:oscillate_quick_smoke> ${OSCILLATE_BUILD_VST3_SCANNER}
    ${CMAKE_BINARY_DIR}/tests/plugins/bundle_loader/fixtures/LoadedGain.vst3 TIER slow)
if(OSCILLATE_BUILD_VST3_SCANNER)
  oscillate_add_test(NAME quick_vst3_instrument_contract
    COMMAND bash ${CMAKE_SOURCE_DIR}/tests/ui/quick/vst3_instrument.sh
      $<TARGET_FILE:oscillate_quick_smoke>
      ${CMAKE_BINARY_DIR}/tests/plugins/bundle_loader/fixtures/TimedInstrument.vst3)
  add_dependencies(oscillate oscillate_vst3_instrument_fixture)
endif()
add_executable(oscillate_quick_vst3_export_tests
  vst3_export_tests.cpp
)
target_link_libraries(oscillate_quick_vst3_export_tests PRIVATE
  oscillate_quick_core Qt6::Test)
if(OSCILLATE_BUILD_VST3_SCANNER)
  target_compile_definitions(oscillate_quick_vst3_export_tests PRIVATE
    VST3_HELPER_PATH="$<TARGET_FILE:oscillate_vst3_scan_helper>"
    VST3_INSTRUMENT_BUNDLE="${CMAKE_BINARY_DIR}/tests/plugins/bundle_loader/fixtures/TimedInstrument.vst3"
    VST3_GAIN_BUNDLE="${CMAKE_BINARY_DIR}/tests/plugins/bundle_loader/fixtures/LoadedGain.vst3")
  add_dependencies(oscillate_quick_vst3_export_tests
    oscillate_quick_vst3_helper_copy oscillate_vst3_instrument_fixture
    oscillate_vst3_loaded_gain_fixture)
endif()
oscillate_target_defaults(oscillate_quick_vst3_export_tests)
oscillate_add_test(NAME quick_vst3_export_contract COMMAND oscillate_quick_vst3_export_tests)
set_tests_properties(quick_vst3_export_contract PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QML_IMPORT_PATH=${oscillate_quick_qml_import_dir}")
add_executable(oscillate_quick_export_recovery_tests
  export_recovery_tests.cpp)
target_link_libraries(oscillate_quick_export_recovery_tests PRIVATE
  oscillate_quick_minimal_core Qt6::Test)
oscillate_target_defaults(oscillate_quick_export_recovery_tests)
oscillate_add_test(NAME quick_export_recovery_contract COMMAND oscillate_quick_export_recovery_tests)
set_tests_properties(quick_export_recovery_contract PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")
add_executable(oscillate_quick_named_revision_tests
  named_revision_tests.cpp
)
target_link_libraries(oscillate_quick_named_revision_tests PRIVATE
  oscillate_quick_minimal_core Qt6::Test)
oscillate_target_defaults(oscillate_quick_named_revision_tests)
oscillate_add_test(NAME quick_named_revision_contract COMMAND oscillate_quick_named_revision_tests)
set_tests_properties(quick_named_revision_contract PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")
add_executable(oscillate_quick_managed_media_tests
  managed_media_repair_tests.cpp
)
target_link_libraries(oscillate_quick_managed_media_tests PRIVATE
  oscillate_quick_minimal_core Qt6::Test)
oscillate_target_defaults(oscillate_quick_managed_media_tests)
oscillate_add_test(NAME quick_managed_media_repair_contract COMMAND oscillate_quick_managed_media_tests)
set_tests_properties(quick_managed_media_repair_contract PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")
add_executable(oscillate_quick_metered_output_tests
  metered_output_tests.cpp
  ${OSCILLATE_QUICK_SOURCE_DIR}/src/metered_output.cpp
  ${OSCILLATE_QUICK_SOURCE_DIR}/src/true_peak_monitor.cpp
)
target_link_libraries(oscillate_quick_metered_output_tests PRIVATE
  oscillate::application oscillate::native_dsp)
oscillate_target_defaults(oscillate_quick_metered_output_tests)
oscillate_add_test(NAME quick_metered_output_contract COMMAND oscillate_quick_metered_output_tests)
add_executable(oscillate_quick_native_synth_audition_tests
  native_synth_audition_tests.cpp
  ${OSCILLATE_QUICK_SOURCE_DIR}/src/native_synth_audition.cpp)
target_link_libraries(oscillate_quick_native_synth_audition_tests PRIVATE
  oscillate::application oscillate::synth_engines oscillate::audio_io)
oscillate_target_defaults(oscillate_quick_native_synth_audition_tests)
oscillate_add_test(NAME quick_native_synth_audition_contract
  COMMAND oscillate_quick_native_synth_audition_tests)

if(TARGET oscillate_quick_minimal_core)
get_target_property(oscillate_minimal_alias oscillate_quick_minimal_core ALIASED_TARGET)
if(NOT oscillate_minimal_alias)
  target_link_libraries(oscillate_quick_minimal_core PUBLIC
    oscillate_quick_native_synth oscillate_quick_native_synth_session
    oscillate_quick_relay_panel)
endif()
endif()


add_executable(oscillate_quick_native_synth_app_integration_tests
  native_synth_app_integration_tests.cpp)
target_link_libraries(oscillate_quick_native_synth_app_integration_tests PRIVATE
  oscillate_quick_test_core Qt6::Gui Qt6::Quick Qt6::Test)
oscillate_target_defaults(oscillate_quick_native_synth_app_integration_tests)
oscillate_add_test(NAME quick_native_synth_app_integration_contract
  COMMAND oscillate_quick_native_synth_app_integration_tests TIER slow)
set_tests_properties(quick_native_synth_app_integration_contract PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QML_IMPORT_PATH=${oscillate_quick_qml_import_dir}")
# ADR-0043: every built-in effect from the browser to a live-edited panel.
add_executable(oscillate_quick_native_effect_app_tests
  native_effect_app_tests.cpp)
target_link_libraries(oscillate_quick_native_effect_app_tests PRIVATE
  oscillate_quick_test_core Qt6::Gui Qt6::Quick Qt6::Test)
oscillate_target_defaults(oscillate_quick_native_effect_app_tests)
oscillate_add_test(NAME quick_native_effect_app_contract
  COMMAND oscillate_quick_native_effect_app_tests TIER slow)
set_tests_properties(quick_native_effect_app_contract PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QML_IMPORT_PATH=${oscillate_quick_qml_import_dir};XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-config/quick_native_effect_app_contract")
add_executable(oscillate_quick_browser_tests
  browser_tests.cpp)
target_link_libraries(oscillate_quick_browser_tests PRIVATE
  oscillate_quick_test_core Qt6::Gui Qt6::Quick Qt6::Test)
oscillate_target_defaults(oscillate_quick_browser_tests)
oscillate_add_test(NAME quick_browser_contract COMMAND oscillate_quick_browser_tests)
set_tests_properties(quick_browser_contract PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QML_IMPORT_PATH=${oscillate_quick_qml_import_dir}")
# Keyboard access (issue #112): the shell's custom controls take Tab focus,
# show a focus ring and work from the keyboard.
add_executable(oscillate_quick_keyboard_access_tests
  keyboard_access_tests.cpp)
target_link_libraries(oscillate_quick_keyboard_access_tests PRIVATE
  oscillate_quick_test_core Qt6::Gui Qt6::Quick Qt6::Test)
oscillate_target_defaults(oscillate_quick_keyboard_access_tests)
oscillate_add_test(NAME quick_keyboard_access_contract COMMAND oscillate_quick_keyboard_access_tests)
set_tests_properties(quick_keyboard_access_contract PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QML_IMPORT_PATH=${oscillate_quick_qml_import_dir}")
# The redesigned mixer view: strips, sends, faders, mute/solo, routing.
add_executable(oscillate_quick_mixer_view_tests
  mixer_view_tests.cpp)
target_include_directories(oscillate_quick_mixer_view_tests PRIVATE src)
target_link_libraries(oscillate_quick_mixer_view_tests PRIVATE
  oscillate_quick_test_core Qt6::Gui Qt6::Quick Qt6::Test)
oscillate_target_defaults(oscillate_quick_mixer_view_tests)
oscillate_add_test(NAME quick_mixer_view_contract COMMAND oscillate_quick_mixer_view_tests)
set_tests_properties(quick_mixer_view_contract PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QML_IMPORT_PATH=${oscillate_quick_qml_import_dir}")
# Clay's Notes tab (docs/ui/clay-mockup.html) on the real controller.
add_executable(oscillate_quick_clay_editor_tests clay_editor_tests.cpp)
target_link_libraries(oscillate_quick_clay_editor_tests PRIVATE
  oscillate_quick_test_core Qt6::Gui Qt6::Quick Qt6::Test)
oscillate_target_defaults(oscillate_quick_clay_editor_tests)
oscillate_add_test(NAME quick_clay_editor_contract COMMAND oscillate_quick_clay_editor_tests)
set_tests_properties(quick_clay_editor_contract PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QML_IMPORT_PATH=${oscillate_quick_qml_import_dir}")
# Mix Check (docs/ui/mix-check-mockup.html): the view against a fake
# controller with the four findings measured on Funky.
add_executable(oscillate_quick_mix_check_view_tests
  mix_check_view_tests.cpp)
target_link_libraries(oscillate_quick_mix_check_view_tests PRIVATE
  oscillate_quick_test_core Qt6::Gui Qt6::Quick Qt6::Test)
oscillate_target_defaults(oscillate_quick_mix_check_view_tests)
oscillate_add_test(NAME quick_mix_check_view_contract COMMAND oscillate_quick_mix_check_view_tests)
set_tests_properties(quick_mix_check_view_contract PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QML_IMPORT_PATH=${oscillate_quick_qml_import_dir}")
oscillate_add_test_executable(oscillate_quick_project_lifecycle_view_tests
  SOURCES project_lifecycle_view_tests.cpp
  LIBS oscillate_quick_test_core Qt6::Gui Qt6::Quick Qt6::Test
  TEST quick_project_lifecycle_view_contract)
set_tests_properties(quick_project_lifecycle_view_contract PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QML_IMPORT_PATH=${oscillate_quick_qml_import_dir}")
add_executable(oscillate_quick_chord_clip_view_tests
  chord_clip_view_tests.cpp)
target_link_libraries(oscillate_quick_chord_clip_view_tests PRIVATE
  oscillate_quick_test_core Qt6::Gui Qt6::Quick Qt6::Test)
oscillate_target_defaults(oscillate_quick_chord_clip_view_tests)
oscillate_add_test(NAME quick_chord_clip_view_contract COMMAND oscillate_quick_chord_clip_view_tests)
set_tests_properties(quick_chord_clip_view_contract PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QML_IMPORT_PATH=${oscillate_quick_qml_import_dir}")
add_executable(oscillate_quick_native_synth_session_adapter_tests
  native_synth_session_adapter_tests.cpp)
target_link_libraries(oscillate_quick_native_synth_session_adapter_tests PRIVATE
  oscillate_quick_native_synth_session)
oscillate_target_defaults(oscillate_quick_native_synth_session_adapter_tests)
oscillate_add_test(NAME quick_native_synth_session_adapter_contract
  COMMAND oscillate_quick_native_synth_session_adapter_tests)
add_executable(oscillate_quick_note_effects_controller_tests
  note_effects_controller_tests.cpp
  ${OSCILLATE_QUICK_SOURCE_DIR}/src/note_effects_controller.cpp
  ${OSCILLATE_QUICK_SOURCE_DIR}/src/note_effects_controller.hpp)
target_link_libraries(oscillate_quick_note_effects_controller_tests PRIVATE
  oscillate_quick_native_synth oscillate_quick_native_synth_session Qt6::Core)
oscillate_target_defaults(oscillate_quick_note_effects_controller_tests)
oscillate_add_test(NAME quick_note_effects_controller_contract
  COMMAND oscillate_quick_note_effects_controller_tests)
# Crate's panel model (spec 09 §5.1) over the in-memory adapter.
add_executable(oscillate_quick_crate_controller_tests crate_controller_tests.cpp)
target_link_libraries(oscillate_quick_crate_controller_tests PRIVATE
  oscillate_quick_native_synth Qt6::Core)
oscillate_target_defaults(oscillate_quick_crate_controller_tests)
oscillate_add_test(NAME quick_crate_controller_contract
  COMMAND oscillate_quick_crate_controller_tests)
add_executable(oscillate_quick_native_synth_controller_tests
  native_synth_controller_tests.cpp)
target_link_libraries(oscillate_quick_native_synth_controller_tests PRIVATE
  oscillate_quick_native_synth oscillate_quick_native_synth_session Qt6::Test)
oscillate_target_defaults(oscillate_quick_native_synth_controller_tests)
oscillate_add_test(NAME quick_native_synth_controller_contract
  COMMAND oscillate_quick_native_synth_controller_tests)
# Every device panel's tests and the device kit's run from one program:
# each test file's main() is renamed at compile time and the generated
# dispatcher runs the test named by its first argument.
# Nova's panel has more tests than one file holds: nova_<part> runs
# tests/ui/quick/nova_<part>_tests.cpp against nova.qrc.
set(oscillate_quick_panel_tests prism ember nova nova_editing nova_browser nova_fx nova_constellation
  nova_arp
  pulse halo torque
  drift contour concert fret wren bow stick anvil crate knock relay orbit forge expanse vista pocket pump loudness lens scope tune
  trim signal shape press ceiling edge hush cast temper strata flyby dust chop deck devicekit)
set(oscillate_quick_panel_sources)
set(oscillate_quick_panel_declarations)
set(oscillate_quick_panel_entries)
foreach(panel IN LISTS oscillate_quick_panel_tests)
  if(panel STREQUAL "devicekit")
    set(source ${CMAKE_SOURCE_DIR}/tests/ui/quick/devicekit_tests.cpp)
  elseif(panel MATCHES "^nova_")
    set(source ${CMAKE_SOURCE_DIR}/tests/ui/quick/${panel}_tests.cpp)
  else()
    set(source ${CMAKE_SOURCE_DIR}/tests/ui/quick/${panel}_panel_tests.cpp)
    list(APPEND oscillate_quick_panel_sources ${OSCILLATE_QUICK_SOURCE_DIR}/qml/${panel}/${panel}.qrc)
  endif()
  list(APPEND oscillate_quick_panel_sources ${source})
  set_source_files_properties(${source} PROPERTIES
    COMPILE_DEFINITIONS "main=oscillate_${panel}_panel_main")
  string(APPEND oscillate_quick_panel_declarations
    "int oscillate_${panel}_panel_main(int, char**);\n")
  string(APPEND oscillate_quick_panel_entries
    "    {\"${panel}\", oscillate_${panel}_panel_main},\n")
endforeach()
configure_file(${CMAKE_SOURCE_DIR}/tests/ui/quick/panel_tests_main.cpp.in
  ${CMAKE_CURRENT_BINARY_DIR}/panel_tests_main.cpp @ONLY)
add_executable(oscillate_quick_panel_tests ${oscillate_quick_panel_sources}
  ${OSCILLATE_QUICK_SOURCE_DIR}/qml/devicekit/devicekit.qrc ${CMAKE_CURRENT_BINARY_DIR}/panel_tests_main.cpp
  ${OSCILLATE_QUICK_SOURCE_DIR}/fonts/fonts.qrc ${OSCILLATE_QUICK_SOURCE_DIR}/src/bundled_fonts.cpp)
target_link_libraries(oscillate_quick_panel_tests PRIVATE
  oscillate_quick_relay_panel Qt6::Gui Qt6::Quick Qt6::Test)
# The device kit's contract checks every theme in the source tree.
target_compile_definitions(oscillate_quick_panel_tests PRIVATE
  OSCILLATE_QUICK_QML_DIR="${OSCILLATE_QUICK_SOURCE_DIR}/qml")
oscillate_target_defaults(oscillate_quick_panel_tests)
get_filename_component(oscillate_quick_prism_qml_import_dir
  "${Qt6_DIR}/../../qt6/qml" ABSOLUTE)
set(oscillate_quick_slow_panels nova nova_fx loudness)
foreach(panel IN LISTS oscillate_quick_panel_tests)
  set(test quick_${panel}_panel_contract)
  if(panel STREQUAL "devicekit")
    set(test quick_devicekit_contract)
  endif()
  # Panels whose contract runs past the fast tier's few seconds.
  set(tier fast)
  if(panel IN_LIST oscillate_quick_slow_panels)
    set(tier slow)
  endif()
  oscillate_add_test(NAME ${test} COMMAND oscillate_quick_panel_tests ${panel} TIER ${tier})
  set_tests_properties(${test} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QML_IMPORT_PATH=${oscillate_quick_prism_qml_import_dir}")
endforeach()

# The bridge finds its VST3 scan helper beside the running executable, so
# the test programs get their own copy, as the app does.
if(OSCILLATE_BUILD_VST3_SCANNER)
  add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/oscillate_vst3_scan_helper
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_FILE:oscillate_vst3_scan_helper>
      ${CMAKE_CURRENT_BINARY_DIR}/oscillate_vst3_scan_helper
    DEPENDS oscillate_vst3_scan_helper
    VERBATIM)
  add_custom_target(oscillate_quick_tests_vst3_helper_copy
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/oscillate_vst3_scan_helper)
  get_property(oscillate_quick_test_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
  foreach(oscillate_quick_test_target IN LISTS oscillate_quick_test_targets)
    get_target_property(oscillate_quick_test_type ${oscillate_quick_test_target} TYPE)
    if(oscillate_quick_test_type STREQUAL "EXECUTABLE")
      add_dependencies(${oscillate_quick_test_target} oscillate_quick_tests_vst3_helper_copy)
    endif()
  endforeach()
endif()

# Keep Quick tests out of the user's own settings and out of each other's:
# each test reads and writes Qt settings (workspace layout, recent projects)
# under its own XDG config home in the build tree.
get_property(oscillate_quick_tests DIRECTORY PROPERTY TESTS)
foreach(oscillate_quick_test IN LISTS oscillate_quick_tests)
set_property(TEST ${oscillate_quick_test} APPEND PROPERTY ENVIRONMENT
  "XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-config/${oscillate_quick_test}")
# Software-rendered Quick scenes are CPU-heavy; reserving four job slots
# keeps a parallel ctest run from starving them past their timeouts.
set_property(TEST ${oscillate_quick_test} PROPERTY PROCESSORS 4)
endforeach()

add_executable(oscillate_quick_computer_keyboard_tests computer_keyboard_tests.cpp)
target_link_libraries(oscillate_quick_computer_keyboard_tests PRIVATE oscillate_quick_test_core)
oscillate_target_defaults(oscillate_quick_computer_keyboard_tests)
oscillate_add_test(NAME quick_computer_keyboard_contract
  COMMAND oscillate_quick_computer_keyboard_tests)

# Every Quick test program uses the shared support library (check, wait,
# temp_dir...), as oscillate_add_test_executable gives the other areas.
get_property(oscillate_quick_programs DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(oscillate_quick_program IN LISTS oscillate_quick_programs)
  get_target_property(oscillate_quick_program_type ${oscillate_quick_program} TYPE)
  if(oscillate_quick_program_type STREQUAL "EXECUTABLE")
    oscillate_link_test_support(${oscillate_quick_program})
  endif()
endforeach()
add_executable(oscillate_quick_background_io_tests background_io_tests.cpp)
target_link_libraries(oscillate_quick_background_io_tests PRIVATE
  oscillate_quick_test_core Qt6::Test)
oscillate_target_defaults(oscillate_quick_background_io_tests)
oscillate_add_test(NAME quick_background_io_contract COMMAND oscillate_quick_background_io_tests)
set_tests_properties(quick_background_io_contract PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_executable(oscillate_quick_arrangement_clip_copy_tests arrangement_clip_copy_tests.cpp)
target_link_libraries(oscillate_quick_arrangement_clip_copy_tests PRIVATE
  oscillate_quick_test_core Qt6::Test)
oscillate_target_defaults(oscillate_quick_arrangement_clip_copy_tests)
oscillate_add_test(NAME quick_arrangement_clip_copy_contract
  COMMAND oscillate_quick_arrangement_clip_copy_tests)
set_tests_properties(quick_arrangement_clip_copy_contract PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QML_IMPORT_PATH=${oscillate_quick_qml_import_dir}")
