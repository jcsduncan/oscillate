oscillate_add_test_executable(oscillate_vst3_effect_host_tests SOURCES ../vst3_effect_host_tests.cpp
  LIBS oscillate::vst3_effect_host
  INCLUDES "${PROJECT_SOURCE_DIR}/src/plugins"
  TEST plugins_vst3_effect_host)
target_include_directories(oscillate_vst3_effect_host_tests SYSTEM PRIVATE "${OSCILLATE_VST3_SDK_ROOT}")
