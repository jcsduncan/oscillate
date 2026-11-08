"""Unit tests for changes.py's path classification."""

import unittest

import changes


class ClassifyTests(unittest.TestCase):
    def test_docs_only_change_skips_code_and_python(self):
        self.assertEqual(
            changes.classify(["README.md", "docs/specs/README.md", "mkdocs.yml"]),
            {"code": False, "cpp": False, "python": False, "docs": True, "packaging": False},
        )

    def test_source_change_needs_the_build(self):
        self.assertEqual(
            changes.classify(["src/engine/graph.cpp"]),
            {"code": True, "cpp": True, "python": False, "docs": False, "packaging": False},
        )

    def test_python_change_needs_code_and_python(self):
        self.assertEqual(
            changes.classify(["tools/listening/warp/report.py"]),
            {"code": True, "cpp": False, "python": True, "docs": False, "packaging": False},
        )

    def test_runtime_dependency_inventory_runs_the_python_suites(self):
        flags = changes.classify(["docs/dependencies/runtime-dependencies.json"])
        self.assertTrue(flags["python"])
        self.assertTrue(flags["docs"])

    def test_packaging_inputs_run_the_package_build(self):
        for path in (
            "scripts/package-deb.py",
            "scripts/package_sbom.py",
            "tests/packaging/install-smoke.sh",
            "third_party/notices/vst3sdk/LICENSE.txt",
            "CMakePresets.json",
            ".github/workflows/packaging.yml",
        ):
            with self.subTest(path=path):
                self.assertTrue(changes.classify([path])["packaging"])

    def test_source_change_skips_the_package_build(self):
        self.assertFalse(
            changes.classify(["src/engine/graph.cpp", "cmake/Tests.cmake"])["packaging"]
        )

    def test_cpp_analysis_follows_cpp_sources_and_its_workflow(self):
        for path in (
            "src/engine/graph.hpp",
            "src/engine/kernel.c",
            ".github/workflows/codeql.yml",
        ):
            with self.subTest(path=path):
                self.assertTrue(changes.classify([path])["cpp"])

    def test_non_cpp_change_skips_the_cpp_analysis(self):
        files = ["apps/daw_quick/qml/ArrangementView.qml", "CMakeLists.txt", "README.md"]
        self.assertFalse(changes.classify(files)["cpp"])

    def test_unknown_path_counts_as_code(self):
        self.assertTrue(changes.classify(["somewhere/new.file"])["code"])

    def test_moved_source_still_counts_as_code(self):
        # --no-renames lists a move as the old and the new path.
        self.assertTrue(changes.classify(["src/engine/graph.cpp", "docs/graph.cpp"])["code"])

    def test_no_files_runs_nothing(self):
        self.assertEqual(changes.classify([]), dict.fromkeys(changes.CLASSES, False))


if __name__ == "__main__":
    unittest.main()
