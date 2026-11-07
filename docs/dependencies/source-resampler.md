# Worker source-rate converter dependency

`OSCILLATE_BUILD_SOURCE_RESAMPLER=ON` links managed-audio import, routed playback and offline master/stem rendering to the already inventoried [libsamplerate 0.2.2 BSD-2-Clause dependency](export-resampler.md). The build uses the shared `libsamplerate.so.0` runtime and the `SRC_SINC_MEDIUM_QUALITY` stereo converter. The option defaults off. Import retains the original encoded media; it does not create a converted source asset. Different-rate clips use the routed worker path in Qt Quick even when the project has no buses. Unsupported rate pairs and converter-disabled imports fail before the source is published.

Packaging must include the libsamplerate runtime dependency and BSD-2 notice when this option is enabled. The [distribution inventory](distribution-sbom-review.md) needs an enabled artifact audit before a release claim. The converter is prepared on a worker and never called from the audio callback.
