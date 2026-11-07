"""Unit tests for known_issues.py."""

import unittest
from datetime import UTC, datetime

import known_issues

REPOSITORY = "owner/project"
GENERATED = datetime(2026, 11, 7, 12, 30, tzinfo=UTC)


def issue(number, title, updated, labels=(known_issues.LABEL,), pull_request=False):
    item = {
        "number": number,
        "title": title,
        "html_url": f"https://github.com/{REPOSITORY}/issues/{number}",
        "updated_at": updated,
        "labels": [{"name": name} for name in labels],
    }
    if pull_request:
        item["pull_request"] = {"url": "x"}
    return item


class FetchTests(unittest.TestCase):
    def test_pages_until_a_short_page_and_skips_pull_requests(self):
        full_page = [issue(n, "t", "2026-11-01T00:00:00Z") for n in range(known_issues.PER_PAGE)]
        last_page = [
            issue(500, "kept", "2026-11-01T00:00:00Z"),
            issue(501, "a pull request", "2026-11-01T00:00:00Z", pull_request=True),
        ]
        requested = []

        def fetch(url):
            requested.append(url)
            return full_page if len(requested) == 1 else last_page

        issues = known_issues.open_known_issues(REPOSITORY, fetch)
        self.assertEqual(len(requested), 2)
        self.assertIn("labels=known-issue", requested[0])
        self.assertIn("state=open", requested[0])
        self.assertIn("page=2", requested[1])
        self.assertEqual(len(issues), known_issues.PER_PAGE + 1)
        self.assertNotIn(501, [item["number"] for item in issues])

    def test_endless_pages_fail(self):
        full_page = [issue(n, "t", "2026-11-01T00:00:00Z") for n in range(known_issues.PER_PAGE)]
        with self.assertRaises(RuntimeError):
            known_issues.open_known_issues(REPOSITORY, lambda url: full_page)


class RenderTests(unittest.TestCase):
    def test_empty_list_says_so(self):
        page = known_issues.render([], REPOSITORY, GENERATED)
        self.assertTrue(page.startswith("# Known issues\n"))
        self.assertIn("No known issues are open.", page)
        self.assertIn("2026-11-07 12:30 UTC", page)
        self.assertIn(known_issues.label_url(REPOSITORY), page)

    def test_rows_newest_first_with_other_labels(self):
        page = known_issues.render(
            [
                issue(3, "Older", "2026-10-01T00:00:00Z", ("known-issue", "plugins")),
                issue(9, "Newer", "2026-11-02T00:00:00Z", ("ui", "known-issue", "bug")),
            ],
            REPOSITORY,
            GENERATED,
        )
        newer = page.index("#9 Newer")
        older = page.index("#3 Older")
        self.assertLess(newer, older)
        self.assertIn("| bug, ui | 2026-11-02 |", page)
        self.assertIn("| plugins | 2026-10-01 |", page)
        self.assertNotIn("known-issue, ", page)

    def test_titles_cannot_inject_markdown_or_html(self):
        page = known_issues.render(
            [issue(1, "Crash | [click](http://x) <script>", "2026-11-01T00:00:00Z")],
            REPOSITORY,
            GENERATED,
        )
        self.assertIn("Crash \\| \\[click\\]\\(http://x\\) &lt;script&gt;", page)
        self.assertNotIn("<script>", page)


if __name__ == "__main__":
    unittest.main()
