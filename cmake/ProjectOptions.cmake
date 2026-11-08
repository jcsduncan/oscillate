# Build policy and the helpers every CMakeLists uses: oscillate_add_library,
# oscillate_add_executable, oscillate_add_test_executable, oscillate_add_test
# and oscillate_add_case_tests. Feature options and third-party lookups are in
# Features.cmake.

# The oscillate source root, also when this project is a subproject (the live
# qualification superproject in tests/qualification adds it with
# add_subdirectory, so CMAKE_SOURCE_DIR is not this checkout's root).
get_filename_component(OSCILLATE_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(OSCILLATE_SOURCE_DIR "${OSCILLATE_SOURCE_DIR}" CACHE INTERNAL "Oscillate source root")

option(OSCILLATE_WARNINGS_AS_ERRORS "Treat project warnings as errors" OFF)
option(OSCILLATE_ENABLE_COVERAGE "Instrument project targets for GCC coverage" OFF)
option(OSCILLATE_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(OSCILLATE_ENABLE_THREAD_SANITIZER "Enable ThreadSanitizer" OFF)
option(OSCILLATE_BUILD_TEST_TARGETS_BY_DEFAULT
  "Include test binaries in the default build target" ON)

if((OSCILLATE_ENABLE_COVERAGE AND OSCILLATE_ENABLE_SANITIZERS)
   OR (OSCILLATE_ENABLE_COVERAGE AND OSCILLATE_ENABLE_THREAD_SANITIZER)
   OR (OSCILLATE_ENABLE_SANITIZERS AND OSCILLATE_ENABLE_THREAD_SANITIZER))
  message(FATAL_ERROR "Coverage, ASan/UBSan and TSan use separate build trees")
endif()
if(OSCILLATE_ENABLE_COVERAGE AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  message(FATAL_ERROR "The coverage preset requires GCC and gcov")
endif()
if((OSCILLATE_ENABLE_SANITIZERS OR OSCILLATE_ENABLE_THREAD_SANITIZER)
   AND NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  message(FATAL_ERROR "The sanitizer presets require GCC or Clang")
endif()

# ccache reuses objects already compiled from identical input, and mold
# links several times faster than BFD; each is used when installed. A
# launcher the user chose (cache or environment) is left alone.
find_program(OSCILLATE_CCACHE ccache)
if(OSCILLATE_CCACHE)
  foreach(lang IN ITEMS C CXX)
    if(NOT DEFINED CMAKE_${lang}_COMPILER_LAUNCHER
       AND NOT DEFINED ENV{CMAKE_${lang}_COMPILER_LAUNCHER})
      set(CMAKE_${lang}_COMPILER_LAUNCHER "${OSCILLATE_CCACHE}")
    endif()
  endforeach()
endif()
find_program(OSCILLATE_MOLD mold)
if(OSCILLATE_MOLD AND NOT DEFINED CMAKE_LINKER_TYPE)
  if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
    set(CMAKE_LINKER_TYPE MOLD)
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_link_options(-fuse-ld=mold)
  endif()
endif()

# Linking writes whole executables: a few at once is enough to saturate
# the disk and stall the desktop, so links share a small pool while
# compiling keeps every job.
set(OSCILLATE_LINK_JOBS 2 CACHE STRING "Concurrent link jobs (Ninja)")
set_property(GLOBAL APPEND PROPERTY JOB_POOLS oscillate_link=${OSCILLATE_LINK_JOBS})
set(CMAKE_JOB_POOL_LINK oscillate_link)

# Development builds link the project's libraries as shared objects, so a
# change relinks one library rather than every executable that embeds it;
# Debug keeps debug info out of the link (split DWARF). Release links the
# libraries statically into one self-contained application.
option(OSCILLATE_SHARED_LIBS "Build project libraries as shared objects" ON)
set(BUILD_SHARED_LIBS ${OSCILLATE_SHARED_LIBS})
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# Executables load their shared libraries at start, so a rebuilt library
# does not relink the programs that use it.
set(CMAKE_LINK_DEPENDS_NO_SHARED ON)
if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-gsplit-dwarf)
endif()

# Install directories (the root CMakeLists installs the application).
include(GNUInstallDirs)

# Debug builds keep symbols and assertions everywhere, but audio kernels and
# the DSP oracle tests that render seconds of audio are compiled optimised:
# at -O0 they run 10-50x slower, which makes auditioning and the test loop
# depend on -O0 throughput. Coverage builds stay at -O0 so line counts hold.
option(OSCILLATE_OPTIMIZE_DSP_IN_DEBUG
  "Optimise audio kernels and DSP oracle tests in Debug builds" ON)
if(OSCILLATE_OPTIMIZE_DSP_IN_DEBUG AND NOT OSCILLATE_ENABLE_COVERAGE
   AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  set(OSCILLATE_DSP_OPTIMISED_IN_DEBUG ON)
else()
  set(OSCILLATE_DSP_OPTIMISED_IN_DEBUG OFF)
endif()
function(oscillate_optimize_in_debug target)
  # Marks the target even where it stays at -O0, so the budgets of tests that
  # run it can account for that (oscillate_verify_test_labels).
  set_target_properties(${target} PROPERTIES OSCILLATE_DSP_OPTIMISED ON)
  if(OSCILLATE_DSP_OPTIMISED_IN_DEBUG)
    target_compile_options(${target} PRIVATE $<$<CONFIG:Debug>:-O3>)
  endif()
endfunction()

# Shared test support (tests/support/include/oscillate/test): check macros,
# the case runner, temporary directories, WAV fixtures, test signals, fake
# outputs and named sample rates and tolerances. Header-only; every test
# program links it (oscillate_add_test_executable, or any executable built
# from tests/ sources), and any other target (a bench, a tool) can link it
# with oscillate_link_test_support.
function(oscillate_link_test_support target)
  if(NOT TARGET oscillate_test_support)
    add_library(oscillate_test_support INTERFACE)
    add_library(oscillate::test_support ALIAS oscillate_test_support)
    target_include_directories(oscillate_test_support INTERFACE
      "${OSCILLATE_SOURCE_DIR}/tests/support/include")
    target_link_libraries(oscillate_test_support INTERFACE oscillate::foundation)
  endif()
  target_link_libraries(${target} PRIVATE oscillate::test_support)
endfunction()

# A test program: links the test support, joins the `oscillate_tests` build
# target and, when the configuration asks for it, leaves the default build.
function(oscillate_mark_test_program target)
  oscillate_link_test_support(${target})
  get_target_property(already_excluded ${target} EXCLUDE_FROM_ALL)
  if(already_excluded)
    return()
  endif()
  set_property(GLOBAL APPEND PROPERTY OSCILLATE_TEST_BUILD_TARGETS ${target})
  if(NOT OSCILLATE_BUILD_TEST_TARGETS_BY_DEFAULT)
    set_target_properties(${target} PROPERTIES EXCLUDE_FROM_ALL ON)
  endif()
endfunction()

function(oscillate_target_defaults target)
  target_compile_features(${target} PUBLIC cxx_std_20)
  set_target_properties(${target} PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
  )
  get_target_property(type ${target} TYPE)
  file(RELATIVE_PATH target_directory "${OSCILLATE_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")
  set(in_tests FALSE)
  if(target_directory MATCHES "^tests(/|$)")
    set(in_tests TRUE)
  endif()

  # A program declared under tests/, or built from tests/ sources wherever it
  # is declared, is a test program. Libraries under tests/ (fixtures) only
  # join the test build target.
  if(type STREQUAL "EXECUTABLE")
    set(test_program ${in_tests})
    if(NOT test_program)
      get_target_property(target_sources ${target} SOURCES)
      foreach(source IN LISTS target_sources)
        get_filename_component(source "${source}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        if(source MATCHES "^${OSCILLATE_SOURCE_DIR}/tests/")
          set(test_program TRUE)
        endif()
      endforeach()
    endif()
    if(test_program)
      oscillate_mark_test_program(${target})
    endif()
  elseif(in_tests)
    get_target_property(already_excluded ${target} EXCLUDE_FROM_ALL)
    if(NOT already_excluded)
      set_property(GLOBAL APPEND PROPERTY OSCILLATE_TEST_BUILD_TARGETS ${target})
      if(NOT OSCILLATE_BUILD_TEST_TARGETS_BY_DEFAULT)
        set_target_properties(${target} PROPERTIES EXCLUDE_FROM_ALL ON)
      endif()
    endif()
  endif()

  if(type STREQUAL "SHARED_LIBRARY")
    # A shared library names every library it uses, or fails to link.
    if(NOT OSCILLATE_ENABLE_SANITIZERS AND NOT OSCILLATE_ENABLE_THREAD_SANITIZER)
      target_link_options(${target} PRIVATE -Wl,--no-undefined)
    endif()
    # Installed with the application (see the root CMakeLists).
    if(NOT in_tests AND NOT target_directory MATCHES "^(bench|tools)(/|$)")
      set_property(GLOBAL APPEND PROPERTY OSCILLATE_INSTALL_LIBRARIES ${target})
    endif()
  endif()

  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
    )
    if(OSCILLATE_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()

  if(OSCILLATE_ENABLE_COVERAGE)
    target_compile_options(${target} PRIVATE --coverage -O0 -g)
    target_link_options(${target} PRIVATE --coverage)
  endif()
  if(OSCILLATE_ENABLE_SANITIZERS)
    target_compile_options(${target} PRIVATE
      -fsanitize=address,undefined -fno-omit-frame-pointer
    )
    target_link_options(${target} PRIVATE -fsanitize=address,undefined)
  endif()
  if(OSCILLATE_ENABLE_THREAD_SANITIZER)
    target_compile_options(${target} PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=thread)
  endif()
endfunction()

# Usage requirements shared by the library and executable helpers.
macro(oscillate_apply_usage target)
  if(arg_PUBLIC OR arg_PRIVATE)
    target_link_libraries(${target} PUBLIC ${arg_PUBLIC} PRIVATE ${arg_PRIVATE})
  endif()
  if(arg_LIBS)
    target_link_libraries(${target} PRIVATE ${arg_LIBS})
  endif()
  if(arg_INCLUDES)
    target_include_directories(${target} PRIVATE ${arg_INCLUDES})
  endif()
  if(arg_DEFINITIONS)
    target_compile_definitions(${target} PRIVATE ${arg_DEFINITIONS})
  endif()
  if(arg_DEPENDS)
    add_dependencies(${target} ${arg_DEPENDS})
  endif()
  oscillate_target_defaults(${target})
  if(arg_OPTIMIZE)
    oscillate_optimize_in_debug(${target})
  endif()
endmacro()

# A project library: oscillate_<name>, aliased oscillate::<name>, with the
# public headers in include/ and the project defaults.
#
#   oscillate_add_library(<name> [STATIC|SHARED] SOURCES <src>...
#                         [PUBLIC <lib>...] [PRIVATE <lib>...]
#                         [INCLUDES <private dir>...] [DEFINITIONS <private def>...]
#                         [OPTIMIZE])
# OPTIMIZE compiles an audio kernel optimised in Debug (oscillate_optimize_in_debug).
function(oscillate_add_library name)
  cmake_parse_arguments(PARSE_ARGV 1 arg "STATIC;SHARED;OPTIMIZE" ""
    "SOURCES;PUBLIC;PRIVATE;INCLUDES;DEFINITIONS")
  if(arg_UNPARSED_ARGUMENTS OR NOT arg_SOURCES)
    message(FATAL_ERROR "oscillate_add_library(${name}): needs SOURCES; unknown ${arg_UNPARSED_ARGUMENTS}")
  endif()
  set(kind)
  if(arg_STATIC)
    set(kind STATIC)
  elseif(arg_SHARED)
    set(kind SHARED)
  endif()
  set(target oscillate_${name})
  add_library(${target} ${kind} ${arg_SOURCES})
  add_library(oscillate::${name} ALIAS ${target})
  target_include_directories(${target} PUBLIC
    $<BUILD_INTERFACE:${OSCILLATE_SOURCE_DIR}/include>)
  oscillate_apply_usage(${target})
endfunction()

# A program (tool, bench or fixture) with the project defaults.
#
#   oscillate_add_executable(<target> SOURCES <src>... [LIBS <lib>...]
#                            [INCLUDES <dir>...] [DEFINITIONS <def>...]
#                            [DEPENDS <target>...] [OPTIMIZE] [EXCLUDE_FROM_ALL])
function(oscillate_add_executable target)
  cmake_parse_arguments(PARSE_ARGV 1 arg "OPTIMIZE;EXCLUDE_FROM_ALL" ""
    "SOURCES;LIBS;INCLUDES;DEFINITIONS;DEPENDS")
  if(arg_UNPARSED_ARGUMENTS OR NOT arg_SOURCES)
    message(FATAL_ERROR "oscillate_add_executable(${target}): needs SOURCES; unknown ${arg_UNPARSED_ARGUMENTS}")
  endif()
  set(exclude)
  if(arg_EXCLUDE_FROM_ALL)
    set(exclude EXCLUDE_FROM_ALL)
  endif()
  add_executable(${target} ${exclude} ${arg_SOURCES})
  oscillate_apply_usage(${target})
endfunction()

# A test program, optionally registered as one test.
#
#   oscillate_add_test_executable(<target> SOURCES <src>... [LIBS <lib>...]
#       [INCLUDES <dir>...] [DEFINITIONS <def>...] [DEPENDS <target>...]
#       [OPTIMIZE] [TEST <name> [TIER fast|slow|live] [LABELS <label>...]])
#
# A directory whose test programs all render audio sets
# OSCILLATE_OPTIMIZE_TESTS_IN_DEBUG before declaring them, which implies
# OPTIMIZE for each. Optimised (inlined) allocation-counting tests replace
# global operator new/delete with malloc/free, which GCC wrongly reports as
# mismatched pairs, so OPTIMIZE also silences -Wmismatched-new-delete.
function(oscillate_add_test_executable target)
  cmake_parse_arguments(PARSE_ARGV 1 arg "OPTIMIZE" "TEST;TIER"
    "SOURCES;LIBS;INCLUDES;DEFINITIONS;DEPENDS;LABELS")
  if(arg_UNPARSED_ARGUMENTS OR NOT arg_SOURCES)
    message(FATAL_ERROR "oscillate_add_test_executable(${target}): needs SOURCES; unknown ${arg_UNPARSED_ARGUMENTS}")
  endif()
  if((arg_TIER OR arg_LABELS) AND NOT arg_TEST)
    message(FATAL_ERROR "oscillate_add_test_executable(${target}): TIER and LABELS need TEST")
  endif()
  if(OSCILLATE_OPTIMIZE_TESTS_IN_DEBUG)
    set(arg_OPTIMIZE TRUE)
  endif()
  add_executable(${target} ${arg_SOURCES})
  # oscillate_target_defaults marks it a test program by its directory or sources.
  oscillate_apply_usage(${target})
  if(arg_OPTIMIZE AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(${target} PRIVATE -Wno-mismatched-new-delete)
  endif()
  if(arg_TEST)
    set(tier)
    if(arg_TIER)
      set(tier TIER ${arg_TIER})
    endif()
    set(labels)
    if(arg_LABELS)
      set(labels LABELS ${arg_LABELS})
    endif()
    oscillate_add_test(NAME ${arg_TEST} COMMAND ${target} ${tier} ${labels})
  endif()
endfunction()

# A contract split into cases (oscillate/test/cases.hpp): one CTest entry per
# case, so -j spreads a long contract over cores and each case has its own
# budget, plus `<name>.cases`, which fails when the binary's `--list` differs
# from CASES so a new case cannot go unregistered. Cases listed under SLOW
# (over ~10 s in a Debug -j16 run) get the slow tier; the rest are fast.
#
#   oscillate_add_case_tests(<name> <target> CASES <case>... [SLOW <case>...])
function(oscillate_add_case_tests name target)
  cmake_parse_arguments(PARSE_ARGV 2 arg "" "" "CASES;SLOW")
  foreach(case IN LISTS arg_SLOW)
    if(NOT case IN_LIST arg_CASES)
      message(FATAL_ERROR "${name}: slow case ${case} is not a case")
    endif()
  endforeach()
  foreach(case IN LISTS arg_CASES)
    set(tier fast)
    if(case IN_LIST arg_SLOW)
      set(tier slow)
    endif()
    oscillate_add_test(NAME ${name}.${case} COMMAND ${target} ${case} TIER ${tier})
  endforeach()
  string(REPLACE ";" "," joined "${arg_CASES}")
  oscillate_add_test(NAME ${name}.cases
    COMMAND ${CMAKE_COMMAND} -DBINARY=$<TARGET_FILE:${target}> -DCASES=${joined}
            -P ${OSCILLATE_SOURCE_DIR}/tests/support/check_cases.cmake)
endfunction()

# Every test is registered through oscillate_add_test so it carries a tier
# and an area label; oscillate_verify_test_labels() fails configuration when
# a raw add_test() slips in.
#
#   oscillate_add_test(NAME <name> COMMAND <command> [args...]
#                      [TIER fast|slow|live] [LABELS <extra>...]
#                      [WORKING_DIRECTORY <dir>] [SKIP_RETURN_CODE <code>])
# A test that cannot run here (a missing optional library, say) exits with
# its SKIP_RETURN_CODE, conventionally OSCILLATE_TEST_SKIP_RETURN_CODE, and
# CTest reports it as skipped rather than passed.
#
# Tiers (Debug wall time, one core):
#   fast  under ~10 s; the pre-commit loop (`ctest --preset quick-fast`).
#   slow  DSP oracles, parity and long renders; run before merging.
#   live  needs real audio devices, a display server or a JACK server.
# The area label is derived from the directory: tests/<area>/..., apps/* are
# "ui", src/<area> keeps <area>.
set(OSCILLATE_TEST_TIERS fast slow live)
set(OSCILLATE_TEST_SKIP_RETURN_CODE 77)

# Test budgets (the CTest timeout), in seconds. Each tier has a default:
# a fast test without an entry in TestBudgets.cmake gets it. Slow and live
# tests must have a measured entry, never below their tier's default, and a
# fast test needs one only when it measures above the default. An entry is
# three times the test's runtime under `ctest -j4`; regenerate the table with
# scripts/update-test-budgets.py, which reads these defaults. Sanitizer and
# coverage presets scale every budget; see also
# OSCILLATE_TEST_UNOPTIMISED_DSP_SCALE below.
set(OSCILLATE_TEST_DEFAULT_BUDGET_fast 5)
set(OSCILLATE_TEST_DEFAULT_BUDGET_slow 30)
set(OSCILLATE_TEST_DEFAULT_BUDGET_live 5)
set(OSCILLATE_TEST_TIMEOUT_SCALE 1 CACHE STRING "Multiplier applied to every test budget")
# Budgets are measured with the DSP kernels and oracle test programs
# optimised (oscillate_optimize_in_debug). A Debug build that leaves them at
# -O0 (coverage, or OSCILLATE_OPTIMIZE_DSP_IN_DEBUG=OFF) runs those programs
# this many times slower, so their tests' budgets are scaled by it as well.
set(OSCILLATE_TEST_UNOPTIMISED_DSP_SCALE 10 CACHE STRING
  "Further budget multiplier for optimised DSP test programs built at -O0")
include("${CMAKE_CURRENT_LIST_DIR}/TestBudgets.cmake")

function(oscillate_test_area out)
  file(RELATIVE_PATH relative "${OSCILLATE_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")
  string(REPLACE "/" ";" parts "${relative}")
  list(GET parts 0 root)
  list(LENGTH parts depth)
  if(root STREQUAL "apps")
    set(area ui)
  elseif(depth GREATER 1)
    list(GET parts 1 area)
  else()
    set(area "${root}")
  endif()
  set(${out} "${area}" PARENT_SCOPE)
endfunction()

function(oscillate_add_test)
  cmake_parse_arguments(PARSE_ARGV 0 arg "" "NAME;TIER;WORKING_DIRECTORY;SKIP_RETURN_CODE"
    "COMMAND;LABELS")
  if(NOT arg_NAME OR NOT arg_COMMAND)
    message(FATAL_ERROR "oscillate_add_test needs NAME and COMMAND")
  endif()
  if(arg_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "oscillate_add_test(${arg_NAME}): unknown arguments ${arg_UNPARSED_ARGUMENTS}")
  endif()
  if(NOT arg_TIER)
    set(arg_TIER fast)
  endif()
  if(NOT arg_TIER IN_LIST OSCILLATE_TEST_TIERS)
    message(FATAL_ERROR "oscillate_add_test(${arg_NAME}): TIER must be one of ${OSCILLATE_TEST_TIERS}")
  endif()
  set(extra)
  if(arg_WORKING_DIRECTORY)
    set(extra WORKING_DIRECTORY "${arg_WORKING_DIRECTORY}")
  endif()
  add_test(NAME ${arg_NAME} COMMAND ${arg_COMMAND} ${extra})
  list(GET arg_COMMAND 0 program)
  if(TARGET ${program})
    get_target_property(dsp_optimised ${program} OSCILLATE_DSP_OPTIMISED)
    if(dsp_optimised)
      set_tests_properties(${arg_NAME} PROPERTIES OSCILLATE_DSP_OPTIMISED ON)
    endif()
  endif()
  oscillate_test_area(area)
  set_tests_properties(${arg_NAME} PROPERTIES LABELS "${arg_TIER};${area};${arg_LABELS}")
  if(DEFINED arg_SKIP_RETURN_CODE)
    set_tests_properties(${arg_NAME} PROPERTIES SKIP_RETURN_CODE ${arg_SKIP_RETURN_CODE})
  endif()
endfunction()

function(oscillate_verify_test_labels directory)
  get_property(tests DIRECTORY "${directory}" PROPERTY TESTS)
  foreach(test IN LISTS tests)
    get_test_property(${test} LABELS DIRECTORY "${directory}" labels)
    set(tier)
    foreach(candidate IN LISTS OSCILLATE_TEST_TIERS)
      if(candidate IN_LIST labels)
        set(tier ${candidate})
      endif()
    endforeach()
    if(NOT tier)
      message(FATAL_ERROR "Test ${test} in ${directory} has no tier label: "
                          "register it with oscillate_add_test()")
    endif()
    if(DEFINED OSCILLATE_TEST_BUDGET_${test})
      set(budget ${OSCILLATE_TEST_BUDGET_${test}})
    elseif(tier STREQUAL "fast")
      set(budget ${OSCILLATE_TEST_DEFAULT_BUDGET_fast})
    else()
      message(FATAL_ERROR "The ${tier} test ${test} has no budget in cmake/TestBudgets.cmake: "
                          "run it and add one with scripts/update-test-budgets.py")
    endif()
    math(EXPR timeout "${budget} * ${OSCILLATE_TEST_TIMEOUT_SCALE}")
    get_test_property(${test} OSCILLATE_DSP_OPTIMISED DIRECTORY "${directory}" dsp_optimised)
    if(dsp_optimised AND NOT OSCILLATE_DSP_OPTIMISED_IN_DEBUG)
      # Only the Debug configuration builds these programs at -O0 here.
      math(EXPR unoptimised "${timeout} * ${OSCILLATE_TEST_UNOPTIMISED_DSP_SCALE}")
      set(timeout "$<IF:$<CONFIG:Debug>,${unoptimised},${timeout}>")
    endif()
    set_tests_properties(${test} DIRECTORY "${directory}" PROPERTIES TIMEOUT ${timeout})
  endforeach()
  get_property(children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
  foreach(child IN LISTS children)
    oscillate_verify_test_labels("${child}")
  endforeach()
endfunction()
