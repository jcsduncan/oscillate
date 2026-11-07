#!/usr/bin/env python3
"""Small contract tests for the local native-package feature gate."""

import importlib.util
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "package-ubuntu-prototype.py"
SPEC = importlib.util.spec_from_file_location("ubuntu_package", SCRIPT)
assert SPEC and SPEC.loader
PACKAGE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PACKAGE)


class PackageProfileTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="oscillate-package-gate-")
        self.addCleanup(self.temporary.cleanup)
        self.build = Path(self.temporary.name)
        self.binary = self.build / "apps/daw_quick/oscillate"
        self.helper = self.build / "apps/daw_quick/oscillate_vst3_scan_helper"
        self.cache = self.build / "CMakeCache.txt"
        self.source_root = SCRIPT.parent.parent

    def write_cache(self, overrides=None):
        values = dict(PACKAGE.PACKAGE_FEATURES)
        values["CMAKE_HOME_DIRECTORY"] = str(self.source_root)
        values.update(overrides or {})
        self.cache.write_text(
            "\n".join(f"{key}:STRING={value}" for key, value in values.items()) + "\n"
        )

    def test_complete_profile_and_exact_sibling_inputs(self):
        self.write_cache()
        profile = PACKAGE.check_package_profile(self.cache, self.binary, self.helper)
        self.assertEqual(profile, dict(sorted(PACKAGE.PACKAGE_FEATURES.items())))

    def test_missing_or_disabled_runtime_feature_is_rejected(self):
        for key in PACKAGE.PACKAGE_FEATURES:
            with self.subTest(key=key):
                self.write_cache({key: "OFF" if PACKAGE.PACKAGE_FEATURES[key] == "ON" else "ON"})
                with self.assertRaises(SystemExit):
                    PACKAGE.check_package_profile(self.cache, self.binary, self.helper)

    def test_foreign_source_or_missing_and_foreign_helper_is_rejected(self):
        self.write_cache({"CMAKE_HOME_DIRECTORY": "/tmp/foreign-source"})
        with self.assertRaises(SystemExit):
            PACKAGE.check_package_profile(self.cache, self.binary, self.helper)
        self.write_cache()
        for helper in (None, self.build / "different/helper"):
            with self.subTest(helper=helper), self.assertRaises(SystemExit):
                PACKAGE.check_package_profile(self.cache, self.binary, helper)
        with self.assertRaises(SystemExit):
            PACKAGE.check_package_profile(self.cache, self.build / "different/app", self.helper)

    def test_bundled_qml_import_requires_runtime_package(self):
        files, imports = PACKAGE.bundled_qml_imports(self.source_root)
        self.assertIn(self.source_root / "apps/daw_quick/qml/DawShell.qml", files)
        self.assertEqual(imports, ["QtQuick"])
        self.assertEqual(
            PACKAGE.complete_dependencies("libc6 (>= 2.38)", imports),
            "libc6 (>= 2.38), qml6-module-qtquick",
        )

    def test_unmapped_qml_import_fails_package_inventory(self):
        source = self.build / "source/apps/daw_quick"
        qml = source / "qml/DawShell.qml"
        qml.parent.mkdir(parents=True)
        qml.write_text("import QtQuick\nimport QtQuick.Controls\nItem {}\n")
        (source / "qml.qrc").write_text(
            '<RCC><qresource prefix="/"><file>qml/DawShell.qml</file></qresource></RCC>'
        )
        with self.assertRaisesRegex(SystemExit, "Unmapped QML runtime imports: QtQuick.Controls"):
            PACKAGE.bundled_qml_imports(self.build / "source")


if __name__ == "__main__":
    unittest.main()
