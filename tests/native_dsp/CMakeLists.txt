# These contracts render seconds of audio through FFT-heavy oracles; at -O0
# they dominate the suite, so every test program here is optimised in Debug.
set(OSCILLATE_OPTIMIZE_TESTS_IN_DEBUG ON)

oscillate_add_test_executable(oscillate_synth_dsp_tests SOURCES synth_dsp_tests.cpp
  LIBS oscillate::native_synth
  TEST native_synth_dsp_contract)

oscillate_add_test_executable(oscillate_wavetable_dsp_tests SOURCES wavetable_dsp_tests.cpp
  LIBS oscillate::native_synth)
# Optimised, GCC inlines WavetableOscillator::next into the test and cannot
# see that wavetable_level_for (out of line) never returns the "no level yet"
# sentinel, so it flags levels[10] on a path set_band rules out.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  target_compile_options(oscillate_wavetable_dsp_tests PRIVATE -Wno-array-bounds)
endif()
oscillate_add_test(NAME native_wavetable_dsp_contract COMMAND oscillate_wavetable_dsp_tests)

oscillate_add_test_executable(oscillate_drum_dsp_tests SOURCES drum_dsp_tests.cpp
  LIBS oscillate::native_synth
  TEST native_drum_dsp_contract)

oscillate_add_test_executable(oscillate_fm_dsp_tests SOURCES fm_dsp_tests.cpp
  LIBS oscillate::native_synth
  TEST native_fm_dsp_contract)

oscillate_add_test_executable(oscillate_bass_dsp_tests SOURCES bass_dsp_tests.cpp
  LIBS oscillate::native_synth
  TEST native_bass_dsp_contract)

oscillate_add_test_executable(oscillate_grain_dsp_tests SOURCES grain_dsp_tests.cpp
  LIBS oscillate::native_synth
  TEST native_grain_dsp_contract)

oscillate_add_test_executable(oscillate_vintage_dsp_tests SOURCES vintage_dsp_tests.cpp
  LIBS oscillate::native_synth
  TEST native_vintage_dsp_contract)

oscillate_add_test_executable(oscillate_ember_tests SOURCES ember_tests.cpp
  LIBS oscillate::synth_engines
  TEST native_ember_contract)

oscillate_add_test_executable(oscillate_pulse_tests SOURCES pulse_tests.cpp
  LIBS oscillate::synth_engines
  TEST native_pulse_contract)

oscillate_add_test_executable(oscillate_halo_tests SOURCES halo_tests.cpp
  LIBS oscillate::synth_engines
  TEST native_halo_contract)

oscillate_add_test_executable(oscillate_torque_tests SOURCES torque_tests.cpp
  LIBS oscillate::synth_engines
  TEST native_torque_contract)

oscillate_add_test_executable(oscillate_drift_tests SOURCES drift_tests.cpp
  LIBS oscillate::synth_engines
  TEST native_drift_contract)

oscillate_add_test_executable(oscillate_prism_tests SOURCES prism_tests.cpp
  LIBS oscillate::synth_engines
  TEST native_prism_contract)

oscillate_add_test_executable(oscillate_eq_tests SOURCES eq_tests.cpp
  LIBS oscillate::native_dsp
  TEST native_eq_contract)

oscillate_add_test_executable(oscillate_compressor_tests SOURCES compressor_tests.cpp
  LIBS oscillate::native_dsp
  TEST native_compressor_contract)

oscillate_add_test_executable(oscillate_limiter_tests SOURCES limiter_tests.cpp
  LIBS oscillate::native_dsp
  TEST native_limiter_contract)

oscillate_add_test_executable(oscillate_meter_tests SOURCES meter_tests.cpp
  LIBS oscillate::native_dsp
  TEST native_meter_contract)

oscillate_add_test_executable(oscillate_true_peak_tests SOURCES true_peak_tests.cpp
  LIBS oscillate::native_dsp
  TEST native_true_peak_contract)

oscillate_add_test_executable(oscillate_correlation_tests SOURCES correlation_tests.cpp
  LIBS oscillate::native_dsp
  TEST native_correlation_contract)

oscillate_add_test_executable(oscillate_loudness_tests SOURCES loudness_tests.cpp
  LIBS oscillate::native_dsp
  TEST native_loudness_contract)

oscillate_add_test_executable(oscillate_spectrum_tests SOURCES spectrum_tests.cpp
  LIBS oscillate::native_dsp
  TEST native_spectrum_contract)

oscillate_add_test_executable(oscillate_synth_switch_tests SOURCES synth_switch_tests.cpp
  LIBS oscillate::synth_engines
  TEST native_synth_switch_contract)

oscillate_add_test_executable(oscillate_tempo_sync_tests SOURCES tempo_sync_tests.cpp
  LIBS oscillate::synth_engines
  TEST native_tempo_sync_contract)

oscillate_add_test_executable(oscillate_dx_envelope_tests SOURCES dx_envelope_tests.cpp
  LIBS oscillate::synth_engines
  TEST native_dx_envelope_contract)

oscillate_add_test_executable(oscillate_telemetry_tests SOURCES telemetry_tests.cpp
  LIBS oscillate::synth_engines
  TEST native_synth_telemetry_contract)

oscillate_add_test_executable(oscillate_pedal_tests SOURCES pedal_tests.cpp
  LIBS oscillate::synth_engines oscillate::native_inserts
  TEST native_pedal_contract)

oscillate_add_test_executable(oscillate_note_effect_tests SOURCES note_effect_tests.cpp
  LIBS oscillate::synth_engines oscillate::native_inserts)
oscillate_add_case_tests(native_note_effect_contract oscillate_note_effect_tests
  CASES the_registry_is_complete the_arp_plays_up_on_the_sixteenth_grid
        the_arp_modes_order_the_notes the_arp_swings_starts_at_once_and_latches
        the_chord_adds_intervals_or_degrees the_echo_repeats_on_the_grid
        random_is_seeded_and_in_key scale_snaps_and_moves the_chain_releases_what_it_made
        the_player_arpeggiates_a_clip the_player_arpeggiates_live_notes)

# Comp (ADR-0083, ADR-0084): held notes played in a rhythm.
oscillate_add_test_executable(oscillate_comp_tests SOURCES comp_tests.cpp
  LIBS oscillate::synth_engines oscillate::native_inserts)
oscillate_add_case_tests(native_comp_contract oscillate_comp_tests
  CASES comp_plays_held_notes_on_its_lanes comp_holds_its_input_and_passes_strangers
        comp_pushes_changes_by_chance comp_top_lane_runs_its_own_length
        comp_changes_chord_mid_bar comp_swings_and_humanises_late_only
        the_lookahead_sees_a_dotted_eighth the_chain_hands_stages_the_notes_ahead
        the_player_pushes_the_clip_chords a_comp_edit_goes_live_without_a_gap
        comp_motion_keeps_a_loop_moving comp_density_thins_and_fills)

oscillate_add_test_executable(oscillate_synth_document_tests SOURCES synth_document_tests.cpp
  LIBS oscillate::synth_engines oscillate::native_inserts
  TEST native_synth_document_contract)

oscillate_add_test_executable(oscillate_concert_tests SOURCES concert_tests.cpp
  LIBS oscillate::synth_engines)
oscillate_add_case_tests(native_concert_contract oscillate_concert_tests
  CASES
  a_missing_library_is_silent
  malformed_libraries_are_rejected
  descriptor_registry_and_presets
  keys_are_in_tune
  layers_crossfade_without_steps
  notes_stream_past_their_resident_start
  pedals_hold_and_release
  una_corda_softens
  any_block_split_renders_identically
  full_load_does_not_allocate
  tail_bound_covers_release
  presets_render_sane_audio
  telemetry_reports_keys_and_pedals
  presets_match_golden_fingerprints)

# The shared sample-streaming core (ADR-0088), on a synthetic mono pack.
oscillate_add_test_executable(oscillate_sample_stream_tests SOURCES sample_stream_tests.cpp
  LIBS oscillate::synth_engines)
oscillate_add_case_tests(native_sample_stream_contract oscillate_sample_stream_tests
  CASES
  pack_opens_and_reads_resident
  malformed_packs_are_rejected
  registry_loads_and_keys_by_serial
  mono_stream_is_bit_exact_across_the_join
  request_words_round_trip
  sinc_rows_sum_to_one)

oscillate_add_test_executable(oscillate_anvil_tests SOURCES anvil_tests.cpp
  LIBS oscillate::synth_engines)
oscillate_add_case_tests(native_anvil_contract oscillate_anvil_tests
  CASES
  descriptor_registry_and_presets
  documents_validate_and_compile
  pitch_follows_the_curve
  the_tail_is_in_tune
  amp_curve_is_the_envelope
  retrigger_never_clicks
  rumble_is_tuned_and_ducks
  drive_is_bounded_and_clean
  the_ceiling_holds
  any_block_split_renders_identically
  full_load_does_not_allocate
  tail_bound_covers_the_rumble
  preview_is_the_kernel
  telemetry_reports_the_hit
  presets_render_sane_audio
  synced_length_follows_the_tempo
  note_length_sets_the_hit)

# Kick & Bass Lock (ADR-0072): Anvil and Nova following each other.
oscillate_add_test_executable(oscillate_lock_tests SOURCES lock_tests.cpp
  LIBS oscillate::synth_engines oscillate::native_inserts)
oscillate_add_case_tests(native_lock_contract oscillate_lock_tests
  CASES
  nova_lock_off_is_untouched
  nova_lock_keeps_levels_and_monos_the_lows
  nova_mono_lows_on_a_unison_bass
  nova_duck_follows_the_hit_with_no_latency
  nova_block_splits_render_identically_with_lock
  anvil_lock_tunes_the_tail_to_the_bass
  anvil_keep_tone_locks_without_retuning
  anvil_block_splits_render_identically_with_lock)

# The modal library (ADR-0068): the felt contact law, resonators and the bank
# Knock rings.
oscillate_add_test_executable(oscillate_modal_dsp_tests SOURCES modal_dsp_tests.cpp
  LIBS oscillate::native_synth)
oscillate_add_case_tests(native_modal_dsp_contract oscillate_modal_dsp_tests
  CASES
  contact_law_is_stulov
  rigid_contact_round_trips
  a_mode_rings_as_designed
  an_impulse_moves_the_mode_by_its_mass
  driven_gain_is_unity_at_resonance
  retune_moves_every_mode
  a_striker_bounces_off_a_heavy_bank
  retire_silences)

# Knock (ADR-0068): physically modelled percussion.
oscillate_add_test_executable(oscillate_knock_tests SOURCES knock_tests.cpp
  LIBS oscillate::synth_engines)
oscillate_add_case_tests(native_knock_contract oscillate_knock_tests
  CASES
  descriptor_registry_and_presets
  any_block_split_renders_identically
  reset_is_deterministic
  every_preset_is_finite_and_bounded
  tone_zero_is_a_thing_tone_one_a_note
  bend_falls_and_rises
  the_gate_ends_the_hit
  lift_attenuates_below_its_cutoff
  crush_quantises
  coupling_rings_the_coupled_modes
  contact_time_follows_hardness
  velocity_brightens
  global_decay_shortens_every_ring
  preset_levels_sit_in_a_window
  design_pad_is_what_the_kernel_plays
  telemetry_reports_hits
  full_load_does_not_allocate
  tail_bound_covers_every_kit
  preset_fingerprints
  spread_widens_and_keeps_lows_centred
  ping_pong_alternates_sides
  scatter_is_seeded_and_varies
  orbit_follows_the_transport
  haas_delays_the_far_ear
  swing_delays_off_beat_sixteenths
  swung_hits_survive_any_block_split_and_reset
  telemetry_reports_landing_side
  calibration_ignores_width
  pad_roles_and_default_widths
  in_key_snaps_to_the_scale
  pads_follow_the_project_key
  key_follow_defaults_from_the_sound
  SLOW tail_bound_covers_every_kit)

oscillate_add_test_executable(oscillate_fret_tests SOURCES fret_tests.cpp
  LIBS oscillate::synth_engines)
oscillate_add_case_tests(native_fret_contract oscillate_fret_tests
  CASES
  a_missing_library_is_silent
  malformed_libraries_are_rejected
  descriptor_registry_and_presets
  keys_are_in_tune
  velocity_layers_and_round_robins
  feel_picks_articulations
  forced_articulation_plays_every_note
  pocket_plays_off_beats_late
  slides_glide_to_the_next_note
  releases_sound_at_note_off
  streaming_block_splits_and_resets_are_deterministic
  the_callback_does_not_allocate
  the_tail_bound_covers_a_released_note)

oscillate_add_test_executable(oscillate_wren_tests SOURCES wren_tests.cpp
  LIBS oscillate::synth_engines)
oscillate_add_case_tests(native_wren_contract oscillate_wren_tests
  CASES
  a_missing_library_is_silent
  malformed_libraries_are_rejected
  descriptor_registry_and_presets
  keys_are_in_tune
  velocity_picks_layers_and_trims
  missing_cells_fall_back
  slurs_skip_the_attack_and_crossfade
  voices_mono_and_poly
  played_follows_voices_and_articulation
  vibrato_blooms_after_its_delay
  loops_hold_a_note_beyond_its_recording
  releases_end_on_time
  graces_use_the_project_key
  block_splits_render_the_same
  a_reset_plays_the_same_again
  the_callback_does_not_allocate
  the_tail_bound_covers_a_released_note)

oscillate_add_test_executable(oscillate_bow_tests SOURCES bow_tests.cpp
  LIBS oscillate::synth_engines)
oscillate_add_case_tests(native_bow_contract oscillate_bow_tests
  CASES
  a_missing_library_is_silent_then_plays_once_installed
  malformed_libraries_are_rejected
  descriptor_registry_and_presets
  keys_are_in_tune_in_both_modes
  the_wheel_crossfades_the_layers
  legato_joins_past_the_attack_without_a_click
  keyswitches_latch_the_articulation
  ensemble_layers_and_voices_are_bounded
  tone_runs_from_tasto_to_ponticello
  vibrato_blooms_after_its_delay
  humanise_is_seeded
  block_splits_render_the_same
  a_reset_plays_the_same_again
  the_callback_does_not_allocate
  the_tail_bound_covers_a_released_note)

# Stick plays its synthetic kits (stick_test_library.hpp).
oscillate_add_test_executable(oscillate_stick_tests SOURCES stick_tests.cpp
  LIBS oscillate::synth_engines)
oscillate_add_case_tests(native_stick_contract oscillate_stick_tests
  CASES
  a_missing_kit_is_silent_then_plays_once_installed
  kits_chosen_load_in_the_background
  malformed_kits_are_rejected
  a_missing_second_drum_plays_the_plain_stroke
  descriptor_registry_and_presets
  velocity_follows_the_recorded_power
  round_robins_are_seeded_and_never_repeat
  the_pedal_opens_the_hat_and_closing_chokes_it
  bleed_mutes_solos_and_mixes_route_the_mics
  pans_place_close_mics_and_balance_cymbals
  shared_close_mics_and_a_centred_room
  the_chain_off_changes_nothing
  inserts_shape_their_own_group
  the_parallel_and_kit_buses
  a_stage_switches_in_without_a_step
  tune_decay_and_brushes
  the_stream_joins_the_resident_start
  voices_are_bounded
  telemetry_lights_the_piece_and_its_mics
  humanise_is_seeded
  block_splits_render_the_same
  a_reset_plays_the_same_again
  the_callback_does_not_allocate
  the_tail_bound_covers_a_hit)

oscillate_add_test_executable(oscillate_wren_breath_tests SOURCES wren_breath_tests.cpp
  LIBS oscillate::synth_engines)
oscillate_add_case_tests(native_wren_breath_contract oscillate_wren_breath_tests
  CASES
  matches_the_prototype
  places_on_the_grid
  as_drawn_and_no_amount_keep_the_notes
  amount_scales_breath
  live_notes_breathe_without_their_length
  fast_repeats_alternate_t_and_k
  articulation_sets_how_notes_join
  graces_follow_the_project_key
  humanize_is_seeded_bounded_and_loops)

oscillate_add_test_executable(oscillate_fret_feel_tests SOURCES fret_feel_tests.cpp
  LIBS oscillate::synth_engines)
oscillate_add_case_tests(native_fret_feel_contract oscillate_fret_feel_tests
  CASES
  matches_the_prototype
  places_on_the_grid
  as_drawn_and_no_amount_keep_the_notes
  low_amount_blends_velocity_but_keeps_plucks
  live_notes_ghost_by_touch
  pocket_delays_only_off_beats
  humanize_is_seeded_bounded_and_loops
  pushes_are_marked)

# Crate (ADR-0063, spec 09): the chop sampler and the public band-limited
# reader it plays through.
oscillate_add_test_executable(oscillate_crate_tests SOURCES crate_tests.cpp
  LIBS oscillate::synth_engines oscillate::warp_dsp)
oscillate_add_case_tests(native_crate_contract oscillate_crate_tests
  CASES
  descriptor_registry_and_presets
  documents_validate_and_compile
  public_reader_is_the_warp_read
  identity_read_at_unity
  reverse_reads_backward
  modes_shape_the_note
  choke_groups_cut_each_other
  ahdsr_lands_on_time
  clean_pitch_up_does_not_alias
  eras_set_bits_and_rate
  follow_tempo_stretches_without_repitching
  follow_tempo_loop_stays_in_its_chop
  swing_delays_odd_sixteenths
  sustain_pedal_holds_a_gate
  any_block_split_renders_identically
  full_load_does_not_allocate
  silent_without_a_sample
  telemetry_reports_the_playheads
  presets_render_sane_audio)

# Shape's slope (ADR-0047): steep pass filters as Butterworth stages.
oscillate_add_test_executable(oscillate_shape_tests SOURCES shape_tests.cpp
  LIBS oscillate::native_effects)
oscillate_add_case_tests(native_shape_contract oscillate_shape_tests
  CASES
  band_stages_follow_the_slope
  slopes_cut_at_their_rate
  a_live_slope_reaches_its_depth
  slope_changes_never_click
  any_block_split_renders_identically
  display_spectra_read_input_and_output_apart
  spectra_of_eqs_started_together_land_in_different_blocks)

oscillate_add_test_executable(oscillate_effect_platform_tests SOURCES effect_platform_tests.cpp
  LIBS oscillate::native_effects)
oscillate_add_case_tests(native_effect_platform_contract oscillate_effect_platform_tests
  CASES
  registry_is_consistent
  block_splits_render_identically
  silence_stays_silent
  tails_end_within_their_bound
  a_seek_clears_history
  constant_lanes_equal_the_same_value
  bad_blocks_are_rejected_untouched
  offers_follow_revisions
  live_offers_are_heard_without_a_revision
  creation_rejects_bad_input
  the_callback_never_allocates
  SLOW
  tails_end_within_their_bound
  block_splits_render_identically)

oscillate_add_test_executable(oscillate_contour_tests SOURCES contour_tests.cpp
  LIBS oscillate::native_effects
  TEST native_contour_contract)

oscillate_add_test_executable(oscillate_forge_tests SOURCES forge_tests.cpp
  LIBS oscillate::native_effects)
oscillate_add_case_tests(native_forge_contract oscillate_forge_tests
  CASES
  aliases_stay_below_minus_90_dbfs
  auto_gain_matches_level
  colour_is_transparent_at_low_level
  curves_have_their_signature
  the_mix_is_phase_aligned
  live_model_switches_are_click_free
  telemetry_draws_the_curve
  presets_match_golden_fingerprints
  the_kernel_matches_the_reference_shaper
  SLOW
  aliases_stay_below_minus_90_dbfs
  auto_gain_matches_level)

oscillate_add_test_executable(oscillate_orbit_tests SOURCES orbit_tests.cpp
  LIBS oscillate::native_effects
  TEST native_orbit_contract)

oscillate_add_test_executable(oscillate_relay_tests SOURCES relay_tests.cpp
  LIBS oscillate::native_effects
  TEST native_relay_contract)

oscillate_add_test_executable(oscillate_expanse_tests SOURCES expanse_tests.cpp
  LIBS oscillate::native_effects)
oscillate_add_case_tests(native_expanse_contract oscillate_expanse_tests
  CASES
  bands_decay_at_their_rt60
  decay_follows_the_control_at_any_size
  echo_density_builds_fast
  channels_are_decorrelated
  the_tail_is_uncoloured
  freeze_holds_and_closes_the_input
  predelay_delays_the_wet
  a_change_of_space_spills_its_tail
  shimmer_adds_its_interval_and_stays_bounded
  presets_match_golden_fingerprints
  a_reset_plays_as_new
  SLOW
  decay_follows_the_control_at_any_size)

oscillate_add_test_executable(oscillate_vista_tests SOURCES vista_tests.cpp
  LIBS oscillate::native_effects
  TEST native_vista_contract)

oscillate_add_test_executable(oscillate_pump_tests SOURCES pump_tests.cpp
  LIBS oscillate::native_effects)
oscillate_add_case_tests(native_pump_contract oscillate_pump_tests
  CASES
  gate_edges_never_click
  the_duck_reaches_its_depth_on_the_beat
  nudge_moves_the_shape
  kicks_trigger_the_duck_on_time
  midi_notes_trigger_the_duck_ahead
  lows_only_spares_the_top
  documents_change_the_sound_live
  the_detector_hears_only_the_kick
  the_note_range_and_velocity_pick_the_kick
  kick_spacing_stretches_the_cycle
  bands_and_stereo_choose_what_pumps
  swing_patterns_and_morph_shape_the_bar
  block_splits_are_deterministic
  telemetry_feeds_the_panel)

# Chop (ADR-0075): a tempo-locked gate and stutter.
oscillate_add_test_executable(oscillate_chop_tests SOURCES chop_tests.cpp
  LIBS oscillate::native_effects)
oscillate_add_case_tests(native_chop_contract oscillate_chop_tests
  CASES
  descriptor_registry_and_presets
  init_is_transparent
  a_closed_step_is_silent
  edges_land_on_the_grid
  ratchets_make_their_hits
  repeats_replay_the_slice
  chance_follows_seed_bar_and_step
  edges_never_click
  swing_delays_the_even_steps
  rotate_shifts_the_pattern
  any_block_split_renders_the_same
  the_callback_never_allocates
  telemetry_feeds_the_panel)

oscillate_add_test_executable(oscillate_temper_tests SOURCES temper_tests.cpp
  LIBS oscillate::native_effects)
oscillate_add_case_tests(native_temper_contract oscillate_temper_tests
  CASES
  at_rest_it_is_the_input_delayed
  the_latency_is_exact
  a_resonance_is_cut_and_the_rest_is_left
  max_cut_bounds_the_cut
  depth_and_sensitivity_steer_the_cut
  hard_cuts_harder_than_soft
  split_spares_the_attacks
  unmask_carves_where_the_key_plays
  dynamic_bands_follow_their_threshold
  stereo_modes_leave_the_rest_untouched
  delta_plus_output_is_the_input
  mix_blends_toward_the_input
  telemetry_feeds_the_panel)

# Strata (ADR-0082): multiband dynamics.
oscillate_add_test_executable(oscillate_strata_tests SOURCES strata_tests.cpp
  LIBS oscillate::native_effects)
oscillate_add_case_tests(native_strata_contract oscillate_strata_tests
  CASES
  descriptor_registry_and_presets
  crossovers_sum_flat
  the_static_curve_matches_the_formula
  attack_and_release_are_time_constants
  range_limits_the_change
  mid_side_touch_only_their_part
  an_external_key_drives_the_band
  delta_is_the_input_less_the_output
  the_latency_is_reported_and_exact
  any_block_split_renders_the_same
  hostile_input_stays_finite
  ott_mode_is_its_bands
  the_callback_never_allocates
  telemetry_feeds_the_panel)

# Ceiling (ADR-0117): the mastering limiter. It checks the classic limiter
# as the oracle it improves on.
oscillate_add_test_executable(oscillate_ceiling_tests SOURCES ceiling_tests.cpp
  LIBS oscillate::native_effects)
oscillate_add_case_tests(native_ceiling_contract oscillate_ceiling_tests
  CASES
  descriptor_registry_and_presets
  true_peak_never_exceeds_the_ceiling
  true_peak_holds_while_the_ceiling_glides
  steady_tones_stay_clean
  distortion_beats_the_classic_limiter
  transients_keep_the_onsets
  the_clipper_does_not_alias
  stereo_link_shares_the_gain
  latency_is_reported_and_exact
  without_room_it_is_an_exact_delay
  opening_the_room_never_clicks
  any_block_split_renders_the_same
  export_equals_live
  a_seek_starts_clean
  hostile_input_stays_finite
  presets_stay_restrained
  the_callback_never_allocates
  telemetry_feeds_the_panel)

# Edge (ADR-0108): the clipper.
oscillate_add_test_executable(oscillate_edge_tests SOURCES edge_tests.cpp
  LIBS oscillate::native_effects)
oscillate_add_case_tests(native_edge_contract oscillate_edge_tests
  CASES
  descriptor_registry_and_presets
  shape_zero_is_a_hard_clip
  no_aliasing_at_16x
  true_peak_never_exceeds_0_dbtp
  true_peak_never_exceeds_a_lowered_ceiling
  true_peak_holds_while_the_ceiling_glides
  delta_nulls_with_the_output
  latency_is_reported_and_exact
  stereo_link_shares_the_gain
  compare_is_level_matched
  any_block_split_renders_the_same
  export_equals_live
  hostile_input_stays_finite
  presets_stay_restrained
  a_seek_starts_clean
  the_callback_never_allocates
  telemetry_feeds_the_panel)

# Hush (ADR-0116): the gate and expander.
oscillate_add_test_executable(oscillate_hush_tests SOURCES hush_tests.cpp
  LIBS oscillate::native_effects)
oscillate_add_case_tests(native_hush_contract oscillate_hush_tests
  CASES
  descriptor_registry_and_presets
  closed_is_exactly_the_range
  open_is_unity
  no_chatter_at_the_threshold
  attack_hold_and_release_are_timed
  the_expander_follows_its_ratio
  lookahead_opens_before_the_transient
  the_key_filter_keeps_bleed_out
  an_external_key_drives_it
  listen_plays_the_filtered_key
  stereo_link_shares_the_gain
  flip_ducks
  any_block_split_renders_the_same
  export_equals_live
  hostile_input_stays_finite
  presets_stay_restrained
  a_seek_starts_clean
  the_callback_never_allocates
  telemetry_feeds_the_panel)

# Strike (ADR-0118): the transient shaper.
oscillate_add_test_executable(oscillate_strike_tests SOURCES strike_tests.cpp
  LIBS oscillate::native_effects)
oscillate_add_case_tests(native_strike_contract oscillate_strike_tests
  CASES
  descriptor_registry_and_presets
  zero_settings_pass_untouched
  bands_sum_flat
  attack_gain_matches_its_setting_at_any_level
  sustain_gain_matches_its_setting_at_any_level
  a_steady_tone_is_left_alone
  attack_time_sets_the_length_of_the_lift
  sustain_time_sets_how_much_decay_counts
  band_settings_stay_in_their_band
  solo_plays_only_its_band
  stereo_link_shares_the_gain
  mix_blends_with_the_dry_bands
  clip_holds_the_ceiling
  latency_is_reported_and_exact
  linear_phase_sums_to_the_input_delayed
  lookahead_lifts_the_first_cycle
  any_block_split_renders_the_same
  export_equals_live
  hostile_input_stays_finite
  the_tail_is_bounded
  presets_stay_restrained
  a_seek_starts_clean
  the_callback_never_allocates
  telemetry_feeds_the_panel)

# Keel (ADR-0119): adaptive tonal balance.
oscillate_add_test_executable(oscillate_keel_tests SOURCES keel_tests.cpp
  LIBS oscillate::native_effects)
oscillate_add_case_tests(native_keel_contract oscillate_keel_tests
  CASES
  descriptor_registry_and_presets
  amount_zero_is_bit_exact
  tilted_pink_converges_to_the_target
  speed_sets_the_time_to_follow
  a_steady_programme_is_not_modulated
  a_moving_correction_has_no_zipper
  minimum_phase_delays_little
  a_curve_target_is_reached
  a_reference_target_is_reached
  genre_targets_are_reached
  learn_captures_the_balance
  regions_weight_the_correction
  limits_hold
  absent_bands_are_not_boosted
  silence_and_transients_do_not_drag_it
  level_match_holds_the_loudness
  delta_plays_the_change
  output_trims
  other_rates_converge_alike
  any_block_split_renders_the_same
  export_equals_live
  hostile_input_stays_finite
  low_rates_leave_the_top_bands_out
  a_tiny_rate_still_runs
  the_tail_is_bounded
  presets_stay_restrained
  a_seek_starts_clean
  the_callback_never_allocates
  telemetry_feeds_the_panel)

# Gild (ADR-0122): the multiband exciter.
oscillate_add_test_executable(oscillate_gild_tests SOURCES gild_tests.cpp
  LIBS oscillate::native_effects)
oscillate_add_case_tests(native_gild_contract oscillate_gild_tests
  CASES
  descriptor_registry_and_presets
  zero_drive_passes_untouched
  bands_sum_flat
  linear_phase_sums_to_the_input_delayed
  harmonics_match_tape
  harmonics_match_tube
  harmonics_match_transistor
  harmonics_match_diode
  even_raises_the_even_harmonics
  aliasing_under_100_dbfs_at_16x
  level_match_holds_the_level
  dynamic_drives_quiet_passages_harder
  band_drive_stays_in_its_band
  solo_plays_only_its_band
  mix_blends_with_the_dry_band
  delta_plays_what_was_added
  latency_is_reported_and_exact
  any_block_split_renders_the_same
  export_equals_live
  hostile_input_stays_finite
  the_tail_is_bounded
  presets_stay_restrained
  a_seek_starts_clean
  a_band_that_rejoins_starts_clean
  the_callback_never_allocates
  telemetry_feeds_the_panel)

# Cast (ADR-0115): the convolution reverb.
oscillate_add_test_executable(oscillate_cast_tests SOURCES cast_tests.cpp
  LIBS oscillate::native_effects)
oscillate_add_case_tests(native_cast_contract oscillate_cast_tests
  CASES
  descriptor_registry_and_presets
  mono_ir_matches_the_direct_sum
  stereo_ir_matches_the_direct_sum
  true_stereo_ir_matches_the_direct_sum
  latency_is_zero
  true_stereo_keeps_each_input_apart
  any_block_split_renders_the_same
  export_equals_live
  bundled_irs_decay_as_modelled
  stretch_lengthens_and_decay_shortens
  start_length_and_reverse_shape_the_ir
  normalise_levels_the_irs
  predelay_delays_the_wet
  tone_and_width_shape_the_wet
  ducking_follows_the_key
  the_mix_is_equal_power
  a_new_ir_crossfades_in
  the_tail_is_reported
  live_ir_edits_recompile
  a_resampled_file_plays_at_the_session_rate
  hostile_input_stays_finite
  presets_stay_restrained
  the_callback_never_allocates
  telemetry_feeds_the_panel)

# Dust (ADR-0069): background texture and age.
oscillate_add_test_executable(oscillate_dust_tests SOURCES dust_tests.cpp
  LIBS oscillate::native_effects)
oscillate_add_case_tests(native_dust_contract oscillate_dust_tests
  CASES
  descriptor_registry_and_presets
  every_layer_off_is_the_input
  each_layer_sits_at_its_level
  each_layer_has_its_character
  pops_follow_the_crackle_rate
  scratches_come_round_with_the_record
  radio_gate_follows_the_tempo
  seeds_are_deterministic_and_distinct
  any_block_split_renders_identically
  reset_is_deterministic
  duck_lowers_the_bed_not_the_input
  age_folds_lows_to_mono
  wow_and_flutter_bend_a_sine
  every_preset_is_finite_and_bounded
  the_callback_never_allocates
  telemetry_feeds_the_panel
  preset_fingerprints)

oscillate_add_test_executable(oscillate_flyby_tests SOURCES flyby_tests.cpp
  LIBS oscillate::native_effects)
oscillate_add_case_tests(native_flyby_contract oscillate_flyby_tests
  CASES
  at_rest_in_front_is_the_input
  the_latency_is_exact
  doppler_follows_the_physics
  the_ears_hear_the_head
  the_level_follows_distance
  the_air_darkens_with_distance
  the_room_grows_with_distance
  a_loop_wraps_without_a_click
  once_passes_on_each_note
  any_block_split_renders_the_same
  telemetry_feeds_the_panel)

oscillate_add_test_executable(oscillate_pocket_tests SOURCES pocket_tests.cpp
  LIBS oscillate::native_effects oscillate::synth_engines)
oscillate_add_case_tests(native_pocket_contract oscillate_pocket_tests
  CASES
  bypass_is_bit_exact
  the_laws_hold_on_steady_tones
  the_cut_lands_on_time_with_lookahead
  focus_bounds_the_cut
  release_and_follow_shape_the_recovery
  lookahead_is_an_exact_latency
  the_mono_sub_spares_the_mid
  steady_cuts_add_no_distortion
  toggles_never_click
  telemetry_feeds_the_panel
  real_kicks_come_out_of_the_bass
  a_fast_pattern_stays_steady
  presets_match_golden_fingerprints
  SLOW
  real_kicks_come_out_of_the_bass)

oscillate_add_test_executable(oscillate_lens_tests SOURCES lens_tests.cpp
  LIBS oscillate::native_effects)
oscillate_add_case_tests(native_lens_contract oscillate_lens_tests
  CASES
  audio_passes_untouched
  sines_read_their_level
  pink_noise_is_flat_and_tilt_is_exact
  close_low_tones_resolve
  highs_follow_faster_than_lows
  peak_hold_holds_and_decays
  views_and_overlay_read_true
  resonances_are_found
  infinite_average_and_freeze
  telemetry_ignores_block_size
  tonal_meter_reads_input_and_sidechain)

# Tonal targets and match EQ (ADR-0120).
oscillate_add_test_executable(oscillate_tonal_targets_tests SOURCES tonal_targets_tests.cpp
  LIBS oscillate::native_dsp oscillate::dsp_primitives)
oscillate_add_case_tests(native_tonal_targets_contract oscillate_tonal_targets_tests
  CASES
  grid_tiles_without_gaps
  k_weighting_is_bs1770
  normalised_curves_ignore_gain
  band_meter_reads_sines_and_noise
  band_meter_allocates_nothing
  targets_rebuild_deterministically
  target_files_are_validated
  factory_targets_are_well_formed
  placement_against_a_range)

oscillate_add_test_executable(oscillate_match_eq_tests SOURCES match_eq_tests.cpp
  LIBS oscillate::native_effects oscillate::native_dsp)
oscillate_add_case_tests(native_match_eq_contract oscillate_match_eq_tests
  CASES
  own_eq_is_recovered
  fit_uses_the_fewest_bands
  fit_reports_what_shape_plays
  correction_follows_its_settings
  targets_become_fine_levels)

oscillate_add_test_executable(oscillate_loudness_meter_tests SOURCES loudness_meter_tests.cpp
  LIBS oscillate::native_effects oscillate::native_dsp
  TEST native_loudness_meter_contract)

oscillate_add_test_executable(oscillate_scope_tests SOURCES scope_tests.cpp
  LIBS oscillate::native_effects oscillate::synth_engines
  TEST native_scope_contract)

oscillate_add_test_executable(oscillate_tune_tests SOURCES tune_tests.cpp
  LIBS oscillate::native_effects oscillate::synth_engines
  TEST native_tune_contract)

oscillate_add_test_executable(oscillate_trim_tests SOURCES trim_tests.cpp
  LIBS oscillate::native_effects
  TEST native_trim_contract)

# Where Ceiling's and Press's gain reduction comes from (reduction_bands.hpp).
oscillate_add_test_executable(oscillate_reduction_bands_tests SOURCES reduction_bands_tests.cpp
  LIBS oscillate::native_effects)
oscillate_add_case_tests(native_reduction_bands_contract oscillate_reduction_bands_tests
  CASES
  ceiling_puts_a_low_sine_in_the_low_band
  ceiling_puts_a_high_sine_in_the_high_band
  press_puts_a_low_sine_in_the_low_band
  press_puts_a_high_sine_in_the_high_band
  press_follows_its_key
  press_weighs_its_detector_high_pass
  nothing_reduced_shows_nothing
  the_callback_never_allocates)

oscillate_add_test_executable(oscillate_deck_tests SOURCES deck_tests.cpp
  LIBS oscillate::native_effects
  TEST native_deck_contract)

oscillate_add_test_executable(oscillate_signal_tests SOURCES signal_tests.cpp
  LIBS oscillate::native_effects
  TEST native_signal_contract)

oscillate_add_test_executable(oscillate_dsp_primitives_tests SOURCES dsp_primitives_tests.cpp
  LIBS oscillate::dsp_primitives
  TEST native_dsp_primitives_contract)

oscillate_add_test_executable(oscillate_meter_parity_tests SOURCES meter_parity_tests.cpp
  LIBS oscillate::native_effects oscillate::native_dsp
  TEST native_meter_parity_contract LABELS parity)

# A built-in effect as an insert stage (ADR-0043): its live monitoring.
oscillate_add_test_executable(oscillate_native_effect_stage_tests SOURCES native_effect_stage_tests.cpp
  LIBS oscillate::native_effects oscillate::native_dsp oscillate::native_inserts oscillate::project
  TEST native_effect_stage_contract)

oscillate_add_test_executable(oscillate_nova_render_tests SOURCES nova_render_tests.cpp
  LIBS oscillate::synth_engines oscillate::native_inserts)
oscillate_add_case_tests(native_nova_render_contract oscillate_nova_render_tests
  CASES
  analog_pad
  glass_pad
  dark_strings
  spectral_drift
  choir_pad
  warm_pad
  sweep_pad
  warehouse_poly_pad
  whispered_ahh
  voices_on_helpers)

oscillate_add_test_executable(oscillate_nova_engine_tests SOURCES nova_engine_tests.cpp
  LIBS oscillate::synth_engines oscillate::native_inserts
  TEST native_nova_engine_contract)

oscillate_add_test_executable(oscillate_nova_analogue_tests SOURCES nova_analogue_tests.cpp
  LIBS oscillate::synth_engines oscillate::native_inserts)
oscillate_add_case_tests(native_nova_analogue_contract oscillate_nova_analogue_tests
  CASES
  off_ignores_every_analogue_setting
  age_zero_without_drift_is_exact
  seeds_repeat_and_differ
  any_block_split_renders_the_same
  a_voice_is_tuned_by_its_seeded_offset
  a_painted_value_overrides_the_seed
  drift_stays_within_its_amount
  lanes_validate_strictly
  the_appended_unison_curves_are_deterministic
  voice_mod_a_reaches_each_voice
  analogue_presets_play_well_under_full_scale
  new_controls_at_their_defaults_are_exact
  the_seventh_note_reuses_the_first_card
  reroll_moves_each_note_and_repeats
  flutter_jitters_within_its_maximum
  noise_follows_an_oscillator
  cards_reroll_flutter_and_noise_am_render_alike_in_any_blocks
  synced_portamento_is_its_note_value
  each_card_glides_at_its_own_speed)

oscillate_add_test_executable(oscillate_nova_tables_tests SOURCES nova_tables_tests.cpp
  LIBS oscillate::synth_engines oscillate::dsp_primitives
  TEST native_nova_tables_contract)

oscillate_add_test_executable(oscillate_nova_filter_tests SOURCES nova_filter_tests.cpp
  LIBS oscillate::synth_engines
  TEST native_nova_filter_contract)

oscillate_add_test_executable(oscillate_nova_fx_tests SOURCES nova_fx_tests.cpp
  LIBS oscillate::synth_engines)
oscillate_add_case_tests(native_nova_fx_contract oscillate_nova_fx_tests
  CASES
  ids_and_defaults_follow_the_contract
  every_module_turns_silence_to_silence_and_noise_to_finite_audio
  parameter_extremes_stay_bounded
  layouts_share_unchanged_instances_and_retire_the_rest
  adoptions_with_reorders_or_replacements_do_not_click
  the_rack_renders_identically_however_blocks_split
  the_split_crossover_sums_flat_with_main_empty
  a_synced_delay_lands_on_the_beat
  reverb_rt60_grows_with_decay
  the_compressor_reduces_gain_above_threshold
  rack_telemetry_and_meters_are_finite
  the_eq_is_transparent_when_flat
  eq_bands_follow_their_types
  an_eq_type_change_does_not_click
  the_shifter_moves_every_partial_by_the_same_hertz
  the_rack_reports_each_slot_level_and_spectrum)

oscillate_add_test_executable(oscillate_nova_arp_tests SOURCES nova_arp_tests.cpp
  LIBS oscillate::synth_engines)
oscillate_add_case_tests(native_nova_arp_contract oscillate_nova_arp_tests
  CASES
  compiles_document_clips
  modes_play_in_order
  steps_lock_to_the_beat_grid
  swing_delays_every_second_step
  gate_lengths_follow_gate_times_step_gate
  ties_hold_into_the_next_step
  octaves_extend_the_pattern
  latch_holds_until_a_new_chord
  block_splits_do_not_change_notes
  seek_reproduces_the_song_position
  every_on_has_one_off
  sequencer_loops_with_the_transport
  note_trigger_quantises_to_a_sixteenth
  follow_transposes_by_the_held_key
  sequencer_feeds_the_arpeggiator
  passes_through_when_off
  telemetry_reports_steps
  new_modes_play_in_order
  lane_lengths_make_a_polymeter
  ratchets_split_a_step
  conditions_follow_the_lane_cycle
  chance_is_seeded
  euclid_places_its_hits
  passing_notes_fill_the_scale
  note_lane_moves_by_held_notes_or_degrees
  chord_lane_stacks_notes
  groove_moves_steps_and_velocity
  accent_and_mod_ride_on_notes
  variation_bars_play_the_fill
  processing_does_not_allocate)
