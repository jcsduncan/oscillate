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

# The zip writer, read back by an independent parser, and the diagnostics bundle.
oscillate_add_test_executable(oscillate_foundation_diagnostics_tests SOURCES diagnostics_bundle_tests.cpp
  LIBS oscillate::foundation
  TEST foundation_diagnostics)

# The display formatter every panel's numbers go through.
oscillate_add_test_executable(oscillate_foundation_value_text_tests SOURCES value_text_tests.cpp
  LIBS oscillate::foundation
  TEST foundation_value_text)
