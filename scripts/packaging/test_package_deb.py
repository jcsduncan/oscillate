#!/usr/bin/env python3
"""Contract tests for the .deb's QML runtime dependencies."""

import importlib.util
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "package-deb.py"
SPEC = importlib.util.spec_from_file_location("package_deb", SCRIPT)
assert SPEC and SPEC.loader
PACKAGE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PACKAGE)


class QmlDependencyTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="oscillate-package-deb-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def test_bundled_qml_import_requires_runtime_package(self):
        files, imports = PACKAGE.bundled_qml_imports(PACKAGE.SOURCE_ROOT)
        self.assertIn(PACKAGE.SOURCE_ROOT / "apps/daw_quick/qml/DawShell.qml", files)
        self.assertEqual(imports, ["QtQuick"])
        self.assertEqual(
            PACKAGE.complete_dependencies("libc6 (>= 2.38)", imports),
            "libc6 (>= 2.38), qml6-module-qtquick",
        )

    def test_unmapped_qml_import_fails_package_inventory(self):
        source = self.root / "source/apps/daw_quick"
        qml = source / "qml/DawShell.qml"
        qml.parent.mkdir(parents=True)
        qml.write_text("import QtQuick\nimport QtQuick.Controls\nItem {}\n")
        (source / "qml.qrc").write_text(
            '<RCC><qresource prefix="/"><file>qml/DawShell.qml</file></qresource></RCC>'
        )
        with self.assertRaisesRegex(ValueError, "Unmapped QML runtime imports: QtQuick.Controls"):
            PACKAGE.bundled_qml_imports(self.root / "source")


if __name__ == "__main__":
    unittest.main()
