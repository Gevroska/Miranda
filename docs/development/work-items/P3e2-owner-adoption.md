# Work item: atomic ownership of one reconstructed recovery episode

- Identifier and milestone: inactive P3E2 owner-adoption slice, following [owned reconstruction](P3e2-owned-reconstruction.md).
- Requirements: DEC-02, DEC-05, DEC-16, DEC-17, DEC-22, DEC-31 through DEC-33 and DEC-46.
- Base revision: `33397e1d20105ab0cab68c60ac9cca5898f74a10`.
- Observable outcome: one live, quiescent RTSP owner consumes the exact reconstructed startup context and generation atomically, serializes its recovery, and retains it at an explicit `awaiting_completion` boundary.
- Scope: RTSP owner/admission boundaries, Windows recovery backend/startup context, immutable publication of actual process claims, their existing regression fixtures and this work item.
- Exclusions: actual B completion and startup-gate release for adopted contexts, source/application claims, capture metadata or epochs, fresh intent, driver acquisition by add, production providers/factory registration, bootstrap/shutdown integration, provisioning and hardware qualification.
- Shared environment owner: the development coordinator owns formatting, native builds/checks, independent review, infrastructure and publication.

## Interfaces and invariants

`adopt_source_recovery(context)` reserves an offer before owner dispatch without waiting for coordinator work. Caller, coordinator operation, offered and adopted custody states exclude competing mutation. Coordinator inspection/reconstruction through aliases becomes inert during an offer and after adoption; it cannot cancel callbacks, reopen stores or replace B. Refusal returns the same caller reference and leaves the used-generation floor unchanged. Successful custody transfer consumes only that reference; aliases remain observers.

The live owner checks stopping state, source/preflight/recovery ownership, process work and queued requests, queued liveness, pending launches, streams and accepted tokens still capable of admission. Published application lifetime and the actual process cleanup claim must also be clear. Claim publication allocates before changing a claim and publishes the exact instance/generation with nonthrowing pointer replacement. General refresh, operation markers and preallocated catalog publication preserve those fields. Process liveness, source ownership and connections remain separate observations.

A preallocated record, exact backend generation, reconstructed context and strictly newer generation in `(0, UINT64_MAX)` are required. Custody, the generation floor and record install commit through nonthrowing operations at one ordering point. The former separate production generation-seed seam is removed. A timer/dispatch exception after commit retains adopted-but-blocked custody and reports successful adoption, without implying recovery success.

One private recovery record owns its backend, optional startup context, generation, checked operation revision, stage, blocked/coalesced-retry state and sole tagged future. Results must match generation, revision and operation. Revision exhaustion blocks without wrapping. Existing retired/native completion and same-kind rollover retain their separate behavior; adopted records prohibit rollover. All B and lease work runs off-owner. Status copies remain inert and busy throughout adopted blocked and awaiting-completion states. Managed and legacy launch/claim boundaries deny new admissions while custody is retained.

The first operation observes reconstruction's existing restoration. Explicit unretired retry uses the existing revalidated restoration path; removal occurs only at actual B permission. Cleanup retains the exact lease, heartbeat, close-only retry and adapter-issued receipt. Retired retries use existing restoration progress without removing twice. Actual B restoration readiness is published separately from the still-pending startup obligation, including unavailable settings and sticky heartbeat failures. Adopted recovery never calls B completion or releases the startup gate in this slice.

A separate shared proof retains immutable paths, sticky observations and the exact startup exclusion without pointing back to the context or owner. It survives coordinator destruction and remains available to a later completion implementation. That next slice must latch successful B completion before fallible checked gate release, retain gate-release-pending state on failure and never repeat removal/retirement/completion after success.

The owner object must outlive the retained episode. Stopping its executor drains the sole in-flight operation and leaves adopted custody in the server object; successful loop shutdown is not successful recovery. If timer registration fails while recovery or process work remains, the existing owner iteration uses bounded executor dispatch and nonblocking ready-future polls independently of that timer. This drain keeps the executor alive after the listener closes, consumes ready work, preserves the blocked record and starts no new adopted B operation. Accepted process posts also delay stop completion until they are enqueued and drained. Actual owner-object/process destruction remains explicit abandonment under the existing managed-lease destruction limitation. Production shutdown custody transfer and qualification are excluded, and this inactive seam must not be registered until that lifetime contract is implemented. No destructor releases the startup gate.

## Acceptance and review

Thirty-three additional authored regressions and one replacement for the obsolete seed test cover exact/refused adoption, partial/zero/MAX/old generations, duplicate aliases, pending/accepted tokens, live applications and actual claims, existing source/preflight/process work, and admission/process/liveness requests queued behind the adoption callback. They also cover legacy denial, slow off-owner observation, explicit restoration retry, postcommit timer failure, revision exhaustion, unavailable restoration and sticky heartbeat.

Existing actual-journal/B/lease fixtures add unarmed, armed-present, armed-absent and retired adoption; offered/adopted coordinator fencing; an in-flight coordinator operation; sole-context retention after coordinator/caller destruction and executor stop; changed mapping/malformed journal after transfer; and removal failure followed by failed-close and close-only retirement. The fixtures retain real journal/settings/receipt code and inject native primitives. A narrow optional checked-storage observation parameter drives one coordinator barrier; its empty default preserves the existing real-read behavior. Existing availability defaults are unchanged. Process fixtures cover rollback/supersession/clear and unrelated catalog/operation publication retaining the exact claim. Backend assertions require the new readiness field to remain false for inert/no-effects state.

Independent source review identified a timer-registration failure that could strand the sole future and prevent executor stop. The correction adds an owner-loop drain that does not depend on successful timer rearming or a worker completion post. Two additional actual-context regressions hold observation in flight, persistently fail registration after its future is installed, request stop and release the worker. They require bounded stop, consumed futures and identical retained context/generation/authorization/heartbeat/gate, with no shutdown removal or completion. The second also holds concurrent process work and requires it to drain before stop completes. Failure cleanup may restore a healthy registration only after an assertion has already failed; it is outside the successful persistent-failure proof.

The author handoff contained unexecuted regressions. Independent source review accepted the actual diff after the timer-drain correction, and the coordinator results below now cover the integrated candidate. There is no VM, installed-driver, physical-streaming or human-acceptance claim.

## Validation

| Check / environment | Candidate | Result | Evidence / limitation |
|---|---|---|---|
| Author source inspection and whitespace check | Working delta on the stated base | Prepared | Not independent review or compilation |
| Integrated native build | e0a4c501 | Pass | Windows UCRT64 application/test and Web production build; verified frozen source overlay, not a clean committed release build |
| Full native suite | e0a4c501 | 1,346 pass / 12 skip / 2 external failures, 154.131 seconds | 1,360 cases; the same two httpbin downloads report CURL7; no other failures |
| New owner/integration regressions | e0a4c501 | All 45 pass, none skipped | 34 adoption additions/replacements, one publication-portability case, two scoped-command cases, five job-policy cases and three checked-reader cases |
| Separate affected component check | e0a4c501 | 166 pass, none skipped | Includes exact B/journal/recovery logic with injected native primitives |
| B completion / startup release | Excluded | Unimplemented for adopted contexts | Recovery remains busy at `awaiting_completion` |
| Production provider / bootstrap / shutdown / provisioning | Excluded | Unimplemented | No active registration or production lifetime guarantee |
| VM / driver / physical streaming | Excluded | Unqualified | Injected primitives establish no hardware result |

## Delivery

The inactive source prerequisite is independently reviewed and locally validated for the stated checkpoint. Exact-head hosted checks and publication remain separate gates; no release is qualified. Exact completion and checked startup-gate release are the following bounded slice; protected provisioning and production lifetime integration remain separate work.

The native correction preserves the actual lease contract: recovery of an absent adopted GUID still issues its exact removal receipt, and successful removal stops heartbeat while a failed transport close remains owned. Fixture journal observations now use the existing serialized, checked read-only transaction. Read failure, absence, valid empty bytes and malformed data remain distinct; three regressions cover protected-write serialization, permanent denial and those storage states. Earlier failed runs remain recorded, and the original native open error was not captured.
