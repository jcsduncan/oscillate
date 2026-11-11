oscillate_add_test_executable(oscillate_audio_io_tests SOURCES audio_io_tests.cpp
  LIBS oscillate::audio_io
  TEST audio_io_contract)

# The direct ALSA output against ALSA's null and file PCMs: no sound card needed.
oscillate_add_test_executable(oscillate_alsa_output_tests SOURCES alsa_output_tests.cpp
  LIBS oscillate::audio_io Threads::Threads)
oscillate_add_case_tests(alsa_output_contract oscillate_alsa_output_tests
  CASES negotiates_periods_and_shuts_down_cleanly refuses_invalid_configurations
        converts_each_format_onto_the_first_two_channels
        writes_the_device_format_through_a_file_pcm recovers_an_injected_xrun
        a_lost_device_stops_streaming the_io_thread_never_allocates
        a_refused_reservation_keeps_the_card_closed a_card_that_does_not_open_is_given_back
        status_reads_race_nothing_while_starting_and_stopping names_cards)

if(OSCILLATE_AUDIO_IO_LIVE_TESTS)
  oscillate_add_test_executable(oscillate_audio_io_live_smoke SOURCES pipewire_live_smoke.cpp
    LIBS oscillate::audio_io
    TEST audio_io_pipewire_live TIER live)
  if(JACK_FOUND)
    oscillate_add_test_executable(oscillate_jack_live_smoke SOURCES jack_live_smoke.cpp
      LIBS oscillate::audio_io PkgConfig::JACK)
    oscillate_add_test(NAME audio_io_jack_live COMMAND oscillate_jack_live_smoke connect TIER live)
    oscillate_add_test(NAME audio_io_jack_live_mismatch COMMAND oscillate_jack_live_smoke mismatch TIER live)
  endif()
endif()

oscillate_add_test_executable(oscillate_render_ahead_tests SOURCES render_ahead_tests.cpp
  LIBS oscillate::audio_io Threads::Threads)
oscillate_add_case_tests(render_ahead_contract oscillate_render_ahead_tests
  CASES plays_exactly_what_just_in_time_plays a_seek_drops_the_window
        an_edit_is_heard_within_the_window_without_a_gap a_load_spike_is_absorbed
        a_stall_plays_from_the_reserve a_part_before_a_held_wrap_keeps_its_place
        live_input_renders_just_in_time stop_fades_and_renders_nothing_more
        a_quiesced_seek_clears_the_window a_restarted_output_plays_on
        the_callback_never_allocates)
