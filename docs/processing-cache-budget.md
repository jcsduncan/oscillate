# Processing-cache budget primitive

`CacheStore` owns only disposable, project-local derived files. `usage()` reports
recognized regular entries and interrupted temporary files; `clear()` removes
recognized names without following symlinks. Source assets, manifests, named
revisions, external links, and user renders are outside its pinned cache
directory. A valid read refreshes access time; failure to do that is reported
through `CacheUsage::recency_degraded` without making the cached payload invalid.

`enforce_global_cache_budget()` takes a caller-owned, stable-ordered list of
pinned stores for reachable projects. It holds their control-side locks, counts
the total bytes, removes interrupted temporary files first, then removes the
least recently accessed completed entries until the total fits the supplied
byte budget. Equal timestamps use the caller's project order, then entry name.
It rejects null stores and duplicate directory identities. The 256-store and
262,144-entry limits bound one enforcement pass. A failure reports an error;
files already evicted are still disposable, and the caller may recount usage.
Each `CacheStore` now also takes an exclusive Linux `flock` on a separate open
description of its pinned cache directory for the duration of each read,
write, scan, clear or budget pass. The global pass takes directory locks in
device/inode order before scanning, while its caller order still resolves LRU
ties. This coordinates separate `CacheStore` objects and cooperating processes
without changing cache entry names or including project state and assets.
Calls block only control/worker threads, never the audio callback. Direct
filesystem writes that bypass `CacheStore` cannot participate in this lock.

`application::CacheControl` now owns a per-user `cache-registry.json` under
`~/.oscillate` (or a supplied test profile). Project open/create registers a
validated committed project in stable order. The registry stores path and
directory identities, writes through a flushed temporary file and atomic
rename, and holds a profile lock. A moved, replaced or unreachable project is
reported by `health()` and omitted from budget enforcement and clear operations.
Reopening the moved project rebinds the same registry entry. A missing or
recreated cache directory is omitted until that project is reopened and
registered again; this also applies if a migration replaces `cache/` while
retaining the project root. Reopening without replacing `cache/` retains the
existing registry identity. A corrupt registry is reported rather than
overwritten.

The service reports per-project status/usage, global reachable usage and budget;
supports a persistent budget change, enforcement, one-project clear, clear
of reachable caches, and `forget_project`, which drops one registry entry
(reachable or not) without touching the project or its cache. It opens each registered project through `ProjectStore`
and uses pinned cache-directory descriptors. The Quick bridge registers opened
projects and reports registration failure in its status line. There is no
processing-panel UI for health, budget or clear yet, and the service does not
run automatic enforcement after writes. Separate live `CacheStore` instances
are now serialized with service operations through their pinned directory lock;
the profile lock excludes another registry controller in the same profile.
All calls remain off the audio callback.
Global health and `clear_reachable()` visit projects one at a time; concurrent
producers can change the total between projects. A clear count reports entries
observed before each individual clear, not an atomic transaction across all
registered projects.
