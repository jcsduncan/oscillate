# Proposed third-party starter-sound provenance

The repository now includes four **original procedurally generated** one-shots at
[`assets/starter/`](../../assets/starter/). Their separate
[`manifest.json`](../../assets/starter/manifest.json) records generator revision,
parameters, WAV format, decoded levels and hashes. Run
`python3 scripts/generate-starter-sounds.py --check` to verify byte-for-byte
regeneration and the committed WAVs. The third-party candidates below remain
uncleared and are **not** included in the starter set.

**Status:** Candidate manifest; no audio is bundled yet.  
**Checked:** 14 October 2026.  
**Scope:** One small WAV loop that the current managed-source import path can read. This is a seed for the later starter library, not a complete sound pack.

## Candidate A — 99 BPM bongo loop

| Field | Record |
|---|---|
| Proposed asset ID/name | `starter.percussion.bongo_cumbia_99` / `bongo-cumbia-99.wav` |
| Creator as credited at the original source | **MarleneAyni**, Freesound account; legal name and independent identity are not verified. |
| Original source and creator statement | [“bongoes.wav” on Freesound](https://freesound.org/people/MarleneAyni/sounds/89608/), published 5 February 2010. The uploader describes an unprocessed mono bongo performance at 99 BPM, recorded with an AKG C214 in a living room, and notes “some leakage.” |
| Original permission evidence | The [individual Freesound sound page](https://freesound.org/people/MarleneAyni/sounds/89608/) identifies **Creative Commons 0**. [Freesound's licence FAQ](https://freesound.org/help/faq/#licenses-0) explains CC0 reuse. [CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/) permits copying, modification and distribution, including commercial use, to the extent the dedicator owns those rights. |
| Accessible WAV copy | [Wikimedia Commons file page](https://commons.wikimedia.org/wiki/File:Bongo_sound.wav) cites the same Freesound source and creator, and labels the file CC0. [Original WAV download](https://upload.wikimedia.org/wikipedia/commons/7/7a/Bongo_sound.wav). Commons uploader `Michielderoo` is the mirror uploader, not the credited sound creator. |
| Verified local copy | 426,254 bytes; RIFF/WAVE PCM, 16-bit mono, 44,100 Hz, 212,419 frames (about 4.817 s). SHA-256 `5c872bacef7a6e6a2e465dfa01046ca0624aa3ede49c8fe088e1a277517b3cc5`; SHA-1 `67403341d6fbdbe279ba21bf6a5c5a4a3bc0b48b`, matching the checksum published in Commons structured data. These hashes identify the Commons copy; the login-gated Freesound original was not independently hashed. |
| Intended transformation | None for the first inclusion. Keep the 44.1 kHz, 16-bit mono original as a managed source. Project import may decode or derive caches without modifying the bundled source. If later trimmed, normalized or resampled, record the exact command, version, output hash and retain the original. |
| Intended use | A short arrangement loop or sampler source for a sample/demo project. The application should show the loop's source title, creator handle, CC0 licence and original URL in its bundled-content credits. |
| Redistribution constraints | CC0 requires no attribution, but retain provenance and do not imply creator endorsement. CC0 does not waive third-party rights that the uploader did not own, nor privacy/publicity rights. Do not relabel the creator or claim authorship. Recheck the individual sound page and retain a dated source record before a release bundle is assembled. |

The WAV was downloaded **only to `/tmp` for audit**, outside the repository. The managed-audio import path (the headless player of the time) accepted it and reported `Imported 212419 frames at 44100 Hz ... (revision 2)`. No output device was opened. The original's “some leakage” note could mean harmless room spill or third-party audio; the page does not resolve that. Listen to and document the leakage before bundling. If it contains recognizable protected music or speech, reject this candidate unless the relevant rights are established. The Commons mirror's published source attribution and matching technical metadata support provenance, but they do not cryptographically prove identity with the login-gated Freesound original.

## Candidate B — synthesized kick, pending file acquisition

The [Freesound “plain synth kick.wav” page](https://freesound.org/people/fons/sounds/101370/) credits **fons** (real-world identity unverified), describes it as synthesized, and marks the individual sound **CC0**. It reports a 2-second stereo 44.1 kHz, 32-bit WAV. This is promising because a synthesized one-shot avoids the bongo recording's ambient-leakage question. Freesound requires login to download its original WAV; no file or hash was acquired in this audit. It is **not approved for bundling** until the original file is acquired through the site's permitted download flow, inspected, hashed and imported successfully, with its individual CC0 record retained.

## Release handoff

1. Resolve Candidate A's leakage and original-versus-mirror identity questions, or obtain and audit Candidate B directly. Do not treat the site's general “free” description as permission for an individual sound.
2. For every accepted sound, keep creator, title, individual source URL, licence URL, acquisition date, immutable original hash, exact transformation record and output hash in the content manifest/SBOM. Include a human-readable credits file even for CC0 material.
3. Validate each final WAV through managed project import and save/reopen before adding it to templates or a demonstration project. Keep starter content out of Git until its size and provenance are reviewed under the implementation plan's asset rule.
