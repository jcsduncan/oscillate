#!/usr/bin/env python3
"""Summarises a Oscillate performance trace (ADR-0061).

Usage: trace_report.py TRACE.json [--top N]

Reports, from a trace recorded with Ctrl+K "Start performance trace" or
OSCILLATE_TRACE=path:
  - audio callbacks: load against the block's budget, blocks over budget and
    the spans (on any thread) inside the worst ones;
  - where the audio time goes: each kind of span on the audio threads;
  - the UI thread: presented-frame intervals (the ui.frame_interval_ms
    counter) and busy spells over 16 ms, with what ran inside;
  - edits while playing: time from an edit's commit to the callback playing it.
"""

import argparse
import collections
import json
import sys

FRAME_MS = 1000.0 / 60.0
AUDIO_THREADS = ("audio callback", "audio worker")


def load(path):
    with open(path) as f:
        data = json.load(f)
    threads = {}
    spans, instants, counters = [], [], collections.defaultdict(list)
    for e in data["traceEvents"]:
        if e["ph"] == "M":
            threads[e["tid"]] = e["args"]["name"]
        elif e["ph"] == "X":
            spans.append(e)
        elif e["ph"] == "i":
            instants.append(e)
        elif e["ph"] == "C":
            counters[e["name"]].append((e["ts"], e["args"]["value"]))
    for e in spans:
        e["thread"] = threads.get(e["tid"], str(e["tid"]))
        e["end"] = e["ts"] + e["dur"]
    for e in instants:
        e["thread"] = threads.get(e["tid"], str(e["tid"]))
    spans.sort(key=lambda e: e["ts"])
    dropped = data.get("otherData", {}).get("dropped_events", 0)
    return spans, instants, counters, dropped


def percentile(values, p):
    if not values:
        return 0.0
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(p / 100.0 * len(ordered)))]


def inside(spans, start, end, exclude):
    """Spans overlapping [start, end), longest first, without `exclude`."""
    found = [s for s in spans if s is not exclude and s["ts"] < end and s["end"] > start]
    return sorted(found, key=lambda s: -s["dur"])


def section(title):
    print("\n" + title)
    print("-" * len(title))


def audio_report(spans, instants, counters, top):
    renders = [s for s in spans if s["args"]["kind"] == "audio.render"]
    section("Audio callbacks")
    if not renders:
        print("no audio callbacks recorded (was playback running?)")
        return
    loads = [v for _, v in counters.get("audio.load", [])]
    over = [i for i in instants if i["name"] == "audio.over_budget"]
    xruns = [i for i in instants if i["name"] == "audio.xrun_recovery"]
    frames = collections.Counter(s["args"]["n"] for s in renders)
    print(f"{len(renders)} blocks, frames {dict(frames)}")
    if loads:
        print(
            f"load of budget: median {percentile(loads, 50):.0%}  p99 {percentile(loads, 99):.0%}"
            f"  max {max(loads):.0%}"
        )
    server_xruns = [i for i in instants if i["name"] == "audio.xrun"]
    print(
        f"over budget: {len(over)}   PipeWire xrun recoveries: {len(xruns)}"
        f"   JACK xruns: {len(server_xruns)}"
    )
    worst = sorted(renders, key=lambda s: -s["dur"])[:top]
    print(f"\nworst {len(worst)} blocks, with the longest spans inside each:")
    for block in worst:
        print(f"  {block['ts'] / 1000:10.1f} ms  {block['dur'] / 1000:6.2f} ms")
        for s in inside(spans, block["ts"], block["end"], block)[:6]:
            print(f"      {s['dur'] / 1000:6.2f} ms  {s['thread']:16s} {s['name']}")


def where_audio_time_goes(spans, top):
    section("Audio time by span (audio threads)")
    totals = collections.defaultdict(list)
    for s in spans:
        if s["thread"] in AUDIO_THREADS and s["args"]["kind"] != "audio.render":
            totals[s["name"]].append(s["dur"])
    rows = sorted(totals.items(), key=lambda kv: -sum(kv[1]))[:top]
    print(f"{'total ms':>9} {'count':>7} {'mean us':>8} {'max us':>8}  name")
    for name, durations in rows:
        print(
            f"{sum(durations) / 1000:9.1f} {len(durations):7d}"
            f" {sum(durations) / len(durations):8.1f}"
            f" {max(durations):8.1f}  {name}"
        )


def ui_report(spans, counters, top):
    section("UI thread")
    ui = [s for s in spans if s["thread"] == "ui"]
    busy = [s for s in ui if s["args"]["kind"] == "ui.busy"]
    intervals = [v for _, v in counters.get("ui.frame_interval_ms", [])]
    if intervals:
        slow = [d for d in intervals if d > FRAME_MS]
        print(
            f"{len(intervals)} frame intervals: median {percentile(intervals, 50):.1f} ms"
            f"  p99 {percentile(intervals, 99):.1f} ms  over {FRAME_MS:.1f} ms: {len(slow)}"
        )
    stalls = sorted((b for b in busy if b["dur"] / 1000 > FRAME_MS), key=lambda s: -s["dur"])
    print(f"busy spells over {FRAME_MS:.1f} ms: {len(stalls)}")
    for stall in stalls[:top]:
        print(f"  {stall['ts'] / 1000:10.1f} ms  {stall['dur'] / 1000:6.1f} ms")
        nested = [
            s
            for s in inside(ui, stall["ts"], stall["end"], stall)
            if s["args"]["kind"] != "ui.busy"
        ]
        for s in nested[:6]:
            print(f"      {s['dur'] / 1000:6.2f} ms  {s['name']}")


def edit_report(spans, instants):
    section("Edits while playing")
    committed = {i["args"]["n"]: i["ts"] for i in instants if i["name"] == "edit.committed"}
    audible = {}
    for i in instants:
        if i["name"] == "edit.audible":
            audible.setdefault(i["args"]["n"], i["ts"])
    latencies = [(audible[r] - t) / 1000 for r, t in committed.items() if r in audible]
    print(f"{len(committed)} edits committed, {len(latencies)} reached the callback")
    if latencies:
        print(
            f"commit to audible: median {percentile(latencies, 50):.1f} ms"
            f"  p99 {percentile(latencies, 99):.1f} ms  max {max(latencies):.1f} ms"
        )
    steps = collections.defaultdict(list)
    for s in spans:
        if s["args"]["kind"].startswith("edit."):
            steps[s["name"]].append(s["dur"] / 1000)
    for name, durations in sorted(steps.items(), key=lambda kv: -max(kv[1])):
        print(
            f"  {name:36s} count {len(durations):5d}  median {percentile(durations, 50):7.2f} ms"
            f"  max {max(durations):7.2f} ms"
        )


def main(argv):
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("trace")
    parser.add_argument("--top", type=int, default=10)
    args = parser.parse_args(argv[1:])
    spans, instants, counters, dropped = load(args.trace)
    if dropped:
        print(
            f"note: the trace buffer was full; {dropped} older events were dropped"
            " (raise OSCILLATE_TRACE_BUFFER_MB)"
        )
    audio_report(spans, instants, counters, args.top)
    where_audio_time_goes(spans, args.top * 2)
    ui_report(spans, counters, args.top)
    edit_report(spans, instants)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
