oscillate_add_test_executable(oscillate_m2_slice_tests SOURCES m2_slice_tests.cpp
  LIBS oscillate::application oscillate::audio_io oscillate::persistence
  TEST m2_managed_audio_slice)

oscillate_add_test_executable(oscillate_m2_prefetch_device_tests SOURCES m2_prefetch_device_tests.cpp
  LIBS oscillate::application oscillate::audio_io
  TEST m2_prefetch_device)
