## What and why

<!-- What this changes and the problem it solves. Link the issue: Fixes #N -->

## How

<!-- The approach, and anything a reviewer should read first. Link the ADR for
     changes to module boundaries, the project format or a real-time contract. -->

## Tests

<!-- The tests you added or ran, and what they check. -->

## Checklist

- [ ] Title follows CONTRIBUTING.md: `type(scope): summary`, scope required for feat, fix, perf, refactor, test and revert
- [ ] Changed behaviour has tests that fail without the change; docs-only changes pass the doc checks
- [ ] No allocation, locks or unbounded work on the audio thread
- [ ] Docs describe the current behaviour
