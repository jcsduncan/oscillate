foreach(bench IN ITEMS loudness eq compressor limiter meter)
  oscillate_add_executable(oscillate_${bench}_bench SOURCES ${bench}_bench.cpp
    LIBS oscillate::native_dsp)
endforeach()

foreach(bench IN ITEMS prism ember pulse halo torque drift concert anvil knock crate nova)
  oscillate_add_executable(oscillate_${bench}_bench SOURCES ${bench}_bench.cpp
    LIBS oscillate::synth_engines)
endforeach()

foreach(bench IN ITEMS effects dust cast ceiling strike)
  oscillate_add_executable(oscillate_${bench}_bench SOURCES ${bench}_bench.cpp
    LIBS oscillate::native_effects INCLUDES ${PROJECT_SOURCE_DIR}/tests/native_dsp)
  # effect_test_support.hpp needs the shared test support headers.
  oscillate_link_test_support(oscillate_${bench}_bench)
endforeach()
