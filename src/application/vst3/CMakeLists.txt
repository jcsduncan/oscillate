# Application-owned VST3 adapters: they bind plugins-module hosts to the
# routed playback control, the project session and the offline exporters, so
# plugins and export never depend on application.
oscillate_add_library(vst3_track_insert SOURCES track_insert_provider.cpp
  PUBLIC oscillate::vst3_graph_node oscillate::application)

oscillate_add_library(vst3_manual_effect_edit SOURCES manual_effect_edit.cpp
  PUBLIC oscillate::application oscillate::vst3_graph_node)

oscillate_add_library(vst3_instrument_provider SOURCES instrument_provider.cpp
  PUBLIC oscillate::vst3_project_binding oscillate::vst3_graph_node oscillate::application
         oscillate::engine)

oscillate_add_library(vst3_insert_service SOURCES insert_service.cpp
  PUBLIC oscillate::plugins oscillate::application)

oscillate_add_library(vst3_instrument_export SOURCES instrument_export.cpp
  PUBLIC oscillate::export oscillate::application oscillate::vst3_instrument_provider
         oscillate::vst3_track_insert oscillate::plugins
  PRIVATE oscillate::source_prep oscillate::render_graph)

oscillate_add_library(vst3_effect_export SOURCES effect_export.cpp
  PUBLIC oscillate::export oscillate::application oscillate::vst3_track_insert
         oscillate::plugins)
