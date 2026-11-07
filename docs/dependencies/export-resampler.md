# Export resampler dependency inventory

| Component | Local qualification | Licence | Linkage and package action |
| --- | --- | --- | --- |
| [libsamplerate](https://github.com/libsndfile/libsamplerate) | Ubuntu `libsamplerate0`/`libsamplerate0-dev` 0.2.2-4build2, upstream 0.2.2; `SRC_SINC_BEST_QUALITY`, stereo float streaming API | [BSD-2-Clause](https://github.com/libsndfile/libsamplerate/blob/master/COPYING) | Optional `OSCILLATE_BUILD_EXPORT_RESAMPLER=ON` adapter; link shared `libsamplerate.so.0`. Package must depend on a compatible Ubuntu runtime or include the shared library with its licence/copyright notice. |

`ldd` on the locally qualified shared library resolves `libm.so.6`, `libc.so.6` and the ELF loader. These are system runtime dependencies for the native Ubuntu package. If an AppImage bundles them or substitutes another build of libsamplerate, regenerate its shipped-file inventory, licence notices and compatibility tests from the actual artifact. No libsamplerate code is copied into this repository. The option defaults off until release packaging selects and qualifies the runtime; rate requests other than the project rate fail explicitly in that build. See [output-rate behavior and quality evidence](../export/output-sample-rate.md).
