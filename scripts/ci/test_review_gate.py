"""Unit tests for review_gate.py."""

import pathlib
import re
import unittest
import urllib.error

import review_gate

REPOSITORY = "jcsduncan/oscillate"
FORK = "someone/oscillate"
NUMBER = 42
HEAD = "c" * 40
EARLIER = "b" * 40
REVIEWED_AT = "2026-11-08T10:00:00Z"
BEFORE_REVIEW = "2026-11-08T09:00:00Z"
AFTER_REVIEW = "2026-11-08T11:00:00Z"
STARTED_FIRST = "2026-11-08T08:00:00Z"
STARTED_LATER = "2026-11-08T09:30:00Z"
WORKFLOW = pathlib.Path(__file__).resolve().parents[2] / ".github/workflows/review-gate.yml"


def pull(ref="fix/thing", head_repo=REPOSITORY):
    return {
        "number": NUMBER,
        "head": {"sha": HEAD, "ref": ref, "repo": {"full_name": head_repo}},
        "base": {"repo": {"full_name": REPOSITORY}},
    }


def run(name, status="completed", conclusion="success", text="", started=STARTED_FIRST):
    return {
        "name": name,
        "status": status,
        "conclusion": conclusion if status == "completed" else None,
        "output": {"summary": text},
        "started_at": started,
        "completed_at": REVIEWED_AT if status == "completed" else None,
    }


def coderabbit(text="Result: Success - Review completed", **kwargs):
    return run("CodeRabbit", text=text, **kwargs)


def comment(body, login=review_gate.CODERABBIT_LOGIN, updated=AFTER_REVIEW):
    return {"user": {"login": login}, "body": body, "updated_at": updated}


class FakeSource:
    def __init__(self, runs=None, earlier_runs=None, statuses=None, comments=None, unresolved=0):
        self.runs = {HEAD: runs or [], EARLIER: earlier_runs or []}
        self.status_list = statuses or []
        self.comments = comments or []
        self.unresolved = unresolved

    def check_runs(self, sha):
        return self.runs[sha]

    def statuses(self, sha):
        return self.status_list

    def commits(self, number):
        return [EARLIER, HEAD]

    def issue_comments(self, number):
        return self.comments

    def unresolved_threads(self, number):
        return self.unresolved


class ReviewTests(unittest.TestCase):
    def state(self, source, pr=None):
        return review_gate.evaluate(pr or pull(), source).state

    def test_completed_review_without_threads_passes(self):
        self.assertEqual(self.state(FakeSource([coderabbit()])), review_gate.SUCCESS)

    def test_no_coderabbit_run_waits(self):
        self.assertEqual(self.state(FakeSource([run("Build")])), review_gate.PENDING)

    def test_review_in_progress_waits(self):
        source = FakeSource([coderabbit(status="in_progress")])
        self.assertEqual(self.state(source), review_gate.PENDING)

    def test_skipped_review_fails(self):
        source = FakeSource([coderabbit(text="Review skipped", conclusion="skipped")])
        self.assertEqual(self.state(source), review_gate.FAILURE)

    def test_success_without_completed_review_fails(self):
        source = FakeSource([coderabbit(text="Rate limited")])
        self.assertEqual(self.state(source), review_gate.FAILURE)

    def test_latest_coderabbit_run_decides(self):
        source = FakeSource([coderabbit(), coderabbit(text="Review paused", started=STARTED_LATER)])
        self.assertEqual(self.state(source), review_gate.FAILURE)

    def test_carried_forward_review_passes_after_an_earlier_completed_review(self):
        for text in review_gate.CARRIED_FORWARD:
            source = FakeSource([coderabbit(text=text)], earlier_runs=[coderabbit()])
            self.assertEqual(self.state(source), review_gate.SUCCESS, text)

    def test_carried_forward_without_an_earlier_review_fails(self):
        source = FakeSource(
            [coderabbit(text=review_gate.CARRIED_FORWARD[0])],
            earlier_runs=[coderabbit(text="Review skipped", conclusion="skipped")],
        )
        self.assertEqual(self.state(source), review_gate.FAILURE)

    def test_limit_notice_after_the_review_fails(self):
        for body in ("## Rate Limit Exceeded", "Review paused", "Reviews paused"):
            source = FakeSource([coderabbit()], comments=[comment(body)])
            self.assertEqual(self.state(source), review_gate.FAILURE, body)

    def test_limit_notice_before_the_review_passes(self):
        source = FakeSource(
            [coderabbit()], comments=[comment("Rate Limit Exceeded", updated=BEFORE_REVIEW)]
        )
        self.assertEqual(self.state(source), review_gate.SUCCESS)

    def test_comment_from_a_deleted_account_is_ignored(self):
        deleted = {"user": None, "body": "Review paused", "updated_at": AFTER_REVIEW}
        self.assertEqual(
            self.state(FakeSource([coderabbit()], comments=[deleted])), review_gate.SUCCESS
        )

    def test_limit_notice_from_someone_else_is_ignored(self):
        source = FakeSource([coderabbit()], comments=[comment("Review paused", login="dev")])
        self.assertEqual(self.state(source), review_gate.SUCCESS)

    def test_unresolved_thread_fails(self):
        source = FakeSource([coderabbit()], unresolved=1)
        self.assertEqual(self.state(source), review_gate.FAILURE)

    def test_other_checks_do_not_decide_a_reviewed_pull(self):
        # The ruleset requires the build checks itself.
        source = FakeSource([coderabbit(), run("Build", status="in_progress")])
        self.assertEqual(self.state(source), review_gate.SUCCESS)


class ReleaseTests(unittest.TestCase):
    RELEASE_REF = review_gate.RELEASE_BRANCH_PREFIX + "branches--main"

    def state(self, source, head_repo=REPOSITORY):
        return review_gate.evaluate(pull(self.RELEASE_REF, head_repo), source).state

    def skipped_review(self):
        return coderabbit(text="Review skipped", conclusion="skipped")

    def test_green_checks_pass_without_a_review(self):
        source = FakeSource([self.skipped_review(), run("Build")])
        self.assertEqual(self.state(source), review_gate.SUCCESS)

    def test_pending_check_waits(self):
        source = FakeSource([self.skipped_review(), run("Build", status="queued")])
        self.assertEqual(self.state(source), review_gate.PENDING)

    def test_failed_check_fails(self):
        source = FakeSource([run("Build", conclusion="failure")])
        self.assertEqual(self.state(source), review_gate.FAILURE)

    def test_rerun_replaces_a_failed_check(self):
        source = FakeSource(
            [run("Build", conclusion="failure"), run("Build", started=STARTED_LATER)]
        )
        self.assertEqual(self.state(source), review_gate.SUCCESS)

    def test_failed_commit_status_fails(self):
        source = FakeSource([run("Build")], statuses=[{"context": "ci", "state": "error"}])
        self.assertEqual(self.state(source), review_gate.FAILURE)

    def test_own_job_and_status_are_ignored(self):
        source = FakeSource(
            [run("Build"), run(review_gate.GATE_JOB_NAME, status="in_progress")],
            statuses=[{"context": review_gate.GATE_CONTEXT, "state": "failure"}],
        )
        self.assertEqual(self.state(source), review_gate.SUCCESS)

    def test_unresolved_thread_fails(self):
        source = FakeSource([run("Build")], unresolved=1)
        self.assertEqual(self.state(source), review_gate.FAILURE)

    def test_fork_branch_named_like_a_release_needs_a_review(self):
        source = FakeSource([self.skipped_review(), run("Build")])
        self.assertEqual(self.state(source, head_repo=FORK), review_gate.FAILURE)


class FakeGitHub:
    def __init__(self, commit_pulls=(), open_pulls=()):
        self.commit_pulls = list(commit_pulls)
        self.open = list(open_pulls)

    def pulls_for_commit(self, sha):
        return self.commit_pulls

    def open_pulls(self):
        return self.open


class EventTests(unittest.TestCase):
    def test_pull_request_events_name_their_pull(self):
        for name in review_gate.PULL_REQUEST_EVENTS:
            event = {"pull_request": {"number": NUMBER}}
            self.assertEqual(review_gate.pull_numbers(name, event, FakeGitHub()), [NUMBER])

    def test_comment_on_an_issue_is_ignored(self):
        event = {"issue": {"number": NUMBER}}
        self.assertEqual(review_gate.pull_numbers("issue_comment", event, FakeGitHub()), [])

    def test_comment_on_a_pull_names_it(self):
        event = {"issue": {"number": NUMBER, "pull_request": {}}}
        numbers = review_gate.pull_numbers("issue_comment", event, FakeGitHub())
        self.assertEqual(numbers, [NUMBER])

    def test_check_run_names_its_pulls(self):
        event = {"check_run": {"head_sha": HEAD, "pull_requests": [{"number": NUMBER}]}}
        self.assertEqual(review_gate.pull_numbers("check_run", event, FakeGitHub()), [NUMBER])

    def test_check_run_without_pulls_looks_up_the_commit(self):
        event = {"check_run": {"head_sha": HEAD, "pull_requests": []}}
        github = FakeGitHub(commit_pulls=[NUMBER])
        self.assertEqual(review_gate.pull_numbers("check_run", event, github), [NUMBER])

    def test_schedule_covers_every_open_pull(self):
        github = FakeGitHub(open_pulls=[NUMBER, NUMBER + 1])
        numbers = review_gate.pull_numbers("schedule", {}, github)
        self.assertEqual(numbers, [NUMBER, NUMBER + 1])


class StatusTests(unittest.TestCase):
    def test_long_description_is_clipped(self):
        text = "x" * (review_gate.DESCRIPTION_LIMIT * 2)
        self.assertEqual(len(review_gate.clip(text)), review_gate.DESCRIPTION_LIMIT)

    def test_repeat_of_the_current_status_is_not_posted(self):
        verdict = review_gate.Verdict(review_gate.FAILURE, "1 unresolved review thread(s)")
        current = {"state": verdict.state, "description": verdict.description}
        self.assertTrue(review_gate.unchanged(current, verdict))
        self.assertFalse(review_gate.unchanged(None, verdict))
        changed = review_gate.Verdict(review_gate.SUCCESS, verdict.description)
        self.assertFalse(review_gate.unchanged(current, changed))

    def test_gate_status_is_found_by_context(self):
        statuses = [{"context": "ci"}, {"context": review_gate.GATE_CONTEXT}]
        self.assertEqual(review_gate.gate_status(statuses), statuses[1])

    def test_workflow_job_name_matches(self):
        names = re.findall(r"^\s+name: (.+)$", WORKFLOW.read_text(encoding="utf-8"), re.MULTILINE)
        self.assertIn(review_gate.GATE_JOB_NAME, names)


class UpdatingGitHub(FakeSource):
    """Fails to read one pull request and records the statuses it sets."""

    def __init__(self, broken, malformed):
        super().__init__([coderabbit()])
        self.broken = broken
        self.malformed = malformed
        self.set = []

    def pull(self, number):
        if number == self.broken:
            raise urllib.error.URLError("server error")
        if number == self.malformed:
            raise ValueError("invalid JSON")
        return {**pull(), "number": number, "state": "open"}

    def set_status(self, sha, verdict, target_url):
        self.set.append(verdict.state)


class UpdateTests(unittest.TestCase):
    def test_api_error_on_one_pull_still_updates_the_others(self):
        github = UpdatingGitHub(broken=NUMBER, malformed=NUMBER + 1)
        numbers = [NUMBER, NUMBER + 1, NUMBER + 2]
        result = review_gate.update(numbers, github, "", dry_run=False)
        self.assertEqual(result, 1)
        self.assertEqual(github.set, [review_gate.SUCCESS])


if __name__ == "__main__":
    unittest.main()
