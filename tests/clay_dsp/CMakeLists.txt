# Clay's DSP oracles (ADR-0081), one CTest case each.
oscillate_add_test_executable(oscillate_clay_dsp_tests SOURCES clay_tests.cpp
  LIBS oscillate::clay_dsp oscillate::warp_dsp
  INCLUDES ${PROJECT_SOURCE_DIR}/tests/native_dsp ${PROJECT_SOURCE_DIR}/src/clay_dsp
  OPTIMIZE)

set(OSCILLATE_CLAY_DSP_CASES
  melodic_finds_sung_notes
  polyphonic_finds_the_chord
  polyphonic_finds_a_stretched_chord
  polyphonic_finds_a_struck_stab
  unedited_render_is_the_original
  shifted_note_sounds_at_its_new_pitch
  shifted_chord_note_leaves_the_others
  formants_stay_unless_shifted
  stretched_note_keeps_its_pitch
  renders_are_block_split_invariant
  analysis_is_deterministic
  percussive_hits_become_notes
  split_and_join_keep_the_audio
  macros_correct_pitch_to_the_scale
  macros_quantize_level_and_vary
  macros_to_midi
  macros_harmonise_and_fit_to_key
  macros_follow_notes
)
foreach(case IN LISTS OSCILLATE_CLAY_DSP_CASES)
  oscillate_add_test(NAME clay_dsp.${case} COMMAND oscillate_clay_dsp_tests ${case})
endforeach()

# The same oracles on recorded audio: chords played on Concert (ADR-0071).
# Without the Salamander library each case passes, saying it was skipped.
oscillate_add_test_executable(oscillate_clay_piano_tests SOURCES clay_piano_tests.cpp
  LIBS oscillate::clay_dsp oscillate::synth_engines
  INCLUDES ${PROJECT_SOURCE_DIR}/tests/native_dsp
  DEFINITIONS OSCILLATE_TEST_SKIP_RETURN_CODE=${OSCILLATE_TEST_SKIP_RETURN_CODE}
  OPTIMIZE)

set(OSCILLATE_CLAY_PIANO_CASES
  am7_close_held
  am7_open_held
  am7_close_staccato
  am9_close_held
  am9_open_held
  am9_open_staccato
  c_close_held
  c_open_held
  c_close_staccato
  c_open_staccato
)
foreach(case IN LISTS OSCILLATE_CLAY_PIANO_CASES)
  oscillate_add_test(NAME clay_dsp.piano.${case} COMMAND oscillate_clay_piano_tests ${case}
    SKIP_RETURN_CODE ${OSCILLATE_TEST_SKIP_RETURN_CODE})
endforeach()
