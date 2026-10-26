# Work item: observable owned display transactions and recoverable SudoVDA intent

- Identifier and milestone: P3b infrastructure, before the integrated P3 admission/capture slice.
- Requirements / feature IDs: D01-D07, D09; DEC-16/17/22/29/31-33/44-46.
- Candidate/base revision: working source on `b55a428bdd59ea080dd9ff76c7441c330954ede4`.
- Scope and owned paths: `src/display_device.{h,cpp}`, `src/platform/windows/sudovda.{h,cpp}` and their component regressions.
- Interfaces and invariants: pre-add topology/primary journal, exact owned monitor identity, observable bounded apply attempts, restoration/readback before removal, distinct source retirement and unavailable-display recovery.
- Explicit exclusions: production acquisition/admission/capture call sites, display policy/UI, driver installation/trust, hardware operations, publication and release qualification.
- Dependencies / shared environment owner: the coordinator owns integration, independent review, formatting, native/Web/CI and hardware campaigns. Dependency pins are unchanged.

## Acceptance

The caller reserves one display-recovery context before hotplug. It persists the actual pre-add topology/primary and exact source identity at a trusted secured journal path. Adapter preflight must prove the GUID absent before atomically arming an add. A new unarmed intent cannot authorize deletion of an unknown existing GUID.

The Windows wrapper uses the existing `SettingsManager`, `PersistentState`, `WinApiLayer`, `WinDisplayDevice` and `RetryScheduler`. Pollable generation-tagged status reports preparation, observed capture selector/mode/HDR, rollback and removal readiness. Legacy mutation is excluded during the recovery episode; existing unmanaged asynchronous behavior remains available outside that reservation.

The owner releases all capture/admission/media pins before requesting restoration. It keeps the lease and heartbeat alive through restoration and its delay, revalidates immediately before explicit exact-GUID cleanup, then presents the adapter's remove-and-close receipt for durable source retirement. An unarmed attempt receives a separate no-add/closed-handle receipt. Failed armed acquisition retains its lease, primary error, heartbeat failure and uncertain output state for checked enumeration and ordered restoration.

Source retirement retains unavailable physical-display obligations. `begin_next` fences idle retries, atomically rotates the source identity/pre-add baseline while preserving library settings verbatim, and reconstructs one coherent manager/cache. A returning display cannot trigger background restoration while the new source is preparing, active or retained. Old journal capabilities and generations cannot mutate the replacement source. The recovery episode ends only after retirement and empty library persistence.

An initially-headless generation is explicit. It never submits empty topology as a valid library configuration. Its private discard path first proves that every persisted initial/modified/mode/HDR/primary reference belongs exclusively to the exact owned output and matches the manager cache. It subtracts only that output while preserving and verifying active unowned topology, modes, HDR and primary; then uses the library's persistence/cache reset and releases retained audio. Physical obligations or ambiguous evidence block this exception. The sole-owned-output case remains active until exact removal.

## Implementation and review

The journal uses the existing secured byte transaction, including its atomic replacement and durability behavior. It contains immutable per-source identity/baseline plus mutable library recovery bytes and add/retirement state. Every expected-byte refusal is propagated as failure using an explicit matched/validated result; a read-only `nullopt` callback alone is not treated as rejection.

Trusted-storage and actual exclusive-deployment evidence come from the caller. A process-local lease guard does not establish exclusivity against other controllers. Journal contents alone are not ownership proof. Journal-backed lease abandonment retains intent and never implicitly removes before restoration; stopping callbacks and closing transport cannot guarantee watchdog liveness after abandonment.

The source remains dormant. Independent review accepted the corrected actual diff, including the four initial recovery/mapping findings and the later native include and JSON value-copy corrections. The coordinator validated the resulting candidate on the P3a integration revision `2d99f8f0ba800137b3c813262e0387f922f8b844`. This work does not complete P3.

The source corrections following the first independent review retain the library episode's latest nonempty state in the same atomic settings update. Restoration readback uses its original topology/primary evidence after cache clear, so a later source's interim panel primary cannot contradict a returning dock's original primary. The per-source pre-add baseline remains ownership evidence.

The unpublished journal schema is version 2; version 1 is rejected without migration. Bind durably records the exact generation/GUID/target/monitor-path-to-library-ID mapping before apply can persist that identity. Recovery reference provenance comes from Windows enumeration and verified mapping, is retained across rollover, and fails explicitly at its bounded capacity without dropping obligations. Exact-GUID absence can reuse that durable owned identity; absence alone cannot authorize arbitrary cached references.

Initially-headless state with valid physical obligations captured before preparation follows ordinary library restoration and checked exact-owned detachment. Only proven owned-only state uses the discard exception. QDC_ALL_PATHS source alternatives are normalized for one exact monitor/target/path/library identity; conflicting identities, targets, monitor paths, source adapters or active sources remain rejected. The capture selector comes from the verified active path after apply.

## Validation

| Check / environment | Candidate | Result | Evidence / limitation |
|---|---|---|---|
| Pinned source and interface inspection | P3b working source | Completed | Existing library apply/revert/persistence/cache and Windows interfaces inspected; no dependency edit. |
| Author diff/ownership and whitespace checks | P3b working source | Completed | Only the bounded source, tests and this work item changed. |
| Locked formatting and independent source review | Corrected seven-path candidate | Passed | No dependency change; strict recovery and ownership assertions retained. |
| Windows AMD64 native application/test build | P3a plus corrected P3b | Passed | Selected production/test objects and full build; Web production bundle also built. |
| Serialized journal envelope regressions | Same candidate | 2/2 passed | Reload/rollover and failed-write cache/disk preservation use assignment-style JSON value copies with the pinned library. |
| Combined display regressions | Same candidate | 110/110 passed | 38 legacy adapter, 29 ownership and 43 owned-display cases; no focused skips. Fake driver/Windows boundaries and the real pinned library manager. |
| Full native suite | Same candidate | 1,097 passed; 11 inherited skips; two baseline failures | 1,110 cases total. The command remains nonzero because both inherited httpbin download cases fail with CURL error 7. |
| Hosted platforms | Corrected candidate | Pending | Exact integrated-candidate validation remains required. |
| Actual driver, VM, GPU, streaming, HDR and human acceptance | P3b working source | Not run | No installed-driver or physical claim. |

## Delivery

Source review and local fixture/native validation are complete for this bounded infrastructure candidate. Final permission-writer ancestry, hosted validation and production integration remain pending. The next production slice must place this context under the existing source owner, enforce pin/lifetime ordering, observe heartbeat failures, bind probe/capture to its exact selector and implement startup recovery before enabling acquisition. Native driver cancellation may hang despite bounded attempts; hotplug/readback/removal are not globally atomic. If all original and recovered fallback displays disappear, admission fails with recovery retained rather than inventing a baseline or clearing obligations.
