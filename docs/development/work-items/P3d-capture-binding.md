# Work item: Bind capture and encoder resources to the owner-selected source

- Identifier and milestone: P3D, Windows display-source integration prerequisite.
- Requirements: DEC-02, DEC-05, DEC-16, DEC-17, DEC-22 and DEC-46; [planning decisions](../../planning/06-decisions-and-questions.md).
- Candidate/base: authored on local checkpoint `d4b328a2bc1437d14c59ad1725d084f922b873c3`, combining the P3B transaction and P3C owner prerequisites. No integration or publication is claimed.
- Scope: `video.{h,cpp}`, Windows capture initialization, a read-only Windows binding helper and focused regressions.
- Interfaces: the immutable `capture_source_t`, its separately preissued `capture_pin_t`, the existing platform display interface and pinned libdisplaydevice Windows API layer.
- Invariants: joining clients retain the current source; global adapter/output settings are never overwritten; missing or changed managed identity never falls back to another display; workers never acquire a new pin or synchronously wait for the RTSP owner.
- Exclusions: concrete native/virtual source backends, factory registration, startup/recovery, provisioning, driver installation and release qualification.

## Acceptance

Initial capture, encoder probing and reopen use the same exact selector and target identity. Windows verification must agree on target LUID/ID, monitor interface path, display-library ID and one available active source/selector. Inactive source alternatives do not select a capture adapter. A native clone target may share that same source adapter/ID and selector; the selected target must still exist with its exact path/library identity and be available. GDI-name collisions on different sources, conflicting target mappings and multiple different active sources fail closed. The actual source adapter selects DXGI independently of the target adapter, configured adapter description or virtual driver's requested render adapter.

Native mode changes remain observable on the same exact output. Owned virtual capture requires the fixed creation dimensions and rational refresh, and checks HDR only when display configuration explicitly requested it. Unknown HDR remains distinct from disabled. Native and legacy capture preserve the existing display-awake request; owned virtual capture does not add a physical-display wake request.

Both shared capture queues reject a different binding, including another generation or epoch, before a joining encoder can use their display. Managed switch-display requests are consumed without changing the source. Fresh generations/epochs cannot reuse capabilities from another source. Lifetime tickets must remain held until the actual display, image/encoder dependencies, detached encoder teardown and final shared worker context have finished.

## Implementation and review

`capture_binding::observe` reads `QueryType::All` through the pinned libdisplaydevice boundary. It normalizes inactive alternatives while requiring consistent target/path/library mappings, validates positive source mode data and returns the active source adapter/ID plus actual mode/HDR. `verify` adds owned-virtual constraints. Monitor interface paths use the same case-insensitive comparison as display ownership. Source selectors remain exact.

Windows capture checks the binding before adapter/output enumeration and again after complete DD/WGC initialization, before returning the display. The latter check includes the opened DXGI adapter LUID, attached output name and dimensions. These checks reject ordinary initialization races; they do not make arbitrary concurrent hotplug atomic. Managed reopen failure ends that capture attempt instead of retrying against another output.

`video::config_t` carries optional source metadata and the separate runtime ticket. Every encoder-probe configuration receives both. Encoder selection records the full descriptor, including generation/epoch, and bypasses the old cached-encoder shortcut when it differs. A fresh managed attempt clears the preceding selection, advertised capabilities and active codec modes before its display guard or invalid-entry check can fail. A same-source attempt and the existing unmanaged guard retain their previous semantics. The no-argument probing entry remains available for unmanaged startup/launch behavior.

The first accepted binding and ticket remain in each shared worker context until final destruction. Individual queue contexts and the fixed managed reopen configuration retain them too. Windows display objects retain the ticket in their actual control-block deleter, so internal `shared_from_this()` references cannot escape it. The deleter releases the ticket after resource destruction, even if stale weak observers remain. Detached asynchronous encoder destruction explicitly captures the preissued ticket. No ticket acquisition or owner wait is added to capture workers.

Independent review requested corrections for unavailable selected targets, legitimate native clones and stale advertisements on an early managed probe failure. Those three findings and the associated regression gaps are corrected in source; renewed review and execution remain pending. Narrow `SUNSHINE_TESTS` boundaries exercise the production guard, producer admission/cleanup, open/reset/switch decisions and teardown closures without native display or codec work. Synchronous join signaling is idempotent and may precede final resource destruction; tickets remain in the final worker context until it is destroyed. No independent acceptance is recorded here.

## Validation

| Check / environment | Candidate | Result | Evidence / limitation |
|---|---|---|---|
| Source inspection | Local P3D work on the checkpoint above | Performed by author | Read the actual capture paths and the pinned libdisplaydevice API/source. No runtime claim. |
| Focused regressions | 27 cases in `test_capture_binding.cpp` and `test_video_capture.cpp` | Authored, not executed | Target availability before/after open; native clones and selected-target disappearance/reuse; adapter/selector conflicts; mode/refresh/HDR constraints; actual managed guard and invalid-entry ordering; concurrent first producers, overflow and stopped cleanup in both queues; common production reset/switch decisions in both modes; owner-counted tickets through the actual detached encoder closure and detached synchronous worker after join signaling with blocked inert destructors; legacy behavior. |
| Source diff whitespace check | Local P3D work | Passed before handoff | Coordinator must repeat after formatting/integration. |
| Formatting and native build/tests | Pending integrated candidate | Not run by this author | Coordinator owns execution, dependency state and required review. |
| DD/WGC capture, GPU/HDR, physical streaming and human acceptance | Pending | Not run | Real owner accounting and production worker closures are exercised only with inert display/encoder boundaries; they cannot qualify hardware, drivers or client presentation. |

## Delivery

This is an inactive source prerequisite. The coordinator must integrate the explicit `requested_hdr` metadata field with P3C, construct native and owned-virtual descriptors from checked transaction/readback state, run formatting/build/regressions and obtain independent review. Production backend registration remains outside this work item. No merged revision or qualified release is claimed.
