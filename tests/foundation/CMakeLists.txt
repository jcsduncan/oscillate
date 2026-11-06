oscillate_add_test_executable(oscillate_foundation_tests SOURCES foundation_tests.cpp
  LIBS oscillate::foundation oscillate::foundation_digest
  TEST foundation_contract)

oscillate_add_test_executable(oscillate_foundation_logging_tests SOURCES logging_tests.cpp
  LIBS oscillate::foundation
  TEST foundation_logging)
