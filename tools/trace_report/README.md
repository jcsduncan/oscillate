# Trace report

Summarises a performance trace (ADR-0061). To record one, use the command palette (Ctrl+K): "Start a performance trace", do the thing that feels slow, then "Stop and save the performance trace". The trace is saved under `~/.local/share/oscillate/traces/`. Alternatively, launch with `OSCILLATE_TRACE=/path/trace.json` to record from start to quit.

```sh
python3 tools/trace_report/trace_report.py ~/.local/share/oscillate/traces/trace-*.json
```

The report covers:
- audio load and blocks over budget;
- the spans on any thread inside the worst blocks;
- audio time by device and track;
- UI busy spells over a frame, with what ran inside them;
- the time from committing an edit to hearing it.

A recording keeps up to `OSCILLATE_TRACE_BUFFER_MB` of events (default 32 MiB); the report says when older events were dropped. For the full timeline, open the JSON at https://ui.perfetto.dev.
