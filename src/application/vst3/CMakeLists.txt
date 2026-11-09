# Application-owned VST3 adapters: they bind plugins-module hosts to the
# routed playback control, the project session and the offline exporters, so
# plugins and export never depend on application.
# Saved project VST3 instances bound to loaded hosts, and graph nodes built
# from them with the project's tempo map and automation (ADR-0104).
oscillate_add_library(vst3_project_binding SOURCES project_binding.cpp
  PUBLIC oscillate::vst3_bundle_loader oscillate::project)

oscillate_add_library(vst3_graph_node SOURCES graph_node.cpp
  PUBLIC oscillate::vst3_project_binding oscillate::vst3_graph_runtime oscillate::automation_project
         oscillate::timeline)

oscillate_add_library(vst3_track_insert SOURCES track_insert_provider.cpp
  PUBLIC oscillate::vst3_graph_node oscillate::application)

oscillate_add_library(vst3_manual_effect_edit SOURCES manual_effect_edit.cpp
  PUBLIC oscillate::application oscillate::vst3_graph_node)

oscillate_add_library(vst3_instrument_provider SOURCES instrument_provider.cpp
  PUBLIC oscillate::vst3_project_binding oscillate::vst3_graph_node oscillate::application
         oscillate::engine)

oscillate_add_library(vst3_insert_service SOURCES insert_service.cpp
  PUBLIC oscillate::plugins oscillate::application)

# Offline export with VST3 stages: verified fresh scans, then the shared
# offline render with this host's stage preparer (ADR-0090).
oscillate_add_library(vst3_offline_export SOURCES offline_export.cpp
  PUBLIC oscillate::export oscillate::plugins
  PRIVATE oscillate::application oscillate::vst3_instrument_provider oscillate::vst3_track_insert
          oscillate::engine oscillate::project)
