oscillate_add_test_executable(oscillate_routing_plan_tests SOURCES project_route_plan_tests.cpp
  LIBS oscillate::routing
  TEST routing_project_plan)

oscillate_add_test_executable(oscillate_send_gain_blocks_tests SOURCES send_gain_blocks_tests.cpp
  LIBS oscillate::routing
  TEST routing_send_gain_blocks)
