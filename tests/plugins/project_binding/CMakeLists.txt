oscillate_add_test_executable(oscillate_vst3_project_binding_tests SOURCES ../bundle_loader/project_binding_tests.cpp
  LIBS oscillate::vst3_project_binding
  DEFINITIONS VST3_HELPER_PATH="$<TARGET_FILE:oscillate_vst3_scan_helper>"
    VST3_GAIN_BUNDLE="$<TARGET_FILE_DIR:oscillate_vst3_loaded_gain_fixture>/../.."
    VST3_CONNECTED_BUNDLE="$<TARGET_FILE_DIR:oscillate_vst3_connected_fixture>/../.."
    VST3_INSTRUMENT_BUNDLE="$<TARGET_FILE_DIR:oscillate_vst3_instrument_fixture>/../.."
  DEPENDS oscillate_vst3_loaded_gain_fixture oscillate_vst3_connected_fixture
    oscillate_vst3_instrument_fixture oscillate_vst3_scan_helper
  TEST plugins_vst3_project_binding)
