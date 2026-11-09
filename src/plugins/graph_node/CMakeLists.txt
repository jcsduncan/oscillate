# The VST3 graph runtime and manual-event slot. Project binding, tempo and
# automation preparation live in application (src/application/vst3).
oscillate_add_library(vst3_graph_runtime SOURCES ../vst3_graph_node.cpp
  PUBLIC oscillate::vst3_bundle_loader oscillate::engine)
