oscillate_add_test_executable(oscillate_plugin_ui_tests SOURCES plugin_ui_tests.cpp
  LIBS oscillate::plugin_ui oscillate::vst3_bundle_loader Qt6::Test Qt6::Qml Qt6::Quick
  INCLUDES "${PROJECT_SOURCE_DIR}/src/plugins"
  DEFINITIONS VST3_HELPER_PATH="$<TARGET_FILE:oscillate_vst3_scan_helper>"
    VST3_CONNECTED_BUNDLE="${PROJECT_BINARY_DIR}/tests/plugins/bundle_loader/fixtures/ConnectedGain.vst3"
  DEPENDS oscillate_vst3_scan_helper oscillate_vst3_connected_fixture
  TEST plugin_ui_run_loop_contract)
target_include_directories(oscillate_plugin_ui_tests SYSTEM PRIVATE "${OSCILLATE_VST3_SDK_ROOT}")
set_tests_properties(plugin_ui_run_loop_contract PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
if(DEFINED ENV{DISPLAY} AND NOT "$ENV{DISPLAY}" STREQUAL "")
  oscillate_add_test(NAME plugin_ui_x11_attach_contract COMMAND oscillate_plugin_ui_tests --x11 TIER live)
  if(OSCILLATE_ENABLE_SANITIZERS)
    # The host Qt xcb theme loads GTK/fontconfig, which retain process-global
    # allocations at exit. ASan/UBSan remain active for the editor bridge.
    set_tests_properties(plugin_ui_x11_attach_contract PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=xcb;ASAN_OPTIONS=detect_leaks=0")
  else()
    set_tests_properties(plugin_ui_x11_attach_contract PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=xcb")
  endif()
endif()

# Manual compatibility probe for an explicitly selected, installed VST3.
# It is never part of the default build or CTest: third-party editor code
# executes in this process and requires an isolated X11 display.
oscillate_add_executable(oscillate_vst3_editor_probe EXCLUDE_FROM_ALL SOURCES vst3_editor_probe.cpp
  LIBS oscillate::plugin_ui oscillate::vst3_bundle_loader Qt6::Qml Qt6::Quick X11::X11
    ${CMAKE_DL_LIBS}
  DEFINITIONS VST3_HELPER_PATH="$<TARGET_FILE:oscillate_vst3_scan_helper>"
  DEPENDS oscillate_vst3_scan_helper)
