"""The native package must declare QML modules absent from ELF DT_NEEDED."""

import importlib.util
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[2] / "tests/packaging/smoke_ubuntu_package.py"
SPEC = importlib.util.spec_from_file_location("ubuntu_package_smoke", SCRIPT)
assert SPEC and SPEC.loader
SMOKE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SMOKE)


class QmlRuntimeDependencyTests(unittest.TestCase):
    def test_missing_qml_module_fails(self):
        inventory = {
            "qml_source_files": [{"path": "apps/daw_quick/qml/DawShell.qml"}],
            "qml_source_imports": ["QtQuick"],
            "qml_runtime_packages": ["qml6-module-qtquick"],
        }
        with self.assertRaisesRegex(ValueError, "qml6-module-qtquick"):
            SMOKE.check_qml_runtime_dependencies("libqt6quick6 (>= 6.6.0)", inventory)
        SMOKE.check_qml_runtime_dependencies(
            "libqt6quick6 (>= 6.6.0), qml6-module-qtquick", inventory
        )


if __name__ == "__main__":
    unittest.main()
