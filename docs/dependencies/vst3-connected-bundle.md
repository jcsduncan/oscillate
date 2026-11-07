# Connected VST3 dynamic bundle fixture

The `ConnectedGain.vst3` fixture is built locally from
`tests/plugins/bundle_loader/connected_plugin.cpp`; only source is committed.
It uses the pinned VST3 `v3.8.1_build_84` interface headers and introduces no
new runtime dependency. Its factory exports distinct component and controller
classes. Both implement `IConnectionPoint`, retain their peer interfaces until
disconnect, and exchange `IMessage`s after connection. The controller exposes
automatable parameter 101, while the component stores a float gain in its
opaque state and accepts a zero-sample parameter flush.

`plugins_vst3_connected_bundle` scans the generated `.vst3` out of process,
uses the exact binary receipt for in-process verified loading, checks both
connection directions and teardown, stages a normalized value on the test UI
thread, flushes it on a processing thread, and verifies the first 64- and
128-frame audible output. It then saves/restores component state and verifies
the next output. Failure modes cover each connection attempt, controller
setter rejection and processor flush rejection, including module teardown.
The test also checks that ordinary first audio processing invokes no host or
fixture `new`/`new[]` allocation. It does not measure scheduling or claim
third-party plug-in compatibility.

The current synchronous `bind_vst3_project_effect` still returns
`parameter_values_unapplied` for this fixture's saved static value without
component state. The UI dispatcher, ownership transfer, graph compatibility
key and publication gate remain required before this path can become a
product feature. See [the static-value restore gate](vst3-static-parameter-restore.md).

Run locally with:

```sh
cmake --preset vst3 -DOSCILLATE_VST3_SDK_ROOT=/path/to/pinned/vst3sdk
cmake --build build/vst3 --target oscillate_vst3_connected_bundle_tests
ctest --test-dir build/vst3 -R '^plugins_vst3_connected_bundle$' --output-on-failure
```
