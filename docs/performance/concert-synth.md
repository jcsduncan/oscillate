# Concert sampled piano: memory and callback cost

ADR-0071, 29 October 2026. The figures below are design numbers measured on the installed library. **CPU has not been measured yet**: the sampled engine has not been built. The modelled engine's figures (ADR-0036, 14.6–54 % of a core) no longer apply.

## Memory

The Salamander Grand Piano V3 (sfzinstruments commit `3382bf94`), measured from its FLAC headers:

| Content | Files | Audio | int16 stereo |
| --- | ---: | ---: | ---: |
| Notes (16 layers, every minor third) | 480 | 6,595 s | 1,208 MiB |
| String-resonance releases | 69 | 159 s | 29 MiB |
| Key releases | 88 | 40 s | 7 MiB |
| Pedal noises | 4 | 14 s | 3 MiB |
| **All** | 641 | 6,809 s | **1,247 MiB** |

As float stereo the whole library would be 2,493 MiB, and as 16-bit 1,247 MiB. Both are too much for a machine that has had the DAW killed by the OOM daemon. Concert therefore keeps only part of it in memory:

| What | Size | Where |
| --- | ---: | --- |
| First 16,384 frames (0.34 s) of each note | 31 MiB | resident, once per process |
| Release, resonance and pedal samples, whole | 39 MiB | resident, once per process |
| 160 stream rings of 16,384 frames | 10 MiB | per Concert instance |
| Two sinc tables, 88 resonance loops | < 0.3 MiB | per Concert instance at 48 kHz |

That is about 70 MiB per process and 10 MiB more per Concert. The pack on disk is 1,247 MiB, plus 1 MiB of 4 KiB alignment.

## Disk

- **Reads.** Each sounding note voice reads its sample beyond the resident start at 192 kB/s (48 kHz int16 stereo), in 16 KiB positioned reads up to 0.34 s ahead of the voice.
- **Load.** A loud pedalled passage with 60 voices reads about 11.5 MB/s, mostly from the page cache after the first pass. That is far below NVMe throughput.
- **Starvation.** It would need a 0.34 s stall in disk service. Late frames play as silence, are counted, and show in the panel.

## Callback work per voice and frame

- **Resampling.**
  - A note at its root at 48 kHz is a copy.
  - Any other note runs a 32-tap windowed sinc with interpolated phases: 32 coefficient blends and 64 multiply-adds for the stereo pair.
  - Each note sounds two layers.
- **Sympathetic resonance.** One loop per key with its damper up: a fractional-delay read, a one-pole lowpass and a write. All 88 loops run under the sustain pedal; loops at rest cost nothing.
- **Allocation.** `process` is allocation-free (`native_concert_contract.full_load_does_not_allocate`).

## To measure

Run `oscillate_concert_bench` (Release, pinned core). It plays the installed Concert Grand with a 10-note chord with the dampers down, then with the sustain pedal, and a 20-note pedalled chord at 64 frames.

Record:
- mean and p99 block time against the 48 kHz budget at 64 and 128 frames;
- the hardware, compiler and settings, as AGENTS.md asks.

If the pedalled case is heavy, the first levers are:
- vectorising the sinc dot product across the stereo pair;
- skipping the lower layer once its weight falls below audibility.
