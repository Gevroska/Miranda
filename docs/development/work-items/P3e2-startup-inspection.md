# Work item: inactive Windows startup inspection prerequisite

- Identifier and milestone: P3E2 stage 1, checked startup classification and continuous ordinary-display exclusion.
- Requirements: DEC-02, DEC-05, DEC-16, DEC-17, DEC-22, DEC-31 through DEC-33 and DEC-46.
- Base: local checkpoint `49d3ef1aeee4ff7a6259b708cb2923f113f97e6f`; no publication or qualification is implied.
- Observable outcome: an explicit operation-lane inspection distinguishes checked-clear state, valid pending legacy or owned state, conflicts and uncertainty before ordinary display mutation.
- Scope: Windows startup coordinator, a narrow ordinary DD exclusion/copied recovery view, the backend admission guard, Windows source registration and focused regressions.
- Exclusions: production callers/registration, protected provisioning or receipt creation, owned episode reconstruction/adoption, bootstrap/drain, driver/service/installer operations and hardware qualification.

## Implementation and invariants

Construction and copied status perform no storage or native work. Explicit inspection acquires a sticky capability retained by DD_DATA, then stops any existing ordinary scheduler before reading state. The gate excludes ordinary initialization, configuration, restoration, reset and their existing unmanaged entry paths. It also covers native-facade mutation and ordinary teardown restoration. The gate is independent of B's reservation, so B construction/recovery can retain continuous ordinary exclusion even when no B object is returned. B's existing pending-legacy busy branch does not restart ordinary work across this gate.

Reads require absolute local fixed-drive paths, checked non-reparse ancestors, matching final handle paths, single-link regular leaves and bounded stable bytes. Open ancestor and leaf handles exclude write/delete opens through classification; missing leaves are checked again after inspecting both stores. Unreadable paths and missing/unreadable ancestors cannot establish absence. Retained owned `.tmp-*` or `.backup-*` material blocks startup without choosing or deleting a file. JSON inspection rejects duplicate fields and excessive nesting, uses the pinned libdisplaydevice converter for legacy/library records and checks the current owned schema. A pending classification is structural evidence, never an ownership capability, cleanup receipt or deployment grant.

Inspection alone never releases the gate. A separate transition rereads both stores and permits ordinary operation only with checked-clear owned state and clear or valid pending legacy state. Existing ordinary managers and B reservations refuse this transition. The transition binds later ordinary initialization to the inspected persistence path and publishes the copied legacy obligation before opening the gate. Backend admission reads only that copied view: unresolved startup excludes both source kinds; legacy recovery excludes Virtual before generation/process claims while preserving Native's existing same-kind behavior. Ordinary persistence remains the authority for completion. No virtual intent is created from legacy state.

Refusal, a failed read, B construction/recovery failure and coordinator destruction retain exclusion. The same retained coordinator can explicitly reinspect; a different coordinator cannot steal or release its capability. After any owned intent or uncertain storage observation, later absence/empty bytes alone cannot discharge the obligation. The copied status requests recovery proof; this stage deliberately has no fabricated completion transition. A later reconstruction stage must retain this coordinator through ownership handoff and connect actual B completion. There is no public bypass Boolean or destructor-based safe-state inference.

No production provisioning verifier exists. Pending owned state therefore remains blocked. Checked-clear Native startup does not require optional Virtual provisioning. The storage checks do not claim protected deployment provenance or exclusion against independently administered controller/writer activity; those require the separate qualified provisioning and deployment lifetime before production Virtual activation.

## Acceptance and evidence

Independent review accepted the startup classification, exclusion and lifetime behavior. The integrated Windows UCRT64 application/test build passes. The new startup test uses the existing protected Windows include group so formatting preserves the required header order; removing that group reproduces the reviewed test body exactly.

| Area | Native evidence | Remaining acceptance |
|---|---|---|
| Inert behavior and checked classification | All 27 startup cases pass, covering construction/status, absent/empty combinations, owned/legacy conflicts, malformed/unreadable/unsafe storage, actual writer schema, alias/link/refusal and transaction residue | Protected deployment provenance and production startup wiring |
| Continuous exclusion and lifetime | The same 27 cases exercise the actual scheduler barrier, native facade and shared DD guard; all 119 owned-display/component cases pass, including failed construction and same-journal recovery under the retained startup gate | Actual owned-episode reconstruction/adoption and shutdown integration |
| Backend and neighboring contracts | All 13 concrete-backend, 89 security/authorization/persistence, 212 session-owner, 27 capture-binding and six managed-process cases pass; four targeted fixture corrections also pass | Exact integrated hosted candidate, installed driver and physical streaming |
| Creation-time scenarios | Three of five cases pass; the close-fault/retry tunneling observation and parent-handle comparison are unexercised after their publication/component checks. Neither is counted as an exercised pass | Preserve these coverage limits and prior scoped observations |

These are native component, fake-display and filesystem results. They do not activate a backend or establish driver, actual display, encoder, VM application or streaming qualification. Earlier managed-entry and process-exit failures remain separately recorded; later scoped passes do not establish their original causes. No new full/GPU suite is claimed.

## Delivery

Integration, formatting/build/native checks and independent review belong to the coordinator. This stage remains inactive. Protected provisioning, reconstruction of actual owned episodes, owner adoption/bootstrap/drain and main/administration ordering remain bounded follow-up stages. No delivery state in `STATUS.md` is changed by this prerequisite.
