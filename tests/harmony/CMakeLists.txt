# Harmony oracles (ADR-0083), one CTest case each.
oscillate_add_test_executable(oscillate_harmony_tests SOURCES harmony_tests.cpp
  LIBS oscillate::harmony
  INCLUDES ${PROJECT_SOURCE_DIR}/tests/native_dsp
  OPTIMIZE)

set(OSCILLATE_HARMONY_CASES
  intervals_match_the_table
  names_and_roman_numerals
  tension_orders_by_pull
  voicing_moves_least_and_stays_in_range
  drop2_is_close_with_second_from_top_dropped
  drone_sits_on_top
  voicing_is_deterministic_and_callback_safe
  every_voicing_is_clean
  lab_loop_in_aeolian_is_the_minor_four
  lab_modes_follow_brightness
  lab_restlessness_shapes_the_dominant
  lab_devices_do_what_they_say
  lab_is_deterministic_and_variants_differ
  next_after_the_dominant_is_home
  substitutions_and_hints_make_valid_lanes
  tile_and_replace_cut_at_the_edges
  open_voicing_sits_like_the_records
  the_added_shapes_write_their_degrees
  lab_colour_stays_in_the_mode
  natural_colour_is_mostly_plain
)
foreach(case IN LISTS OSCILLATE_HARMONY_CASES)
  oscillate_add_test(NAME harmony.${case} COMMAND oscillate_harmony_tests ${case})
endforeach()

# Chords as notes: recognise, rewrite and write (ADR-0083), one case each.
oscillate_add_test_executable(oscillate_harmony_clip_tests SOURCES clip_tests.cpp
  LIBS oscillate::harmony
  INCLUDES ${PROJECT_SOURCE_DIR}/tests/native_dsp
  OPTIMIZE)

set(OSCILLATE_HARMONY_CLIP_CASES
  identify_reads_every_voiced_chord
  recognise_reads_written_lab_lanes
  recognise_ignores_passing_notes_and_inherits
  rewrite_changes_only_the_edited_region
  rewrite_keeps_notes_outside_the_lane
  rewrite_split_cuts_held_notes
  rewrite_revoices_a_region_in_its_rhythm
  rewrite_writes_into_silence
  write_holds_each_chord_with_one_bass
  recognise_scales_to_full_clips
  layers_follow_the_chords
  held_chords_anticipate_and_land
  the_bass_switches_off_and_on
  the_bass_off_keeps_the_voicing_open
  written_chords_sit_like_the_records
  the_drone_rings_at_one_pitch
)
foreach(case IN LISTS OSCILLATE_HARMONY_CLIP_CASES)
  oscillate_add_test(NAME harmony_clip.${case} COMMAND oscillate_harmony_clip_tests ${case})
endforeach()
