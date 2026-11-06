# Standard MIDI File reading (ADR-0045): untrusted input, bounded and strict.
oscillate_add_library(midi_file SOURCES smf.cpp PUBLIC oscillate::foundation)
