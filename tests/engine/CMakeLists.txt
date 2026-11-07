oscillate_add_test_executable(oscillate_engine_tests SOURCES graph_tests.cpp
  LIBS oscillate::engine Threads::Threads
  TEST engine_graph_contract LABELS parity)

oscillate_add_test_executable(oscillate_midi_events_tests SOURCES midi_events_tests.cpp
  LIBS oscillate::engine
  TEST midi_events_contract)

oscillate_add_test_executable(oscillate_routing_latency_tests SOURCES routing_latency_tests.cpp
  LIBS oscillate::engine
  TEST routing_latency_contract)

oscillate_add_test_executable(oscillate_delay_compensation_tests SOURCES delay_compensation_tests.cpp
  LIBS oscillate::engine
  TEST delay_compensation_contract)

oscillate_add_test_executable(oscillate_audio_worker_pool_tests SOURCES audio_worker_pool_tests.cpp
  LIBS oscillate::engine
  TEST audio_worker_pool_contract)

oscillate_add_test_executable(oscillate_routed_mixer_tests SOURCES routed_mixer_tests.cpp
  LIBS oscillate::engine
  TEST routed_mixer_contract)

oscillate_add_test_executable(oscillate_routed_node_meters_tests SOURCES routed_node_meters_tests.cpp
  LIBS oscillate::engine
  TEST routed_node_meters_contract)

oscillate_add_test_executable(oscillate_routed_pdc_transition_probe SOURCES routed_pdc_transition_probe.cpp
  LIBS oscillate::engine
  TEST routed_pdc_transition_probe LABELS parity)

oscillate_add_test_executable(oscillate_routed_publisher_tests SOURCES routed_publisher_tests.cpp
  LIBS oscillate::engine Threads::Threads
  TEST routed_publisher_contract)
