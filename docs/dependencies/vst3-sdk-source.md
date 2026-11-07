# VST3 SDK source inventory for the first host slice

**Status:** Source baseline verified locally; first scanner slice uses SDK interface headers only.
**Checked:** 14 October 2026. **Boundary:** FS-04 PI-01–05 and ADR-0015.

The host adapter will target Steinberg's `v3.8.1_build_84` release, commit
`3cdf9ca5d1f5b1b21e0a86832aa4abe55607bd96`. The release's root
`LICENSE.txt` is MIT. A shallow local checkout verified these gitlinks and
their separate `LICENSE.txt` files:

| SDK component | Pinned commit | Licence | Intended use |
| --- | --- | --- | --- |
| `base` | `fcf9da0bd27a16f7f03773a3a39822f28f5c8477` | MIT | Host support types, only if the selected SDK hosting code requires them |
| `pluginterfaces` | `4f547e8e102b47de4a8b8aaf343c73b700786372` | MIT | VST3 interfaces and IDs at the adapter boundary |
| `public.sdk` | `586dc5e6c8012c3e4b01c79389375cbe96bdb1da` | MIT | Linux module loader and narrowly selected hosting support |

The root release also pins `cmake`, `doc`, `tutorials`, and `vstgui4`, but the
scanner does not compile, copy, or ship them. In particular,
the SDK's top-level CMake project enables examples by default; the DAW should
not import its whole build graph. The adapter's CMake target should list the
exact SDK source files it compiles and link only their required system libraries.
The SDK checkout belongs in a local dependency cache, not the project
source tree or a user project. A reproducible package fetch still needs to be
selected for release builds.

The scanner is opt-in for ordinary developer builds while that fetch path is
open. Configuration checks the pinned release manifest, all 63
`pluginterfaces` headers and both MIT licence files by digest. A different or
locally modified interface set fails configuration. The full notices are
retained at `third_party/notices/vst3sdk/` and
`third_party/notices/vst3_pluginterfaces/`; a release package must include
them with its binaries. With a checkout at the pinned revision and its
`pluginterfaces` submodule, configure the dedicated CMake preset using
`cmake --preset vst3 -DOSCILLATE_VST3_SDK_ROOT=/path/to/vst3sdk`, then build
and test with the matching `vst3` presets. Release packaging must enable the
scanner and include its helper and MIT notices; a build that skips this option
cannot claim VST3 discovery.

### First scanner build inventory

The scanner helper compiles **no SDK `.cpp` files**. It includes
`pluginterfaces/base/ipluginbase.h` and the following transitive interface
headers: `fplatform.h`, `fstrdefs.h`, `ftypes.h`, `funknown.h`, and
`smartpointer.h`. These are all from the pinned `pluginterfaces` submodule;
the root and `pluginterfaces/LICENSE.txt` MIT notices must accompany a
distributed helper. Neither `base` nor `public.sdk` code is compiled into
this slice. The helper deliberately implements the narrow Linux bundle loader
and factory call sequence in its own process, so it can validate each field
before returning a bounded versioned response.

The control-side service lists `.vst3` bundle directories under caller-provided
search roots, with bundle-count and visited-entry budgets, launches the helper
without a shell, caps elapsed time and output bytes, and quarantines a path
after repeated failures until explicit retry. The read pipe is nonblocking on
the supervisor side while the helper's write end stays blocking; a fixture
returns 95,770 bytes to exercise responses beyond ordinary pipe capacity.
The versioned response rejects fields above 512 bytes and class lists above
256 entries; the helper reports invalid metadata instead of truncating them.
Discovery does not load a binary. The headless
[catalogue](../plugins/vst3-catalogue.md) selects the documented Linux roots
and explicit paths with canonical deduplication and bounded results. The app
must schedule catalogue refresh away from its UI and audio threads. Persistent
quarantine and compatibility records are future work. The helper itself is
**process-isolated, not sandboxed**: a malicious plug-in can still access the
scanner user's files and network. Packaging should use a restricted helper
environment when the threat model and Ubuntu distribution format are settled.

Only x86-64 Linux directory bundles with
`Contents/x86_64-linux/<bundle-stem>.so` are accepted. The helper calls
`ModuleEntry`, `GetPluginFactory`, and `ModuleExit`, and enumerates
`IPluginFactory`/`IPluginFactory2` class metadata. A VST3 v1 factory lacks
class version metadata, which is returned empty rather than invented. It does
not instantiate a plug-in, inspect buses, load state, run DSP, host an editor,
or implement the UI-thread run loop. These are separate PI-02–05 tasks. Passing
synthetic scan fixtures is not a claim of production plug-in compatibility.

Sources: [pinned SDK release](https://github.com/steinbergmedia/vst3sdk/tree/v3.8.1_build_84),
[root licence](https://github.com/steinbergmedia/vst3sdk/blob/v3.8.1_build_84/LICENSE.txt),
and [Steinberg's Linux module loading documentation](https://steinbergmedia.github.io/vst3_dev_portal/pages/Technical%2BDocumentation/VST%2BModule%2BArchitecture/Loading.html).
