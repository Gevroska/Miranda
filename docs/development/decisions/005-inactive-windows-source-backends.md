# ADR-005: inactive Windows source backends and recovery admission

- Status: proposed; implemented source candidate, execution and independent review pending.
- Date: 2026-10-26.
- Relevant requirement IDs: DEC-02, DEC-05, DEC-16, DEC-17, DEC-22, DEC-31 through DEC-33 and DEC-46.

## Context

The owner and capture prerequisites provide an immutable source record, counted lifetime pins, an operation lane and exact Windows capture verification. SudoVDA control and the owned display transaction already provide durable intent, heartbeat, restoration and adapter-issued retirement receipts. Production activation would additionally require protected provisioning, startup recovery and shutdown integration; those are separate work.

Native preparation must retain ordinary Sunshine display management, including optional configuration fields, tolerated apply failures, retries, display wake and delayed restoration. An owned Virtual recovery episode excludes ordinary display mutation until completion. Ordinary Native restoration likewise prevents a new Virtual episode while its physical recovery remains pending.

## Decision

Construct an inert Windows dispatcher. Construction, status and admission inspection invoke no native or driver boundary. `begin` chooses the requested kind on the existing serialized source operation lane. Every fresh Windows video source, including Native, uses the owner record; joins retain its exact identity and mode.

Freeze the ordinary parser's configure, disabled/revert or unchanged action and its complete optional configuration in the source request. Run checked read-only Windows availability preflight off the owner. A failed query does not establish a headless baseline. Native uses the existing DD_DATA scheduler and persistence through a copied-progress facade. It selects a capturable output, resolves its exact Windows target/path/library identity and obtains actual positive mode and optional HDR through D. Native has no monitor UUID, SudoVDA lease or fabricated cleanup receipt. Successful scheduling retires only its logical source; ordinary pending restoration remains independent. Completion never erases ordinary persistence.

Virtual requires an injected independent authority verifier before B intent or driver access. No production verifier is included, and absence refuses Virtual without qualifying or touching SudoVDA. The interface is for a future protected provisioning verifier; a Boolean setting, driver enumeration, process scan or self-issued journal grants no authority. Once B intent is entered, uncertain failures remain owned. An authority denial before intent has an explicit NoEffects logical retirement. An armed or durable transaction requires its actual adapter receipt.

Virtual composes B begin/acquire/bind/prepare and D verification, preserving separate acquisition, cleanup and liveness diagnostics. It retains heartbeat through restore and failure. Retained reconnect verifies the exact identity in a new owner-issued epoch; the owner must drain earlier pins before dispatch. Probe receives only the existing preissued pin and has no unmanaged fallback. Cleanup refreshes the exact lease, obtains current B removal permission, cleans the lease, validates its receipt and retires the transaction.

Inspect copied admission state before allocating an owner source, increasing its generation or claiming an application. Opposite-kind recovery is busy. A retired Virtual episode can roll to another actual monitor GUID through B `begin_next`; it never invents an identity for Native. Idle retired recovery progresses and completes on the same operation lane, fencing new admissions while that work runs and retaining its logical generation. An internal quiescent seam seeds the owner generation above recovered journals for later startup integration.

## Alternatives and consequences

Reusing the Virtual transaction for Native would require fabricated monitor identities or a new recovery-transfer design. Running ordinary DD_DATA alongside unresolved Virtual recovery would violate B's exclusion. Neither is adopted. Waiting for delayed Native physical restoration before logical retirement would unnecessarily prevent fresh Native preparation; same-kind scheduler supersession remains available.

The copied view avoids entering the scheduler or native API from RTSP status inspection. Periodic off-owner progress adds a bounded polling interval and may return busy during idle completion. Failed cleanup or uncertain durable intent remains unavailable until an explicit retry or later startup recovery succeeds.

Completed progress observations take precedence over a new refresh, with its interval measured from completion. Copied restoration status distinguishes stopped failed work from scheduled unavailable-device recovery. Public retry is generation fenced and serialized; it reschedules the existing stopped Native or B restoration while preserving its journal/settings/receipt and never redoing exact removal. It does not reconstruct the separate uncertain-begin or failed-manager-rollover cases reserved for E2.

## Validation

The [P3E1 work item](../work-items/P3e-inactive-source-backends.md) records the source cases and unexecuted checks. Required evidence includes inert-construction regressions, exact Native/Virtual metadata, optional-field preservation, NoEffects versus receipt retirement, armed partial failures and heartbeat, retained reconnect/pin drain, both cross-kind admission directions, actual-GUID rollover, idle completion and generation fencing. Native execution and independent actual-diff review remain pending. VM, physical streaming and production provisioning are separate gates.

## Supersession

None. This record does not authorize factory registration or production activation.
