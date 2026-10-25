# Work item: Persist and resolve immutable display preferences

- Identifier and milestone: P3a; bounded policy/identity prerequisite for P3.
- Requirements / feature IDs: DEC-16/17/22/29/31/32/33/44/45/46; D06/D07/D09.
- Candidate/base revision: uncommitted implementation on `codex/display-policy`, based on `b55a428bdd59ea080dd9ff76c7441c330954ede4`; final ancestry/integration belongs to the coordinator.
- Scope: typed global/application/device preferences, deterministic monitor identities, pure immutable source request, authenticated administration controls and focused regressions. The shared-file credential writer is included only to preserve typed display settings during password changes.
- Interfaces and invariants: [ADR-004](../decisions/004-immutable-display-policy.md). Existing pairing UUID, P1c application UUID, host-enforced grants, secure byte transactions and modern Sunshine display selection remain the boundaries.
- Explicit exclusions: production SudoVDA activation, source ownership/state machine, monitor mapping, capture changes, actual restoration/removal, driver/installer/migration qualification, Input Only implementation and release publication.
- Dependencies / shared environment owner: coordinator owns formatting, builds, native/Web tests, CI, VM/hardware and publication. No infrastructure changes or dependency/lockfile updates are included.

## Acceptance

Global, app and authenticated-device opt-ins combine without a physical veto. Exact per-device dimensions/rational rates override only fresh ordinary mode selection. Identity uses full namespace/domain-separated persistent UUID tuples, with reserved protocol Desktop. A later viewer returns the same const source request and cannot change its identity, mode, HDR, topology or retention policy. Display-free admissions acquire no video source.

Saved preferences survive restart and typed unknown metadata survives successful edits, including credentials sharing the state file. Malformed device metadata remains visible, is preserved through unrelated grants and cannot silently produce a fresh physical source. Valid administration repairs it. New pairings gain no rights; preference-only saves do not invalidate prepared tokens, and combined permission/preferences changes remain atomic. Failed saves preserve exact original bytes and prior authority/preferences.

DEC-46 resolves tracked-application retention by default and restoration/removal when Restore on disconnect is enabled, with the existing delay. A UUID alone does not establish application lifetime. Runtime transitions remain unimplemented in this slice.

## Implementation and review

Source implementation is prepared; independent actual-diff review, formatting and execution remain pending. See [ADR-004](../decisions/004-immutable-display-policy.md) for encoding/schema contracts and tradeoffs. UI text explicitly states that virtual-display activation is unavailable in this build.

## Validation

| Check / environment | Candidate | Result | Evidence / limitation |
|---|---|---|---|
| Source inspection and whitespace check | Working P3a source | `git diff --check` passed during authoring; final coordinator check pending | No runtime claim. |
| UUID vector fixture generation | Frozen encoding v1 | Independent Python constants generated | Does not execute C++ or test a monitor. |
| Focused native regressions and build | To be frozen by coordinator | Not run by worker | New policy tests plus existing config, paired-client, process, HTTP and secure-file fixtures. |
| Web regressions/build | To be frozen by coordinator | Not run by worker | Device mode entry, exact rational conversion, app/global settings and authoritative readback. |
| Independent review / hosted checks | To be frozen by coordinator | Pending | Critical shared-state/authentication delta requires actual-diff review. |
| Driver / VM / physical streaming | No activated source | Not run | No virtual-source lifecycle implementation or qualification is claimed. |

## Delivery

No commit, push, merge or release was performed by this worker. Next: coordinator integrates final P1b/P1c ancestry, reviews the candidate, runs locked formatting and relevant native/Web/CI checks, and records the resulting revision/evidence. The next production interface takes the current authenticated client's `display`/`display_valid`, selected/captured app UUID/force/lifetime semantics, modern Sunshine's already selected source mode and an optional active immutable source snapshot. Display transaction, lifecycle admission and capture binding must be integrated together before any production acquisition.
