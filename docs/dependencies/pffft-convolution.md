# PFFFT inventory

**Status:** Vendored at [third_party/pffft](../../third_party/pffft/README.md)
and linked statically by the warp DSP and the native effects (Cast's
partitioned convolution, [ADR-0115](../decisions/0115-cast-convolution-reverb.md)), in every build.

| Field | Value |
| --- | --- |
| Upstream | [marton78/pffft](https://github.com/marton78/pffft) |
| Pin | Annotated tag `v1.1.0`, peeled commit `1fdcb508e5082052ab406d560b7c4622b8b4b0dd` |
| Needed API | `PFFFT` single-precision real transforms and `pffft_zconvolve_accumulate`; `PFFASTCONV` is excluded |
| Licence | [BSD-like upstream terms](https://github.com/marton78/pffft/blob/v1.1.0/LICENSE.txt): preserve notices/disclaimer in source or binary materials, no endorsement |
| Linkage | Static C library target `oscillate_pffft` (`third_party/pffft/CMakeLists.txt`), private to `oscillate_warp_dsp` and `oscillate_native_effects` |
| External runtime | None beyond the platform C/math runtime |
| Local change | [`allocation-failure.patch`](../../third_party/pffft/patches/allocation-failure.patch), already applied to the vendored `src/pffft_priv_impl.h` |

Only the source files the project compiles are vendored. Upstream's
benchmark submodules (including KissFFT), tests, examples and install rules
are not included.

## Allocation-failure patch

GNU allocator fault injection found that PFFFT v1.1.0's `pffft_new_setup` does not check
the initial struct `malloc` or the later aligned-data allocation before
dereferencing them (`src/pffft_priv_impl.h` around line 1,079). The vendored
copy carries a two-check patch so that setup returns null instead; `malloc`
fault injection under ASan shows both setup paths return an error and leave
the next preparation usable. The patch SHA-256 is
`be11eea40046c2dfa69880f2906737c6dc94b55cb4dae757def0153a65a4dbb0`. The
changed header keeps its original notices; binary distribution must reproduce
the complete upstream `LICENSE.txt` and record this as a modified PFFFT
variant.

## Exploratory FFT-only timing

Host: Intel Core i7-10700K, 16 logical CPUs, x86-64. GCC 15.2.0, `-O3`,
single precision; upstream `v1.1.0` static library; no sanitizer or coverage.
An aligned buffer was filled with repeating finite samples. Each size was
warmed for 1,000 forward transforms; 20,000 transforms per run were timed with
`steady_clock`, five runs, median shown. Setup and aligned allocation were
outside timing. The SIMD build used upstream's default x86 path; this is not a
runtime-dispatched AVX qualification. Small results are subject to CPU clock
variation. The spike does not include multiply accumulation, inverse FFT or
the partition scheduler and must not be treated as convolution CPU evidence.

| Real FFT size | Scalar µs/forward transform | Upstream SIMD µs/forward transform |
| ---: | ---: | ---: |
| 128 | 0.259 | 0.117 |
| 256 | 0.235 | 0.199 |
| 512 | 0.473 | 0.410 |
| 2,048 | 2.025 | 1.798 |
