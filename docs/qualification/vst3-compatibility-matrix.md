# VST3 compatibility matrix

The matrix runs Oscillate's VST3 host against a pinned set of free Linux
plug-ins. The [VST3 matrix workflow](../../.github/workflows/vst3-matrix.yml)
runs every Monday on `main`, on demand, and on pull requests that change the
matrix. Each run publishes `matrix.md` (also shown as the run summary) and
`matrix.json` as the `vst3-matrix` artifact.

## Plug-in set

[`vst3_matrix_plugins.json`](../../tests/qualification/vst3_matrix_plugins.json)
pins each plug-in by download URL and SHA-256: Surge XT (instrument and
effects), Dexed, OB-Xd, Odin 2, Cardinal (Synth and FX), LSP Plugins (four
named classes), Airwindows Consolidated and the CHOW Tape Model. Vital stays in
the manifest as unavailable: its Linux build downloads only through an
account. A bundle with many classes can name the ones to test; otherwise every
audio-module class is tested.

To bump a plug-in, change its URL, version and SHA-256 together
(`curl -sL URL | sha256sum`). A download whose digest differs from the pin is
reported as `download-failed`, never used. Airwindows Consolidated publishes
only a rolling build, so its pin needs a bump whenever the old asset is
replaced.

## What each column checks

Every class runs in its own process
(`tests/qualification/vst3_matrix_probe.cpp`), so a crash ends only that
class's row. The probe rewrites its evidence after each step, so a row shows
which step crashed.

| Column | Check |
| --- | --- |
| Scan | The out-of-process scanner the app uses, with its timeouts. |
| Load | Load and activate at 48 kHz with 128-frame maximum blocks; Notes give the reported latency, the latency re-check, the tail and the parameter count. |
| Render 64, Render 128 | Two seconds in 64- and 128-frame blocks: a repeating C major chord for an instrument, a 440 Hz sine for an effect. Fails on a process error or a non-finite sample; warns on silence or on allocation on the process thread. |
| State | Saves the component and controller state, restores both into a fresh instance and renders again. |
| Editor | Asks for an X11 editor, then attaches it, shows it under Xvfb and closes it. A plug-in without one warns: the generic parameter editor is used. |
| Save/reopen | Puts the class in a new project (an instrument track with notes, or an insert on an audio track playing a sine), saves, reopens and checks the saved identity. |
| Export | The offline master export with VST3 stages, trimmed at the silent tail; checks a finite, audible WAV. |

Statuses are `pass`, `warn`, `fail`, `skip`, `crash` (the step the process
died in or timed out in), `missing` (a bundle or a named class the archive
lacks) and `not-run` (after a crash or a failed earlier step). Plug-in failures are results, so the workflow fails only on a harness
error.

## Run it locally

Configure the qualification project with the VST3 host and the Quick UI (for
the editor column), build the probe and run the matrix. Use the local
dependency paths from [local development](../dependencies/local-development.md).

```sh
cmake -S tests/qualification -B build/qualification -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DOSCILLATE_BUILD_QUICK_UI=ON \
  -DOSCILLATE_BUILD_VST3_SCANNER=ON -DOSCILLATE_VST3_SDK_ROOT=/path/to/vst3sdk
scripts/build.sh build/qualification --target oscillate_vst3_matrix_probe
python3 -I tests/qualification/qualify_vst3_matrix.py \
  --probe build/qualification/oscillate_vst3_matrix_probe \
  --work build/qualification/vst3-matrix --output build/qualification/vst3-matrix/result \
  --only 'Surge XT' --editor
```

`--only` (repeatable) limits the run to named plug-ins. Without `--editor` the
Editor column records only whether the plug-in offers an X11 editor. Downloads
are kept in `--work/downloads` by digest, so later runs reuse them.
