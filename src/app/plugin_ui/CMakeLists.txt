add_library(oscillate_plugin_ui
  qt_vst3_run_loop.cpp
  vst3_editor_attachment.cpp
  x11_embed.cpp)
add_library(oscillate::plugin_ui ALIAS oscillate_plugin_ui)
target_include_directories(oscillate_plugin_ui PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}>
  PRIVATE "${PROJECT_SOURCE_DIR}/src/plugins")
target_include_directories(oscillate_plugin_ui SYSTEM PRIVATE
  "${OSCILLATE_VST3_SDK_ROOT}")
target_link_libraries(oscillate_plugin_ui PUBLIC
  oscillate::vst3_effect_host Qt6::Core Qt6::Gui Qt6::Widgets PRIVATE X11::X11)
oscillate_target_defaults(oscillate_plugin_ui)
