foreach(bench IN ITEMS loudness eq compressor limiter meter)
  oscillate_add_executable(oscillate_${bench}_bench SOURCES ${bench}_bench.cpp
    LIBS oscillate::native_dsp)
endforeach()

foreach(bench IN ITEMS prism ember pulse halo torque drift concert anvil knock crate nova)
  oscillate_add_executable(oscillate_${bench}_bench SOURCES ${bench}_bench.cpp
    LIBS oscillate::synth_engines)
endforeach()

# The offline transform against the realtime one, per size (ADR-0126).
oscillate_add_executable(oscillate_fft_bench SOURCES fft_bench.cpp
  LIBS oscillate::dsp_primitives oscillate::warp_dsp)

foreach(bench IN ITEMS effects dust cast ceiling strike gild keel master_chain)
  oscillate_add_executable(oscillate_${bench}_bench SOURCES ${bench}_bench.cpp
    LIBS oscillate::native_effects INCLUDES ${PROJECT_SOURCE_DIR}/tests/native_dsp)
  # effect_test_support.hpp needs the shared test support headers.
  oscillate_link_test_support(oscillate_${bench}_bench)
endforeach()
