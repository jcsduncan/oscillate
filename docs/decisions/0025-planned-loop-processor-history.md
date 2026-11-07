# ADR-0025 — Planned loop processor history

**Status:** Accepted; implemented: `BlockContext` carries separate transport and processing generations, and routed playback plans loop wraps through `PrefetchControl::plan_loop_wrap`. **Date:** 17 October 2026.
**Requirements:** ER-04b, ER-10, ER-11.

## Context

The source prefetch ring needs a new generation at every traversal of a loop, including a planned wrap. Using that same generation to reset all processor state drops sampler voices, effect tails and delay-compensation samples at the boundary. The listener hears an artificial break and parallel routes can lose alignment.

## Decision

`BlockContext` carries a transport generation for source and prefetch identity and a processing generation for stateful DSP. Existing two-field contexts treat the transport generation as the processing generation. The control side requests a planned loop wrap only after the next source iteration is ready. The callback changes the project sample and transport generation together, retains the processing generation, and marks the first wrapped block. Every subsequent block uses the new transport generation and the retained processing generation. An ordinary seek changes both; device restart continues to use its explicit quiescent recovery procedure.

Delay-compensation lines keep their ring samples at a marked wrap and rebase their expected project position to the new interval. Native effects, sampler voices and routed tail drains use the processing generation. The meter retains its partial window but publishes the new transport generation and position. Source rings and graph-context validation still use the transport generation. No callback allocation, lock, project read or worker wait is introduced.

The wrap request is one callback-boundary operation. The current headless M2 prefetch fixture exercises it after next-iteration priming. The Quick arrangement does not yet schedule this operation, and device recovery after a wrap still needs a coherent processing-generation handoff before it can claim held-voice continuity. A graph transition that needs new source-history replay close to the beginning of a loop can remain pending if its current project position cannot describe the retained pre-wrap history; it must not activate an unprimed delay line. These are explicit integration limits, not permission to reset DSP history silently.

## Evidence

Focused contracts cover the two generations, delayed impulse and parallel-path alignment across a wrap, reverb and EQ state, a partial meter window, and ordinary-seek reset. The M2 prefetch/device fixture verifies that the wrap request follows successful priming. Full local and sanitizer results are recorded in the implementation history after integration.
