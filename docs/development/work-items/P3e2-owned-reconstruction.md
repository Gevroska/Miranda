# Work item: inactive reconstruction of an exact owned recovery episode

- Identifier and milestone: P3E2 reconstruction prerequisite, following startup inspection.
- Requirements: DEC-02, DEC-05, DEC-16, DEC-17, DEC-22, DEC-31 through DEC-33 and DEC-46.
- Base: `768dc97a9822c3c2b65c0d690cbcafc90d1c5d8a`; source changes remain inactive.
- Observable outcome: an explicit operation reconstructs the actual pending B journal/settings episode and its required SudoVDA lease or adapter-issued receipt, preserving the original generation and ordinary exclusion across failures.
- Scope: Windows startup coordinator, concrete source backend, checked journal loading and unarmed adapter recovery, a narrow expected-byte B recovery pass-through, and matching component fixtures.
- Exclusions: atomic RTSP adoption, new source/application claims, capture readiness, production provisioning or registration, bootstrap/shutdown integration, gate completion/release, installation and hardware qualification.
- Shared environment owner: the development coordinator owns formatting, builds, native checks, infrastructure and publication.

## Interfaces and invariants

The existing fresh-source `authority_verifier_t::verify()` contract remains compatible. The separate `verify_recovery()` contract denies by default and returns an opaque held authorization rather than requiring an invented source request. A future provider must independently establish protected storage/write authority, deployment identity and exclusive driver control, and retain those guarantees for the authorization object's entire lifetime, including all B callbacks, heartbeats and cleanup. This prerequisite supplies no production provider. The test-only issuer models that lifetime; it does not establish deployment provenance. Journal content, path classification, enum values and caller Booleans do not issue recovery authorization.

Startup distinguishes the expected owned-pending obligation from sticky earlier unsafe, unreadable, malformed, conflicting or changed-pending observations. It reinspects both stores and requires exactly owned pending plus ordinary clear without transaction residue. Earlier uncertainty refuses reconstruction even after apparently safe bytes appear. Later absence or empty bytes discharge neither an owned obligation nor uncertainty. One opaque context is retained before qualification and B work. Competing verifier/path inputs cannot replace it, and same-context retries retain all partial ownership.

Inspection pins checked files and ancestors without write/delete sharing. Retained B retry callbacks are fenced before a later inspection so their own checked transactions cannot conflict with the inspection handles. Those temporary handles close before the B load opens the existing checked byte transaction. That load compares the exact inspected journal bytes inside the transaction; a change in the handoff window refuses recovery. The loaded journal's independent deployment identity and original generation are checked again before driver access. Once B exists, its own unchanged-byte capability validates retries, including legitimate B journal mutations.

Reconstruction owns the actual `owned_display_configuration_t::recover()` result. It does not call `begin`, `begin_next`, add, mode preparation, adapter selection, capture enumeration or probing, and does not synthesize request fields missing from the journal. Armed unretired episodes use the actual recovery lease and exact present/absent binding before restoration. Retired episodes resume B restoration without a driver acquisition or removal. Unarmed episodes obtain an adapter-issued receipt only after unchanged, unretired journal validation, then use the existing receipt-bound retirement and restoration path.

The context and backend retain the authorization, B reservation, optional lease and receipt across refusal, exceptions and retry. An adapter exception after lease adoption returns that actual lease rather than abandoning it. A failed transport close before adoption is retried without removing an output or reopening B, then the same generation can be recovered. Acquisition, cleanup and heartbeat diagnostics remain separate. Existing restoration/removal/close/receipt/retirement ordering is reused. Reconstruction never publishes a capture source, admits a new source, allocates an epoch or releases startup exclusion. Completion remains unavailable in this prerequisite, including after restoration succeeds. The caller must retain the coordinator or returned context; deliberate abandonment still has the existing managed-lease destruction limitation and is not successful recovery.

Heartbeat creation exceptions latch on the actual retained lease, including a repeated failure-handler attempt. Recovery reconstruction, restoration and pre-removal restoration refuse that sticky liveness uncertainty without replacing the lease or clearing diagnostics. A later worker pointer alone cannot establish recovery protection. A disabled watchdog legitimately needs no heartbeat worker. Fresh-source ordered cleanup and pre-adoption close-only retries retain their existing behavior; this prerequisite adds no implicit liveness repair operation.

Every armed recovery restoration entry uses a phase-independent B validation boundary. It fences B callbacks, refreshes the actual lease, validates the unchanged journal, generation, current exact Windows mapping and lease protection, then updates only the transient present/absent binding. Existing durable mappings are immutable. Changed target/path/library identity, ambiguity and disagreement between driver absence and an available Windows target refuse before settings mutation. Unavailable all-path entries are excluded before presence-conflict checks, including a transiently active entry after removal; available inactive targets still conflict. This follows the documented [Windows target availability semantics](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-displayconfig_path_target_info). Exact present/absent transitions remain valid, and an interrupted episode without a durable mapping can persist its first qualified present binding. Fenced retries cannot reuse stale `bound_display` or `owned_absent` flags to authorize restoration.

## Acceptance and review

Thirty-five regressions cover missing or denied held authority, pending state versus prior uncertainty, conflicting stores and changed bytes, exact unarmed/armed-present/armed-absent/retired reconstruction, independent deployment mismatch, stable generation/GUID/baseline/mapping/settings, no repeated add or adapter selection, actual receipt ordering, partial ownership retention, exceptions and competing inputs. Correction cases cover heartbeat-creation exceptions, sticky lease liveness, phase-independent retry binding, exact present/absent transitions, interrupted first binding and Windows target availability.

Independent source review required corrections for heartbeat exception latching, stale retry binding and available versus unavailable all-path entries. Renewed review accepted the resulting source. Locked formatting and the integrated Windows UCRT64 application/test build pass. All 35 new cases pass within the complete affected component, startup and backend suites at source checkpoint `33397e1d20105ab0cab68c60ac9cca5898f74a10`.

## Validation

| Check / environment | Candidate | Result | Evidence / limitation |
|---|---|---|---|
| Independent source review and formatting | Reconstruction integrated in `33397e1d` | Pass | Reviewed failure ownership and corrections; no production authority provider |
| Native affected suites | Verified source overlay equivalent to `33397e1d` | 150 component, 30 startup and 14 backend cases pass without skips | Includes all 35 new reconstruction regressions; injected authorization/native boundaries with real journal, transaction and B settings code |
| Native neighboring suites | Same source overlay | 27 capture, 100 security and 41 source-owner cases pass without skips | Component/integration evidence only; not physical capture or streaming |
| Managed process/permission suite | Same source overlay | Four passes, two preparation failures | Handler-return admission wait and command-entered prerequisite failed; the latter did not reach publication or revocation assertions. Neither establishes a fresh reconciliation-timeout reproduction |
| Hosted portability repair | Follow-up to the stated checkpoint | Source review and formatting pass; execution pending | Conditional atomic shared-pointer APIs and test-only owner capture need renewed native/hosted checks; the new publication regression is not yet executed |
| Production provisioning / RTSP adoption / completion | Excluded | Unimplemented in this prerequisite | No provider, registration, owner adoption or gate-release transition |
| Driver / VM / physical streaming | Excluded | Unqualified | Injected primitives establish no installed-driver or hardware result |

Earlier incomplete full-suite and process-exit failures remain separate evidence. Later scoped passes do not establish their original causes. The native build uses a verified source overlay and is not a clean committed release build.

## Delivery

The exact reconstruction prerequisite has passed independent source review and its affected native component checks. It remains inactive and unreleased. Renewed validation of the portability repair and complete hosted matrix remain outstanding. Atomic owner adoption, ordered completion and protected provisioning are separate bounded work.
