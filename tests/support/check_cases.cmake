# Fails when the cases registered with oscillate_add_case_tests differ from
# the ones the contract binary runs (`<binary> --list`), so a new case can't
# go unregistered and silently never run.
#   cmake -DBINARY=<path> -DCASES=a,b,c -P check_cases.cmake
execute_process(COMMAND "${BINARY}" --list OUTPUT_VARIABLE listed RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "${BINARY} --list failed: ${status}")
endif()
string(STRIP "${listed}" listed)
string(REPLACE "\n" "," listed "${listed}")
if(NOT listed STREQUAL CASES)
  message(FATAL_ERROR "registered cases differ from the binary's\n  registered: ${CASES}\n  binary:     ${listed}")
endif()
