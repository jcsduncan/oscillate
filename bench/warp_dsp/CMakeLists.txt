oscillate_add_executable(oscillate_warp_dsp_bench SOURCES warp_dsp_bench.cpp
  LIBS oscillate::warp_dsp)

oscillate_add_executable(oscillate_warp_dsp_rate_view_bench SOURCES rate_view_bench.cpp
  LIBS oscillate::warp_dsp)
