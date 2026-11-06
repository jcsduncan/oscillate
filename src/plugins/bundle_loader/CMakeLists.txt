oscillate_add_library(vst3_bundle_loader SOURCES ../vst3_bundle_loader.cpp
  PUBLIC oscillate::plugins oscillate::vst3_effect_host
  PRIVATE dl
  INCLUDES "${CMAKE_CURRENT_LIST_DIR}/..")
target_include_directories(oscillate_vst3_bundle_loader SYSTEM PRIVATE "${OSCILLATE_VST3_SDK_ROOT}")
