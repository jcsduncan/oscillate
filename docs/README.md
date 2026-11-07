# Documentation map

Product and engineering contracts. Measurement pages are evidence cited by
ADRs and specs.

| Need | Read |
| --- | --- |
| Product scope and release acceptance | [PRD](PRD.md) |
| Module boundaries and real-time contracts | [HLD](HLD.md) |
| Observable behaviour and acceptance tests | [Functional specs](specs/README.md) |
| Open work | [Issues](https://github.com/jcsduncan/oscillate/issues) |
| Cross-component decisions | [ADR index](decisions/README.md) |
| Build, test and contribution rules | [README](../README.md#build), [AGENTS.md](../AGENTS.md), [CONTRIBUTING.md](../CONTRIBUTING.md) |
| Environment variables and user files | [Environment](environment.md) |

## Evidence and reference

- [Performance](performance/) holds the CPU, latency and qualification
  measurements that ADRs and specs cite. Each page names its build, hardware
  and workload; the numbers describe the code at that time.
- [Qualification](qualification/) describes the fuzz and process-kill
  procedures.
- [Dependencies](dependencies/) and [third-party notices](../third_party/notices/)
  record licensing and package investigations. Distribution approval is
  separate from a successful local build.
- [UI](ui/) holds the interactive HTML mockups that serve as design contracts
  for panels; [plugins](plugins/), [export](export/), [integrity](integrity/)
  and [apps](apps/) explain specific implementations and their limits.

When documents disagree, the PRD controls scope, the HLD controls boundaries
and the functional specs control observable behaviour. Resolve a conflict in
the affected documents before changing that component.
