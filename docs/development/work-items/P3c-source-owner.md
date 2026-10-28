# Work item: serialize managed source admission and lifetime

- Identifier and milestone: P3c, source prerequisite for the first release's display lifecycle.
- Requirements: DEC-22/31–33/44–46; joining viewers preserve the active application, display and mode; prepared, media, capture and process lifetimes remain distinct.
- Candidate/base: uncommitted source candidate on `codex/display-owner`, based on `2d99f8f0ba800137b3c813262e0387f922f8b844`.
- Scope: RTSP owner, HTTPS admission, process lifetime snapshots/events, stream ownership transitions, one shared capture contract, and focused regressions.
- Dependencies: corrected P3b transaction/lease interfaces and P1b persistence correction; the coordinator owns integration, formatting, native checks, deployment and publication.
- Exclusions: production SudoVDA activation, capture implementation, startup recovery/headless probing, trusted deployment provisioning, installer, hardware operations and release qualification.

## Observable outcome

An explicitly injected backend reserves one source before blocking preparation. A second first admission is busy. An active join inherits the exact frozen source metadata and P3a request; it cannot replace the application or negotiate another source mode. A retained reconnect validates the original mapping and starts a new capture/probe epoch. Merely observing a running app without an owner record still uses fresh P1c configure/probe behavior on the legacy path.

The source record separately owns prepared-token, successful-media-slot and actual-capture-resource accounting. ANNOUNCE converts prepared ownership only after successful start. Timeout, revocation, failed ANNOUNCE, handshake clear and slot erasure use an idempotent release transition. Slot removal follows stop/join; actual capture pins can outlive it. Every outstanding prepared pin survives another viewer's departure.

At zero pins, DEC-46 retains the frozen source only for its matching, genuinely tracked app incarnation and retention policy. Desktop, placebo and auto-detached placeholders supply no lifetime. Restore-on-disconnect uses the frozen restoration delay. An owner timer observes retained app exit without media workers. A stale app event cannot release a different incarnation.

Revocation answers a pending admission immediately, then drains blocking work before application rollback and source restoration. Fresh authority is checked before worker effects, before application dispatch and before ready publication. Managed process cleanup posts an instance-tagged event and skips direct legacy restoration. Unmanaged behavior remains available.

## Interfaces and ordering

`source_backend_t` is a dormant portable seam modeled around P3b's owned transaction and lease. The backend owns actual journal/recovery state and the private checked cleanup receipt. Blocking begin/retained validation/probe/application/restore/cleanup/complete operations run outside the RTSP owner, with generation/revision-tagged completions. The owner polls copied status; it never sleeps awaiting apply or revert.

Sticky heartbeat failure invalidates affected admissions and stops actual media slots while preserving ownership. Restoration precedes checked cleanup/receipt retirement. Removal/retirement failures block new admissions and require explicit retry; they do not silently drop the source. Retired unavailable-display recovery remains retained independently for a later backend `begin_next` episode.

Successful cleanup followed by in-flight recovery completion remains busy without reporting a cleanup failure. The blocked flag is published only after an actual failed cleanup, claim release or completion, so a retry made in response to that flag cannot be consumed before the failing completion is observed. Completion retry does not repeat restoration/removal.

`capture_source_t` contains immutable generation, epoch, kind (native/owned virtual), exact selector/library identity, typed optional Windows target LUID/ID/UTF-8 path, observed mode, optional observed HDR and a separate trailing optional requested HDR. Native never fabricates a monitor GUID and keeps ordinary tolerant mode/HDR observations, including retained revalidation. Owned virtual creation checks exact dimensions/rational rate and only an explicit parsed HDR setting. The owner copies `requested_hdr` from the frozen source request into published capture metadata before validation/probing; structural equality includes it. This optional runtime policy comes from the actual ordinary parser; a client stream-HDR hint is not a display mutation. The owner preissues a probe pin before dispatch and a session pin during stream allocation. Metadata owns no pin; consumers share/transfer the existing ticket through actual resource destruction. Final release only posts an event, and failed delivery leaves accounting outstanding.

**D capture integration remains pending.** It must pass metadata and the preissued ticket through both capture implementations, probing, reset/reinitialization and detached teardown. It must reject selector disappearance, reuse, ambiguity, display switching and cross-epoch queue mixing without fallback. A stream join alone is not capture quiescence. This source candidate does not establish those properties.

The C-R1–R4 source correction adds an immutable published process/catalog snapshot and one owner-ordered FIFO for execution, rollback, claims, catalog work, Stop and coalesced liveness. Managed preflight refresh is an owned process operation followed by owner continuation. Owner readers and indirect stream/control cleanup use published observations rather than taking the process mutex. HTTP callers await deferred operations outside the owner; paired authority is rechecked after process-lock acquisition and before response publication. Catalog mutation inputs are owned values and retain exact-content preconditions and save-before-publication environment handling. Actual Stop immediately invalidates admissions and queues exact-incarnation/source termination; source restoration waits for that work. A configured command may still delay final process termination/shutdown outside the owner; no universal native cancellation bound is claimed.

Execution reports the exact incarnation created by the attempt, with zero output for pre-execution rejection/failure; rollback matches and terminates under one process guard. A worker skipped before begin drops only its local reservation, restores an exact prior management claim if necessary and preserves independent older recovery. Entered uncertain backend operations remain fail closed. Null/throwing allocation cannot steal legacy process cleanup ownership.

The C-R5 correction resolves logical running/auto-detach state and genuine process/group tracking in one observation under the process guard before publishing it. A successful fast launcher remains a logical placeholder while its genuine tracked flag is false, so legacy stream/control decisions preserve it and DEC-46 cannot retain it. Owner/control readers still take no process mutex. The C-R6 regression invokes malformed ANNOUNCE on the actual owner and checks the wire rejection, prepared-pin release and retirement. Catalog tests separately verify the process lane, nested inline execution and startup fallback.

No production backend factory is registered. Registration alone supplies no deployment authority. Corrected B + C + D, startup recovery/headless admission, protected journal storage and an independently qualified provisioning receipt must be integrated and reviewed before enabling production acquisition. Automatic headless environment selection and Artemis virtual-request integration also remain integration work; the dormant HTTPS path presently resolves configured global/app/device policy using the pinned ordinary mode parser.

## Acceptance and evidence

The inert owner cases cover the source lifecycle plus skipped begin, independent pending recovery rollover, deferred-join reservations, native-first/force-virtual joins and explicit/unspecified HDR propagation, equality and native retained tolerance. Real short-command regressions gate exit before initial publication and between a live observation and later publication, then exercise the actual control termination and last-session legacy restoration decisions before a second refresh. Actual TLS/real-process regressions cover app removal before execution, cancellation at the process-lock boundary, null/throwing factories, claim restoration, and a real configured command gated while owner expiry, revocation, snapshot reads, catalog work, authorized Stop and shutdown dispatch proceed. The authenticated administration Stop route has a deferred-work regression. Stream fault injection enters actual start after control registration and after one worker starts, using inert I/O dependencies. These tests remain uncompiled/unexecuted in this worker checkpoint.

| Check / environment | Candidate | Result | Limitation |
|---|---|---|---|
| Source inspection and whitespace/diff checks | Uncommitted candidate above | Worker source checks only | Final private handover records exact paths/hashes and outcomes. |
| Formatter, native build and unit execution | Same candidate | Not run by worker | Coordinator owns this campaign. |
| Independent review | Actual final diff | Pending | Critical owner/permission/process transitions require independent review. |
| D capture and concrete B integration | Later integrated candidate | Pending | No capture-lifetime or driver claim. |
| VM, physical streaming and release | Later qualified candidate | Not run | No production activation or release qualification. |

## Delivery

This is a bounded, inactive source prerequisite. No commit, push, PR, deployment, dependency operation, native test or hardware operation was performed by the worker. The next action is coordinator source review and formatting/build checks, followed by D/backend/startup/provisioning integration and independent review of that actual candidate.
