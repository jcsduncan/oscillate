# The offline graph builder: a snapshot's track chains (ADR-0050), routing,
# master chain and built-in instruments, prepared for export.
oscillate_add_library(render_graph SOURCES offline_graph.cpp
  PUBLIC oscillate::project oscillate::engine oscillate::routing oscillate::automation_project
         oscillate::track_chain
  PRIVATE oscillate::native_inserts oscillate::synth_engines
          oscillate::source_prep oscillate::persistence oscillate::timeline)
