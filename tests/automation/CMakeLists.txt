oscillate_add_test_executable(oscillate_automation_tests SOURCES curve_tests.cpp
  LIBS oscillate::automation
  TEST automation_curve_contract)
