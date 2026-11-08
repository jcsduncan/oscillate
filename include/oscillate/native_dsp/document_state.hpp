#pragma once

namespace oscillate::native_dsp {

// A native engine's compiled structured state (routes, curves, chains,
// sequences, media): immutable, built on control, read by its kernel on the
// callback. Synths (ADR-0057) and effects (ADR-0058) derive their own.
class DocumentState {
public:
  virtual ~DocumentState() = default;
};

} // namespace oscillate::native_dsp
