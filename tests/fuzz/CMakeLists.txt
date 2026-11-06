add_library(oscillate_fuzz_exercise STATIC parser_exercise.cpp)
target_include_directories(oscillate_fuzz_exercise PUBLIC ${CMAKE_CURRENT_LIST_DIR})
target_include_directories(oscillate_fuzz_exercise PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/../../src/persistence)
target_link_libraries(oscillate_fuzz_exercise PRIVATE oscillate::persistence oscillate::media
  oscillate::midi_file)
oscillate_target_defaults(oscillate_fuzz_exercise)

oscillate_add_test_executable(oscillate_fuzz_corpus_replay SOURCES corpus_replay.cpp
  LIBS oscillate_fuzz_exercise)
oscillate_add_test(NAME fuzz_manifest_corpus COMMAND oscillate_fuzz_corpus_replay manifest
  ${CMAKE_CURRENT_LIST_DIR}/corpus/manifest)
oscillate_add_test(NAME fuzz_wav_corpus COMMAND oscillate_fuzz_corpus_replay wav
  ${CMAKE_CURRENT_LIST_DIR}/corpus/wav)
oscillate_add_test(NAME fuzz_midi_corpus COMMAND oscillate_fuzz_corpus_replay midi
  ${CMAKE_CURRENT_LIST_DIR}/corpus/midi)

oscillate_add_executable(oscillate_fuzz_manifest_seed EXCLUDE_FROM_ALL SOURCES manifest_seed.cpp
  LIBS oscillate::persistence
  INCLUDES ${CMAKE_CURRENT_LIST_DIR}/../../src/persistence)

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  foreach(parser IN ITEMS manifest wav midi)
    add_executable(oscillate_fuzz_${parser} EXCLUDE_FROM_ALL fuzz_${parser}.cpp)
    target_link_libraries(oscillate_fuzz_${parser} PRIVATE oscillate_fuzz_exercise)
    target_link_options(oscillate_fuzz_${parser} PRIVATE -fsanitize=fuzzer,address,undefined)
    oscillate_target_defaults(oscillate_fuzz_${parser})
  endforeach()
else()
  message(STATUS "libFuzzer targets require Clang; GCC corpus replay remains available")
endif()
