# ADR-0124 — The master assistant: a proposed master chain from a reference and a loudness target

**Status:** Accepted and implemented. **Date:** 10 November 2026.
**Builds on:** ADR-0049 (live graph switching), ADR-0074 (Mix Check), ADR-0117 (Ceiling), ADR-0118 (Strike), ADR-0119 (Keel), ADR-0120 (tonal targets and match EQ), ADR-0122 (Gild), ADR-0123 (master rebalance).
**Contract:** `include/oscillate/native_dsp/master_assistant.hpp` (measurement, rules, offline chain, gain learning; target `oscillate_master_assistant`), `include/oscillate/application/master_assistant.hpp` (the project: measure, propose, operations, accept, verify, the A/B player), `EffectService::apply_master_operations`. No schema change.

## Context

The project has every device a master needs (Shape, Keel, Press, Strata, Gild, Strike, Vista, Trim, Ceiling) and a rebalance by role, but choosing and setting them is the user's job. Mix Check reports problems one at a time; nothing plans a whole master toward a reference and a loudness target.

## Decision

1. **The assistant suggests and never commits on its own.** The user chooses a reference (a genre target from ADR-0120, or reference tracks), a loudness target (Streaming −14 LUFS under −1 dBTP, Club −9 and Loud −7 under −0.3 dBTP, or a custom loudness under −1 dBTP), a character (Clean, Punchy or Warm) and the whole song or the loop. Analyse measures and proposes on a worker. The project changes only on Accept.
2. **What is measured** is the mix as it enters the master chain: the pinned snapshot is rendered offline (`render_master_taps`) with the master chain left out and the master level at unity, the rebalance kept. The programme is kept in an unnamed file (`O_TMPFILE`) in the project's cache directory, up to 30 minutes; a longer song needs the loop. One pass (`native_dsp::assistant::Analyser`) reads:
   - integrated loudness (BS.1770 gating on 400 ms blocks), the loudest momentary and short-term loudness, and the 4x true peak;
   - the mid's tonal curve on ADR-0120's 29-band grid (the same band meter) and, per band, the side over the mid;
   - per region (under 150 Hz, to 2.5 kHz, above): the crest factor (each 100 ms step's peak over its RMS, averaged over the steps within 20 dB of the loudest 5%), onsets per second (20 ms hops in the lows, 5 ms above; a 6 dB rise over the last ten hops, within 20 dB of the loudest), side over mid, and the loud level (the 90th percentile of the gated steps);
   - low-end mono-ness: side over mid under 120 Hz;
   - tonal spread: the standard deviation, over 4 s segments within 10 LU of the loudest, of the lows-to-mids and highs-to-mids balance;
   - the Rebalance roles' share of each region, from the tracks' outputs grouped by the role whose stage acts on them.
3. **The rules** (`derive`, pure and deterministic) build the chain in this order, each device with one plain line naming its measurement:
   - **Rebalance nudges** when a region sits 2 dB or more from the reference and one role makes 60% or more of it: that role's gain moves by the distance times its share, at most 2 dB, in 0.5 dB steps. With nudges the mix is rendered again with them before the chain is planned.
   - **Trim** first, when the mix is already past the target (its gain down), and for a wideband width toward reference tracks whose mids and highs sit 2 dB or more from the mix's on average (70–130%). A genre target says nothing about width.
   - **Corrective EQ** from 1 dB RMS of tonal distance (40 Hz–16 kHz, both curves centred): Lens's match fitted into Shape bands (80%, a third of an octave, at most +4 and −6 dB), or Keel toward the reference's curve (70%, ±4 dB, 8 s) when the tonal spread is 2 dB or more.
   - **Dynamics** are measured, not guessed. The chain is first learned with no compressor; the loudness the limiter then costs (its gain less the loudness it adds) is compared with the character's share (Clean 3, Punchy 4, Warm 3.5 LU). From 1 LU over it, Press (and Strata, linear phase, when the regions' crests differ by 4 dB) are each derived for the excess (at most 4 dB, with makeup) and learned to the target, and the variant on target with the most crest at the output wins; a compressor must keep 0.25 dB more crest than the limiter alone (Warm, which wants its glue, may keep that much less).
   - **Colour:** none for Clean. Warm adds Gild's Warm Mids. Gild's Air Lift is added when the top above 6 kHz sits 3 dB or more under the reference. For Punchy, Strike (linear phase, +2 dB on the lows' attack) is a candidate when the lows hit once a second or more and the limiter costs 3 LU, kept only if it keeps more crest.
   - **Vista** only for mono bass, when the side under 120 Hz sits less than 12 dB under the mid (Mix Check's mono-lows line). Its band split turns the mid's phase, which costs the kick's onset contrast (below), so it is not used for width.
   - **Ceiling** last, at the ceiling less the master fader's gain, its Target at the loudness, and the character as Transients, Clip, Shape, Release, Character and Transient Link (Clean is the Transparent preset's limiter; Punchy full transients, 0.5 dB clip and a hard knee; Warm 1 dB clip and a softer knee; 0.5 dB more clip above −8 LUFS).
4. **Learn on the audio it will play.** `learn_gain` runs the chain offline over the programme, each device's latency taken out: the devices before the limiter once into a store, then the limiter on by secant steps on its Gain (0–24 dB), or below 0 dB on Trim's gain, until the output is within 0.1 LU of the target (at most eight passes). When the gain tops out short, Ceiling's clip room grows by the shortfall (to 6 dB, up to three times). The chain learned is the project's planned master: the proposal's operations applied to a copy of the snapshot, so devices the user keeps on the master are part of it.
5. **Operations.** Each accepted device sets the first device of its engine on the master (every parameter, switched on), else is added before the first device the proposal places after it. Nudges move the roles' gains in the rebalance, switching it on (from neutral when it was off). `EffectService::apply_master_operations` applies them as one undo step and refuses anything but the master's chain and the rebalance. Accept is refused when the mix changed since the measurement (Mix Check's `same_mix`). Reject drops the proposal; the project never changed.
6. **The A/B** plays the loudest 30 s of the programme through the current master (the programme itself when the master is empty at unity, else a render up to the window) and through the proposal (the last learning pass), looped on the preview device. The louder of the two is turned down to the quieter's integrated loudness, so louder never wins. Switching sides crossfades over 20 ms at the same position; the callback allocates nothing and takes no lock.
7. **Verify.** After Accept the master is rendered as the project now plays it, over the same range, and reported against the goals: loudness within ±0.5 LU, the true peak at or under the ceiling, the tonal distance and the crest factor, each against the mix as measured.
## Measured

`oscillate_limiter_audit --assistant` plans each programme toward the All References target: the synthetic drum loop and 30 s of each of the eight mastered tracks, normalised to −20 LUFS, and each again through a low shelf up 4 dB at 120 Hz and a high shelf down 5 dB at 5 kHz (a muddy, dull mix): 18 programmes. The naive chain is Ceiling alone with no room (Transients and Clip at 0), learned to the same loudness under the same ceiling. Crest loss is the change in the 16x short-term crest factor; onset loss the change in onset contrast below 150 Hz, to 2.5 kHz and above (ADR-0117's measures; positive is a loss).

| Goal | Within ±0.5 LU, under the ceiling | Worst error | Tonal distance | Crest loss (naive) | Onset loss low/mid/high (naive) |
|---|---|---|---|---|---|
| Streaming, Clean | 18 of 18 | 0.09 LU | 4.54 → 3.13 dB | −0.52 (0.00) dB | 0.40/−0.13/−0.09 (−0.01/−0.02/−0.01) dB |
| Club, Punchy | 18 of 18 | 0.09 LU | 4.54 → 3.11 dB | 0.14 (0.32) dB | −0.08/−0.22/−0.10 (−0.63/−0.13/−0.17) dB |

- **Loudness and peaks:** every master lands within 0.1 LU of its target, and no true peak passes the ceiling (the worst sits 0.01 dB under it). Learning took one to four passes.
- **Tonal balance:** the distance from the target falls on every programme, by 1.4 dB RMS on average.
- **Transients:** at Club the proposed chains lose less crest than the naive limiter (0.14 against 0.32 dB). The exception is one track whose lows carry side 12 dB or less under the mid: Vista's mono bass turns the mid's phase at its crossovers, and the kick's onset contrast below 150 Hz falls by 6.1–7.3 dB on it at both targets. That track alone makes the mean low-band onset loss larger than the naive chain's; the line under Vista says what it does, and it can be left out.
- **Determinism:** the same programme and goal give the same proposal, value for value.

## Consequences

- **Tests:**
  - `native_master_assistant_contract`: loudness against the project's analyser, a sine's true peak and crest, width, mono bass and onset rate on built signals, the tonal distance and the loudest window; each rule from its measurement; nudges only with a clear owner; the offline chain lined up sample for sample; each loudness preset reached under its ceiling with the balance moved toward the reference; the master fader allowed for; the same input planning the same proposal word for word; level matching.
  - `application_master_assistant`: the measurement leaves the master chain out; proposing changes nothing; Accept is one undo step that keeps an existing Trim in place and adds the rest in order, and one undo takes it back; accepting part of it edits an existing Ceiling rather than doubling it; a changed mix is refused; the rendered master meets the goals within 0.25 LU of the prediction (the song also carries the chain's tail); nudges render again and land in the rebalance; the A/B is level-matched, crossfades and allocates nothing.
  - `application_routed_live_edit` (`assistant_proposal_is_accepted_without_a_gap`): a proposal with a lookahead limiter accepted onto an empty master while playing becomes active without a restart or a silent block, with the app's node input history (which primes a fresh chain, ADR-0049).
- **Cost:** planning runs the chain over the programme several times on a worker: one to four passes of the limiter, more when compressors are weighed. A 30 s programme plans in seconds; a whole song takes about as long per pass as Ceiling takes to process it.
- **Vista's mono bass** keeps its phase cost; a mono bass with no crossover phase on the mid would need Vista's DSP to change.
