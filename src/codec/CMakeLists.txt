oscillate_add_library(codec SOURCES encoded_reader.cpp
  PUBLIC oscillate::foundation
  PRIVATE ${OSCILLATE_SNDFILE_LIBRARY})
target_include_directories(oscillate_codec SYSTEM PRIVATE ${OSCILLATE_SNDFILE_INCLUDE_DIR})
