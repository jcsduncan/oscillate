# PFFFT

Vendored from [marton78/pffft](https://github.com/marton78/pffft) at tag
v1.1.0, commit `1fdcb508e5082052ab406d560b7c4622b8b4b0dd`. Only the files the
project compiles are kept: `src/pffft.c`, `src/pffft_common.c`,
`src/pffft_priv_impl.h`, the single-precision headers under `src/simd/` and
`include/pffft/pffft.h`.

`patches/allocation-failure.patch` is applied to `src/pffft_priv_impl.h`. It
makes setup return null instead of dereferencing a failed allocation. To update,
copy the same files from a new release, re-apply the patch and rerun the
convolution and warp tests.

Licence: BSD-style, see `LICENSE.txt`.
