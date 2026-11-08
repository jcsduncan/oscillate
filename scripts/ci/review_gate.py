"""Publish the "Review gate" commit status on open pull requests.

Usage: review_gate.py [--dry-run] (--event EVENT_NAME EVENT_PATH | --pr N ...)
Needs GITHUB_TOKEN and GITHUB_REPOSITORY; GITHUB_SERVER_URL and GITHUB_RUN_ID
link the status to the run that set it.

Auto-merge waits for the required "Review gate" status, which is success only
when nothing can slip through a review:

1. CodeRabbit completed a real review of the head commit: its latest check
   run there is completed/success and says "Review completed". A head that
   only carries an earlier review forward ("Prior review carried forward" or
   "No files to review") passes when an earlier commit of the same pull
   request had a completed review. A skipped, paused or rate-limited review
   never passes, although GitHub counts a skipped check as passing.
2. CodeRabbit posted no rate-limit or paused notice after that review.
3. No review thread is unresolved.

Release Please pull requests (head branch release-please--*, pushed to this
repository) are never reviewed by CodeRabbit. They pass when every other
check on the head commit has finished green and no thread is unresolved.

The script reads the GitHub API and sets the status; it never checks out
or runs pull request code.
"""

import argparse
import dataclasses
import datetime
import json
import os
import re
import sys
import urllib.parse
import urllib.request
from typing import Protocol

GATE_CONTEXT = "Review gate"
# The review-gate.yml job's name: its own check run is never one of the
# checks it waits for.
GATE_JOB_NAME = "Evaluate the review gate"
CODERABBIT_CHECK = re.compile("coderabbit", re.IGNORECASE)
CODERABBIT_LOGIN = "coderabbitai[bot]"
REVIEW_COMPLETED = "Review completed"
CARRIED_FORWARD = ("Prior review carried forward", "No files to review")
LIMIT_NOTICE = re.compile(r"Rate Limit Exceeded|Review paused|Reviews paused", re.IGNORECASE)
# Each status's target URL records when its evaluation began.
EVALUATED_KEY = "evaluated"
EVALUATED = re.compile(EVALUATED_KEY + r"=([0-9T:.+-]+)")
EVALUATION_FAILED = "The gate could not be evaluated; the next run retries"
RELEASE_BRANCH_PREFIX = "release-please--"
GREEN_CONCLUSIONS = frozenset({"success", "skipped", "neutral"})
SUCCESS, PENDING, FAILURE = "success", "pending", "failure"
# GitHub rejects a commit status description longer than this.
DESCRIPTION_LIMIT = 140
SHORT_SHA = 7
PAGE_SIZE = 100
API_URL = "https://api.github.com"
PULL_REQUEST_EVENTS = ("pull_request", "pull_request_review", "pull_request_review_comment")
THREADS_QUERY = """
query($owner: String!, $name: String!, $number: Int!, $after: String) {
  repository(owner: $owner, name: $name) {
    pullRequest(number: $number) {
      reviewThreads(first: 100, after: $after) {
        pageInfo { hasNextPage endCursor }
        nodes { isResolved }
      }
    }
  }
}
"""


@dataclasses.dataclass(frozen=True)
class Verdict:
    state: str
    description: str


class Source(Protocol):
    """What the gate reads about a pull request."""

    def check_runs(self, sha: str) -> list[dict]: ...

    def statuses(self, sha: str) -> list[dict]: ...

    def commits(self, number: int) -> list[str]: ...

    def issue_comments(self, number: int) -> list[dict]: ...

    def unresolved_threads(self, number: int) -> int: ...


def parse_time(stamp: str) -> datetime.datetime:
    return datetime.datetime.fromisoformat(stamp)


def first_line(text: str) -> str:
    return text.strip().splitlines()[0] if text.strip() else ""


def latest_by_name(check_runs: list[dict]) -> dict[str, dict]:
    """Each check's latest run: a re-run replaces the earlier result."""
    latest: dict[str, dict] = {}
    for run in sorted(check_runs, key=lambda run: run.get("started_at") or ""):
        latest[run["name"]] = run
    return latest


def coderabbit_runs(check_runs: list[dict]) -> list[dict]:
    runs = [run for run in check_runs if CODERABBIT_CHECK.search(run["name"])]
    return sorted(runs, key=lambda run: run.get("started_at") or "")


def summary(run: dict) -> str:
    return (run.get("output") or {}).get("summary") or ""


def completed_review(run: dict) -> bool:
    return (
        run["status"] == "completed"
        and run.get("conclusion") == "success"
        and REVIEW_COMPLETED in summary(run)
    )


def carries_review_forward(run: dict) -> bool:
    return run["status"] == "completed" and any(
        phrase in summary(run) for phrase in CARRIED_FORWARD
    )


def is_release_pull(pull: dict) -> bool:
    """A Release Please branch in this repository; a fork can't claim one."""
    head_repo = (pull["head"].get("repo") or {}).get("full_name")
    return (
        pull["head"]["ref"].startswith(RELEASE_BRANCH_PREFIX)
        and head_repo == pull["base"]["repo"]["full_name"]
    )


def review_verdict(pull: dict, source: Source) -> Verdict | None:
    """Conditions 1 and 2; None when CodeRabbit's review holds."""
    sha = pull["head"]["sha"]
    runs = coderabbit_runs(source.check_runs(sha))
    if not runs:
        return Verdict(PENDING, f"Waiting for CodeRabbit to review {sha[:SHORT_SHA]}")
    head_run = runs[-1]
    if head_run["status"] != "completed":
        return Verdict(PENDING, f"CodeRabbit is reviewing {sha[:SHORT_SHA]}")
    if not completed_review(head_run):
        if not carries_review_forward(head_run):
            return Verdict(
                FAILURE,
                f"CodeRabbit has not completed a review of {sha[:SHORT_SHA]} "
                f"({head_run.get('conclusion')}: {first_line(summary(head_run))})",
            )
        earlier = [commit for commit in source.commits(pull["number"]) if commit != sha]
        reviews = [
            run
            for commit in earlier
            for run in coderabbit_runs(source.check_runs(commit))
            if completed_review(run)
        ]
        if not reviews:
            return Verdict(
                FAILURE,
                f"CodeRabbit carried a review forward to {sha[:SHORT_SHA]}, "
                "but no earlier commit has a completed review",
            )
        # A notice after the review that was carried forward counts, even
        # when it came before the head's own check run.
        reviewed_at = max(parse_time(run["completed_at"]) for run in reviews)
    else:
        reviewed_at = parse_time(head_run["completed_at"])
    for comment in source.issue_comments(pull["number"]):
        if (
            (comment.get("user") or {}).get("login") == CODERABBIT_LOGIN
            and LIMIT_NOTICE.search(comment.get("body") or "")
            and parse_time(comment["updated_at"]) > reviewed_at
        ):
            return Verdict(
                FAILURE, "CodeRabbit posted a rate-limit or paused notice after its review"
            )
    return None


def checks_verdict(pull: dict, source: Source) -> Verdict | None:
    """Every other check and status on the head finished green; None when so."""
    sha = pull["head"]["sha"]
    waiting, failed = [], []
    for name, run in sorted(latest_by_name(source.check_runs(sha)).items()):
        if name == GATE_JOB_NAME:
            continue
        if run["status"] != "completed":
            waiting.append(name)
        elif run.get("conclusion") not in GREEN_CONCLUSIONS:
            failed.append(name)
    for status in source.statuses(sha):
        if status["context"] == GATE_CONTEXT:
            continue
        if status["state"] == PENDING:
            waiting.append(status["context"])
        elif status["state"] != SUCCESS:
            failed.append(status["context"])
    if failed:
        return Verdict(FAILURE, "Checks failed: " + ", ".join(failed))
    if waiting:
        return Verdict(PENDING, "Waiting for checks: " + ", ".join(waiting))
    return None


def evaluate(pull: dict, source: Source) -> Verdict:
    release = is_release_pull(pull)
    blocker = checks_verdict(pull, source) if release else review_verdict(pull, source)
    if blocker:
        return blocker
    unresolved = source.unresolved_threads(pull["number"])
    if unresolved:
        return Verdict(FAILURE, f"{unresolved} unresolved review thread(s)")
    if release:
        return Verdict(SUCCESS, "Release PR: all checks green, no unresolved threads")
    return Verdict(SUCCESS, "CodeRabbit review completed, no unresolved threads")


def clip(text: str) -> str:
    return text if len(text) <= DESCRIPTION_LIMIT else text[: DESCRIPTION_LIMIT - 1] + "…"


def pull_numbers(event_name: str, event: dict, source: "GitHub") -> list[int]:
    """The pull requests an event can change the gate of."""
    if event_name in PULL_REQUEST_EVENTS:
        return [event["pull_request"]["number"]]
    if event_name == "issue_comment":
        issue = event["issue"]
        return [issue["number"]] if "pull_request" in issue else []
    if event_name == "check_run":
        numbers = [pull["number"] for pull in event["check_run"].get("pull_requests") or []]
        return numbers or source.pulls_for_commit(event["check_run"]["head_sha"])
    return source.open_pulls()


class GitHub:
    """The REST and GraphQL calls the gate makes."""

    def __init__(self, repository: str, token: str):
        self.repository = repository
        self.token = token

    def request(self, url: str, payload: dict | None = None) -> tuple[object, str]:
        data = json.dumps(payload).encode() if payload is not None else None
        request = urllib.request.Request(
            url if url.startswith("https://") else API_URL + url,
            data=data,
            method="POST" if payload is not None else "GET",
            headers={
                "Accept": "application/vnd.github+json",
                "Authorization": f"Bearer {self.token}",
                "X-GitHub-Api-Version": "2022-11-28",
            },
        )
        with urllib.request.urlopen(request) as response:
            return json.load(response), response.headers.get("Link") or ""

    def pages(self, path: str, key: str | None = None) -> list:
        """Every item of a paginated list, following the Link header."""
        separator = "&" if "?" in path else "?"
        url = f"{path}{separator}per_page={PAGE_SIZE}"
        items: list = []
        while url:
            body, link = self.request(url)
            items.extend(body[key] if key else body)
            match = re.search(r'<([^>]+)>;\s*rel="next"', link)
            url = match.group(1) if match else ""
        return items

    def pull(self, number: int) -> dict:
        return self.request(f"/repos/{self.repository}/pulls/{number}")[0]

    def open_pulls(self) -> list[int]:
        pulls = self.pages(f"/repos/{self.repository}/pulls?state=open")
        return [pull["number"] for pull in pulls]

    def pulls_for_commit(self, sha: str) -> list[int]:
        pulls = self.pages(f"/repos/{self.repository}/commits/{sha}/pulls")
        return [pull["number"] for pull in pulls if pull["state"] == "open"]

    def check_runs(self, sha: str) -> list[dict]:
        return self.pages(f"/repos/{self.repository}/commits/{sha}/check-runs", "check_runs")

    def statuses(self, sha: str) -> list[dict]:
        """The latest status of each context."""
        return self.pages(f"/repos/{self.repository}/commits/{sha}/status", "statuses")

    def commits(self, number: int) -> list[str]:
        commits = self.pages(f"/repos/{self.repository}/pulls/{number}/commits")
        return [commit["sha"] for commit in commits]

    def issue_comments(self, number: int) -> list[dict]:
        return self.pages(f"/repos/{self.repository}/issues/{number}/comments")

    def unresolved_threads(self, number: int) -> int:
        owner, name = self.repository.split("/")
        unresolved, after = 0, None
        while True:
            variables = {"owner": owner, "name": name, "number": number, "after": after}
            body, _ = self.request("/graphql", {"query": THREADS_QUERY, "variables": variables})
            if body.get("errors"):
                raise RuntimeError(f"GraphQL error: {body['errors']}")
            threads = body["data"]["repository"]["pullRequest"]["reviewThreads"]
            unresolved += sum(not thread["isResolved"] for thread in threads["nodes"])
            if not threads["pageInfo"]["hasNextPage"]:
                return unresolved
            after = threads["pageInfo"]["endCursor"]

    def set_status(self, sha: str, verdict: Verdict, target_url: str) -> None:
        payload = {
            "state": verdict.state,
            "context": GATE_CONTEXT,
            "description": clip(verdict.description),
            "target_url": target_url,
        }
        self.request(f"/repos/{self.repository}/statuses/{sha}", payload)


def evaluated_at(status: dict | None) -> datetime.datetime | None:
    """When the evaluation behind a gate status began (from its target URL)."""
    match = EVALUATED.search((status or {}).get("target_url") or "")
    return parse_time(match.group(1)) if match else None


def stamped(target_url: str, started: datetime.datetime) -> str:
    return f"{target_url}#{EVALUATED_KEY}={started.isoformat()}"


def gate_status(statuses: list[dict]) -> dict | None:
    return next((status for status in statuses if status["context"] == GATE_CONTEXT), None)


def unchanged(current: dict | None, verdict: Verdict) -> bool:
    """A repeat of the current status is not posted; a commit holds at most
    1000 statuses per context and the schedule re-evaluates every PR."""
    return bool(
        current
        and current["state"] == verdict.state
        and current.get("description") == clip(verdict.description)
    )


def publish(github: GitHub, sha: str, verdict: Verdict, target_url: str, started) -> None:
    """Set the status unless it repeats the current one or a run that began
    later already set it: runs for one pull request may finish out of order,
    and a stale success must never replace a newer verdict."""
    current = gate_status(github.statuses(sha))
    newer = evaluated_at(current)
    if unchanged(current, verdict) or (newer and newer > started):
        return
    github.set_status(sha, verdict, stamped(target_url, started))


def update(numbers: list[int], github: GitHub, target_url: str, dry_run: bool) -> int:
    """Evaluate and set each pull request's gate. An API, network or
    response error on one pull request is reported, its gate drops to
    pending so no earlier success stands, and the rest still update; the
    exit code is 1."""
    failed = False
    for number in sorted(set(numbers)):
        started = datetime.datetime.now(datetime.UTC)
        sha = None
        try:
            pull = github.pull(number)
            if pull["state"] != "open":
                continue
            sha = pull["head"]["sha"]
            verdict = evaluate(pull, github)
            print(f"#{number} {sha[:SHORT_SHA]}: {verdict.state}: {verdict.description}")
            if not dry_run:
                publish(github, sha, verdict, target_url, started)
        except (OSError, ValueError, RuntimeError) as error:
            print(f"::error::#{number}: {error}")
            failed = True
            if sha and not dry_run:
                withdraw(github, sha, target_url, started)
    return 1 if failed else 0


def withdraw(github: GitHub, sha: str, target_url: str, started) -> None:
    try:
        github.set_status(sha, Verdict(PENDING, EVALUATION_FAILED), stamped(target_url, started))
    except (OSError, ValueError) as error:
        print(f"::error::{sha[:SHORT_SHA]}: could not withdraw the gate: {error}")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--dry-run", action="store_true", help="print, don't set statuses")
    target = parser.add_mutually_exclusive_group(required=True)
    target.add_argument("--event", nargs=2, metavar=("NAME", "PATH"))
    target.add_argument("--pr", type=int, nargs="+")
    args = parser.parse_args(argv)

    github = GitHub(os.environ["GITHUB_REPOSITORY"], os.environ["GITHUB_TOKEN"])
    if args.event:
        event_name, event_path = args.event
        with open(event_path, encoding="utf-8") as handle:
            numbers = pull_numbers(event_name, json.load(handle), github)
    else:
        numbers = args.pr
    run_id = os.environ.get("GITHUB_RUN_ID")
    server = os.environ.get("GITHUB_SERVER_URL", "https://github.com")
    target_url = f"{server}/{github.repository}"
    if run_id:
        target_url += f"/actions/runs/{run_id}"

    return update(numbers, github, target_url, args.dry_run)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
