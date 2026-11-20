# Work item: complete adopted recovery and release its original startup exclusion

- Identifier and milestone: inactive P3E2 completion and checked-release slice, following [owner adoption](P3e2-owner-adoption.md).
- Requirements: DEC-02, DEC-05, DEC-16, DEC-17, DEC-22, DEC-31 through DEC-33 and DEC-46.
- Base revision: `62c437ee153854b54d47c14b53bd77814fef153b`.
- Observable outcome: the same adopted context, generation, concrete B, independently held authorization and startup proof continue through verified journal completion and fresh checked release of their original ordinary exclusion. RTSP admission stays blocked until the owner consumes the matching terminal result.
- Scope: the Windows source backend and startup coordinator, B completion, RTSP recovery progression, and their existing component fixtures.
- Exclusions: production provider/factory registration, bootstrap and shutdown integration, source/application claims, capture metadata/epochs, new intent or driver acquisition, driver/storage transaction changes, provisioning, physical streaming and hardware qualification. Actual owner-object/process destruction remains abandonment under the existing lifetime contract.

## Acceptance

Unarmed, armed-present, armed-absent and already-retired episodes keep their original identity and generation. Actual retirement, clear restored settings, no retry or sticky liveness failure, and no lease or transport cleanup obligation precede B completion. An absent adopted GUID still needs the exact adapter-issued removal receipt; successful removal stops its heartbeat even if transport close must be retried.

The retained context has a private atomic committed-effects latch: not completed, release pending, or released. Only the owner can invoke its private operations. Arbitrary backends, inert fixture contexts, copied readiness, a caller Boolean, empty files or another coordinator cannot create completion authority. Generic backend completion continues to deny recovery-only episodes.

B completion holds its command lane, fences scheduler callbacks without holding the status mutex, and rechecks the actual generation, restored/retired state, unavailable settings and retry state. All required status locking precedes the checked journal clear. Successful clear has no later fallible status-lock/publication tail; reservation release is nonthrowing. The context latches release pending immediately on successful return and retains B and independent authorization.

Release uses the original shared proof after coordinator destruction. Fresh bounded storage reads reuse the checked path/identity/stable-byte reader and classifiers. Both immutable obligations must be clear, transaction residue absent, and historical uncertainty absent. Inspection handles remain alive through the exact capability's ordinary transition, which also refuses another B reservation or ordinary manager. Successful release resets only that retained capability and latches released before copied publication.

A failed, throwing, stale or unpublished result retains owner custody. Once B completion succeeds, explicit retries perform only fresh release checks; removal, retirement, restoration, progress and completion do not repeat. A verified clear with secured transaction residue is completed B with release pending. A false or uncertain clear followed by an empty destination never creates proof. Resolving retained transaction material remains external to this slice.

The sole tagged future retains generation/revision/operation matching, the generation floor, coalesced explicit retries and revision-overflow refusal. Stop consumes accepted completion/release futures through ready-only polling independent of timer registration, starts no following phase and retains unfinished custody. An accepted release may finish during drain. Caller/coordinator aliases remain fenced after terminal completion and cannot recover custody.

## Implementation and review

The existing backend, checked storage reader, ordinary exclusion transition and RTSP operation lane are extended at their current interfaces. No parallel recovery subsystem or new authority provider is introduced. The test storage reader moves with the private proof so injected native boundaries can survive coordinator destruction without changing production reads.

Test-only lifecycle barriers and one-result tag/exception faults exercise the actual context and owner handoff. Existing successful adoption cases advance through terminal release; their custody/failure coverage remains alongside new completion/release barriers. Independent source review accepted the first frozen candidate without an actionable production defect. The callback-observation and checked-reader corrections require renewed review of the actual integrated diff.

The real scheduled restoration callback has a test-only function-scope finalization observer. Its existing status guards naturally leave scope before the observer runs; the pinned scheduler still holds its invocation mutex until callback return. The observer can read the revision fence but cannot mutate B. A regression observes completion invalidate that revision, remain blocked on the actual callback, allow copied status reads, and clear only after callback finalization with the actual terminal predicates satisfied. Genuine unavailable, exceptional-retry and unretired callback outcomes refuse completion and retain the reservation.

The current command mutex prevents a generation-changing command during completion. The callback publishes its terminal predicates together under one status guard and then requests stop before returning; a restored callback has no later production status write that could invalidate those predicates. Negative post-fence status mutation is therefore not fabricated in tests. The repeated post-fence checks remain source-reviewed defensive checks, alongside the direct positive fence and genuine refusal cases.

The actual-journal fixture reads through the existing checked read-only byte transaction, sharing its serialization and successful-snapshot identity/byte/path checks. Native denial, absent storage, successful empty storage and malformed actual JSON are distinct. No journal helper recursively enters that transaction from a state hook. This fixture correction changes no production storage policy or sharing rules.

## Validation

Forty-seven new cases and one renamed/updated unarmed case are **authored and unexecuted**, alongside updated successful adoption expectations. No formatting, compilation, test execution, VM operation, CI operation, integration or publication was performed for this slice.

| Check / environment | Candidate | Result | Evidence / limitation |
|---|---|---|---|
| Source and whitespace inspection | Working candidate on the stated base | Inspected; whitespace check clean | This establishes source structure only. |
| Actual journal/B/lease fixtures with injected native boundaries | Working candidate | Authored, unexecuted | Success variants, changed bytes, exact command-lane fencing, copied status during clear, and failed/uncertain versus verified-residue transactions. |
| Checked release and owner fixtures | Working candidate | Authored, unexecuted | Read/path/obligation/residue/exception failures; exact gate, B reservation and ordinary-manager refusal; release-only retries, duplicate coalescing, dispatch/result/revision failure and final admission after matching terminal consumption. |
| Stop and persistent timer-registration failure | Working candidate | Authored, unexecuted | Before entry, during actual clear, between phases, before accepted release entry and within checked release read. Existing observation/process drain cases retain their original deadlines. |
| Formatting, native focused/full suites and hosted checks | Working candidate | Not run | Required on the integrated, independently reviewed candidate. Earlier base evidence does not qualify these changes. |
| Production lifetime, clean target and physical streaming | None | Excluded, unqualified | No production recovery registration or hardware claim. |

## Delivery

This is an inactive source candidate. The next actions are independent review, formatting and focused/full affected native validation of the integrated revision. Production lifetime integration and protected provisioning remain separate work; process loss cannot reconstruct this in-memory completion handoff from empty storage.
