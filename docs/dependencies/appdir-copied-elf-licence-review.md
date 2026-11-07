# Copied AppDir ELF licence evidence

**Later packaging change:** The staging script now copies the installed copyright file for every copied ELF's binary package and embeds the file-to-notice mapping in the AppDir. The committed ledger and counts below describe the earlier AppDir that contained no notices. See [release packages](release-packages.md); source, SPDX and LGPL replacement review remain open.

**Status:** Local Ubuntu 26.04 x86-64 evidence, 15 October 2026. **No distribution sign-off.** This audit maps the exact local AppImage prototype payload to installed Ubuntu binary/source packages and their copyright files. It does not decide a final SPDX expression for any linked ELF or supply the notices and source needed for publication.

## Reproducible inventory

The companion [JSON ledger](appdir-copied-elf-licence-ledger.json) has one entry per copied ELF, with payload path and SHA-256, binary package, source package and version, installed copyright path and SHA-256, a `Files: *` licence **clue** when the copyright file is machine-readable, and explicit review flags. A package section preserves all short licence labels and any structured upstream `Source:` URL. The labels are source-package declarations; they are **not** an automatic claim that every label is present in a given compiled `.so` or that a broad `Files: *` stanza overrides more specific files. [Debian's copyright format](https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/) gives `Files` stanzas source-file scope and makes this distinction necessary. Ubuntu installed copyright files are used as the primary evidence for the actual Ubuntu package build; hashes prevent silent substitution during this local replay.

The tested input is the AppDir at tree SHA-256 `4c886a7f5b99be79bf7c7a38defab7164b257486b60055a8240bc14c4955d067`, first staged for project commit `16397d9`. For this audit, the preserved image was extracted and its sibling Quick executable and VST3 scan helper were restaged on the **same Ubuntu host**. The restaged file-and-mode tree hash matched that earlier value exactly. `scripts/packaging/appdir_licence_ledger.py` validates every staged file, mode and hash against the staging inventory; checks each copied host source byte hash; checks `dpkg-query` ownership, binary version and source-package metadata; hashes its installed copyright file; then generates the ledger. `--verify` recomputes the ledger and fails on drift. The tool deliberately refuses a missing package, missing copyright, unsafe inventory path or changed bytes.

```sh
python3 scripts/packaging/appdir_licence_ledger.py \
  --appdir build/audit-source.AppDir \
  --inventory build/audit-source.AppDir.inventory.json \
  --ledger docs/dependencies/appdir-copied-elf-licence-ledger.json \
  --verify
python3 -m unittest discover -s scripts/packaging -p 'test_*.py' -v
```

The committed ledger describes the prototype AppDir, which bundled the codec chain. The release [staging script](../../scripts/stage-appdir.py) leaves the codec adapter out and refuses `libmp3lame.so.0` ([release packages](release-packages.md)), so it cannot reproduce that AppDir. To replay it, use `scripts/stage-appdir-prototype.py` from revision `a93d4eb` with `--binary` and `--scanner-helper` set to the extracted image's `usr/bin/oscillate` and `oscillate_vst3_scan_helper`. A replay depends on the same installed Ubuntu package versions and can fail intentionally after an OS package update. Do not overwrite the committed ledger to make a drift failure disappear; inspect the changed package and image first.

## Result and review queue

| Evidence | Observed result |
| --- | ---: |
| Copied shared libraries | 111 |
| Bundled Qt plugins | 4 |
| Installed binary packages / source packages | 102 / 67 |
| Missing installed copyright files or mismatched source bytes | 0 |
| Copied-file package copyright notices present in AppDir | 0 |
| Files with non-machine-readable copyright text | 27 |
| Files without any machine-readable `License:` label | 26 |
| Files with more than one source-package licence label | 79 |
| Files with an LGPL text/label review signal | 48 |

The four Qt plugins are `libqoffscreen.so`, `libqxcb.so` and the ibus input-context plugin from `libqt6gui6`, plus `libqwayland.so` from `libqt6waylandclient6`. The ledger maps **17 Qt files** overall. Their Ubuntu source packages are `qt6-base` and `qt6-declarative`, with the installed copyright files showing a broad `LGPL-3 or GPL-2` clue and many specific source-file declarations. [Qt's own LGPL guidance](https://www.qt.io/development/open-source-lgpl-obligations) requires licence/use notice, source availability and a way to replace/relink LGPL components and run the modified result. The local AppImage extraction test did not replace a Qt library or plugin, so that gate stays open.

The codec path maps `libsndfile.so.1` to source package `libsndfile` 1.2.2-4, `libmpg123.so.0` to `mpg123` 1.33.3-2, and `libmp3lame.so.0` to `lame` 3.101~svn6525+dfsg-2. Their broad copyright clues include LGPL terms; specific embedded or linked subcomponents still require review. The [MP3/FLAC redistribution gate](mp3-flac-redistribution-gate.md) retains the LAME `fft.c` uncertainty. `libFLAC.so.14` has a broad source-package clue of `GPL-2+ or LGPL-2.1+`, illustrating why the ledger must not be reported as an exact binary licence; its more specific declarations and actual compiled file set need inspection.

## What this evidence does not close

The AppDir contains **none** of the installed package copyright files. The ledger points to each notice and hashes it, but does not assemble a distribution notice bundle, common licence texts, corresponding source archives or written offers. Some packages use prose copyright text; even DEP-5 files can contain multiple grants, exceptions and build-dependent third-party code. The tool flags these rather than inventing one SPDX expression. Exact compiled-source mapping, Qt and other LGPL replacement procedure, licence obligations for the statically linked type-2 runtime, product code/starter-sound grants and X11/Wayland clean-target tests remain open. This is an engineering inventory to drive that manual review, not legal advice or a release approval.
