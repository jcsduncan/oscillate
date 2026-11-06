oscillate_add_library(vst3_effect_host SOURCES ../vst3_effect_host.cpp
  PUBLIC oscillate::plugins
  INCLUDES "${CMAKE_CURRENT_LIST_DIR}/..")
target_include_directories(oscillate_vst3_effect_host SYSTEM PRIVATE "${OSCILLATE_VST3_SDK_ROOT}")
