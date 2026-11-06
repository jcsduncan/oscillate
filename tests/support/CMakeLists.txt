# The shared test support library itself is header-only and defined by
# oscillate_link_test_support (cmake/ProjectOptions.cmake); this directory
# tests it: failure reports, integer and float comparison semantics, the case
# runner, temporary directories, WAV fixtures, signals and waits.
oscillate_add_test_executable(oscillate_test_support_tests SOURCES support_tests.cpp)
oscillate_add_case_tests(test_support oscillate_test_support_tests
  CASES comparisons_pass_and_report_both_values integers_compare_by_value_whatever_their_sign
        near_uses_the_operands_types_and_fails_on_nan results_report_their_error
        temp_dirs_are_real_and_removed wav_fixtures_round_trip signals_and_levels
        waits_return_once_ready)
