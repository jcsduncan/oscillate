# Synthetic stereo effect with 65 continuous parameters for event-capacity tests.
add_library(oscillate_vst3_lane_effect_fixture MODULE lane_effect_plugin.cpp)
set_target_properties(oscillate_vst3_lane_effect_fixture PROPERTIES
  PREFIX ""
  OUTPUT_NAME "LaneEffect"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixtures/LaneEffect.vst3/Contents/x86_64-linux")
target_include_directories(oscillate_vst3_lane_effect_fixture SYSTEM PRIVATE
  "${OSCILLATE_VST3_SDK_ROOT}")
oscillate_target_defaults(oscillate_vst3_lane_effect_fixture)

oscillate_add_test_executable(oscillate_vst3_graph_node_tests SOURCES graph_node_tests.cpp
  LIBS oscillate::vst3_graph_node ${CMAKE_DL_LIBS}
  DEFINITIONS VST3_HELPER_PATH="$<TARGET_FILE:oscillate_vst3_scan_helper>"
    VST3_GAIN_BUNDLE="$<TARGET_FILE_DIR:oscillate_vst3_loaded_gain_fixture>/../.."
    VST3_CONNECTED_BUNDLE="$<TARGET_FILE_DIR:oscillate_vst3_connected_fixture>/../.."
    VST3_SIDECHAIN_BUNDLE="$<TARGET_FILE_DIR:oscillate_vst3_sidechain_fixture>/../.."
    VST3_LANE_EFFECT_BUNDLE="$<TARGET_FILE_DIR:oscillate_vst3_lane_effect_fixture>/../.."
  DEPENDS oscillate_vst3_loaded_gain_fixture oscillate_vst3_connected_fixture
    oscillate_vst3_sidechain_fixture oscillate_vst3_lane_effect_fixture oscillate_vst3_scan_helper
  TEST plugins_vst3_graph_node)
