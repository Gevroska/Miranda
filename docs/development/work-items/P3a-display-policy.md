# Work item: Persist and resolve immutable display preferences

- Identifier and milestone: P3a; bounded policy/identity prerequisite for P3.
- Requirements / feature IDs: DEC-16/17/22/29/31/32/33/44/45/46; D06/D07/D09.
- Candidate/base revision: reviewed implementation on `codex/display-policy-integration`, based on `b55a428bdd59ea080dd9ff76c7441c330954ede4`; final P1b ancestry and hosted integration remain pending.
- Scope: typed global/application/device preferences, deterministic monitor identities, pure immutable source request, authenticated administration controls and focused regressions. The shared-file credential writer is included only to preserve typed display settings during password changes.
- Interfaces and invariants: [ADR-004](../decisions/004-immutable-display-policy.md). Existing pairing UUID, P1c application UUID, host-enforced grants, secure byte transactions and modern Sunshine display selection remain the boundaries.
- Explicit exclusions: production SudoVDA activation, source ownership/state machine, monitor mapping, capture changes, actual restoration/removal, driver/installer/migration qualification, Input Only implementation and release publication.
- Dependencies / shared environment owner: coordinator owns formatting, builds, native/Web tests, CI, VM/hardware and publication. No infrastructure changes or dependency/lockfile updates are included.

## Acceptance

Global, app and authenticated-device opt-ins combine without a physical veto. Exact per-device dimensions/rational rates override only fresh ordinary mode selection. Identity uses full namespace/domain-separated persistent UUID tuples, with reserved protocol Desktop. A later viewer returns the same const source request and cannot change its identity, mode, HDR, topology or retention policy. Display-free admissions acquire no video source.

Saved preferences survive restart and typed unknown metadata survives successful edits, including credentials sharing the state file. Malformed device metadata remains visible, is preserved through unrelated grants and cannot silently produce a fresh physical source. Valid administration repairs it. New pairings gain no rights; preference-only saves do not invalidate prepared tokens, and combined permission/preferences changes remain atomic. Failed saves preserve exact original bytes and prior authority/preferences.

DEC-46 resolves tracked-application retention by default and restoration/removal when Restore on disconnect is enabled, with the existing delay. A UUID alone does not establish application lifetime. Runtime transitions remain unimplemented in this slice.

## Implementation and review

The actual source diff and persistence-refusal correction passed independent review. Rejected persisted-document shapes report failure rather than the byte transaction's successful read-only result, so failed saves cannot publish authority, preferences or credentials. Native build and local native/Web validation are complete within the limits below. See [ADR-004](../decisions/004-immutable-display-policy.md) for encoding/schema contracts and tradeoffs. UI text explicitly states that virtual-display activation is unavailable in this build.

## Validation

| Check / environment | Candidate | Result | Evidence / limitation |
|---|---|---|---|
| Source inspection and formatting | Final reviewed P3a source | Independent review, locked formatter and whitespace checks pass | No driver/runtime claim. |
| UUID vector fixture generation | Frozen encoding v1 | Independent Python constants generated | Does not execute C++ or test a monitor. |
| Native Windows build | Frozen final source | Application and test build pass; corrected CSRF fixture rebuilt successfully | MSYS2 UCRT64, pinned dependencies. |
| Persistence-refusal regressions | Final production inputs | Five tests pass | Malformed pairing/credential documents, unchanged published state and prepared tokens, valid recovery and byte-transaction contracts. |
| CSRF fixture correction | Final source | Corrected test passes with missing-token and invalid-token denial checks | Initial focused run exposed an incorrect fixture expectation of 403; established denial response is 400. Independent review accepted the test-only correction. |
| Full native suite | Final source | 1,025 pass, 11 skip, two fail out of 1,038 | Both failures are the inherited httpbin base64/redirect downloads, each with CURL connection error 7. No new failure remains. Skipped platform, licensed-driver and workflow cases remain unqualified. |
| Web regressions/build | Unchanged final Web inputs | 248 tests across 13 files and production bundle pass | Device mode entry, exact rational conversion, app/global settings and authoritative readback. |
| Hosted checks / integration | Final candidate not published | Pending | Current P1b ARM64 security-preservation failure must be corrected and reconciled; local results do not qualify that platform. |
| Driver / VM / physical streaming | No activated source | Not run | No virtual-source lifecycle implementation or qualification is claimed. |

## Delivery

Local validation is complete; no P3a push, merge or release is qualified. Next: reconcile corrected P1b/P1c ancestry and run exact-candidate hosted checks. The next production interface takes the current authenticated client's `display`/`display_valid`, selected/captured app UUID/force/lifetime semantics, modern Sunshine's already selected source mode and an optional active immutable source snapshot. Display transaction, lifecycle admission and capture binding must be integrated together before any production acquisition.
