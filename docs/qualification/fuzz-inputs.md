# PM-08 malformed-input fuzz qualification

The opt-in targets feed arbitrary bytes to the production strict manifest decoder and to
`media::WavReader::open_fd`. WAV bytes go into an anonymous Linux memfd, so exercising
the fd parser creates no named media file and cannot write outside the build tree.
On an accepted WAV, the harness also reads the first and final frames. Inputs are
limited to 1 MiB by the harness; the example campaign limits mutations to 64 KiB.
The checked-in corpus contains a current valid manifest, duplicate keys, malformed
JSON, excessive nesting, invalid UTF-8, valid PCM and float WAVs, truncated RIFF,
and duplicate data chunks. The current manifest seed can be regenerated with the
`oscillate_fuzz_manifest_seed` target when the schema changes.

## Local campaign

Use a Clang installation with matching compiler-rt/libFuzzer and the repository's
usual local CMake dependency paths. Keep both the build and all scratch files under
the checkout. For a normal system Clang installation:

```sh
cmake -S . -B build/fuzz -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON -DOSCILLATE_BUILD_FUZZ=ON -DCMAKE_CXX_COMPILER=clang++
mkdir -p build/fuzz/scratch/manifest-corpus build/fuzz/scratch/wav-corpus
cp -a tests/fuzz/corpus/manifest/. build/fuzz/scratch/manifest-corpus/
cp -a tests/fuzz/corpus/wav/. build/fuzz/scratch/wav-corpus/
TMPDIR="$PWD/build/fuzz/scratch" cmake --build build/fuzz \
  --target oscillate_fuzz_manifest oscillate_fuzz_wav oscillate_fuzz_corpus_replay
TMPDIR="$PWD/build/fuzz/scratch" ctest --test-dir build/fuzz \
  -R '^fuzz_(manifest|wav)_corpus$' --output-on-failure
TMPDIR="$PWD/build/fuzz/scratch" build/fuzz/tests/fuzz/oscillate_fuzz_manifest \
  -max_total_time=30 -max_len=65536 -timeout=2 -rss_limit_mb=1024 \
  -artifact_prefix="$PWD/build/fuzz/scratch/" build/fuzz/scratch/manifest-corpus
TMPDIR="$PWD/build/fuzz/scratch" build/fuzz/tests/fuzz/oscillate_fuzz_wav \
  -max_total_time=30 -max_len=65536 -timeout=2 -rss_limit_mb=1024 \
  -artifact_prefix="$PWD/build/fuzz/scratch/" build/fuzz/scratch/wav-corpus
```

`oscillate_fuzz_corpus_replay` and its two CTest cases also build with GCC, for
machines without libFuzzer. That gate replays fixed inputs; it is not a substitute
for coverage-guided mutation. An ASan/UBSan report, timeout or memory-limit failure
is a failing fuzz campaign. Keep crash inputs in the build scratch area until
triaged, then add a small reduced regression seed and a focused production fix.

## Initial local evidence, 18 October 2026

Clang 21 with matching compiler-rt was staged only under the isolated worktree's
`build/fuzz/scratch`. The parser libraries were compiled with libFuzzer coverage,
ASan and UBSan. The 15-second per-target smoke campaigns completed:

| Target | Executions | Final edge coverage | Sanitizer failures |
| --- | ---: | ---: | ---: |
| Manifest | 48,721 | 9,790 | 0 |
| WAV | 559,410 | 696 | 0 |

This is a short bounded smoke result, not a claim that all malformed inputs are
safe. The two GCC corpus replay tests passed. The campaign's mutation corpus and
logs were kept only under `build/`; checked-in seeds remain small.
