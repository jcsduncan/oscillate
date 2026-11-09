#!/usr/bin/env python3
"""Download VSCO 2 CE and University of Iowa MIS strings and install them as Bow's library.

Bow (ADR-0111) plays sampled strings from two open sources:

- VSCO 2 Community Edition (Versilian Studios, CC0 1.0): the violin, viola
  and cello sections (sustain with vibrato, spiccato, pizzicato and tremolo),
  the solo contrabass (the Basses section, layered by the engine) and the
  solo violin's spiccato, pizzicato and tremolo. 44.1 kHz stereo WAV, one
  note per file, two dynamics, up to two round robins.
- The University of Iowa MIS solo strings (pre-2012 session, "may be
  downloaded and used for any projects, without restrictions"): violin, viola,
  cello and double bass, arco, pp, mf and ff, each string's chromatic run in
  one 44.1 kHz mono AIFF. Their pizzicato runs ring into each other and their
  soft takes sit near the noise floor, so solo pizzicati are VSCO's (the solo
  violin's; the others play their section's).

This script:

1. fetches VSCO with git at three pinned commits (git checks every object's
   hash, so the pinned commit and tree ids are the integrity check): the
   samples, the SFZ maps, and the 2016 "Seeded Samples" commit. Every sample
   an SFZ map names must sit under one of the string folders below and be
   byte-identical (the same git blob) to the seeded Community Edition
   recording, so only Versilian's own CC0 recordings are used;
2. downloads the Iowa AIFFs, each checked against a SHA-256 pinned below
   (there is no git commit to pin);
3. reads each VSCO note's key from its SFZ map (pitch_keycenter), its
   dynamic layer from the dynamic its file is named with (v1-v3, p or f; a
   second take of one dynamic is a round robin), and its round robin from
   its order, and cuts the Iowa runs into notes (a 10 ms RMS envelope, onset 18 dB
   and offset 10 dB above the run's floor), assigning keys by position in
   the run, or by measured pitch (within 0.4 semitone of a key in the run)
   where the cut count differs from the name or a note is out of place;
   takes outside the keys Bow plays on their instrument are left out;
4. measures every note's pitch (YIN, within two semitones of its key) and
   drops a note more than half a semitone off; `cents` is the offset;
5. trims the silence before the attack to 2 ms with a fade-in; marks the
   attack end (where the 10 ms RMS first reaches 90 % of the steady level);
6. sustains and tremolos: finds a loop of 0.6-2 s within 3 s after the
   attack end (so samples stay short) whose ends match in level, refines it
   by waveform correlation, bakes a 150 ms
   equal-power crossfade (corrected for the passages' correlation) into the
   frames before loop_end, and ends the sample at loop_end (a held or
   releasing note never reads past it); shorts (spiccato, pizzicato): trims
   the tail where it falls 60 dB under its loudest 10 ms or to within 6 dB of
   its noise floor, at most 4 s, and fades out its last 20 ms;
7. measures each note's level (the RMS of its steady part, or of its first
   200 ms for shorts) and scales each set (mode, instrument, articulation) so
   its loudest layer's median level is the target; layers keep their
   recorded difference. `level_db` is the note's level after that scale;
8. dithers to 16-bit and writes `bow-mono.pack` and `bow-stereo.pack`
   (little-endian int16, each sample aligned to 4 KiB) and `bow.manifest`,
   with ATTRIBUTION.txt and the VSCO licence, into the library directory.

The manifest's first line is `oscillate bow library 1`, then `name`,
`attribution`, `rate 44100`, `channels 2`, `pack bow-stereo.pack <bytes>`,
`mono_pack bow-mono.pack <bytes>`, then one line per sample:

    sample <channels> <section|solo> <violin|viola|cello|bass>
           <sustain|staccato|pizzicato|tremolo> <key> <layer> <round_robin>
           <offset> <frames> <attack_end> <loop_start> <loop_end>
           <cents> <scale> <level_db>

`offset` is the sample's first frame in its pack; the frame counts are
relative to its own first frame, and loop_end is exclusive (0 for a one-shot).
`scale` turns an int16 value into the set's level.

The library directory is $OSCILLATE_BOW_LIBRARY when set, otherwise
$XDG_DATA_HOME/oscillate/instruments/bow/strings (~/.local/share when
XDG_DATA_HOME is unset); Bow looks in the same place. The new library
replaces an old one only once it is complete.

Requires git and ffmpeg. Run with `python3 -I tools/bow/install_strings.py`.
"""

from __future__ import annotations

import argparse
import array
import concurrent.futures
import hashlib
import math
import os
import random
import re
import shutil
import statistics
import subprocess
import sys
import urllib.parse
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path

# ---- Sources ---------------------------------------------------------------------------------

VSCO_REPOSITORY = "https://github.com/sgossner/VSCO-2-CE.git"
VSCO_URL = "https://github.com/sgossner/VSCO-2-CE"
# master: the samples.
VSCO_SAMPLES_COMMIT = "440300901dfe9275fd84e0b7763af1f8443ae62e"
VSCO_SAMPLES_TREE = "0ecebcfca0c7e0fe501f71964027f8f59fa0e432"
# The SFZ branch: the maps.
VSCO_SFZ_COMMIT = "6dd651d55dde97fd4028699be9d4481f26917891"
VSCO_SFZ_TREE = "553ef3b90c87fe43ef19a3a8f8965a4a2945d570"
# "Seeded Samples" (8 July 2016): the Community Edition's own recordings.
VSCO_SEED_COMMIT = "2049252628dc847df037919fc51b76d0e38fa63d"
VSCO_LICENCE = "LICENSE"
VSCO_FOLDERS = (
    "Strings/Violin Section",
    "Strings/Viola Section",
    "Strings/Cello Section",
    "Strings/Solo Violin",
    "Strings/Solo Contrabass",
)
# Map -> (mode, instrument, articulation).
VSCO_MAPS: dict[str, tuple[str, str, str]] = {
    "ViolinEnsSusVib.sfz": ("section", "violin", "sustain"),
    "ViolinEnsSpic.sfz": ("section", "violin", "staccato"),
    "ViolinEnsPizz.sfz": ("section", "violin", "pizzicato"),
    "ViolinEnsTrem.sfz": ("section", "violin", "tremolo"),
    "ViolaEnsSusVib.sfz": ("section", "viola", "sustain"),
    "ViolaEnsSpic.sfz": ("section", "viola", "staccato"),
    "ViolaEnsPizz.sfz": ("section", "viola", "pizzicato"),
    "ViolaEnsTrem.sfz": ("section", "viola", "tremolo"),
    "CelloEnsSusVib.sfz": ("section", "cello", "sustain"),
    "CelloEnsSpic.sfz": ("section", "cello", "staccato"),
    "CelloEnsPizz.sfz": ("section", "cello", "pizzicato"),
    "CelloEnsTrem.sfz": ("section", "cello", "tremolo"),
    # The solo contrabass is the Basses section: the engine layers it.
    "ContrabassSusVB.sfz": ("section", "bass", "sustain"),
    "ContrabassSpic.sfz": ("section", "bass", "staccato"),
    "ContrabassPizz.sfz": ("section", "bass", "pizzicato"),
    "ContrabassTrem.sfz": ("section", "bass", "tremolo"),
    "SViolinSpic.sfz": ("solo", "violin", "staccato"),
    "SViolinTrem.sfz": ("solo", "violin", "tremolo"),
    "SViolinPizz.sfz": ("solo", "violin", "pizzicato"),
}
# A sample's dynamic, from its file name: VSCO's v1..v3, or a marking.
DYNAMIC_TAG = re.compile(r"_(v\d|pp|p|mp|mf|f|ff)(?=[_.])")
MARKINGS = ("pp", "p", "mp", "mf", "f", "ff")

IOWA_INDEX_URL = "https://theremin.music.uiowa.edu/MIS.html"
IOWA_SITE = "https://theremin.music.uiowa.edu/"
IOWA_FOLDERS = {
    "Violin": "sound files/MIS/Strings/violin/",
    "Viola": "sound files/MIS/Strings/viola/",
    "Cello": "sound files/MIS/Strings/cello/",
    "Bass": "sound files/MIS/Strings/doublebass/",
}
# The pre-2012 arco runs and their SHA-256 (the pizzicato runs are not used).
IOWA_SOURCES: dict[str, str] = {
    "Bass.arco.ff.sulA.A1B1.aiff": "dc9c29d65e621981429e12b87ef660f4ca21b4208aef752364dbf72ab3636f45",  # noqa: E501
    "Bass.arco.ff.sulA.C2B2.aiff": "1a626fe2f64afcc1e6ba784745986b1e0b796feec5ae3ed1c662b6fcafca90d0",  # noqa: E501
    "Bass.arco.ff.sulA.C3A3.aiff": "b77435e4634cf80b6827e648bf9a4e0403c7ab504ce2ddbd0e9b7fc589147572",  # noqa: E501
    "Bass.arco.ff.sulD.C3B3.aiff": "0ed5245fa5598848f0eaafc53519af41c150a2d21be93d1b8aa90b9397cfa361",  # noqa: E501
    "Bass.arco.ff.sulD.C4D4.aiff": "7b5406a1742e4ec278317fc1d4bb1ce164701e777ec950bf2ee7a0b248c2cbb9",  # noqa: E501
    "Bass.arco.ff.sulD.D2B2.aiff": "036eb8dbeb0bfb9d79d1970efe3f814ae56f4fa4eada1acc4355d7412671315c",  # noqa: E501
    "Bass.arco.ff.sulE.C2B2.aiff": "1faa592610772a14c473e948023ffa14a78d85742457301578df2718c54dcd1d",  # noqa: E501
    "Bass.arco.ff.sulE.C3D3.aiff": "2c8dc40903c38e5e3ddb5fbcd1b91864ab7d9ce792eec61346125511e662576c",  # noqa: E501
    "Bass.arco.ff.sulE.E1B1.aiff": "ebe75b308a51ea7d90a374637e4ffd52f92c721f56feb4b02cd968c01fbe4f11",  # noqa: E501
    "Bass.arco.ff.sulG.C3B3.aiff": "387639bb08faedbac23baf8f89f426519917696c426517bda0968d8fa4b8084c",  # noqa: E501
    "Bass.arco.ff.sulG.C4G4.aiff": "4b856493c786234339c732de9c76284e327ad4864f842e16973b1cb60b35b17e",  # noqa: E501
    "Bass.arco.ff.sulG.G2B2.aiff": "721c46df8ad6564462af5aac9f2f9dcc08e5c9c962266eff85bd2224268d71b3",  # noqa: E501
    "Bass.arco.mf.sulA.A1B1.aiff": "4300fad3045d8490e85dd4b734cae766958a6e8ae359b6feefc1618975a0a263",  # noqa: E501
    "Bass.arco.mf.sulA.C2B2.aiff": "e1ae947a051268e52a51e74130c89b32b987fa0ebd26976f449f919df53e8f92",  # noqa: E501
    "Bass.arco.mf.sulA.C3A3.aiff": "0ba2324b7a02588a1bd12c856235b3911009597f97799a683c1530dd1217fb94",  # noqa: E501
    "Bass.arco.mf.sulD.C3B3.aiff": "ec81c6d27f9ccdedf33f50ccb960a28d397abf59e92e2ef3723d758f5ffe6944",  # noqa: E501
    "Bass.arco.mf.sulD.C4D4.aiff": "8bbd8c567b91b6d50edbb706371fe6cd411ae5fae30d55d599fe59ad536ade35",  # noqa: E501
    "Bass.arco.mf.sulD.D2B2.aiff": "392f2c1e14663b42ad2a238aff4a6b079d845dd6713fafe65fc63d6d7a5ae1f3",  # noqa: E501
    "Bass.arco.mf.sulE.C2B2.aiff": "b77c407360344334fffe418d63152f6377b66fe7afca47b7dd10b7b5263fffd8",  # noqa: E501
    "Bass.arco.mf.sulE.C3D3.aiff": "7d21b87de057aff7f66206524d052dccee97a7584f60df2157755a5ff2444ca4",  # noqa: E501
    "Bass.arco.mf.sulE.E1B1.aiff": "661b339e4c4aacc0c58712013bd8a473b21b52f3b6f1bb4d603bf2787d25a5a8",  # noqa: E501
    "Bass.arco.mf.sulG.C3B3.aiff": "9b062e95e5b647aa362ced0ae3d890404f1fba2d48fc1aec31a700b30336db43",  # noqa: E501
    "Bass.arco.mf.sulG.G2B2.aiff": "76285057cbe140e12c47326485371076244be810d349c85cab1db4817ca66811",  # noqa: E501
    "Bass.arco.pp.sulA.A1B1.aiff": "09c595431130712d17b3fa3653489bce608c8fc6d40745d19fe446de497fa80d",  # noqa: E501
    "Bass.arco.pp.sulA.C2B2.aiff": "10f4264e449d0d7d8c799d1516519007c37f363b9f0288c26d050ff785825c57",  # noqa: E501
    "Bass.arco.pp.sulA.C3A3.aiff": "195af55de8d1ce14742d77a673c18cc28f87271ae5471885c56d546a73a2d3e9",  # noqa: E501
    "Bass.arco.pp.sulD.C3B3.aiff": "7251faa3e0f7cdda481502d74dd28b28174c4960c569ea525ceb1b1c026cb61d",  # noqa: E501
    "Bass.arco.pp.sulD.C4D4.aiff": "7fd6d49419ccb646cdacccc9cbf023ff31e2f5388c527d53eebea909d27e98b6",  # noqa: E501
    "Bass.arco.pp.sulD.D2B2.aiff": "db912668463c87c25f4718109590dc452faf32dd5d80d28f0b27f4930af1c0bc",  # noqa: E501
    "Bass.arco.pp.sulE.C2B2.aiff": "51192bb37b5eb928bbebbe4f6ab19d53e85ed58b639aa2bcff2928deef29048f",  # noqa: E501
    "Bass.arco.pp.sulE.C3D3.aiff": "3a068edcd9216234228923e6f5c5192e920a32b5b99865a71b2f16d9a52399ad",  # noqa: E501
    "Bass.arco.pp.sulE.E1B1.aiff": "ad4f8ad2dfc153ae3de00e352ac05e873a4f1a94ce6a427cf11607fe9f055980",  # noqa: E501
    "Bass.arco.pp.sulG.C3B3.aiff": "021ff35a2da03d7ac036fad5d1c0d78b18059ae9decaa1d3c5c1c842b9c9f79b",  # noqa: E501
    "Bass.arco.pp.sulG.C4G4.aiff": "ee03ee90a358ddf49264bcbab6d92e43eecc2591aee725bde6079414e63880d6",  # noqa: E501
    "Bass.arco.pp.sulG.G2B2.aiff": "74b0be446b2ba1d223cf1005c1d8e7bc818d48b9e71109504422b21195a17cbe",  # noqa: E501
    "Cello.arco.ff.sulA.A3B3.aiff": "404fc6d966868e0409ef9437c2785ea9ff37bf571ecc089ab97752a148632dea",  # noqa: E501
    "Cello.arco.ff.sulA.C4B4.aiff": "53cacc8ba58dfd5a8c731d03dc4411d26168a9e863e3e9193f0fae4c972cf33d",  # noqa: E501
    "Cello.arco.ff.sulA.C5Bb5.aiff": "6241e6103128995a879acd4dc07ca7a358b3e2c5e56d5975edd096ba4b521ea5",  # noqa: E501
    "Cello.arco.ff.sulC.Bb2F3.aiff": "37627af18e75943f0520b51b2bc0cce4d2af2fcd381af550156a4f9cbdab4c7b",  # noqa: E501
    "Cello.arco.ff.sulC.C2A2.aiff": "cd1c57b6f2cf5373b1d2aa950beab967652ae2efdf4c6bd243511dae35bcbe8e",  # noqa: E501
    "Cello.arco.ff.sulC.Gb3Db4.aiff": "ace3f6905235efd7e91bfeb50ed3d83b21762c0eeb53a4c76e81149f9bdad0c0",  # noqa: E501
    "Cello.arco.ff.sulD.B4D5.aiff": "405530b0c3e68f72de9757a702b8d7e21f94b1daa975930e2d8c32a96ed895bd",  # noqa: E501
    "Cello.arco.ff.sulD.C4Bb4.aiff": "f49f2df8d3a14db743e65a0f6771469d130e62615846c4aed12f9b76450866a7",  # noqa: E501
    "Cello.arco.ff.sulD.D3B3.aiff": "fb9ca888cd0fd803848b584c4c9664a100648cf64dd06b56841fef5ae60b5bdd",  # noqa: E501
    "Cello.arco.ff.sulG.B2Bb3.aiff": "1ad2d6bf99d54b36c5eeb24b994bcc7e5f82042c309f44ac0df8e1819011e09c",  # noqa: E501
    "Cello.arco.ff.sulG.B3Gb4.aiff": "1ea8ad543d65db0389c6a8bab2b4edf319ae2be307be90b3840dba9d609f539c",  # noqa: E501
    "Cello.arco.ff.sulG.G2B2.aiff": "e6e308a363c65c9c55292d7176247ef486a1c3357f71dd6ddcaeab1cafaa7e4e",  # noqa: E501
    "Cello.arco.ff.sulG.G4.aiff": "1c7fbe793c310669a9ea6c1971aaa36c68b33a5c337f12ee7ab990588a8677be",  # noqa: E501
    "Cello.arco.mf.sulA.A3B3.aiff": "59d2457357d62df927a0b4ff80e685df8c8b028386322576ac3efa562c61bf9c",  # noqa: E501
    "Cello.arco.mf.sulA.C4C5.aiff": "4d2da113625445434f331e0d453e47f82be01b83795e99b29d43f8551b12d4fc",  # noqa: E501
    "Cello.arco.mf.sulA.C5Ab5.aiff": "874f6da697ccc8d0d598aa30903f521e7b3e177545da70d1a85c3bd31c90fe97",  # noqa: E501
    "Cello.arco.mf.sulC.A2Gb3.aiff": "3289772e71ff803b1904625704cf9351ca6e390449799943abc910661cd2fc16",  # noqa: E501
    "Cello.arco.mf.sulC.C2Ab2.aiff": "8942170087befce24773228679e3a8961c34b8d684ce287db0e551f3a3236fb4",  # noqa: E501
    "Cello.arco.mf.sulC.C4Gb4.aiff": "d6db2d2dd3a92979c0b28de8f3b0453ab53f3e7427c7aa80fd9acd11fb3f240f",  # noqa: E501
    "Cello.arco.mf.sulD.B4Eb5.aiff": "3e33b437e3d7c202b00ad5b582640c67245cf46cf710800a676f0f2dcfeb4183",  # noqa: E501
    "Cello.arco.mf.sulD.C4Bb4.aiff": "0eb5b4bf2f38999cbf2b697cc277564ed84d3bac1986201607ace1e34a6efb50",  # noqa: E501
    "Cello.arco.mf.sulD.D3B3.aiff": "a42352e65954ae2b8ffe427a9044b8ad1f2b7c04b8623c30e205ece0a7485ce5",  # noqa: E501
    "Cello.arco.mf.sulG.B2A3.aiff": "4565c2165d0afe874cd6d39872160f242b153d987d94ef79dc562e04df09a7c2",  # noqa: E501
    "Cello.arco.mf.sulG.Bb3Eb4.aiff": "2c48c01cdd866a90c8d3f21890c728e6e006a89b2e92826098974a9388541cf5",  # noqa: E501
    "Cello.arco.mf.sulG.E4G4.aiff": "77080024dcad2790e9f160caa53b1b66e3c476ecec43ec979e0754edf5f7abf9",  # noqa: E501
    "Cello.arco.mf.sulG.G2Bb2.aiff": "549afb3c833c7f0ecbf8bf8898719c49c57437c2170baafed84e673ac122c59d",  # noqa: E501
    "Cello.arco.pp.sulA.A3B3.aiff": "d034f1279299c8273f954e2f38d42f21df649a554109d5b50c49d9832b03948a",  # noqa: E501
    "Cello.arco.pp.sulA.Ab5.aiff": "dc51fef017750989b91b45ac584c15f9eaf19c836ae6760baec94e867fe46c03",  # noqa: E501
    "Cello.arco.pp.sulA.B4Ab5.aiff": "08b6b3a3a5030f530f4c1093f8760cd506eea99b3791447e5663030e6f8d47bc",  # noqa: E501
    "Cello.arco.pp.sulA.C4Bb4.aiff": "8d444d8c7bc3f135dcae0fe58dd369f05781a2f61ac13fb6e2b670ed86668080",  # noqa: E501
    "Cello.arco.pp.sulC.B3Db4.aiff": "384fd9b5bf694266167ad5cb9fab90fca463946f0cd1982a732e06ae00740f3a",  # noqa: E501
    "Cello.arco.pp.sulC.C2Gb2.aiff": "2fbf30dfbe08428bd3ea46d1731055d46e1eaaa750839c43dea6b1b449a7a73a",  # noqa: E501
    "Cello.arco.pp.sulC.D3Bb3.aiff": "360ad4db331cc532ab07f6ef8e088788cc2e45f44b90bf1f14b8ccea12ecabc9",  # noqa: E501
    "Cello.arco.pp.sulC.Gb2D3.aiff": "d6c98e8359261e9f81eba103dbb185912ed0dabbffc91b5d4dc27a9ebbf548e5",  # noqa: E501
    "Cello.arco.pp.sulD.Bb4D5.aiff": "498bb2965f22226736a249f9a7bddb3eb70c07d854eac27b81b38e4b02cb7114",  # noqa: E501
    "Cello.arco.pp.sulD.C4A4.aiff": "377c2e8e635047d0067a00050d2f0951f25238f49079d15379f1be9d7f33cdd1",  # noqa: E501
    "Cello.arco.pp.sulD.D3B3.aiff": "5fd8cc2d35be7230afd5c5b96bc008bcdc9063ddbd8fdbb160c916e7c2c49e3a",  # noqa: E501
    "Cello.arco.pp.sulG.B2A3.aiff": "3cb5a5517861beb9c84436cbaea14ee1ddddb4602c74a6b864eca4f8e3a99834",  # noqa: E501
    "Cello.arco.pp.sulG.Bb3Gb4.aiff": "38ca264eacddfdaf83d1c81cf06b2da77eab2a5d7bd7aa6cbbc187c944a3e8c3",  # noqa: E501
    "Cello.arco.pp.sulG.G2Bb2.aiff": "8b82be1e53a68893159933287c775fc097804259a0f1d2dba06049232d00fc94",  # noqa: E501
    "Cello.arco.pp.sulG.Gb4G4.aiff": "330f66955f5a0642c33690b00c9f026fd14ea0d312cb9a07d8f894bf5d4a8159",  # noqa: E501
    "Viola.arco.sulA.ff.A4B4.aiff": "f1c5a9ab792afacb308cd7ebc63341f76b7b5243c1074b4eaa35281f760d7963",  # noqa: E501
    "Viola.arco.sulA.ff.C5B5.aiff": "4147580f3d3aa87c70644d234956df03610f8dcb96d6178d9755e44c80ba8dce",  # noqa: E501
    "Viola.arco.sulA.ff.C6Bb6.aiff": "e08f44b882355291437d2d64ed492ecc90108aee31017f5b1fe855796e79d6c7",  # noqa: E501
    "Viola.arco.sulA.mf.A4B4.aiff": "53bcbf77a8b92d7cca0bc4bc58e242bfe14ef5b1d866b3e9d32fca74594b2284",  # noqa: E501
    "Viola.arco.sulA.mf.C5B5.aiff": "38ddcccf6fff4d35f3ef99e73bf7b76a04cee0f6916339243b6e295e225441d5",  # noqa: E501
    "Viola.arco.sulA.mf.C6Ab6.aiff": "aa24fbf2311eef8063f17d857b517277936ebdac48beaa78749a4fc4efb76052",  # noqa: E501
    "Viola.arco.sulA.pp.A4B4.aiff": "3e73a1970738e58ec80ad36ab7bd9719695a2c783709af19f02cb64ea27e5efb",  # noqa: E501
    "Viola.arco.sulA.pp.C5B5.aiff": "ae12e142a107738511ed18a7d2a2e748fa9aca8becfc7288fa3a4b9b924c50c5",  # noqa: E501
    "Viola.arco.sulA.pp.C6G6.aiff": "2efb51d4b16cd1cb931ee9ab7bbb6d87a782c4baa24d8dea3827d8828796464a",  # noqa: E501
    "Viola.arco.sulC.ff.C3B3.aiff": "7cbc2973460f33195f6799f8805b4d3162b869c2a839b8fc018a91c48a5309a0",  # noqa: E501
    "Viola.arco.sulC.ff.C4B4.aiff": "e2b434f14ce90eb05d44f9c00bd3381cd157ef68ec3d2bbfe0aaa1bd0f723d90",  # noqa: E501
    "Viola.arco.sulC.mf.C3B3.aiff": "c718be7173117c1aa0fc0039a2df54191bc3c40582bf4fed1376f482c2f52e1e",  # noqa: E501
    "Viola.arco.sulC.mf.C4B4.aiff": "20f56a6320ba515a834f4e8b98d646e455be8c17320ab962774b9be305abf67b",  # noqa: E501
    "Viola.arco.sulC.pp.C3B3.aiff": "8f68bead6985602b70babdb899c263489017de82da830062ddbf5ab2e3ab8490",  # noqa: E501
    "Viola.arco.sulC.pp.Db4C5.aiff": "902b5cd66cd2d76939c1f2caf5faa18f2a165f8a3a2ec5a30722d3fb57b00138",  # noqa: E501
    "Viola.arco.sulD.ff.C5B5.aiff": "c3b3ad49694529dd3637e1f0086910e15ece1ea3058c96be832070e11daae112",  # noqa: E501
    "Viola.arco.sulD.ff.C6D6.aiff": "2f08dcfc7b90a73120ae5614104ade485b110ee7c150a26c3c5b2583352320e3",  # noqa: E501
    "Viola.arco.sulD.ff.D4B4.aiff": "a5d419cae973718a3a601f946f5f46be6cdb7cfe361c58810abf14ddffbce74f",  # noqa: E501
    "Viola.arco.sulD.mf.C5B5.aiff": "40a089ff05154a70d1919eb8422a91b13631e39c3d1432e0a5e92a2c56f41f6b",  # noqa: E501
    "Viola.arco.sulD.mf.C6Eb6.aiff": "3a234777cb8559d396c3cd77dc0c0a835b4158ad22a2b5e924139629c56bf4da",  # noqa: E501
    "Viola.arco.sulD.mf.D4B4.aiff": "eefd4a8d9e913da891c716439d5bfd4112a3c51210037c48f5f316051fb7f2ef",  # noqa: E501
    "Viola.arco.sulD.pp.C5B5.aiff": "f23cac7ec2893aef534c52b40f9708cda113ac130dbd11c21b679827cee43bb7",  # noqa: E501
    "Viola.arco.sulD.pp.D4B4.aiff": "913280032362d36c4b568fe2c74fadb2b736cbd2ab42494f5518d62539658fac",  # noqa: E501
    "Viola.arco.sulG.ff.C4B4.aiff": "d10f7132abdc2a698b5fbd9ff01e2166fd79548b82d89d026755cef5c59c3413",  # noqa: E501
    "Viola.arco.sulG.ff.C5Ab5.aiff": "5f9604a5b53d530108f7923fd89ce68ac15c032ad458b9498aa013533ffa0e80",  # noqa: E501
    "Viola.arco.sulG.ff.G3B3.aiff": "cf94072d4b63b404196b4a74703a31508bbd9e68f340aecd0c3130aefc1f5a82",  # noqa: E501
    "Viola.arco.sulG.mf.C4B4.aiff": "41ba41e8011514508594e19dab5098c38370940825c16cd0940875b49c918a35",  # noqa: E501
    "Viola.arco.sulG.mf.C5G5.aiff": "d5ea5f621a04ed0610f1ad050f1b36cfa1e24fc775c9cfd4ad0504bc0d64bed7",  # noqa: E501
    "Viola.arco.sulG.mf.G3B3.aiff": "531cdc507259bdf51a74a7cc42429077e75e0b945e4d5e95a716a327cfc0f97a",  # noqa: E501
    "Viola.arco.sulG.pp.C5Gb5.aiff": "1390ff12ff0da0453fd11950510f2e4bf6dea03828f55c33be6e2b9e4d704c3f",  # noqa: E501
    "Viola.arco.sulG.pp.Db4B4.aiff": "9917415943656fef69991e77f60d8d6ba1812683028e825dbe0dad1cd5da1907",  # noqa: E501
    "Viola.arco.sulG.pp.G3B3.aiff": "60eade21bd37286a61b5b2eac44b30d6837e6aeeef93d7d052f5e5020f1f937a",  # noqa: E501
    "Violin.arco.ff.sulA.A4B4.aiff": "a16a1281b31a703ec275baa5aed7e538692d049e9f1b01b163c49a40baef24e2",  # noqa: E501
    "Violin.arco.ff.sulA.C5Db6.aiff": "bea3ed0ee046eccf93df15e4352ce55a7cc6c4de692e0f0d2b0ec743a769472f",  # noqa: E501
    "Violin.arco.ff.sulA.D6G6.aiff": "b8b9c1735324e4454ea4cbb4f77ec932de23dba3800c6cce45981e26586e3279",  # noqa: E501
    "Violin.arco.ff.sulD.C5D6.aiff": "f270df9c5845ebd632db6714bde6a552460b9a4f8da3e877b988db5b0ba4d48d",  # noqa: E501
    "Violin.arco.ff.sulD.D4B4.aiff": "042b27a88302fa9c03579d8a693ec9ae9a3e8ad8afad0a7d0fb6da3622a9304e",  # noqa: E501
    "Violin.arco.ff.sulE.Ab7B7.aiff": "031d5079b7329dc14f2c53557bfbfb2ede172d6d79eb6b30a6641e343f85d684",  # noqa: E501
    "Violin.arco.ff.sulE.C6Gb7.aiff": "218b4f84f0f505a86839bc58b499226e13eaa6b34d6ecb2789f00951db4594a0",  # noqa: E501
    "Violin.arco.ff.sulE.E5B5.aiff": "6b9f797cb5892192d3a28048c6c6903c2b50d3dcd3ebf26c9be9def68eefffb6",  # noqa: E501
    "Violin.arco.ff.sulG.C4Eb5.aiff": "d17e195d627161af271c052694b41e2c09d78811cad3d58b2c0d2d1207e133a2",  # noqa: E501
    "Violin.arco.ff.sulG.E5Gb5.aiff": "22def3cb5211b40040de3e683c3f2e6e7a0fdd8577e9560b234019f22617c8b4",  # noqa: E501
    "Violin.arco.ff.sulG.G3B3.aiff": "39f124996a859462dccb74ff94ae810b933846cd64c76e162358fca91c3e162a",  # noqa: E501
    "Violin.arco.mf.sulA.A4B4.aiff": "d314f949de13cd2326e539131e59d8c63fd9c044bc1000baac0684b0d5c3b57b",  # noqa: E501
    "Violin.arco.mf.sulA.C5C6.aiff": "6cd86026a7786f6052bb78467e2428fe79c2e95a128b1f89637e10d28d1e3c67",  # noqa: E501
    "Violin.arco.mf.sulA.Db6Ab6.aiff": "c0da38204683b7f36bfe60c2a809b4ab3d8f51b1ed9d60f88dd2f885f5f820e4",  # noqa: E501
    "Violin.arco.mf.sulD.Bb4C6.aiff": "d7dd7ee936e790f83e500c73ab2b99fedc3f15490e64ee82e8eeb60301b6a537",  # noqa: E501
    "Violin.arco.mf.sulD.D4A4.aiff": "412bc9f34bc2ba15242aacc18eff105277d746cb52878c478f7704c5f6b079b6",  # noqa: E501
    "Violin.arco.mf.sulD.D6.aiff": "1010427c6450fe7d4c7ef50ce02a0cd868b06b77ac5ba8e2817e0245e2b61595",  # noqa: E501
    "Violin.arco.mf.sulE.Ab7A7.aiff": "4d68687e7ba87a770e58dbdf149f8ee22171dd704c1516f36c8eca5e4c8fdf3a",  # noqa: E501
    "Violin.arco.mf.sulE.C6Gb7.aiff": "cee2b64078e54e68395234278d259a470b530568e5615003d08530359fef422b",  # noqa: E501
    "Violin.arco.mf.sulE.E5B5.aiff": "aec7ba76149b5bf7fd7cdaf5631a619966bc91ea472a7effaf50726811f4812f",  # noqa: E501
    "Violin.arco.mf.sulG.B3B4.aiff": "962d27fccc8bbdc30ef4734d99afe58060a1930308ab9c9b00867b1de54ac770",  # noqa: E501
    "Violin.arco.mf.sulG.C5G5.aiff": "60a2859cde464365a5cf9c3e627dbbef531d219918f8b1cd7e13976eb95efeb9",  # noqa: E501
    "Violin.arco.mf.sulG.G3B3.aiff": "50c53128788531062b239f5007fb48cf17c4e8b4474f9caf871163993d46b80d",  # noqa: E501
    "Violin.arco.pp.sulA.A4B4.aiff": "e2e761ed114eec59eec000cc220d0cccef6a32a9fb06dd89d54c0c667ddaddca",  # noqa: E501
    "Violin.arco.pp.sulA.Bb5Ab6.aiff": "98707404e37d8dbb26ee07156c98882403d150d1180d38f92c939712703bbd6d",  # noqa: E501
    "Violin.arco.pp.sulA.C5A5.aiff": "febe4764b4d3831f5b0c6696cff60db8e905df3dd6e09159ff3d2293bc6873e0",  # noqa: E501
    "Violin.arco.pp.sulD.B4A5.aiff": "1513e2a0c5c37f71780168da639a3aa82a3360aa0b6efc4b72f62654911a4eff",  # noqa: E501
    "Violin.arco.pp.sulD.Bb5Db6.aiff": "01a38549cc2c714610ce5319ddce29b7ac176dfdb4f4dd851375ccb060d9ce13",  # noqa: E501
    "Violin.arco.pp.sulD.D4Bb4.aiff": "55bbfe5d1b8272758c4dbc5352a37c80113b938f1d2cd26408f5a62c827f2f62",  # noqa: E501
    "Violin.arco.pp.sulE.A5G6.aiff": "790312b3f1b077dc51ad38ec84c2e60076b630efec24a23ca0c17a6352133aca",  # noqa: E501
    "Violin.arco.pp.sulE.Ab6B7.aiff": "bad32f044446e441d4ce1b0cb43a0b22c91f6af24814045030b44cb7bfdab00a",  # noqa: E501
    "Violin.arco.pp.sulE.E5A5.aiff": "faef815aee638d74b1464cd8e284af9aea3316a5b4dc25c11ae970543e479692",  # noqa: E501
    "Violin.arco.pp.sulG.A4Gb5.aiff": "7903cfe7c2eaacca76111189db86b6178eba6b85e8fbb863ef0a3364a5e5dad2",  # noqa: E501
    "Violin.arco.pp.sulG.B3Ab4.aiff": "49d2e10936e71c5f34b94e67f5a729d453e8e8a6e8ec19a5312876e77bb44980",  # noqa: E501
    "Violin.arco.pp.sulG.G3B3.aiff": "d283c884c4143c215634739453b788fea23d4f22d777aabaabda654224baf721",  # noqa: E501
}
IOWA_NAME = re.compile(
    r"(Violin|Viola|Cello|Bass)\.(arco|pizz)\."
    r"(?:(pp|mf|ff)\.sul([A-G])|sul([A-G])\.(pp|mf|ff))\.([A-G]b?\d)([A-G]b?\d)?\.aiff"
)
IOWA_INSTRUMENTS = {"Violin": "violin", "Viola": "viola", "Cello": "cello", "Bass": "bass"}
IOWA_ARTICULATIONS = {"arco": "sustain"}
# Each instrument's open strings and their keys: of the takes of one note,
# the one played lowest on its string (nearest the open string, the plainest
# ordinario) is kept.
OPEN_STRINGS = {
    "violin": {"G": 55, "D": 62, "A": 69, "E": 76},
    "viola": {"C": 48, "G": 55, "D": 62, "A": 69},
    "cello": {"C": 36, "G": 43, "D": 50, "A": 57},
    "bass": {"E": 28, "A": 33, "D": 38, "G": 43},
}
DOWNLOAD_TIMEOUT_SECONDS = 120
HASH_CHUNK_BYTES = 1 << 20

NAME = "VSCO 2 CE strings and University of Iowa MIS solo strings"
ATTRIBUTION = (
    "VSCO 2 Community Edition by Versilian Studios (Sam Gossner), CC0 1.0; "
    "University of Iowa Musical Instrument Samples (Lawrence Fritts)"
)
IOWA_TERMS = "may be downloaded and used for any projects, without restrictions"
FORMAT_LINE = "oscillate bow library 1"
MANIFEST = "bow.manifest"
MONO = 1
STEREO = 2
PACKS = {MONO: "bow-mono.pack", STEREO: "bow-stereo.pack"}
ATTRIBUTION_FILE = "ATTRIBUTION.txt"
INSTALLED_VSCO_LICENCE = "LICENSE-VSCO.txt"
DEFAULT_SOURCE_NAME = ".strings-source"

# The keys Bow plays on each instrument (bow.hpp's instrument_ranges): a
# take outside is not installed.
INSTRUMENT_RANGES = {"violin": (55, 103), "viola": (48, 88), "cello": (36, 76), "bass": (28, 67)}
MODES = ("section", "solo")
INSTRUMENTS = ("violin", "viola", "cello", "bass")
ARTICULATIONS = ("sustain", "staccato", "pizzicato", "tremolo")
LOOPED = ("sustain", "tremolo")
IOWA_LAYERS = {"pp": 0, "mf": 1, "ff": 2}

# ---- Units -----------------------------------------------------------------------------------

RATE = 44_100
A4_HZ = 440.0
A4_KEY = 69
SEMITONES_PER_OCTAVE = 12
CENTS_PER_SEMITONE = 100.0
DECIBELS_PER_DECADE = 20.0
POWER_DECIBELS_PER_DECADE = 10.0
HALF_STEP = 0.5
TINY_POWER = 1e-20
PERCENT = 100.0
INT16_FULL_SCALE = 32_768.0
INT16_MIN = -32_768
INT16_MAX = 32_767
BYTES_PER_SAMPLE = 2
ALIGN_BYTES = 4_096
MILLISECONDS_PER_SECOND = 1_000
NOTE_NAME = re.compile(r"([A-G])(#|b?)(-?\d)")
PITCH_CLASSES = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}


def frames_of(milliseconds: float) -> int:
    return round(milliseconds * RATE / MILLISECONDS_PER_SECOND)


# ---- Cutting the Iowa runs -------------------------------------------------------------------
CUT_HOP_FRAMES = frames_of(10)
ONSET_DB_ABOVE_FLOOR = 18.0
OFFSET_DB_ABOVE_FLOOR = 10.0
FLOOR_PERCENTILE = 10.0
MIN_NOTE_FRAMES = frames_of(250)
CUT_PRE_ROLL_FRAMES = frames_of(10)

# ---- Pitch -----------------------------------------------------------------------------------
PITCH_WINDOW_FRAMES = frames_of(300)
SHORT_PITCH_SKIP_FRAMES = frames_of(80)
SHORT_PITCH_WINDOW_FRAMES = frames_of(200)
# YIN's dip threshold (de Cheveigne and Kawahara's 0.1-0.15).
YIN_THRESHOLD = 0.15
# Pitch is looked for this far either side of the expected key.
PITCH_SEARCH_SEMITONES = 2.0
# A note further than this from its key (in semitones) is not that note.
MAX_KEY_ERROR = 0.5
# Keys read from a measured pitch must sit this close to a key.
MEASURED_KEY_ERROR = 0.4

# ---- Trim ------------------------------------------------------------------------------------
ONSET_FRACTION = 0.01  # -40 dB re the peak.
TRIM_PRE_ROLL_FRAMES = frames_of(2)
END_FADE_FRAMES = frames_of(20)
SHORT_TAIL_DB = 60.0
# A one-shot also ends this far above the noise floor of its take.
SHORT_FLOOR_MARGIN_DB = 6.0
MAX_SHORT_FRAMES = frames_of(4_000)
SHORT_LEVEL_FRAMES = frames_of(200)

# ---- Attack end and loop ---------------------------------------------------------------------
ENVELOPE_WINDOW_FRAMES = frames_of(10)
ENVELOPE_STEP_FRAMES = frames_of(1)
ATTACK_LEVEL_FRACTION = 0.9
STEADY_START_FRACTION = 0.30
STEADY_END_FRACTION = 0.90
CROSSFADE_FRAMES = frames_of(150)
MIN_LOOP_FRAMES = frames_of(600)
MAX_LOOP_FRAMES = frames_of(2_000)
# Loops are looked for this soon after the attack, so a sample ends early.
LOOP_REGION_FRAMES = frames_of(3_000)
LOOP_HOP_FRAMES = frames_of(10)
# Coarse loop points: the level tracks over a crossfade before each point
# must agree; a shorter loop costs this much (in dB of mismatch) at nothing.
SHORTER_LOOP_COST_DB = 1.0
COARSE_CANDIDATES = 8
# Fine search around a coarse end, and the passage correlated there.
FINE_SEARCH_FRAMES = frames_of(6)
FINE_WINDOW_FRAMES = frames_of(40)
CROSSFADE_BLOCK_FRAMES = frames_of(10)
LEAST_CORRECTED_CORRELATION = -0.5
QUARTER_TURN = math.pi / 2.0

# ---- Level -----------------------------------------------------------------------------------
# Each set's loudest layer has this median level (dBFS RMS): sustains and
# tremolos over their steady part, shorts over their first 200 ms.
TARGET_LEVEL_DB = {"sustain": -20.0, "tremolo": -20.0, "staccato": -16.0, "pizzicato": -16.0}
# Under full scale so dither never clips.
NORMALISED_PEAK = 0.98
DEFAULT_JOBS = 4


@dataclass
class Job:
    """One source file and how to read it."""

    path: str
    channels: int
    mode: str
    instrument: str
    articulation: str
    # VSCO: one note per file.
    key: int = 0
    layer: int = 0
    round_robin: int = 0
    # Iowa: a run of notes on one string.
    run_first: int = 0
    run_last: int = 0
    string: str = ""


@dataclass
class Note:
    channels: int
    mode: str
    instrument: str
    articulation: str
    key: int
    layer: int
    round_robin: int
    source: str
    cents: float = 0.0
    position: int = 0  # Semitones above its open string (Iowa).
    interior: bool = True
    frames: int = 0
    attack_end: int = 0
    loop_start: int = 0
    loop_end: int = 0
    loop_match: float = 0.0
    level_db: float = 0.0  # Before the set's scale.
    peak: float = 0.0
    scale: float = 0.0
    pcm: bytes = b""
    offset: int = 0

    @property
    def set_key(self) -> tuple[str, str, str]:
        return (self.mode, self.instrument, self.articulation)


@dataclass
class Result:
    source: str
    kept: list[Note] = field(default_factory=list)
    dropped: list[str] = field(default_factory=list)


def data_directory() -> Path:
    override = os.environ.get("OSCILLATE_BOW_LIBRARY")
    if override:
        return Path(override)
    base = os.environ.get("XDG_DATA_HOME") or str(Path.home() / ".local" / "share")
    return Path(base) / "oscillate" / "instruments" / "bow" / "strings"


# ---- VSCO: git at pinned commits -------------------------------------------------------------


def git(repository: Path, *arguments: str) -> str:
    return subprocess.run(
        ["git", "-C", str(repository), *arguments], capture_output=True, check=True, text=True
    ).stdout


def fetch_vsco(source: Path) -> None:
    """A partial clone holding the pinned commits, the string folders checked out."""
    if not (source / ".git").is_dir():
        source.mkdir(parents=True, exist_ok=True)
        subprocess.run(["git", "init", "-q", str(source)], check=True)
        git(source, "remote", "add", "origin", VSCO_REPOSITORY)
    print("  fetching VSCO 2 CE at its pinned commits")
    git(
        source,
        "fetch",
        "-q",
        "--filter=blob:none",
        "--depth",
        "1",
        "origin",
        VSCO_SAMPLES_COMMIT,
        VSCO_SFZ_COMMIT,
        VSCO_SEED_COMMIT,
    )
    for commit, tree in (
        (VSCO_SAMPLES_COMMIT, VSCO_SAMPLES_TREE),
        (VSCO_SFZ_COMMIT, VSCO_SFZ_TREE),
    ):
        if git(source, "rev-parse", f"{commit}^{{tree}}").strip() != tree:
            raise SystemExit(f"VSCO commit {commit} does not hold its pinned tree")
    git(source, "sparse-checkout", "set", "--no-cone", *(f"/{folder}/" for folder in VSCO_FOLDERS))
    git(source, "-c", "advice.detachedHead=false", "checkout", "-q", VSCO_SAMPLES_COMMIT)


def blobs(source: Path, commit: str) -> dict[str, str]:
    """Path -> blob id of every file under the string folders at a commit."""
    listing = git(source, "ls-tree", "-r", "-z", commit, "--", *VSCO_FOLDERS)
    found = {}
    for entry in listing.split("\0"):
        if entry:
            meta, path = entry.split("\t", 1)
            found[path] = meta.split()[2]
    return found


def sfz_regions(text: str) -> tuple[str, list[dict[str, str]]]:
    """The map's default_path and its regions' opcodes."""
    default_path = ""
    regions: list[dict[str, str]] = []
    current: dict[str, str] | None = None
    opcode = re.compile(r"(\w+)=(.*?)(?=\s+\w+=|$)")
    for raw in text.splitlines():
        line = raw.split("//", 1)[0].strip()
        if not line:
            continue
        if line.startswith("<"):
            current = {} if line.startswith("<region>") else None
            if current is not None:
                regions.append(current)
            line = line.split(">", 1)[1].strip()
        for name, value in opcode.findall(line):
            if name == "default_path":
                default_path = value.strip().replace("\\", "/")
            elif current is not None:
                current[name] = value.strip()
    return default_path, regions


def dynamic_rank(tag: str) -> int:
    """Softest first: v1 < v2 < v3, pp < ... < ff."""
    return int(tag[1:]) if tag.startswith("v") else MARKINGS.index(tag)


def vsco_jobs(source: Path) -> list[Job]:
    """Every note the maps play, each checked to be a seeded CE recording."""
    pinned = blobs(source, VSCO_SAMPLES_COMMIT)
    seeded = blobs(source, VSCO_SEED_COMMIT)
    jobs = []
    for map_name, (mode, instrument, articulation) in VSCO_MAPS.items():
        text = git(source, "show", f"{VSCO_SFZ_COMMIT}:{map_name}")
        default_path, regions = sfz_regions(text)
        # A map may give one recording to two velocity ranges: it is one layer.
        unique: dict[str, dict[str, str]] = {}
        for region in regions:
            unique.setdefault(region["sample"], region)
        regions = list(unique.values())
        # Layers by the dynamic each file is named with (a second take of one
        # dynamic is a round robin), else by the map's velocity splits.
        tags = [DYNAMIC_TAG.search(region["sample"]) for region in regions]
        if all(tags):
            loudness = [dynamic_rank(tag.group(1)) for tag in tags]
        else:
            loudness = [int(region["lovel"]) for region in regions]
        floors = sorted(set(loudness))
        seen: dict[tuple[int, int], int] = {}
        for region, level in zip(regions, loudness, strict=True):
            relative = default_path + region["sample"]
            if not any(relative.startswith(folder + "/") for folder in VSCO_FOLDERS):
                raise SystemExit(f"{map_name}: {relative} is outside the string folders")
            if relative not in pinned:
                raise SystemExit(f"{map_name}: {relative} is not in the pinned commit")
            if seeded.get(relative) != pinned[relative]:
                raise SystemExit(f"{map_name}: {relative} is not a seeded CE recording")
            key = int(region["pitch_keycenter"])
            layer = floors.index(level)
            round_robin = seen.get((key, layer), 0)
            seen[(key, layer)] = round_robin + 1
            jobs.append(
                Job(
                    str(source / relative),
                    STEREO,
                    mode,
                    instrument,
                    articulation,
                    key=key,
                    layer=layer,
                    round_robin=round_robin,
                )
            )
    return jobs


# ---- Iowa: pinned hashes ---------------------------------------------------------------------


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(HASH_CHUNK_BYTES):
            digest.update(chunk)
    return digest.hexdigest()


def iowa_url(name: str) -> str:
    instrument = name.split(".", 1)[0]
    return IOWA_SITE + urllib.parse.quote(IOWA_FOLDERS[instrument] + name)


def fetch_iowa(source: Path) -> None:
    """Download any run `source` lacks (or holds with the wrong hash)."""
    source.mkdir(parents=True, exist_ok=True)
    for name, expected in IOWA_SOURCES.items():
        path = source / name
        if path.is_file() and sha256(path) == expected:
            continue
        print(f"  downloading {name}")
        partial = path.with_name(name + ".partial")
        with (
            urllib.request.urlopen(iowa_url(name), timeout=DOWNLOAD_TIMEOUT_SECONDS) as reply,
            partial.open("wb") as output,
        ):
            shutil.copyfileobj(reply, output)
        if sha256(partial) != expected:
            partial.unlink()
            raise SystemExit(f"{name}: the download does not match its pinned SHA-256")
        partial.rename(path)


def key_of(note_name: str) -> int:
    match = NOTE_NAME.fullmatch(note_name)
    if match is None:
        raise SystemExit(f"cannot read the note name {note_name!r}")
    letter, accidental, octave = match.groups()
    shift = {"#": 1, "b": -1, "": 0}[accidental]
    return (int(octave) + 1) * SEMITONES_PER_OCTAVE + PITCH_CLASSES[letter] + shift


def iowa_jobs(source: Path) -> list[Job]:
    jobs = []
    for name, expected in IOWA_SOURCES.items():
        path = source / name
        if not path.is_file() or sha256(path) != expected:
            raise SystemExit(f"{name} is missing or does not match its pinned SHA-256")
        match = IOWA_NAME.fullmatch(name)
        if match is None:
            raise SystemExit(f"cannot read the file name {name}")
        family, technique, dyn_a, string_a, string_b, dyn_b, first, last = match.groups()
        first_key = key_of(first)
        jobs.append(
            Job(
                str(path),
                MONO,
                "solo",
                IOWA_INSTRUMENTS[family],
                IOWA_ARTICULATIONS[technique],
                layer=IOWA_LAYERS[dyn_a or dyn_b],
                run_first=first_key,
                run_last=key_of(last) if last else first_key,
                string=string_a or string_b,
            )
        )
    return jobs


# ---- Signal helpers --------------------------------------------------------------------------


def decode(path: str, channels: int) -> list[array.array]:
    """The recording as one array of doubles per channel (both sources are 44.1 kHz)."""
    raw = subprocess.run(
        [
            "ffmpeg",
            "-nostdin",
            "-v",
            "error",
            "-i",
            path,
            "-af",
            "aformat=sample_fmts=dbl",
            "-ac",
            str(channels),
            "-ar",
            str(RATE),
            "-f",
            "f64le",
            "-c:a",
            "pcm_f64le",
            "-",
        ],
        capture_output=True,
        check=True,
    ).stdout
    values = array.array("d")
    values.frombytes(raw)
    if sys.byteorder != "little":
        values.byteswap()
    if channels == MONO:
        return [values]
    return [values[channel::channels] for channel in range(channels)]


def mix(signal: list[array.array]) -> array.array:
    if len(signal) == MONO:
        return signal[0]
    return array.array("d", map(lambda a, b: 0.5 * (a + b), signal[0], signal[1]))


def power_db(mean_square: float) -> float:
    return POWER_DECIBELS_PER_DECADE * math.log10(mean_square + TINY_POWER)


def percentile(values: list[float], percent: float) -> float:
    ordered = sorted(values)
    position = percent / PERCENT * (len(ordered) - 1)
    low = math.floor(position)
    high = min(low + 1, len(ordered) - 1)
    return ordered[low] + (ordered[high] - ordered[low]) * (position - low)


def parabolic(left: float, centre: float, right: float) -> float:
    curvature = left - 2.0 * centre + right
    offset = 0.5 * (left - right) / curvature if curvature != 0.0 else 0.0
    return max(-HALF_STEP, min(HALF_STEP, offset))


def hz_of_key(key: float) -> float:
    return A4_HZ * 2.0 ** ((key - A4_KEY) / SEMITONES_PER_OCTAVE)


def key_of_hz(hz: float) -> float:
    return A4_KEY + SEMITONES_PER_OCTAVE * math.log2(hz / A4_HZ)


def correlation(signal, a: int, b: int, length: int) -> float:
    view = memoryview(signal)
    first, second = view[a : a + length], view[b : b + length]
    cross = math.sumprod(first, second)
    energy = math.sumprod(first, first) * math.sumprod(second, second)
    return cross / math.sqrt(energy) if energy > 0.0 else 0.0


def fundamental(segment, low_key: float, high_key: float) -> float:
    """Pitch between two keys by YIN: the first lag whose cumulative-mean-
    normalised difference dips under YIN_THRESHOLD (else the deepest),
    refined by a parabola. The first dip, not the deepest, keeps a note from
    reading an octave low; the range keeps it from reading a harmonic."""
    count = len(segment)
    mean = math.fsum(segment) / count
    centred = [value - mean for value in segment]
    shortest = max(2, int(RATE / hz_of_key(high_key)))
    longest = int(RATE / hz_of_key(low_key)) + 2
    width = count - longest
    if width <= longest:
        return math.nan
    head = centred[:width]
    head_energy = math.sumprod(head, head)
    normalised = [1.0]
    running = 0.0
    for lag in range(1, longest + 1):
        tail = centred[lag : lag + width]
        difference = head_energy + math.sumprod(tail, tail) - 2.0 * math.sumprod(head, tail)
        running += difference
        normalised.append(difference * lag / running if running > 0.0 else 1.0)
    chosen = min(range(shortest, longest), key=normalised.__getitem__)
    for lag in range(shortest, longest):
        if normalised[lag] < YIN_THRESHOLD:
            while lag + 1 < longest and normalised[lag + 1] < normalised[lag]:
                lag += 1
            chosen = lag
            break
    offset = parabolic(-normalised[chosen - 1], -normalised[chosen], -normalised[chosen + 1])
    return RATE / (chosen + offset)


def measured_key(
    analysis, start: int, end: int, low_key: float, high_key: float, short: bool
) -> float:
    """A note's pitch as a fractional key: a short note's just after its
    attack, a sustained note's in its middle (a bowed low note can take most
    of a second to speak)."""
    length = SHORT_PITCH_WINDOW_FRAMES if short else PITCH_WINDOW_FRAMES
    first = start + SHORT_PITCH_SKIP_FRAMES if short else (start + end - length) // 2
    first = max(0, min(first, len(analysis) - length))
    window = analysis[first : first + length]
    if len(window) < length:
        return math.nan
    hz = fundamental(window, low_key, high_key)
    return key_of_hz(hz) if not math.isnan(hz) else math.nan


def cut(signal: array.array) -> list[tuple[int, int]]:
    """(start, end) frames of each note in a run."""
    view = memoryview(signal)
    hops = len(signal) // CUT_HOP_FRAMES
    levels = []
    for hop in range(hops):
        chunk = view[hop * CUT_HOP_FRAMES : (hop + 1) * CUT_HOP_FRAMES]
        levels.append(power_db(math.sumprod(chunk, chunk) / CUT_HOP_FRAMES))
    floor = percentile(levels, FLOOR_PERCENTILE)
    spans: list[tuple[int, int]] = []
    sounding, first = False, 0
    for hop, level in enumerate(levels):
        if not sounding and level > floor + ONSET_DB_ABOVE_FLOOR:
            sounding, first = True, hop
        elif sounding and level < floor + OFFSET_DB_ABOVE_FLOOR:
            sounding = False
            if (hop - first) * CUT_HOP_FRAMES >= MIN_NOTE_FRAMES:
                spans.append((first, hop))
    if sounding and (hops - first) * CUT_HOP_FRAMES >= MIN_NOTE_FRAMES:
        spans.append((first, hops))
    return [
        (max(0, a * CUT_HOP_FRAMES - CUT_PRE_ROLL_FRAMES), b * CUT_HOP_FRAMES) for a, b in spans
    ]


def prefix_squares(signal) -> list[float]:
    total = 0.0
    out = [0.0]
    for value in signal:
        total += value * value
        out.append(total)
    return out


def window_rms(squares: list[float], start: int, length: int) -> float:
    return math.sqrt(max(0.0, squares[start + length] - squares[start]) / length)


# ---- One note --------------------------------------------------------------------------------


def onset_of(analysis) -> int:
    """The first frame over ONSET_FRACTION of the peak."""
    threshold = max(map(abs, analysis)) * ONSET_FRACTION
    return next(index for index, value in enumerate(analysis) if abs(value) > threshold)


def trim(channels: list[array.array]) -> list[array.array]:
    """From 2 ms before the attack, faded in over those 2 ms."""
    first = max(0, onset_of(mix(channels)) - TRIM_PRE_ROLL_FRAMES)
    out = [channel[first:] for channel in channels]
    for channel in out:
        for index in range(min(TRIM_PRE_ROLL_FRAMES, len(channel))):
            channel[index] *= index / TRIM_PRE_ROLL_FRAMES
    return out


def levels_of(squares: list[float], frames: int, step: int) -> list[float]:
    starts = range(0, frames - ENVELOPE_WINDOW_FRAMES, step)
    return [window_rms(squares, start, ENVELOPE_WINDOW_FRAMES) for start in starts]


def attack_end_of(squares: list[float], frames: int) -> tuple[int, float]:
    """Where the 10 ms RMS first reaches 90 % of the steady level, and that level."""
    levels = levels_of(squares, frames, ENVELOPE_STEP_FRAMES)
    first = int(STEADY_START_FRACTION * len(levels))
    last = max(first + 1, int(STEADY_END_FRACTION * len(levels)))
    steady = statistics.median(levels[first:last])
    for index, level in enumerate(levels):
        if level >= ATTACK_LEVEL_FRACTION * steady:
            return index * ENVELOPE_STEP_FRAMES + ENVELOPE_WINDOW_FRAMES // 2, steady
    return 0, steady


def find_loop(analysis: array.array, squares: list[float], note: Note) -> bool:
    """Loop points in the steady part whose ends match; False when there is no room."""
    frames = len(analysis)
    low = note.attack_end + CROSSFADE_FRAMES
    high = min(int(STEADY_END_FRACTION * frames), low + LOOP_REGION_FRAMES)
    if high - low < MIN_LOOP_FRAMES:
        low = max(CROSSFADE_FRAMES + note.attack_end // 2, CROSSFADE_FRAMES)
        high = frames - FINE_SEARCH_FRAMES - 1
        if high - low < MIN_LOOP_FRAMES:
            return False
    hop = LOOP_HOP_FRAMES
    track_start = low - CROSSFADE_FRAMES
    track = [
        power_db(window_rms(squares, frame, hop) ** 2)
        for frame in range(track_start, high - hop + 1, hop)
    ]
    span = CROSSFADE_FRAMES // hop
    longest = min(MAX_LOOP_FRAMES, high - low) // hop
    shortest = min(MIN_LOOP_FRAMES, high - low) // hop
    scored = []
    for start in range(span, len(track) - shortest):
        for length in range(shortest, min(longest, len(track) - start - 1) + 1):
            end = start + length
            mismatch = (
                sum(abs(track[start - i] - track[end - i]) for i in range(1, span + 1)) / span
            )
            cost = mismatch + SHORTER_LOOP_COST_DB * (1.0 - length / max(1, longest))
            scored.append((cost, start, end))
    scored.sort()
    best = (-math.inf, 0, 0)
    for _, start, end in scored[:COARSE_CANDIDATES]:
        loop_start = track_start + start * hop
        coarse_end = track_start + end * hop
        for loop_end in range(
            max(loop_start + MIN_LOOP_FRAMES // 2, coarse_end - FINE_SEARCH_FRAMES),
            min(frames, coarse_end + FINE_SEARCH_FRAMES),
        ):
            match = correlation(
                analysis,
                loop_start - FINE_WINDOW_FRAMES,
                loop_end - FINE_WINDOW_FRAMES,
                FINE_WINDOW_FRAMES,
            )
            if match > best[0]:
                best = (match, loop_start, loop_end)
    note.loop_match, note.loop_start, note.loop_end = best
    return note.loop_end > note.loop_start


def bake_crossfade(channels: list[array.array], analysis: array.array, note: Note) -> None:
    """Crossfade the frames before loop_end into those before loop_start: equal
    power, corrected for the passages' correlation in blocks."""
    out_start = note.loop_end - CROSSFADE_FRAMES
    in_start = note.loop_start - CROSSFADE_FRAMES
    blocks = max(1, CROSSFADE_FRAMES // CROSSFADE_BLOCK_FRAMES)
    alike = [
        min(
            1.0,
            max(
                LEAST_CORRECTED_CORRELATION,
                correlation(
                    analysis,
                    out_start + index * CROSSFADE_BLOCK_FRAMES,
                    in_start + index * CROSSFADE_BLOCK_FRAMES,
                    CROSSFADE_BLOCK_FRAMES,
                ),
            ),
        )
        for index in range(blocks)
    ]
    gains = []
    for index in range(CROSSFADE_FRAMES):
        position = (index + 1) / CROSSFADE_FRAMES
        fade_out = math.cos(QUARTER_TURN * position)
        fade_in = math.sin(QUARTER_TURN * position)
        r = alike[min(blocks - 1, index // CROSSFADE_BLOCK_FRAMES)]
        power = math.sqrt(fade_out * fade_out + fade_in * fade_in + 2.0 * r * fade_out * fade_in)
        gains.append((fade_out / power, fade_in / power))
    for channel in channels:
        for index, (gain_out, gain_in) in enumerate(gains):
            channel[out_start + index] = (
                gain_out * channel[out_start + index] + gain_in * channel[in_start + index]
            )


def end_short(channels: list[array.array], squares: list[float]) -> list[array.array]:
    """A one-shot ending where it falls SHORT_TAIL_DB under its loudest 10 ms,
    or into the recording's noise floor, whichever comes first."""
    frames = len(channels[0])
    levels = levels_of(squares, frames, ENVELOPE_WINDOW_FRAMES)
    loudest = max(levels) if levels else 0.0
    floor = percentile(levels, FLOOR_PERCENTILE) if levels else 0.0
    threshold = max(
        loudest * 10.0 ** (-SHORT_TAIL_DB / DECIBELS_PER_DECADE),
        floor * 10.0 ** (SHORT_FLOOR_MARGIN_DB / DECIBELS_PER_DECADE),
    )
    last = max((index for index, level in enumerate(levels) if level > threshold), default=0)
    end = min(frames, (last + 1) * ENVELOPE_WINDOW_FRAMES + END_FADE_FRAMES, MAX_SHORT_FRAMES)
    out = [channel[:end] for channel in channels]
    for channel in out:
        for index in range(min(END_FADE_FRAMES, end)):
            channel[end - 1 - index] *= index / END_FADE_FRAMES
    return out


def dither(channels: list[array.array], gain: float, seed: str) -> bytes:
    generator = random.Random(seed)
    uniform = generator.random
    frames = len(channels[0])
    pcm = array.array("h", bytes(BYTES_PER_SAMPLE * frames * len(channels)))
    for index, channel in enumerate(channels):
        pcm[index :: len(channels)] = array.array(
            "h",
            (
                min(
                    INT16_MAX,
                    max(INT16_MIN, round(value * gain * INT16_FULL_SCALE + uniform() - uniform())),
                )
                for value in channel
            ),
        )
    if sys.byteorder != "little":
        pcm.byteswap()
    return pcm.tobytes()


def convert(channels: list[array.array], note: Note) -> bool:
    """Trim, mark, loop or end, measure and dither one note; False when unusable."""
    channels = trim(channels)
    analysis = mix(channels)
    squares = prefix_squares(analysis)
    looped = note.articulation in LOOPED
    if looped:
        note.attack_end, steady = attack_end_of(squares, len(analysis))
        if not find_loop(analysis, squares, note):
            return False
        bake_crossfade(channels, analysis, note)
        channels = [channel[: note.loop_end] for channel in channels]
        analysis = mix(channels)
        squares = prefix_squares(analysis)
        note.level_db = DECIBELS_PER_DECADE * math.log10(
            window_rms(squares, note.attack_end, note.loop_end - note.attack_end) + TINY_POWER
        )
    else:
        channels = end_short(channels, squares)
        analysis = mix(channels)
        squares = prefix_squares(analysis)
        length = min(SHORT_LEVEL_FRAMES, len(analysis) - 1)
        note.level_db = DECIBELS_PER_DECADE * math.log10(
            window_rms(squares, 0, length) + TINY_POWER
        )
    note.frames = len(channels[0])
    note.peak = max(max(map(abs, channel)) for channel in channels)
    gain = NORMALISED_PEAK / note.peak
    note.scale = 1.0 / (gain * INT16_FULL_SCALE)
    note.pcm = dither(
        channels,
        gain,
        f"{note.mode} {note.instrument} {note.articulation} {note.key} {note.layer} "
        f"{note.round_robin} {Path(note.source).name}",
    )
    return True


# ---- One source file -------------------------------------------------------------------------


def process_vsco(job: Job) -> Result:
    result = Result(job.path)
    channels = decode(job.path, job.channels)
    note = Note(
        job.channels,
        job.mode,
        job.instrument,
        job.articulation,
        job.key,
        job.layer,
        job.round_robin,
        job.path,
    )
    analysis = mix(channels)
    exact = measured_key(
        analysis,
        onset_of(analysis),
        len(analysis),
        job.key - PITCH_SEARCH_SEMITONES,
        job.key + PITCH_SEARCH_SEMITONES,
        job.articulation not in LOOPED,
    )
    if math.isnan(exact) or abs(exact - job.key) > MAX_KEY_ERROR:
        result.dropped.append(f"{Path(job.path).name}: measures as key {exact:.2f}, not {job.key}")
        return result
    note.cents = CENTS_PER_SEMITONE * (exact - job.key)
    if convert(channels, note):
        result.kept.append(note)
    else:
        result.dropped.append(f"{Path(job.path).name}: too short to loop")
    return result


def process_iowa(job: Job) -> Result:
    result = Result(job.path)
    name = Path(job.path).name
    channels = decode(job.path, MONO)
    signal = channels[0]
    spans = cut(signal)
    short = job.articulation not in LOOPED
    by_position = len(spans) == job.run_last - job.run_first + 1
    open_key = OPEN_STRINGS[job.instrument][job.string]
    for index, (start, end) in enumerate(spans):
        key, exact = 0, math.nan
        if by_position:
            key = job.run_first + index
            exact = measured_key(
                signal,
                start,
                end,
                key - PITCH_SEARCH_SEMITONES,
                key + PITCH_SEARCH_SEMITONES,
                short,
            )
            if math.isnan(exact) or abs(exact - key) > MAX_KEY_ERROR:
                exact = math.nan
        if math.isnan(exact):
            # Out of place, or a cut that does not match the name's span: the
            # key the note measures as, within the run.
            exact = measured_key(
                signal,
                start,
                end,
                job.run_first - PITCH_SEARCH_SEMITONES,
                job.run_last + PITCH_SEARCH_SEMITONES,
                short,
            )
            key = round(exact) if not math.isnan(exact) else 0
            if (
                math.isnan(exact)
                or abs(exact - key) > MEASURED_KEY_ERROR
                or not job.run_first - 1 <= key <= job.run_last + 1
            ):
                result.dropped.append(f"{name} note {index + 1}: no clear key")
                continue
        note = Note(
            MONO,
            job.mode,
            job.instrument,
            job.articulation,
            key,
            job.layer,
            0,
            job.path,
            cents=CENTS_PER_SEMITONE * (exact - key),
            position=key - open_key,
            interior=0 < index < len(spans) - 1,
        )
        if convert([signal[start:end]], note):
            result.kept.append(note)
        else:
            result.dropped.append(f"{name} note {index + 1} (key {key}): too short to loop")
    return result


def process(job: Job) -> Result:
    return process_iowa(job) if job.run_first else process_vsco(job)


# ---- The library -----------------------------------------------------------------------------


def choose(notes: list[Note]) -> tuple[list[Note], int]:
    """One Iowa take per (instrument, articulation, key, layer): the one nearest
    its open string, inside its run first."""
    groups: dict[tuple, list[Note]] = {}
    for note in notes:
        groups.setdefault(
            (*note.set_key, note.key, note.layer, note.round_robin, note.channels), []
        ).append(note)
    kept, duplicates = [], 0
    for group in groups.values():
        group.sort(key=lambda note: (note.position < 0, not note.interior, note.position))
        kept.append(group[0])
        duplicates += len(group) - 1
    kept.sort(
        key=lambda n: (
            n.channels,
            MODES.index(n.mode),
            INSTRUMENTS.index(n.instrument),
            ARTICULATIONS.index(n.articulation),
            n.key,
            n.layer,
            n.round_robin,
        )
    )
    return kept, duplicates


def level_sets(notes: list[Note]) -> None:
    """Scale each set so its loudest layer's median level is the target."""
    sets: dict[tuple[str, str, str], list[Note]] = {}
    for note in notes:
        sets.setdefault(note.set_key, []).append(note)
    for (mode, instrument, articulation), members in sets.items():
        top = max(note.layer for note in members)
        reference = statistics.median(note.level_db for note in members if note.layer == top)
        shift_db = TARGET_LEVEL_DB[articulation] - reference
        gain = 10.0 ** (shift_db / DECIBELS_PER_DECADE)
        for note in members:
            note.scale *= gain
            note.level_db += shift_db
        layers = sorted({note.layer for note in members})
        spread = [
            statistics.median(note.level_db for note in members if note.layer == layer)
            for layer in layers
        ]
        keys = sorted({note.key for note in members})
        print(
            f"  {mode:7} {instrument:6} {articulation:9} {len(members):3} samples, keys "
            f"{keys[0]}-{keys[-1]}, layers "
            + " ".join(f"{level:+.1f}" for level in spread)
            + f" dB (shifted {shift_db:+.1f})"
        )


def write_packs(notes: list[Note], target: Path) -> dict[int, int]:
    sizes = {}
    for channels, pack_name in PACKS.items():
        members = [note for note in notes if note.channels == channels]
        frame_bytes = BYTES_PER_SAMPLE * channels
        offset_bytes = 0
        for note in members:
            note.offset = offset_bytes // frame_bytes
            offset_bytes += (len(note.pcm) + ALIGN_BYTES - 1) // ALIGN_BYTES * ALIGN_BYTES
        with (target / pack_name).open("wb") as pack:
            for note in members:
                pack.seek(note.offset * frame_bytes)
                pack.write(note.pcm)
            pack.truncate(offset_bytes)
            pack.flush()
            os.fsync(pack.fileno())
        sizes[channels] = offset_bytes
    return sizes


def build(vsco: Path, iowa: Path, target: Path, jobs: int) -> None:
    work = vsco_jobs(vsco) + iowa_jobs(iowa)
    print(f"converting {len(work)} recordings")
    with concurrent.futures.ProcessPoolExecutor(jobs) as pool:
        results = list(pool.map(process, work, chunksize=1))
    notes = [note for result in results for note in result.kept]
    dropped = [line for result in results for line in result.dropped]
    inside = [
        note
        for note in notes
        if INSTRUMENT_RANGES[note.instrument][0]
        <= note.key
        <= INSTRUMENT_RANGES[note.instrument][1]
    ]
    outside = len(notes) - len(inside)
    notes = inside
    print(f"{outside} takes outside their instrument's range left out")
    notes, duplicates = choose(notes)
    print(f"{len(notes)} samples kept, {duplicates} duplicate takes, {len(dropped)} dropped")
    for line in dropped:
        print(f"  dropped {line}")
    level_sets(notes)
    target.mkdir(parents=True)
    sizes = write_packs(notes, target)
    lines = [
        FORMAT_LINE,
        f"name {NAME}",
        f"attribution {ATTRIBUTION}",
        f"rate {RATE}",
        f"channels {STEREO}",
        f"pack {PACKS[STEREO]} {sizes[STEREO]}",
        f"mono_pack {PACKS[MONO]} {sizes[MONO]}",
    ]
    for n in notes:
        lines.append(
            " ".join(
                str(value)
                for value in (
                    "sample",
                    n.channels,
                    n.mode,
                    n.instrument,
                    n.articulation,
                    n.key,
                    n.layer,
                    n.round_robin,
                    n.offset,
                    n.frames,
                    n.attack_end,
                    n.loop_start,
                    n.loop_end,
                    f"{n.cents:.2f}",
                    f"{n.scale:.9e}",
                    f"{n.level_db:.2f}",
                )
            )
        )
    (target / MANIFEST).write_text("\n".join(lines) + "\n")
    (target / INSTALLED_VSCO_LICENCE).write_text(
        git(vsco, "show", f"{VSCO_SAMPLES_COMMIT}:{VSCO_LICENCE}")
    )
    (target / ATTRIBUTION_FILE).write_text(
        "VSCO 2 Community Edition, recorded by Sam Gossner and Simon Dalzell for\n"
        f"Versilian Studios: CC0 1.0 ({INSTALLED_VSCO_LICENCE}). Source: {VSCO_URL}, the\n"
        f"string folders at commit {VSCO_SAMPLES_COMMIT}, mapped by the SFZ branch at\n"
        f"{VSCO_SFZ_COMMIT}; every sample is the recording seeded in {VSCO_SEED_COMMIT}.\n\n"
        "University of Iowa Musical Instrument Samples, recorded by Lawrence Fritts,\n"
        f"Electronic Music Studios. The MIS page ({IOWA_INDEX_URL}) says the recordings\n"
        f'"{IOWA_TERMS}". There is no formal licence. Source: the pre-2012 violin,\n'
        "viola, cello and double bass runs, each checked against a pinned SHA-256.\n\n"
        "Converted for Oscillate's Bow: Iowa runs cut into single notes, attack pre-roll\n"
        "trimmed to 2 ms, a loop crossfade baked into the steady part of each sustained\n"
        "note, levels set per articulation, dithered to 16-bit; the sound is otherwise\n"
        "unchanged.\n"
    )
    loops = [(n.loop_end - n.loop_start) / RATE for n in notes if n.loop_end]
    matches = [n.loop_match for n in notes if n.loop_end]
    cents = [abs(n.cents) for n in notes]
    print(
        f"  loops {min(loops):.2f}-{max(loops):.2f} s (median {statistics.median(loops):.2f}), "
        f"passage correlation min {min(matches):.2f}, median {statistics.median(matches):.2f}; "
        f"|cents| median {statistics.median(cents):.1f}, max {max(cents):.1f}"
    )
    total = sum(sizes.values())
    print(f"packs: {total} bytes ({total / 2**20:.1f} MiB), {len(notes)} samples")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument(
        "--destination",
        type=Path,
        default=data_directory(),
        help="where Bow reads its library (default: %(default)s)",
    )
    parser.add_argument(
        "--source",
        type=Path,
        help="a folder holding vsco/ (the git checkout) and iowa/ (the AIFF runs)",
    )
    parser.add_argument("--keep-source", action="store_true", help="keep the downloaded files")
    parser.add_argument(
        "--jobs", type=int, default=DEFAULT_JOBS, help="files converted in parallel"
    )
    arguments = parser.parse_args()
    for tool in ("ffmpeg", "git"):
        if shutil.which(tool) is None:
            raise SystemExit(f"{tool} is required")
    library = arguments.destination.resolve()
    library.parent.mkdir(parents=True, exist_ok=True)
    source = (arguments.source or library.parent / DEFAULT_SOURCE_NAME).resolve()
    print(f"fetching the sources into {source}")
    fetch_vsco(source / "vsco")
    fetch_iowa(source / "iowa")
    partial = library.with_name(library.name + ".partial")
    if partial.exists():
        shutil.rmtree(partial)
    build(source / "vsco", source / "iowa", partial, max(1, arguments.jobs))
    previous = library.with_name(library.name + ".previous")
    if previous.exists():
        shutil.rmtree(previous)
    if library.exists():
        library.rename(previous)
    partial.rename(library)
    if previous.exists():
        shutil.rmtree(previous)
    if not arguments.keep_source and arguments.source is None:
        shutil.rmtree(source)
    print(f"installed {NAME} in {library}\n{ATTRIBUTION}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
