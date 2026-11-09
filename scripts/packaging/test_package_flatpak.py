#!/usr/bin/env python3
"""The Flatpak manifest, its Flathub form, staged documents, source offer and inventory."""

import json
import re
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import package_flatpak
from package_flatpak import (
    LIBRARY_ELF,
    LICENCE_DIR,
    LOCAL_COMMIT,
    LOCAL_VERSION_ARGS,
    MANIFEST,
    METAINFO,
    OFFER_JSON,
    OFFER_TEXT,
    OTHER_ELF,
    PROFILE,
    PROJECT_ELF,
    build_offer,
    classify_elf,
    concrete_manifest,
    git_source,
    inventory,
    load_manifest,
    offer_problems,
    offer_text,
    parse_elf_report,
    patch_paths,
    project_module,
    runtime_ref,
    stage,
    stage_command,
    version_args,
    write_flathub,
)
from package_profiles import (
    APP_RELATIVE,
    FLATPAK_APP_ID,
    FLATPAK_RUNTIME,
    FLATPAK_RUNTIME_VERSION,
    HELPER_RELATIVE,
    SOURCE_ROOT,
)
from package_sbom import PROJECT_URL

COMMIT = "a" * 40
TAG = "v1.2.3"
PROJECT_VERSION = "1.2.3"
SUFFIX = "ci.1.gabcdef0"
DOC = SOURCE_ROOT / "docs/dependencies/flatpak.md"
LIBSNDFILE = "libsndfile"
LIBSNDFILE_SONAME = "libsndfile.so.1"
LIBSNDFILE_FILE = "lib/libsndfile.so.1.0.37"
LICENCE_TEXT = "GNU LESSER GENERAL PUBLIC LICENSE\n"
RUNTIME_METADATA = (
    "[Application]\n"
    f"name={FLATPAK_APP_ID}\n"
    f"runtime={FLATPAK_RUNTIME}/x86_64/{FLATPAK_RUNTIME_VERSION}\n"
)
APP_LDD = (
    "--- ldd\n"
    "\tlinux-vdso.so.1 (0x00007ffd)\n"
    "\tlibsndfile.so.1 => /app/lib/libsndfile.so.1 (0x00007f01)\n"
    "\tlibQt6Core.so.6 => /usr/lib/x86_64-linux-gnu/libQt6Core.so.6 (0x00007f02)\n"
)
LIBRARY_REPORT = f"{LIBSNDFILE_SONAME}\n--- ldd\n\tlibmp3lame.so.0 => /usr/lib/libmp3lame.so.0\n"


class ManifestTests(unittest.TestCase):
    def setUp(self):
        self.manifest = load_manifest()

    def test_manifest_names_the_app_and_the_profile_runtime(self):
        self.assertEqual(self.manifest["id"], FLATPAK_APP_ID)
        self.assertEqual(self.manifest["runtime"], FLATPAK_RUNTIME)
        self.assertEqual(self.manifest["runtime-version"], FLATPAK_RUNTIME_VERSION)
        self.assertEqual(MANIFEST.name, f"{FLATPAK_APP_ID}.json")

    def test_the_source_tree_manifest_builds_with_the_profile_preset_and_stages_locally(self):
        commands = project_module(self.manifest)["build-commands"]
        self.assertTrue(commands[0].startswith(f"cmake --preset {PROFILE.preset} "))
        self.assertEqual(commands[-1], stage_command(LOCAL_COMMIT, LOCAL_VERSION_ARGS))

    def test_every_bundled_module_is_built_by_the_manifest(self):
        names = {module["name"] for module in self.manifest["modules"]}
        self.assertLessEqual(set(PROFILE.runtime.bundled), names)

    def test_every_patch_exists(self):
        patches = patch_paths(self.manifest)
        self.assertTrue(patches)
        for relative in patches:
            with self.subTest(patch=relative):
                self.assertTrue((MANIFEST.parent / relative).is_file())

    def test_every_remote_source_is_pinned(self):
        for module in self.manifest["modules"]:
            for source in module["sources"]:
                with self.subTest(module=module["name"], source=source.get("url")):
                    if source["type"] == "archive":
                        self.assertRegex(source["sha256"], r"^[0-9a-f]{64}$")
                    elif source["type"] == "git":
                        self.assertRegex(source["commit"], r"^[0-9a-f]{40}$")

    def test_every_permission_is_documented(self):
        text = DOC.read_text()
        for permission in self.manifest["finish-args"]:
            with self.subTest(permission=permission):
                self.assertIn(f"| `{permission}` |", text)

    def test_the_desktop_file_and_icon_it_renames_exist(self):
        assets = SOURCE_ROOT / "assets/packaging"
        self.assertTrue((assets / self.manifest["rename-desktop-file"]).is_file())
        self.assertTrue((assets / f"{self.manifest['rename-icon']}.svg").is_file())


class ConcreteManifestTests(unittest.TestCase):
    def test_the_flathub_form_builds_the_commit_and_leaves_the_rest(self):
        template = load_manifest()
        manifest = concrete_manifest(
            template,
            project_source=git_source(COMMIT, TAG),
            commit=COMMIT,
            versions=version_args(TAG, None),
        )
        module = project_module(manifest)
        self.assertEqual(
            module["sources"][0],
            {"type": "git", "url": f"{PROJECT_URL}.git", "tag": TAG, "commit": COMMIT},
        )
        self.assertEqual(
            module["build-commands"][-1], stage_command(COMMIT, ("--release-tag", TAG))
        )
        self.assertEqual(module["sources"][1:], project_module(template)["sources"][1:])
        self.assertEqual(project_module(template)["sources"][0]["type"], "dir")
        unchanged = [name for name in manifest if name != "modules"]
        self.assertEqual(
            {name: manifest[name] for name in unchanged},
            {name: template[name] for name in unchanged},
        )

    def test_exactly_one_version_is_required(self):
        for tag, suffix in ((None, None), (TAG, SUFFIX)):
            with self.subTest(tag=tag, suffix=suffix), self.assertRaises(ValueError):
                version_args(tag, suffix)

    def test_a_manifest_without_the_stage_step_is_rejected(self):
        template = load_manifest()
        project_module(template)["build-commands"].pop()
        with self.assertRaisesRegex(ValueError, "stage step"):
            concrete_manifest(
                template,
                project_source=git_source(COMMIT),
                commit=COMMIT,
                versions=version_args(None, SUFFIX),
            )


class FlathubTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="oscillate-flathub-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.metainfo = self.root / "metainfo.xml"
        self.metainfo.write_text(
            f'<component><releases><release version="{PROJECT_VERSION}"/></releases></component>'
        )

    def test_submission_has_the_manifest_its_patches_and_flathub_json(self):
        output = write_flathub(
            self.root / "out",
            tag=TAG,
            commit=COMMIT,
            project=PROJECT_VERSION,
            metainfo=self.metainfo,
        )
        manifest = json.loads(output.read_text())
        self.assertEqual(project_module(manifest)["sources"][0]["commit"], COMMIT)
        for relative in patch_paths(manifest):
            self.assertTrue((output.parent / relative).is_file())
        flathub = json.loads((output.parent / "flathub.json").read_text())
        self.assertEqual(flathub, {"only-arches": ["x86_64"]})

    def test_a_tag_that_is_not_the_project_version_is_refused(self):
        with self.assertRaisesRegex(ValueError, "does not match"):
            write_flathub(
                self.root,
                tag="v9.9.9",
                commit=COMMIT,
                project=PROJECT_VERSION,
                metainfo=self.metainfo,
            )

    def test_a_version_without_release_notes_is_refused(self):
        self.metainfo.write_text("<component><releases/></component>")
        with self.assertRaisesRegex(ValueError, "no <release"):
            write_flathub(
                self.root, tag=TAG, commit=COMMIT, project=PROJECT_VERSION, metainfo=self.metainfo
            )


class MetainfoTests(unittest.TestCase):
    def setUp(self):
        self.root = ET.parse(METAINFO).getroot()

    def test_ids_match_the_app(self):
        self.assertEqual(self.root.findtext("id"), FLATPAK_APP_ID)
        self.assertEqual(self.root.findtext("launchable"), f"{FLATPAK_APP_ID}.desktop")

    def test_screenshots_are_images_in_the_tagged_tree(self):
        pattern = re.compile(
            r"^https://raw\.githubusercontent\.com/jcsduncan/oscillate/v[0-9.]+/(docs/images/.+)$"
        )
        images = [image.text for image in self.root.iter("image")]
        self.assertTrue(images)
        for url in images:
            with self.subTest(url=url):
                match = pattern.match(url)
                self.assertIsNotNone(match)
                self.assertTrue((SOURCE_ROOT / match.group(1)).is_file())


class OfferTests(unittest.TestCase):
    def setUp(self):
        self.offer = build_offer(load_manifest(), version=PROJECT_VERSION, commit=COMMIT)

    def test_offer_lists_every_built_module_pinned(self):
        modules = {module["module"]: module["sources"] for module in self.offer["modules"]}
        self.assertLessEqual(set(PROFILE.runtime.bundled), set(modules))
        patches = [source for source in modules[LIBSNDFILE] if source["type"] == "patch"]
        self.assertTrue(patches)
        for patch in patches:
            self.assertEqual(patch["url"], f"{PROJECT_URL}/blob/{COMMIT}/{patch['path']}")
        self.assertEqual(offer_problems(self.offer, set(PROFILE.runtime.bundled)), [])

    def test_text_names_every_source(self):
        text = offer_text(self.offer)
        self.assertIn(f"{PROJECT_URL}/archive/{COMMIT}.tar.gz", text)
        for module in self.offer["modules"]:
            for source in module["sources"]:
                self.assertIn(source["url"], text)

    def test_missing_or_unpinned_modules_are_reported(self):
        self.offer["modules"] = [
            module for module in self.offer["modules"] if module["module"] != LIBSNDFILE
        ]
        self.offer["modules"][0]["sources"][0].pop("sha256", None)
        self.offer["modules"][0]["sources"][0].pop("commit", None)
        problems = offer_problems(self.offer, {LIBSNDFILE})
        self.assertEqual(len(problems), 2)
        self.assertIn("unpinned", problems[0])
        self.assertIn(f"no source entry for {LIBSNDFILE}", problems[1])


class StageTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="oscillate-flatpak-stage-")
        self.addCleanup(self.temporary.cleanup)
        root = Path(self.temporary.name)
        self.build = root / "build"
        self.prefix = root / "app"
        values = dict(PROFILE.switches)
        values["CMAKE_HOME_DIRECTORY"] = str(SOURCE_ROOT)
        values["CMAKE_PROJECT_VERSION"] = PROJECT_VERSION
        self.build.mkdir()
        (self.build / "CMakeCache.txt").write_text(
            "\n".join(f"{key}:STRING={value}" for key, value in values.items()) + "\n"
        )
        for relative in (APP_RELATIVE, HELPER_RELATIVE):
            (self.build / relative).parent.mkdir(parents=True, exist_ok=True)
            (self.build / relative).write_bytes(b"\x7fELF")
        for executable in package_flatpak.PROJECT_EXECUTABLES:
            (self.prefix / executable).parent.mkdir(parents=True, exist_ok=True)
            (self.prefix / executable).write_bytes(b"\x7fELF")
        self.licence = self.prefix / LICENCE_DIR / LIBSNDFILE / "COPYING"
        self.licence.parent.mkdir(parents=True)
        self.licence.write_text(LICENCE_TEXT)

    def stage(self):
        return stage(self.build, self.prefix, commit=COMMIT, release_tag=TAG, version_suffix=None)

    def test_stages_readme_notices_and_offer(self):
        offer = self.stage()
        doc = self.prefix / "share/doc/oscillate"
        self.assertIn(f"Oscillate {PROJECT_VERSION} (flatpak)", (doc / "README").read_text())
        self.assertNotIn("sbom", (doc / "README").read_text())
        notices = (doc / "third-party/NOTICES.txt").read_text()
        self.assertIn(LICENCE_TEXT, notices)
        self.assertIn("vst3sdk", notices)
        self.assertIn("/share/doc/oscillate/third-party/", notices)
        self.assertEqual(json.loads((self.prefix / OFFER_JSON).read_text()), offer)
        self.assertEqual((self.prefix / OFFER_TEXT).read_text(), offer_text(offer))

    def test_a_bundled_module_without_a_licence_is_refused(self):
        self.licence.unlink()
        with self.assertRaisesRegex(ValueError, LIBSNDFILE):
            self.stage()

    def test_an_uninstalled_app_is_refused(self):
        (self.prefix / package_flatpak.PROJECT_EXECUTABLES[0]).unlink()
        with self.assertRaisesRegex(ValueError, "not installed"):
            self.stage()


class InventoryTests(unittest.TestCase):
    def test_elf_reports_are_parsed(self):
        soname, resolved = parse_elf_report(LIBRARY_REPORT)
        self.assertEqual(soname, LIBSNDFILE_SONAME)
        self.assertEqual(resolved, {"libmp3lame.so.0": "/usr/lib/libmp3lame.so.0"})
        soname, resolved = parse_elf_report(APP_LDD + "\tlibgone.so.2 => not found\n")
        self.assertIsNone(soname)
        self.assertEqual(resolved["libsndfile.so.1"], "/app/lib/libsndfile.so.1")
        self.assertEqual(resolved["libgone.so.2"], "not found")

    def test_elfs_are_classified(self):
        self.assertEqual(classify_elf("bin/oscillate", None)["kind"], PROJECT_ELF)
        library = classify_elf(LIBSNDFILE_FILE, LIBSNDFILE_SONAME)
        self.assertEqual((library["kind"], library["module"]), (LIBRARY_ELF, LIBSNDFILE))
        self.assertEqual(classify_elf("lib/libother.so.1", "libother.so.1")["kind"], OTHER_ELF)

    def test_runtime_ref_comes_from_the_metadata(self):
        self.assertEqual(runtime_ref(RUNTIME_METADATA), PROFILE.runtime.system[0])

    def test_inventory_of_a_finished_build(self):
        with tempfile.TemporaryDirectory(prefix="oscillate-flatpak-inventory-") as temporary:
            build = Path(temporary)
            files = build / "files"
            (build / "metadata").write_text(RUNTIME_METADATA)
            for relative in (*package_flatpak.PROJECT_EXECUTABLES, LIBSNDFILE_FILE):
                (files / relative).parent.mkdir(parents=True, exist_ok=True)
                (files / relative).write_bytes(b"\x7fELF")
            (files / "lib/libsndfile.so.1").symlink_to("libsndfile.so.1.0.37")
            offer = build_offer(load_manifest(), version=PROJECT_VERSION, commit=COMMIT)
            (files / OFFER_JSON).parent.mkdir(parents=True)
            (files / OFFER_JSON).write_text(json.dumps(offer))

            def report(_build, _manifest, relative):
                if relative == LIBSNDFILE_FILE:
                    return LIBRARY_REPORT
                return APP_LDD + "\tlibgone.so.2 => not found\n"

            sidecar = inventory(build, MANIFEST, report)
        self.assertEqual(sidecar["version"], PROJECT_VERSION)
        self.assertEqual(sidecar["runtime"], PROFILE.runtime.system[0])
        kinds = {record["path"]: record["kind"] for record in sidecar["elf_files"]}
        self.assertEqual(kinds[LIBSNDFILE_FILE], LIBRARY_ELF)
        self.assertEqual(kinds["bin/oscillate"], PROJECT_ELF)
        self.assertNotIn("lib/libsndfile.so.1", kinds)
        self.assertEqual(sidecar["unresolved"]["bin/oscillate"], ["libgone.so.2"])
        links = [item for item in sidecar["payload"] if "symlink" in item]
        self.assertEqual(
            links, [{"path": "lib/libsndfile.so.1", "symlink": "libsndfile.so.1.0.37"}]
        )


if __name__ == "__main__":
    unittest.main()
