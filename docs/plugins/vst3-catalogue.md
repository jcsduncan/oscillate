# VST3 discovery catalogue

**Scope:** PI-01 discovery service. It is headless, control/background-side work;
the audio callback and Qt views never traverse directories or scan plug-ins.

`Vst3Catalogue` owns a bounded, deterministic snapshot of bundles and their
`ScanResult` receipts. Its production constructor delegates to the existing
out-of-process `Vst3Scanner`; tests can inject scanner actions. A refresh
handles explicitly supplied paths first, then the standard Linux roots in
[Steinberg's documented order](https://steinbergmedia.github.io/vst3_dev_portal/pages/Technical%2BDocumentation/Locations%2BFormat/Plugin%2BLocations.html):
`$HOME/.vst3`, `/usr/lib64/vst3`, `/usr/lib/vst3`,
`/usr/local/lib64/vst3`, `/usr/local/lib/vst3`. Pass an application-local
`vst3/` directory as an explicit path when packaging provides one. Explicit
paths can be search directories or individual `.vst3` bundles. A missing
explicit bundle remains visible with an invalid-bundle scan status.
An unresolvable explicit path is retained in `snapshot.issues` with its
requested path and a diagnostic; a malformed path does not silently disappear.

Search paths and bundles are canonicalized, so symlink aliases are scanned
once. Bundles within a root have canonical-path order; the first search path
that finds a bundle determines its catalogue position. Limits bound searched
paths, visited directory entries and returned bundles. `truncated` means the
catalogue may be incomplete because a limit or traversal error was hit; callers
must show that condition and any discovery issues, and may offer a narrower rescan.

Each entry retains the scanner's exact status, metadata and verified binary
identity, including canonical bundle/binary paths, byte size and SHA-256. The
catalogue does not turn a failed scan into a loadable plugin. Repeated failures
and quarantine follow `Vst3Scanner`'s canonical-bundle policy. A missing or
invalid binary remains visible as `invalid_bundle` without consuming a
quarantine attempt; symlink aliases of a valid bundle share one failure count.
The scanner protects its failure index when control and worker callers share
an instance. An explicit Retry invalidates failure accounting from scans that
were already in flight. `retry(bundle)` is an
explicit action for an already discovered canonical bundle: it clears that
scanner quarantine and rescans the single bundle once. Refreshing without a
retry preserves quarantine state for the scanner's lifetime.

This service does not persist compatibility records, choose between distinct
binaries advertising the same class ID, load plug-ins, render audio or provide
the eventual searchable browser UI. An application worker must run refresh
away from the UI and callback threads before binding any saved plugin.
The [verified insertion service](vst3-verified-insert.md) consumes a selected
receipt and rescans it before committing a project command.
