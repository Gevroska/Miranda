# Work item: complete adopted recovery and release its original startup exclusion

- Identifier and milestone: inactive P3E2 completion and checked-release slice, following [owner adoption](P3e2-owner-adoption.md).
- Requirements: DEC-02, DEC-05, DEC-16, DEC-17, DEC-22, DEC-31 through DEC-33 and DEC-46.
- Integrated source checkpoint `2eb9e57edd23219cf41469ff054348ba18fff3ad` builds on `f75e2377a8bc93c00c7f71a7994d069f7fe69d88`.
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

Test-only lifecycle barriers and one-result tag and exception faults exercise the actual context and owner handoff. Existing successful adoption cases advance through terminal release and retain their custody and failure coverage. Independent review accepted the formatted integration, including the callback observation and the three checked-reader cases already present on its base.

The real scheduled restoration callback has a test-only function-scope finalization observer. Its existing status guards naturally leave scope before the observer runs; the pinned scheduler still holds its invocation mutex until callback return. The observer can read the revision fence but cannot mutate B. A regression observes completion invalidate that revision, remain blocked on the actual callback, allow copied status reads, and clear only after callback finalization with the actual terminal predicates satisfied. Genuine unavailable, exceptional-retry and unretired callback outcomes refuse completion and retain the reservation.

The current command mutex prevents a generation-changing command during completion. The callback publishes its terminal predicates together under one status guard and then requests stop before returning; a restored callback has no later production status write that could invalidate those predicates. Negative post-fence status mutation is therefore not fabricated in tests. The repeated post-fence checks remain source-reviewed defensive checks, alongside the direct positive fence and genuine refusal cases.

The actual-journal fixture reads through the existing checked read-only byte transaction, sharing its serialization and successful-snapshot identity/byte/path checks. Native denial, absent storage, successful empty storage and malformed actual JSON are distinct. No journal helper recursively enters that transaction from a state hook. This fixture correction changes no production storage policy or sharing rules.

## Validation

All 346 focused Windows native tests passed without skips. They include the 44 new cases and the renamed unarmed case, alongside existing adoption and managed-session regressions. The Windows UCRT64 application and tests and the Web production build passed on a verified source overlay.

The first full run reached the five-minute campaign limit after 592 passes and one skip while the suite was still progressing. A repeat used the same executable and test deadlines with an eight-minute campaign limit. It ran all 1,404 cases in 309.705 seconds and recorded 1,390 passes, 12 skips and the two established external httpbin download failures with CURL7. All 45 new or renamed cases passed in both the focused checks and the completed full run.

| Check / environment | Candidate | Result | Evidence / limitation |
|---|---|---|---|
| Independent review, locked formatting and whitespace | Integrated source checkpoint | Passed | Published reader fixture and its three regressions are preserved. |
| Native Windows component, startup and backend | Integrated source overlay | 207, 32 and 15 passed | Includes actual journal and callback boundaries, refusal of unsafe release, and release-only retries. Driver interfaces are injected. |
| Native Windows owner, managed HTTPS and capture | Integrated source overlay | 59, 6 and 27 passed | Includes Stop during both recovery phases and admission after matching terminal release. |
| Full local native suite | Integrated source overlay | 1,390 passed, 12 skipped, 2 external download failures | First run timed out at the campaign limit. The complete run reported only the established external failures. Hosted checks for this completion change remain pending. |
| Production lifetime, installed driver and physical streaming | None | Unqualified | This slice adds no production recovery registration. |

## Delivery

This is an inactive source candidate. Protected provisioning and production lifetime integration remain pending. After process loss, an empty journal alone supplies insufficient authority to reconstruct the in-memory completion handoff.
