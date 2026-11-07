# Pinned AppImage runtime: static dependency notices

**Status:** 15 October 2026 engineering notice bundle for the local AppImage
prototype. This is not source-version attestation, LGPL fulfilment or
distribution approval.

The exact pinned x86-64 [type-2 runtime 20251108 release](https://github.com/AppImage/type2-runtime/releases/tag/20251108)
has SHA-256 `2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d`.
Its embedded version string points to source commit
`dd6cebedcbddde9c82f89b011e8e1d40b6e43868`. That commit's
[Makefile](https://github.com/AppImage/type2-runtime/blob/20251108/src/runtime/Makefile)
uses `-static -lsquashfuse -lsquashfuse_ll -lzstd -lz -lfuse3 -lmimalloc`.
The binary also contains mimalloc diagnostic strings. The upstream
[runtime licence](https://github.com/AppImage/type2-runtime/blob/20251108/LICENSE)
lists musl, libfuse, squashfuse, zstd and zlib, **but omits mimalloc**. The
local notice inventory includes mimalloc explicitly. These observations do
not prove that the link pulled every object from every named archive, or that
the named set is exhaustive for squashfuse's optional codecs.

## Staged notices and provenance

The AppDir and its extracted AppImage now contain each complete file below
under `usr/share/doc/oscillate/third-party/runtime-static/`. Staging and
extraction verify the checked-in SHA-256 values. For libfuse and squashfuse,
the runtime build script downloads exact, hashed source archives. For the
Alpine-provided components, the notice files come from plausible upstream
source versions; the **actual binary package versions are unproven** because
the release workflow builds from `alpine:3.21` and runs `apk add` without an
image digest, APK lockfile or package-version manifest. A current Alpine v3.21
package page is not evidence of the package selected in November 2025.

| Static component | Complete notice files staged, with SHA-256 | Source evidence |
| --- | --- | --- |
| musl | `musl` `f9bc4423732350eb0b3f7ed7e91d530298476f8fec0c6c427a1c04ade22655af` | [musl v1.2.5 `COPYRIGHT`](https://git.musl-libc.org/cgit/musl/tree/COPYRIGHT?h=v1.2.5), including file-level exceptions. Alpine 3.21 uses the 1.2.5 family; exact package revision in the pinned runtime is unknown. |
| libfuse | `libfuse-license` `b8832d9caaa075bbbd2aef24efa09f8b7ab66a832812d88c602da0c7b4397fad`; `libfuse-lgpl2` `dc626520dcd53a22f727af3ee42c770e56c97a64fe3adb063799d8ab032fe551` | Runtime script pins [fuse-3.15.0 source archive](https://github.com/libfuse/libfuse/releases/download/fuse-3.15.0/fuse-3.15.0.tar.xz) SHA-256 `70589cfd5e1cff7ccd6ac91c86c01be340b227285c5e200baa284e401eea2ca0`. Its `LICENSE` assigns `include/` and `lib/` to LGPL-2.1; `LGPL2.txt` supplies the full terms. The runtime build applies a local `libfuse/mount.c.diff` patch. |
| squashfuse | `squashfuse` `9e909cc8a8ba27b1a649c964cf9eda37de911f3138c2b64e6d2f01703904ac13` | Runtime script pins [squashfuse 0.5.2 archive](https://github.com/vasi/squashfuse/archive/0.5.2.tar.gz) SHA-256 `db0238c5981dabbd80ee09ae15387f390091668ca060a7bc38047912491443d3`; its `LICENSE` is a two-clause BSD grant and acknowledgement. |
| zstd | `zstd` `7055266497633c9025b777c78eb7235af13922117480ed5c674677adc381c9d8` | [zstd v1.5.6 `LICENSE`](https://github.com/facebook/zstd/blob/v1.5.6/LICENSE) contains the BSD option. Alpine v3.21 currently reports zstd 1.5.6, but the runtime's exact APK revision is not attested. |
| zlib | `zlib` `845efc77857d485d91fb3e0b884aaa929368c717ae8186b66fe1ed2495753243` | [zlib v1.3.1 `LICENSE`](https://github.com/madler/zlib/blob/v1.3.1/LICENSE). Alpine v3.21 has since advanced to 1.3.2; the runtime's exact version is not attested. |
| mimalloc | `mimalloc` `19c99805e7a44a34b297a75d1edea9985e300066dfc024d5c99d4236d4573b5d` | [mimalloc v2.1.7 `LICENSE`](https://github.com/microsoft/mimalloc/blob/v2.1.7/LICENSE). Alpine v3.21's `mimalloc2-dev` provides `mimalloc-dev` at 2.1.7; the runtime's exact APK build is not attested. |

The two exact archive hashes above are checked by the pinned runtime's
[install-dependencies script](https://github.com/AppImage/type2-runtime/blob/20251108/scripts/common/install-dependencies.sh).
The [Dockerfile](https://github.com/AppImage/type2-runtime/blob/20251108/scripts/docker/Dockerfile)
installs musl, zstd, zlib and mimalloc from floating Alpine repositories.
The upstream top-level licence has links for those components; it is not a
substitute for their full applicable notice files.

## Closure work for a distribution candidate

1. Obtain upstream build logs, package-lock metadata or a reproducible build
   from the same `alpine:3.21` image digest and APK index, then bind the exact
   static archives, patches, compiled source and notice versions to the
   released runtime bytes. If that proof is unavailable, build and pin a
   controlled runtime with a complete source and binary manifest.
2. Audit squashfuse's configure result and the final link map for any
   additional static codec libraries or source-file exceptions. Keep the
   notice inventory fail-closed when the runtime binary changes.
3. Resolve libfuse LGPL-2.1 static-link fulfilment with a product-controlled
   corresponding-source and relink/replacement route. Unpacking an AppImage
   and swapping a dynamic Qt library does not demonstrate replacement of
   libfuse code already linked into the runtime executable. Record an actual
   modified-libfuse runtime build and execution test, plus source/offer and
   installation information as applicable.
4. Review the product code and starter-sound grants, LAME `fft.c`, all other
   copied libraries, and clean Ubuntu X11/Wayland/audio behaviour separately.

The local package notice tests check omission and byte tampering; the AppImage
builder checks the notices in the **extracted image** before publication. They
show notice presence and integrity, not that every licence condition has been
met.
