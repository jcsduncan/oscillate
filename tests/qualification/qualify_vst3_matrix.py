#!/usr/bin/env python3
"""VST3 compatibility matrix over a pinned set of free Linux plug-ins.

Downloads each archive in the manifest, checks its SHA-256, unpacks it and runs
oscillate_vst3_matrix_probe once per plug-in class, each in its own process.
Writes matrix.json and matrix.md into --output.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import shutil
import signal
import subprocess
import sys
import tarfile
import urllib.request
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DEFAULT_MANIFEST = Path(__file__).resolve().with_name("vst3_matrix_plugins.json")
STEPS = ("scan", "load", "render_64", "render_128", "state", "editor", "project", "export")
STEP_TITLES = {
    "scan": "Scan",
    "load": "Load",
    "render_64": "Render 64",
    "render_128": "Render 128",
    "state": "State",
    "editor": "Editor",
    "project": "Save/reopen",
    "export": "Export",
}
ARCHIVES = ("tar", "zip", "deb", "7z")
# The probe exits with this after recording a failed step itself.
PROBE_STEP_FAILED = 1
DEFAULT_CLASS_TIMEOUT_SECONDS = 300
LIST_TIMEOUT_SECONDS = 120
DOWNLOAD_TIMEOUT_SECONDS = 600
DOWNLOAD_CHUNK_BYTES = 1 << 20


class HarnessError(RuntimeError):
    """A failure of the harness itself, not of a plug-in."""


def load_manifest(path: Path) -> list[dict]:
    plugins = json.loads(path.read_text(encoding="utf-8"))["plugins"]
    names = set()
    for plugin in plugins:
        name = plugin.get("name")
        if not name or name in names:
            raise HarnessError(f"manifest: missing or duplicate name {name!r}")
        names.add(name)
        if plugin.get("url") is None:
            if not plugin.get("unavailable"):
                raise HarnessError(f"manifest: {name} has no URL and no reason")
            continue
        if plugin.get("archive") not in ARCHIVES:
            raise HarnessError(f"manifest: {name} has an unknown archive type")
        if len(plugin.get("sha256", "")) != 64 or not plugin.get("bundles"):
            raise HarnessError(f"manifest: {name} needs a SHA-256 and at least one bundle")
    return plugins


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(DOWNLOAD_CHUNK_BYTES), b""):
            digest.update(chunk)
    return digest.hexdigest()


def fetch(plugin: dict, downloads: Path) -> Path:
    """The verified archive, downloaded once into `downloads` (a CI cache)."""
    target = downloads / plugin["sha256"]
    if target.is_file() and sha256_of(target) == plugin["sha256"]:
        return target
    downloads.mkdir(parents=True, exist_ok=True)
    pending = target.with_suffix(".pending")
    with (
        urllib.request.urlopen(plugin["url"], timeout=DOWNLOAD_TIMEOUT_SECONDS) as response,
        pending.open("wb") as out,
    ):
        shutil.copyfileobj(response, out, DOWNLOAD_CHUNK_BYTES)
    actual = sha256_of(pending)
    if actual != plugin["sha256"]:
        pending.unlink()
        raise ValueError(f"SHA-256 is {actual}, the pin is {plugin['sha256']}")
    pending.replace(target)
    return target


def unpack(archive: Path, kind: str, destination: Path) -> None:
    shutil.rmtree(destination, ignore_errors=True)
    destination.mkdir(parents=True)
    if kind == "tar":
        with tarfile.open(archive) as bundle:
            bundle.extractall(destination, filter="data")
    elif kind == "zip":
        with zipfile.ZipFile(archive) as bundle:
            bundle.extractall(destination)
    elif kind == "deb":
        subprocess.run(["dpkg-deb", "-x", str(archive), str(destination)], check=True)
    else:
        tool = shutil.which("7z") or shutil.which("7zz")
        if tool is None:
            raise OSError("7z archives need 7z or 7zz (Ubuntu package 7zip)")
        subprocess.run(
            [tool, "x", "-y", f"-o{destination}", str(archive)],
            check=True,
            stdout=subprocess.DEVNULL,
        )


def find_bundle(root: Path, name: str) -> Path | None:
    matches = sorted(path for path in root.rglob(name) if path.is_dir())
    return matches[0] if matches else None


def bundles_in(root: Path) -> list[str]:
    return sorted({path.name for path in root.rglob("*.vst3") if path.is_dir()})


def describe_exit(returncode: int) -> str:
    if returncode < 0:
        try:
            return f"crashed with {signal.Signals(-returncode).name}"
        except ValueError:
            return f"crashed with signal {-returncode}"
    return f"exited with status {returncode}"


def complete_steps(evidence: dict | None, outcome: str) -> dict:
    """Every step for one class: those the probe recorded, then the step it
    died in (`outcome`) and the rest not run."""
    recorded = dict((evidence or {}).get("steps", {}))
    steps = {}
    stopped = False
    for step in STEPS:
        if step in recorded:
            steps[step] = recorded[step]
        elif not stopped and outcome:
            steps[step] = {"status": "crash", "detail": outcome, "metrics": {}}
            stopped = True
        else:
            steps[step] = {"status": "not-run", "detail": "", "metrics": {}}
    return steps


def run_probe(command: list[str], output: Path, timeout: int, env: dict) -> tuple[dict | None, str]:
    output.unlink(missing_ok=True)
    try:
        completed = subprocess.run(
            command,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            errors="replace",
            timeout=timeout,
            check=False,
        )
        recorded = completed.returncode in (0, PROBE_STEP_FAILED) and output.is_file()
        outcome = "" if recorded else describe_exit(completed.returncode)
        log = completed.stdout
    except subprocess.TimeoutExpired as expired:
        outcome = f"timed out after {timeout} s"
        log = expired.stdout or ""
        if isinstance(log, bytes):
            log = log.decode("utf-8", errors="replace")
    try:
        evidence = (
            json.loads(output.read_text(encoding="utf-8", errors="replace"))
            if output.is_file()
            else None
        )
    except json.JSONDecodeError:
        evidence = None
        outcome = outcome or "wrote unreadable evidence"
    if outcome and log.strip():
        outcome += ": " + log.strip().splitlines()[-1]
    return evidence, outcome


def select_classes(listing: dict, wanted: list[str] | None) -> tuple[list[dict], list[str]]:
    classes = [klass for klass in listing["classes"] if klass["category"] == "Audio Module Class"]
    if not wanted:
        return classes, []
    by_name = {klass["name"]: klass for klass in classes}
    return [by_name[name] for name in wanted if name in by_name], [
        name for name in wanted if name not in by_name
    ]


def qualify_bundle(probe: Path, bundle: Path, plugin: dict, scratch: Path, args, env: dict) -> dict:
    listing_path = scratch / "listing.json"
    listing, outcome = run_probe(
        [str(probe), "list", str(bundle), str(listing_path)],
        listing_path,
        LIST_TIMEOUT_SECONDS,
        env,
    )
    result = {"bundle": bundle.name, "classes": []}
    if listing is None or not listing.get("ok"):
        result["scan"] = {
            "status": "fail",
            "detail": (listing or {}).get("diagnostic") or outcome or "scan failed",
        }
        return result
    result["scan"] = {
        "status": "pass",
        "scan_ms": listing["scan_ms"],
        "binary_sha256": listing["binary_sha256"],
        "class_names": [klass["name"] for klass in listing["classes"]],
    }
    selected, missing = select_classes(listing, plugin.get("classes"))
    for name in missing:
        result["classes"].append(
            {
                "name": name,
                "kind": "",
                "steps": {
                    step: {"status": "missing" if step == "scan" else "not-run", "detail": ""}
                    for step in STEPS
                },
            }
        )
    for klass in selected:
        class_scratch = scratch / klass["id"]
        shutil.rmtree(class_scratch, ignore_errors=True)
        class_scratch.mkdir(parents=True)
        output = class_scratch / "evidence.json"
        command = [str(probe), "class", str(bundle), klass["id"], str(class_scratch), str(output)]
        if args.editor:
            command.append("--editor")
        evidence, outcome = run_probe(command, output, args.timeout, env)
        print(f"{plugin['name']} / {klass['name']}: {outcome or 'completed'}", flush=True)
        result["classes"].append(
            {
                "name": klass["name"],
                "id": klass["id"],
                "kind": klass["kind"],
                "version": klass["version"],
                "steps": complete_steps(evidence, outcome),
            }
        )
        shutil.rmtree(class_scratch / "project", ignore_errors=True)
    return result


def qualify_plugin(plugin: dict, probe: Path, work: Path, args, env: dict) -> dict:
    row = {key: plugin.get(key) for key in ("name", "version", "url", "sha256")}
    if plugin.get("url") is None:
        return {**row, "status": "unavailable", "detail": plugin["unavailable"], "bundles": []}
    try:
        archive = fetch(plugin, work / "downloads")
    except (OSError, ValueError) as error:
        return {**row, "status": "download-failed", "detail": str(error), "bundles": []}
    root = work / "unpacked" / plugin["sha256"][:16]
    try:
        unpack(archive, plugin["archive"], root)
    except (OSError, subprocess.CalledProcessError, tarfile.TarError, zipfile.BadZipFile) as error:
        return {**row, "status": "unpack-failed", "detail": str(error), "bundles": []}
    bundles = []
    for name in plugin["bundles"]:
        bundle = find_bundle(root, name)
        if bundle is None:
            bundles.append(
                {
                    "bundle": name,
                    "scan": {
                        "status": "missing",
                        "detail": "not in the archive; it has " + ", ".join(bundles_in(root)),
                    },
                    "classes": [],
                }
            )
            continue
        scratch = work / "scratch" / plugin["sha256"][:16] / name
        scratch.mkdir(parents=True, exist_ok=True)
        bundles.append(qualify_bundle(probe, bundle, plugin, scratch, args, env))
    shutil.rmtree(root, ignore_errors=True)
    return {**row, "status": "qualified", "detail": "", "bundles": bundles}


def cell(step: dict) -> str:
    status = step.get("status", "not-run")
    detail = step.get("detail", "")
    if status == "pass" or not detail:
        return status
    return f"{status}: {detail}".replace("|", "/").replace("\n", " ")


def notes(steps: dict) -> str:
    load = steps.get("load", {}).get("metrics", {})
    if not load:
        return ""
    tail = load.get("tail_samples")
    return (
        f"latency {load.get('latency_samples')} ({load.get('latency_check')}), "
        f"tail {'infinite/unknown' if tail is None else tail}, "
        f"{load.get('parameters')} parameters"
    )


def markdown(matrix: dict) -> str:
    header = ["Plug-in", "Class", "Kind", *(STEP_TITLES[step] for step in STEPS), "Notes"]
    lines = [
        "| " + " | ".join(header) + " |",
        "| " + " | ".join("---" for _ in header) + " |",
    ]
    for plugin in matrix["plugins"]:
        title = f"{plugin['name']} {plugin['version']}"
        if plugin["status"] != "qualified":
            lines.append(
                f"| {title} | | | {plugin['status']}: {plugin['detail']} |"
                + " |" * (len(STEPS) - 1)
                + " |"
            )
            continue
        for bundle in plugin["bundles"]:
            if bundle["scan"]["status"] != "pass" or not bundle["classes"]:
                lines.append(
                    f"| {title} | {bundle['bundle']} | | {cell(bundle['scan'])} |"
                    + " |" * (len(STEPS) - 1)
                    + " |"
                )
                continue
            for klass in bundle["classes"]:
                steps = klass["steps"]
                cells = [cell(steps[step]) for step in STEPS]
                lines.append(
                    f"| {title} | {klass['name']} | {klass['kind']} | "
                    + " | ".join(cells)
                    + f" | {notes(steps)} |"
                )
    return "\n".join(lines) + "\n"


def tally(matrix: dict) -> dict[str, int]:
    counts: dict[str, int] = {}
    for plugin in matrix["plugins"]:
        for bundle in plugin["bundles"]:
            for klass in bundle["classes"]:
                for step in klass["steps"].values():
                    counts[step["status"]] = counts.get(step["status"], 0) + 1
    return dict(sorted(counts.items()))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, required=True, help="oscillate_vst3_matrix_probe")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--work", type=Path, required=True, help="downloads and scratch space")
    parser.add_argument("--output", type=Path, required=True, help="directory for the matrix")
    parser.add_argument("--only", action="append", help="qualify only this plug-in (repeatable)")
    parser.add_argument("--editor", action="store_true", help="attach each X11 editor")
    parser.add_argument("--timeout", type=int, default=DEFAULT_CLASS_TIMEOUT_SECONDS)
    args = parser.parse_args()
    args.probe = args.probe.resolve()
    if not args.probe.is_file():
        parser.error("build oscillate_vst3_matrix_probe first")
    if args.editor and not os.environ.get("DISPLAY"):
        parser.error("--editor needs an X11 display (run under xvfb-run)")
    plugins = load_manifest(args.manifest)
    if args.only:
        unknown = set(args.only) - {plugin["name"] for plugin in plugins}
        if unknown:
            parser.error(f"not in the manifest: {', '.join(sorted(unknown))}")
        plugins = [plugin for plugin in plugins if plugin["name"] in args.only]
    work = args.work.resolve()
    (work / "tmp").mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, TMPDIR=str(work / "tmp"))
    matrix = {
        "host": platform.platform(),
        "probe_sha256": sha256_of(args.probe),
        "manifest_sha256": sha256_of(args.manifest),
        "editor_attach": args.editor,
        "steps": list(STEPS),
        "plugins": [qualify_plugin(plugin, args.probe, work, args, env) for plugin in plugins],
    }
    matrix["tally"] = tally(matrix)
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "matrix.json").write_text(json.dumps(matrix, indent=2) + "\n", encoding="utf-8")
    (args.output / "matrix.md").write_text(markdown(matrix), encoding="utf-8")
    print(json.dumps(matrix["tally"]))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except HarnessError as error:
        print(f"VST3 matrix: {error}", file=sys.stderr)
        sys.exit(1)
