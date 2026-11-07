# Functional and component specifications

**Status:** Drafts for review. These refine the [PRD](../PRD.md) and use the boundaries in the [HLD](../HLD.md). The PRD wins if wording conflicts; resolve the conflict in both files before implementation.

| Spec | Primary owners | PRD scope |
| --- | --- | --- |
| [Timing and arrangement](01-timing-arrangement.md) | timeline, project, arrangement UI | §§9–11, 13 |
| [Engine and routing](02-engine-routing.md) | engine, audio I/O, native DSP | §§7, 15, 22 |
| [Project and media integrity](03-project-media.md) | project, persistence, media | §§8, 18, 21 |
| [Plugins and instruments](04-plugins-instruments.md) | plugins, native DSP, UI | §§13, 15–16 |
| [Processing and analysis](05-processing-analysis.md) | media, processing scheduler | §§10–12 |
| [Automation, mix, and export](06-automation-mix-export.md) | timeline, engine, export, UI | §§14–17 |
| [UI, diagnostics, and release](07-ui-quality.md) | UI, app, QA | §§6, 19–23 |
| [Nova flagship synth](08-nova-synth.md) | native DSP, project, persistence, UI | §13 |
| [Crate chop sampler](09-crate-sampler.md) | native DSP, project, persistence, application, UI | §6 |
| [Nova panel ↔ engine contract](08a-nova-panel-contract.md) | native DSP, UI | §13 |

Each requirement ID is stable for tests and traceability. A component PR records the IDs it implements, its failure cases, and the tests that prove them. An acceptance test may be automated, measured on reference hardware, or performed in a structured usability session; the type is stated in each spec. Unresolved numbers or library choices are named gates, never implicit defaults.
