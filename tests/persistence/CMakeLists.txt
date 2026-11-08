oscillate_add_test_executable(oscillate_persistence_tests SOURCES store_tests.cpp
  LIBS oscillate::persistence nlohmann_json::nlohmann_json OpenSSL::Crypto
  INCLUDES ${CMAKE_CURRENT_LIST_DIR}/../../src/persistence
  TEST persistence_contract TIER slow)

oscillate_add_test_executable(oscillate_persistence_process_kill_tests SOURCES process_kill_tests.cpp
  LIBS oscillate::persistence
  TEST persistence_process_kill)

oscillate_add_test_executable(oscillate_insert_schema_tests SOURCES insert_schema_tests.cpp
  LIBS oscillate::persistence nlohmann_json::nlohmann_json OpenSSL::Crypto
  INCLUDES ${CMAKE_CURRENT_LIST_DIR}/../../src/persistence
  TEST persistence_insert_schema)

oscillate_add_test_executable(oscillate_key_review_tests SOURCES key_review_tests.cpp
  LIBS oscillate::persistence nlohmann_json::nlohmann_json OpenSSL::Crypto
  INCLUDES ${CMAKE_CURRENT_LIST_DIR}/../../src/persistence
  TEST persistence_key_review)

oscillate_add_test_executable(oscillate_clip_fade_schema_tests SOURCES clip_fade_schema_tests.cpp
  LIBS oscillate::persistence nlohmann_json::nlohmann_json OpenSSL::Crypto
  INCLUDES ${CMAKE_CURRENT_LIST_DIR}/../../src/persistence
  TEST persistence_clip_fade_schema)

oscillate_add_test_executable(oscillate_native_instrument_schema_tests SOURCES native_instrument_schema_tests.cpp
  LIBS oscillate::persistence nlohmann_json::nlohmann_json OpenSSL::Crypto
  INCLUDES ${CMAKE_CURRENT_LIST_DIR}/../../src/persistence
  TEST persistence_native_instrument_schema)

if(OSCILLATE_BUILD_CODEC_ADAPTER)
  target_compile_definitions(oscillate_persistence_tests PRIVATE OSCILLATE_HAVE_CODEC=1)
  oscillate_add_test_executable(oscillate_encoded_source_store_tests SOURCES encoded_store_tests.cpp
    LIBS oscillate::persistence oscillate::codec
    DEFINITIONS OSCILLATE_CODEC_FIXTURE_DIR="${CMAKE_CURRENT_LIST_DIR}/../codec/assets"
    TEST persistence_encoded_sources)
else()
  target_compile_definitions(oscillate_persistence_tests PRIVATE OSCILLATE_HAVE_CODEC=0)
endif()

oscillate_add_test_executable(oscillate_midi_pedal_schema_tests SOURCES midi_pedal_schema_tests.cpp
  LIBS oscillate::persistence nlohmann_json::nlohmann_json OpenSSL::Crypto
  INCLUDES ${CMAKE_CURRENT_LIST_DIR}/../../src/persistence
  TEST persistence_midi_pedal_schema)

oscillate_add_test_executable(oscillate_native_effect_schema_tests SOURCES native_effect_schema_tests.cpp
  LIBS oscillate::persistence nlohmann_json::nlohmann_json OpenSSL::Crypto
  INCLUDES ${CMAKE_CURRENT_LIST_DIR}/../../src/persistence
  TEST persistence_native_effect_schema)

oscillate_add_test_executable(oscillate_presentation_schema_tests SOURCES presentation_schema_tests.cpp
  LIBS oscillate::persistence nlohmann_json::nlohmann_json OpenSSL::Crypto
  INCLUDES ${CMAKE_CURRENT_LIST_DIR}/../../src/persistence
  TEST persistence_presentation_schema)

oscillate_add_test_executable(oscillate_schema_migration_tests SOURCES schema_migration_tests.cpp
  LIBS oscillate::persistence nlohmann_json::nlohmann_json OpenSSL::Crypto
  INCLUDES ${CMAKE_CURRENT_LIST_DIR}/../../src/persistence
  DEFINITIONS OSCILLATE_SCHEMA_GOLDEN_DIR="${CMAKE_CURRENT_LIST_DIR}/golden"
  TEST persistence_schema_migration)
