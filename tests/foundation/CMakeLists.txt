oscillate_add_test_executable(oscillate_foundation_tests SOURCES foundation_tests.cpp
  LIBS oscillate::foundation oscillate::foundation_digest
  TEST foundation_contract)

oscillate_add_test_executable(oscillate_foundation_logging_tests SOURCES logging_tests.cpp
  LIBS oscillate::foundation
  TEST foundation_logging)

# Kills child processes and reads their crash reports back.
oscillate_add_test_executable(oscillate_foundation_crash_report_tests SOURCES crash_report_tests.cpp
  LIBS oscillate::foundation
  TEST foundation_crash_report)
