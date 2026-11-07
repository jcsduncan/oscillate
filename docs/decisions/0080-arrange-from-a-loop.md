# ADR-0080 — Arrange from a loop: a whole track from 8 or 16 bars, kept or binned in one step

**Status:** Accepted; implemented. **Date:** 31 October 2026.

**Date:** 31 October 2026.

**Builds on:**
- ADR-0051 (arrangement automation);
- ADR-0059 (one-step edits, the command palette);
- ADR-0074 (Mix Check's one-step apply);
- ADR-0075 (Chop).

**Design:** the Arrange tab of [ui/creative-tools-mockup.html](../ui/creative-tools-mockup.html#arrange).

**Contract:**
- `include/oscillate/application/arrange_from_loop.hpp`: roles, templates, `plan`, `rules`, `move_edge` and `loop_content`.
- `ProjectSession::apply_arrangement`, in `src/application/project_session_arrange.cpp`.
- The Qt `ArrangeController`, as `ProjectBridge.arrange`.
- `ArrangeLoopView.qml` and `qml/arrange/`.

## Context

Minimal techno and house are built from hypnotic loops that become six- to seven-minute, DJ-friendly tracks. Going from a good 16-bar loop to that arrangement is slow, repetitive work, and the rules are well known:
- 32 bars of beats at each end for mixing;
- changes on 8, 16 and 32-bar lines;
- something new every 16 bars;
- a build, a break and a drop;
- filter sweeps, kick-out bars, throws and fills.

The workflow: pick a loop and a template, see the roles found, see the proposal on a timeline with its automation and rule checks, then lock tracks, drag section edges, regenerate, and keep or bin it.

Two open questions were settled on 31 October 2026:
- **Lock** means "leave this track exactly as it is", and a separate role override corrects role detection.
- **Arrange may add sounds** behind an "Add transitions" switch, which is on by default.

## Decision

1. **A deterministic rule engine first.** `arrange::plan` is a pure function of the snapshot and the options. Every choice it makes by chance comes from a SplitMix64 seeded by `options.seed`; it never uses `<random>`'s distributions, which differ between standard libraries. The same loop, template and seed therefore always give the same plan, and Regenerate is the next seed.

   An LLM agent comes later on the same tools: the roles and the beat grid it reads (Ears), and the session commands it writes with (Hands). Its proposals arrive the same way, as one change to keep or bin.

2. **Roles.** Each audio and instrument track gets a role: Kick, Hats, Perc, Bass, Stab, Lead, Pad, Fx or Vocal. They are found in this order:
   1. the user's override;
   2. words in the track's name;
   3. the engine (Anvil is the kick; Knock and Crate are percussion);
   4. the loop's notes, by median key, polyphony and mean length (low single notes are bass; three or more held notes are a pad; two or more short ones are a stab; a single line is a lead);
   5. the names of its audio clips;
   6. otherwise Fx, with "set its role".

   Every role carries its evidence in plain words.

   **Tracks that are not arranged:**
   - locked tracks;
   - tracks with nothing in the loop;
   - frozen tracks;
   - tracks whose only audio crosses the loop's edges.

   None of these is named in any operation. A track is locked when it is in `options.locked`.

3. **The loop's content.** `loop_content` reads the loop's notes with each MIDI clip's pattern repeats and cuts, relative to the loop's start. Placements in samples are read through the tempo map. Audio clips lying wholly inside the loop are copied; clips crossing its edges are counted and left out, and the role card says so.

   A MIDI clip's own automation and pedals are not carried into the arrangement. An audio clip's automation goes with each copy, under fresh ids.

4. **Templates and sections.**

   | Template | Target length | Build | Break | Drop | Kick-out |
   |---|---|---|---|---|---|
   | DJ Tool | 6:30 | 16 | 16 | 16 | 1 bar, before the groove |
   | Hypnotic | 7:00 | 32 | 32 | 16 | 1 bar, before the groove |
   | Peak-time build | 6:00 | 16 | 16 | 32 | 2 bars, before the groove and before Groove 2 |

   - **Length.** The target is turned into bars at the tempo of bar 1 and rounded to 8 bars. The length can be changed in steps of 8 bars, from the template's minimum up to 512.
   - **Sections:** Intro, Build, Groove, Break, Drop, Groove 2, Outro.
   - **Intro and outro:** 16, 32 or 64 bars each.
   - **Groove and Groove 2** share what is left, split by "Breakdown at" and rounded to 8 bars. Each is at least 16 bars.
   - **Dragging an edge** (`move_edge`) snaps it to 8 bars, keeps at least 8 bars on each side, and plans again over that layout.

5. **Where each role plays.** A fixed table per section kind sets where each role plays.
   - **Intro:** the kick plays throughout; hats come in after a phrase, and perc at the halfway point. No music.
   - **Build:** the beat and the bass; stab, lead, vocal and fx come in halfway.
   - **Groove:** everything, with the lead coming in halfway once the groove runs 32 bars.
   - **Break:** no kick, bass or perc. Peak-time brings the hats back for its last 8 bars.
   - **Drop:** everything.
   - **Groove 2:** lead, pad and vocal leave halfway.
   - **Outro:** kick and hats; perc leaves halfway.

   Kick-out bars end the build (and, in Peak-time, the drop). The break has no kick to take out.

6. **The moves.**
   - **Sweeps:**
     - The music playing through the build opens a 24 dB low-pass from 250 Hz to 18 kHz.
     - The music playing through the break's last 4 bars rises through a 24 dB high-pass from 40 Hz to 900 Hz, into the drop.
     - Each kind of sweep is its own Contour, added at the end of the track's chain. The Contour's mix is automated to 0 except during its sweeps, so it is out of the way the rest of the time.
   - **Throws:**
     - Where: the last half bar before the break and before the drop.
     - What: each music part except the bass, through its first audible send to a return track, at gain 1.
     - When not: if the send is already at gain 1 or more, or already has a lane.
   - **Fills:**
     - One bar of Chop, on its own Chop added with the "Glitch fill" preset, its mix automated to 1 for that bar only. An existing Chop keeps its own job.
     - Where: the last bar of each groove and the drop. With more variation, also each 32-bar phrase, or each 16-bar phrase above 75 %.
     - Which part: the first of perc, hats, stab or bass that is playing there.
   - **Variation:** in the grooves and the drop, each 16-bar block may lose a part (never the kick or the bass) for its second 8 bars, at a rate of variation × 0.35.
   - **Transitions** (when on):
     - Risers: an 8-bar "Long Noise Riser" into the drop, and out of the build too for Peak-time or a 32-bar build.
     - A 2-bar "Noise Drop" downlifter into the break.
     - "Impact Boom" on the groove's and the drop's downbeats.
     - Each kind is one new Nova track named after it, with one note per use. Nova's factory presets already had these sounds, so none were added.
   - **Markers:** each section start gets a marker named after the section, unless a marker is already there.

7. **The rules, checked on every plan** (`arrange::rules`):
   - **Changes land on 8-bar lines.** Section edges and parts entering or leaving land on the grid; kick-out bars are the only exception.
   - **Something new every 16 bars.** For each 16-bar line, the 16 bars after it hold a part entering or leaving, a section, a sweep, a fill, a throw or a transition. Where one does not, the planner adds one: a part sitting out the block's second 8 bars, or else a fill on its last bar. This shows on the timeline as an amber dot, against teal for held. If it can add neither, the dot is red ("failed"), and the rule chip says the rule is broken.
   - **The intro and outro are beats only.**
   - **Locked tracks are left as they are.**

   A part that carries a fill, a throw or a sweep in a block is never the one dropped out.

8. **Apply as one undo step** (`ProjectSession::apply_arrangement`), the way Mix Check applies its fixes: one `ProjectModel::apply` with every operation, validated as a whole.
   - **The arrangement starts at bar 1.** Each arranged track's clips are removed. Each run of bars it plays gets either:
     - a looping MIDI clip of the loop's notes, rotated so the loop stays in phase with bar 1 (a note that wraps continues from the pattern's start); or
     - copies of its audio clips on each pass of the loop that falls wholly inside the run.
   - **New ids.** Ids continue from the session's next track, clip and marker numbers. Notes, lanes and points share a namespace derived from the revision, as clip duplication does.
   - **A stale plan is refused.** A plan records the revision it was made from, and a plan from an older revision is refused with "The project changed since this arrangement was planned".
   - **Locked tracks never appear in any operation,** so they are unchanged byte for byte.

9. **Hearing it before keeping it: apply, then undo.** A preview must not change the document until Keep, and the session has no scratch-graph playback. So the controller's Listen applies the proposal as that one undo step, and the transport plays the real project:
   - **Bin and Listen-off** undo exactly that step. They do this only while it is still the project's latest revision; after any other edit the proposal stays, and the user is told to undo it from the history.
   - **Keep** applies the proposal if it is not already applied, then leaves it.
   - **Changing an option while listening** withdraws the proposal, plans again and applies the new one.
   - **Leaving the view** (Escape, the palette, another view) bins a proposal that is only being heard.

   We considered rendering the proposal from a scratch snapshot instead. We rejected it: it would need a second live graph, and it would play something other than what Keep writes.

10. **Where it lives.** A full view, like Mix Check, reached three ways:
    - "Arrange from the loop" in the command palette;
    - "Arrange from the loop" in a MIDI clip's menu while the loop brace is on;
    - `ArrangeController::openRequested` from anywhere else.

    It takes the loop brace's start and its length rounded to 8 or 16 bars; with no loop it uses bars 1–16 and says so. The view, top to bottom:
    - loop, template and Listen;
    - length, intro/outro, breakdown, variation and transitions;
    - Regenerate with its seed, Bin and Keep;
    - role cards with lock and role choice;
    - the timeline: sections with draggable edges, each track's runs and fills, the five automation lanes, the 16-bar rule dots and the playhead while listening;
    - the rule chips;
    - every decision in words.

## Consequences

- An 8- or 16-bar sketch becomes a structured, mixable track in one step, and every choice in it can be explained and undone. Tracks the user cares about can be locked.
- Plans are cheap (no rendering), so every option change re-plans at once.
- **Roles are guessed** from names, engines and notes; the user corrects them per track. Audio tracks with no name clue fall back to Fx until the user sets a role.
- **Audio clips across the loop's edges are left out.** The user splits them at the loop first; the role card says so.
- **The arrangement replaces each arranged track's clips across the whole timeline.** Material outside the loop on those tracks goes, recoverable with one undo. Lock a track to keep it.
- **Tests** (`tests/application/arrange_from_loop_tests.cpp`):
  - `roles`: roles from names, engines and notes, plus the user's override;
  - `determinism`: the same seed gives the same plan for every template;
  - `rules`: every template, intro length, variation and seed keeps every rule, and so does a dragged layout;
  - `keep`: one undo step; a locked track identical byte for byte; undo restores every track and marker exactly; redo returns it; a stale plan is refused;
  - `transitions`: one Nova track per kind, only when asked for.
