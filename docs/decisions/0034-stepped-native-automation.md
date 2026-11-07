# ADR-0034 — Stepped automation for discrete and next-note synth parameters

**Status:** Accepted. **Date:** 19 October 2026.
**Amends:** ADR-0027 (native-synth platform). It applies to every native engine (Prism, Ember, Lumen, Pulse, Halo, Torque) and to Drift.

## Context

ADR-0027 limited automation to continuous, live parameters. Three places enforced that:
- `validate_engine` rejected tables that marked discrete or next-note parameters automatable;
- the runtime rejected automation lanes on them;
- `native_parameter_index` refused them as automation targets.

So waveforms, filter types, algorithms, models, switches and sequencer steps could not be automated. The worry was clicks and unclear next-note semantics.

That is stricter than mainstream DAWs, and it blocks useful work:
- automating Freeze on and off;
- varying Torque's pattern during a song;
- switching a Halo algorithm per section.

The shared automation layers already support discrete domains. `AutomationDomain` has `discrete` and `step_count`, points have a `hold` shape, and the evaluator holds values for discrete curves. So this change needs no project-format change.

## Decision

Every native synth parameter is automatable, unless its engine explicitly opts out with `automatable = false`, which no shipped engine does.

- **Discrete parameters** (choice and stepped) get a *discrete* automation domain over their integer range, with `step_count = maximum − minimum`. Points hold until the next point, and values are integers, so a lane is a staircase. The evaluator and model validation enforce this.
- **Timing.** A lane's value is applied at each 32-frame control tick, exactly like a panel edit arriving then. Live playback and export therefore stay identical, and any block split renders the same audio.
- **Live discrete parameters** switch at the tick through each engine's existing panel-edit path. Any declick an engine has for panel switches applies to automation too.
- **Next-note parameters** follow the panel semantics: a sounding note keeps its value, the next note uses the automated one, and the deferral is reported in status. *Amended by [ADR-0038](0038-live-structural-switching.md): structural parameters now switch live, so only note-start settings (start phase, LFO mode, voice and chord mode) remain next-note.*
- **Continuous parameters** are unchanged: linear and curved shapes, evaluated per frame, sampled at ticks.

## Consequences

- **Code changes:**
  - `validate_engine` drops both rejections.
  - The runtime accepts lanes on discrete and next-note parameters, and requires discrete lane values to be integers.
  - `native_parameter_index` accepts them, and `native_automation_domain` emits discrete domains.
  - Automation preparation accepts discrete native domains, and gives the evaluator the domain's discrete flag.
  - All engine tables mark their discrete parameters automatable.
- **Tests:**
  - The platform tests assert the new rule.
  - Export parity gains a discrete-lane case: an automated Torque sequencer step note and the acid wave switch.
- **Panels.** They read `automatable` from the controller as before, now true everywhere.
- **VST3.** Automation of VST3 parameters is unchanged and still continuous-only; this decision covers native synth parameters only.

## Click-free switching (contract)

Now that switches can be automated, they must never click.

- **The contract.** `native_synth_switch_contract` covers every engine. It switches every live discrete parameter mid-chord through a stepped automation lane. The switch window's largest second difference must stay within 2× the larger of the steady sound before and after the switch.
- **What it found.** Two switches stepped, and they already stepped when changed from the panel:
  - Halo's operator waveforms, up to 23×.
  - Torque's distortion mode, up to 43×.
- **The fixes:**
  - `FmOperator` crossfades a wave change at the same phase over 128 oversampled frames.
  - `BassDistortion` keeps every mode's settings current and crossfades modes over 10 ms.
- **Cost and scope.** Both fixes live in the primitives, so panel edits benefit too. The first value after a reset is adopted without a fade. Halo's budget case went from 8.4 % to 8.8 % of a core, which is within measurement noise.
- **Rule for future engines.** A new engine passes the contract automatically or must fix its switches.
