# ADR-0065 — Nova's FX rack: a signal-flow FX page, a parametric EQ and a shifter

**Status:** Accepted; implemented. **Date:** 29 October 2026.
**Builds on:** ADR-0057 (Nova), spec 08 §7 and contract 08a §2.9, §2.10, §5 and §6.
**Panel design:** [ui/nova-fx-rack-mockup.html](../ui/nova-fx-rack-mockup.html).

## Context

Nova's FX page fell short of current flagship wavetable synths. The engine could already reorder modules and run three chains, but the page hid it:
- a 4 × 2 grid of cards that did not read as a chain;
- a small ⋮⋮ grip for reordering, and module swapping behind a click on the name;
- half of each module's controls behind MORE;
- visuals drawn from the settings, not from the sound.

The decision takes all of the design's proposals, plus a bigger EQ and a new effect.

## Decision

1. **The page** (NOVA-UI-11):
   - **Left:** the selected bus's chain as a signal-flow list. Drag a whole row to reorder, or drop it on another bus to move it (Ctrl copies it). Each row has its own in/out meter and a ⋯ menu.
   - **Right:** every module as a full-width strip with all of its controls and a live display.
   - **Presets:** factory and user presets per module type. User presets are JSON under the user preset folder, in `fx/<module>/`.
   - **Browser:** a categorised browser, with search, for adding and swapping modules.
2. **Sixteen values per slot.** `nova.fx.<u>.p0..p15` (was p0..p7), so a module can carry a real band layout. Nova's table grows from 918 to 1110 parameters. The platform limit for a native instrument (`max_engine_parameters`, `max_native_instrument_parameters`) rises from 1024 to 2048. Old modules keep their mappings: the mode stays on p7, and p8..p15 are unused.
3. **EQ: four parametric bands** in place of two fixed shelves and a dynamic bell.
   - **Controls per band:** a type, frequency, gain and Q. The types are bell, low and high shelf, 12 and 24 dB low and high cuts, notch, band pass, and off.
   - **Filters:** Simper SVF sections. A 24 dB cut is a Butterworth pair whose peaked section's Q scales with the band's Q.
   - **Type changes** crossfade over 10 ms.
   - **Dynamic EQ moves to Temper (ADR-0064)**, which does it far better.
4. **Shifter, the fifteenth module:** a frequency shifter.
   - **Method:** Niemitalo's allpass Hilbert pair (within 0.7° of quadrature from about 30 Hz at 48 kHz) and a quadrature oscillator.
   - **Shift:** a cubic law up to ±5 kHz, so small shifts are easy to set.
   - **Feedback:** soft-clipped, which gives barber-pole effects.
   - **Spread:** turns the right channel's shift the other way.
   - **Ring mode:** keeps both sidebands.
5. **Telemetry grows from 744 to 2016 values.** The panel draws from the sound, not from the settings:
   - each slot's input and output peaks;
   - Multiband's per-band gain change;
   - a 48-band spectrum of each placed slot's output. This is a 1024-point Hann FFT at 48 kHz, longer at higher rates so its resolution in hertz holds. It runs 30 times a second per slot, staggered across slots, and falls 36 dB/s.

## Consequences

- **Cost:** about 7 µs per spectrum. Twenty-four placed slots cost around 1% of a core, and a typical rack of four to six costs about 0.2%.
- **Saved projects:** a Nova project saved before this keeps p0..p7. p8..p15 load at 0, and an old EQ slot reads its values as band types. Nothing is migrated (greenfield).
- **The Quick panel:** the document model gains `move_chain_module_to` and `insert_chain_module`, and the controller gains the FX preset store.
