"""Unit tests for check_pr_title.py."""

import unittest

import check_pr_title

SCOPES = {"app", "engine", "native_dsp", "tools", "deps", "main"}


class TitleTests(unittest.TestCase):
    def test_scoped_feature_passes(self):
        self.assertEqual(check_pr_title.problems("feat(app): live strip views", SCOPES), [])

    def test_feature_without_scope_fails(self):
        self.assertTrue(check_pr_title.problems("feat: live strip views", SCOPES))

    def test_unknown_scope_fails(self):
        self.assertTrue(check_pr_title.problems("fix(native): wrap phase", SCOPES))

    def test_several_scopes_pass(self):
        self.assertEqual(
            check_pr_title.problems("feat(app,native_dsp): reduction bands", SCOPES), []
        )

    def test_bot_titles_pass(self):
        for title in (
            "build: bump gcovr from 8.6 to 8.8 in /scripts",
            "chore(main): release oscillate 0.2.0",
            "docs: factual README",
        ):
            self.assertEqual(check_pr_title.problems(title, SCOPES), [], title)

    def test_breaking_marker_passes(self):
        self.assertEqual(check_pr_title.problems("feat(engine)!: new graph API", SCOPES), [])

    def test_malformed_title_fails(self):
        self.assertTrue(check_pr_title.problems("Fix the engine", SCOPES))

    def test_scopes_come_from_src_modules(self):
        scopes = check_pr_title.known_scopes()
        self.assertIn("engine", scopes)
        self.assertIn("app", scopes)
        self.assertIn("deps", scopes)


if __name__ == "__main__":
    unittest.main()
