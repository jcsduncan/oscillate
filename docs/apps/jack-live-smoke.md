# JACK dummy-server qualification smoke

This opt-in harness exercises the JACK adapter against an isolated JACK2 dummy server. It does not open ALSA hardware or change the desktop's PipeWire/default sink. It is a local component qualification, not a DAW playback path or a reference-hardware latency result.

## Run

Install JACK2 and its development package, or use a locally unpacked matching package. Start a uniquely named server in non-realtime mode:

```sh
JACK_DEFAULT_SERVER=oscillate_smoke jackd -r -n oscillate_smoke -d dummy -r 48000 -p 128
```

In another terminal, configure the opt-in targets and run the connection and mismatch checks:

```sh
cmake -S . -B build/jack-live -G Ninja \
  -DBUILD_TESTING=ON -DOSCILLATE_AUDIO_IO_LIVE_TESTS=ON
cmake --build build/jack-live --target oscillate_jack_live_smoke
JACK_DEFAULT_SERVER=oscillate_smoke ctest --test-dir build/jack-live \
  -R '^audio_io_jack_live' --output-on-failure
```

The `connect` mode opens the adapter and a separate JACK capture client, connects the adapter's left/right output ports to capture inputs, and checks the rendered samples `(0.25, -0.5)`. It then closes the callback permission gate, checks that captured output becomes `(0, 0)` and that neither rendering nor the completed transport cursor advances, and reopens the gate. The `mismatch` mode requests a 44.1 kHz project against the 48 kHz server and checks rejection before callback activation.

To exercise shutdown and explicit reopen in one client process, run:

```sh
JACK_DEFAULT_SERVER=oscillate_smoke \
  build/jack-live/tests/audio_io/oscillate_jack_live_smoke recover
```

When it prints `STOP_NAMED_SERVER_NOW`, stop **only** the named dummy `jackd` process with Ctrl-C. The harness waits for `disconnected`, closes the old client, and retains the last completed sample while incrementing the transport generation. When it prints `RESTART_NAMED_SERVER_NOW`, restart the same named dummy server with the first command. The harness retries explicit `start`, then checks that callback processing advances from the retained boundary. It does not claim automatic JACK application recovery; the application must own quiescence, feed invalidation, source priming and gate reopening under ADR-0011.

## 14 October 2026 local evidence

The host had `libjack-jackd2-0` 1.9.22 but no installed `jackd2` binary or development headers. The matching Ubuntu `jackd2` package was downloaded and unpacked under `/tmp/oscillate-jack-server`; the existing temporary JACK development overlay was `/tmp/oscillate-jack-dev`. The command actually used was:

```sh
JACK_DRIVER_DIR=/tmp/oscillate-jack-server/root/usr/lib/x86_64-linux-gnu/jack \
JACK_DEFAULT_SERVER=oscillate_smoke_20261014 \
/tmp/oscillate-jack-server/root/usr/bin/jackd -r -n oscillate_smoke_20261014 \
  -d dummy -r 48000 -p 128
```

The build also used `PKG_CONFIG_PATH=/tmp/oscillate-jack-dev` and `CMAKE_PREFIX_PATH=/tmp/oscillate-json-dev/root/usr` because the host lacked the development packages. The live `connect` run reported `rate=48000 frames=128 signal_blocks=5 silent_blocks=16 completed_sample=1280`; both live CTest cases passed. The `mismatch` run reported `project_rate=44100 server_rate=48000 render_callbacks=0`.

In the controlled shutdown/restart run, the first connection reported 48 kHz, 128 frames and captured signal and silence blocks. Stopping the named server triggered `disconnected`; after quiescing, the completed boundary was sample 140672, generation 2. Restarting the same dummy server and explicitly reopening the adapter produced `RECOVERED first_sample=140672 generation=2 completed_sample=140800 render_callbacks=1100`. The first post-reopen block therefore advanced exactly 128 frames from the preserved boundary. The server was stopped after qualification.

The dummy server emitted memory-lock warnings because it ran without realtime privileges, and its own logs reported a few xruns while the interactive test was being controlled. These are not performance evidence. The harness does not force a null JACK port buffer or oversized callback from a real server; deterministic fake-server tests cover those error/silence paths. Physical-device loss, JACK routing in the application, bounded source re-priming on a JACK recovery path, and reference-workload underrun qualification remain open. JACK's [client callback API](https://jackaudio.org/api/group__ClientCallbacks.html) documents shutdown notification, and its [client API](https://jackaudio.org/api/group__ClientFunctions.html) documents explicit client opening and closing.
