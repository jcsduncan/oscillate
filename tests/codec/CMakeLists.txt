oscillate_add_test_executable(oscillate_codec_tests SOURCES encoded_reader_tests.cpp
  LIBS oscillate::codec
  DEFINITIONS OSCILLATE_CODEC_FIXTURE_DIR="${CMAKE_CURRENT_LIST_DIR}/assets"
  TEST codec_encoded_reader)
