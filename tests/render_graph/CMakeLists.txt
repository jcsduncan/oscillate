oscillate_add_test_executable(oscillate_render_graph_tests SOURCES offline_graph_tests.cpp
  LIBS oscillate::render_graph oscillate::application
  TEST render_graph_contract)
