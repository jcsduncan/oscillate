# ADR-0015 — VST3 SDK baseline for host work

**Status:** Accepted for the SDK selection; pinned source inventory recorded, compiled-file inventory and host compatibility remain open
**Date:** 14 October 2026  
**Requirements:** PRD §§5, 16, 23; FS-04 PI-01–05; G8 dependency gate.

## Context

The first plugin-host slice needs an exact SDK baseline. Steinberg changed VST3 SDK licensing at version 3.8. Older developer-portal pages still describe the former GPL/proprietary choice, so the licence must be checked on the selected release rather than inferred from those pages.

## Decision

Target Steinberg VST3 SDK tag [`v3.8.1_build_84`](https://github.com/steinbergmedia/vst3sdk/tree/v3.8.1_build_84), Git commit `3cdf9ca5d1f5b1b21e0a86832aa4abe55607bd96`. Its pinned [`LICENSE.txt`](https://github.com/steinbergmedia/vst3sdk/blob/v3.8.1_build_84/LICENSE.txt) is MIT. Steinberg's [current licensing page](https://steinbergmedia.github.io/vst3_dev_portal/pages/VST%2B3%2BLicensing/Index.html) says the SDK has used MIT since 3.8 and identifies VSTGUI and mda files as separate BSD-like cases. No older SDK release is covered by this decision.

Use the SDK only at the VST3 host adapter boundary. Keep VST3 types out of the generic processor API and project model. Avoid importing VSTGUI, mda examples, tutorial code or the SDK's test binaries unless a specific host task needs them and their own notices have been inventoried. Record the exact submodule commits and notices of files actually compiled or shipped in the component-level SBOM. Preserve the MIT copyright and permission notice in distributions. Use of Steinberg's VST name or logo is optional under the SDK licence; any branding used must follow its trademark rules.

The host still needs the PI-01 scanner and PI-03 editor/run-loop compatibility fixtures before it can claim VST3 support. This decision does not establish plugin compatibility, approve a binary package, or determine the project's own source licence.

## Consequences

Plugin work can build against one reproducible SDK API and avoid an unnecessary GPL/proprietary licence assumption. The integration owner reviews the pinned SDK source and submodule inventory before adding it to CMake, then validates Linux host ABI, custom editor paths, state restore and the generic-editor fallback on the target Ubuntu release. Upgrading the SDK requires a new API and licence inventory review plus regression tests for the compatibility fixture set.

The checked source pins and intended first-host boundary are recorded in the
[SDK source inventory](../dependencies/vst3-sdk-source.md).
