oscillate_add_executable(oscillate_delay_compensation_bench SOURCES delay_compensation_bench.cpp
  LIBS oscillate::engine)

oscillate_add_executable(oscillate_routed_mixer_bench SOURCES routed_mixer_bench.cpp
  LIBS oscillate::engine oscillate::native_dsp)

oscillate_add_executable(oscillate_routed_sidechain_bench SOURCES routed_sidechain_bench.cpp
  LIBS oscillate::engine oscillate::native_dsp)
