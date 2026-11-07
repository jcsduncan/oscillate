oscillate_add_library(foundation SOURCES id.cpp trace.cpp file_io.cpp hex.cpp log.cpp diagnostic.cpp
  crash_report.cpp)

# SHA-256 lives in its own target so foundation itself stays dependency-free.
oscillate_add_library(foundation_digest SOURCES sha256.cpp
  PUBLIC oscillate::foundation
  PRIVATE OpenSSL::Crypto)
