"""Unit tests for changes.py's path classification."""

import unittest

import changes


class ClassifyTests(unittest.TestCase):
    def test_docs_only_change_skips_code_and_python(self):
        self.assertEqual(
            changes.classify(["README.md", "docs/specs/README.md", "mkdocs.yml"]),
            {"code": False, "python": False, "docs": True},
        )

    def test_source_change_needs_the_build(self):
        self.assertEqual(
            changes.classify(["src/engine/graph.cpp"]),
            {"code": True, "python": False, "docs": False},
        )

    def test_python_change_needs_code_and_python(self):
        self.assertEqual(
            changes.classify(["tools/listening/warp/report.py"]),
            {"code": True, "python": True, "docs": False},
        )

    def test_unknown_path_counts_as_code(self):
        self.assertTrue(changes.classify(["somewhere/new.file"])["code"])

    def test_moved_source_still_counts_as_code(self):
        # --no-renames lists a move as the old and the new path.
        self.assertTrue(changes.classify(["src/engine/graph.cpp", "docs/graph.cpp"])["code"])

    def test_no_files_runs_nothing(self):
        self.assertEqual(changes.classify([]), dict.fromkeys(changes.CLASSES, False))


if __name__ == "__main__":
    unittest.main()
