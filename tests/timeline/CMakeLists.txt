oscillate_add_test_executable(oscillate_timeline_tests SOURCES timing_tests.cpp
  LIBS oscillate::timeline
  TEST timeline_contract)
