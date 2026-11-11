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

oscillate_add_test_executable(oscillate_routed_wavefront_tests SOURCES routed_wavefront_tests.cpp
  LIBS oscillate::engine Threads::Threads)
oscillate_add_case_tests(routed_wavefront_contract oscillate_routed_wavefront_tests
  CASES session_parts_match_whole_blocks session_with_automation_matches
        nested_groups_match_whole_blocks flat_mix_matches_whole_blocks
        staged_master_matches_whole_blocks
        parts_carry_their_positions a_failed_fill_fails_the_block plugins_render_whole_in_a_split_block
        parts_never_allocate
        cheap_levels_after_the_tracks_stay_whole
        the_policy_splits_only_for_a_clear_gain)
