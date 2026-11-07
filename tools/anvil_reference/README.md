# Anvil reference tools

These opt-in tools tuned Anvil's presets against real kicks (ADR-0060, "Revision: fitted to reference kicks"). They are excluded from the ordinary build.

- `oscillate_anvil_loops` renders presets through the engine's own runtime. Each one becomes a four-bar, four-to-the-floor loop at 124 BPM, written as 24-bit stereo WAV. You can override any value or curve, so you can audition a change before it goes into `anvil_presets.hpp`.
- `kick_reference.py` measures kicks: their pitch fall and decay, attack band balance, the body's harmonics and level after the hit. It does this both for reference intros and for those loops. It needs numpy, ffmpeg and ffprobe.

## The method

1. Pick tracks whose intros are kick only, and decode them.
2. Build the reference: the median of each track's median kick.
3. Render the presets and compare them. Judge each change by these numbers and by ear.

The first tuning used ten kick-only intros from commercial melodic techno and house releases. They are not distributed with the project; use your own. The reference kick:
- falls fast in pitch, from about 150–280 Hz with a time constant of 3–6 ms;
- gets its 400 Hz–1.5 kHz knock from driving only the attack;
- keeps a nearly pure sine body.

```sh
scripts/build.sh build/quick --target oscillate_anvil_loops
mkdir -p build/quick/scratch/kicks/loops
python3 tools/anvil_reference/kick_reference.py decode /path/to/reference-tracks build/quick/scratch/kicks
# Optional trailing arguments keep only files whose names start with PREFIX_.
python3 tools/anvil_reference/kick_reference.py target build/quick/scratch/kicks
build/quick/tools/anvil_reference/oscillate_anvil_loops build/quick/scratch/kicks/loops \
  "909 Tribute" "Tech House Thump|anvil.punch=0|file=Thump_no_punch"
python3 tools/anvil_reference/kick_reference.py compare build/quick/scratch/kicks \
  build/quick/scratch/kicks/loops/*.wav
```

`oscillate_anvil_loops --list` prints each preset's output level, and `--dump NAME` prints every value of one preset.
