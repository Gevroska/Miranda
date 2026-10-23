# Work item: caller-invoked SudoVDA adapter foundation

- Identifier and milestone: P2a, source foundation before P3 display integration.
- Requirements / feature IDs: D01-D07 and D09; DEC-13, DEC-16, DEC-17, DEC-20, DEC-22 and DEC-31 through DEC-33.
- Candidate/base revision: based on the reviewed P1a candidate `b75f26710901e2688eb169056d775ac7db65a66a`; integration remains pending.
- Scope and owned paths: `src/platform/windows/sudovda.{h,cpp}`, Windows fake-boundary regressions and narrow Windows source registration.
- Interfaces and invariants: exact protocol, full caller-supplied persistent identity, checked rational mode conversion, adapter-and-target enumeration, one owned display-source lease and a cancellable heartbeat.
- Explicit exclusions: no streaming/application/client call sites, topology/HDR/power changes, UI/settings, VDD abstraction, driver implementation or packages, installation, signing or hardware qualification.
- Dependencies / shared environment owner: native validation and any later driver/VM campaign belong to the coordinator. Tests use fake transport, enumeration and scheduling exclusively.

## Acceptance

Acquisition reports missing/open/incompatible/malformed driver responses without selecting another output. It sets the chosen render-adapter LUID before adding a monitor, preserves the full persistent GUID and bounded metadata, and resolves the created target using both adapter LUID and target ID plus the monitor-container identity. Failed bounded enumeration rolls back the attempted GUID. Cleanup stops and joins the heartbeat before removing the owned monitor and closing its handle; failed removal retains a recoverable lease and restarts heartbeat ownership.

The regressions cover exact byte counts, valid custom/portrait modes and varied rates, precision/range rejection, duplicate/unowned protection, multi-adapter selection, rollback, move/idempotent cleanup, asynchronous failure reporting and cleanup failure recovery. They neither open a production device nor wait for real time.

## Implementation and review

The wire declarations are independently expressed from SudoVDA's [pinned public header](https://github.com/SudoMaker/SudoVDA/blob/a4b09fa2aa731a964d0cb5d139cb1e6240e4da12/Common/Include/sudovda-ioctl.h). No driver/sample implementation, EDID data, host helper functions, binaries or installation scripts are imported. Existing upstream notices remain in place. This source change does not resolve the driver's inherited notices, source-to-binary identity or distribution qualification.

The adapter accepts protocol major 0 with minor at least 2 and exposes incremental/TestBuild fields. Refresh rates must be exactly representable as positive integer millihertz and at least 1 Hz: the pinned driver reinterprets wire values below 1000 as integer Hz. More precise rational requests receive a precision failure rather than silent rounding. Positive UINT-representable dimensions and rates are wire validation only; they do not prove GPU, driver, codec or client support. The pinned legacy non-HDR mode path rounds fractional rates; encoded-stream and observed refresh acceptance remain separate work.

Acquisition requires the caller to guarantee exclusive SudoVDA control. Present matching monitor-container GUIDs are rejected before render selection/add, and a process-local guard permits one outstanding lease. The protocol has no atomic newly-created result or per-handle monitor ownership. Another process can race preflight, and re-adding an existing GUID can return its old mode or fail at capacity. The API therefore does not claim atomic cross-process ownership, safe coexistence with independent driver controllers, mode updates, idempotent re-add or restart/crash recovery. Caller identity persistence and coordination must be designed in P3.

The watchdog is driver-global, defaults to three seconds and can disconnect all virtual outputs. Checked watchdog metadata determines the heartbeat cadence; zero timeout disables scheduling and enabled timeouts must be at least two seconds. A one-second timeout receives an explicit unsupported-watchdog failure before render selection, heartbeat or add: the pinned driver decrements its countdown on each one-second tick, so resetting the countdown to one cannot prevent the next tick from expiring it. A faster heartbeat does not fix that driver limitation. Asynchronous ping failures remain observable even after later recovery. Removing only the attempted GUID is the cleanup operation; watchdog expiry or driver reload is never requested as cleanup. A failed or timed-out add is also eligible for exact-GUID rollback under the required exclusive-use contract; an already-absent owned GUID completes cleanup.

Explicit cleanup exposes removal/close failures, retains resources for retries and reports acquisition and rollback failures separately. Destruction is best effort and logs unresolved failures. Native overlapped requests request cancellation after one second, then join cancellation before releasing their buffers. A driver that never completes cancellation can delay joining. None of this is driver-crash recovery or proof of clean shutdown on installed hardware.

The Windows output path and GDI name are returned as metadata, not treated as libdisplaydevice IDs or proof that an output is active and capture-ready. Enumeration includes possible inactive display paths. Modern Sunshine/libdisplaydevice retain topology preparation, mode application, HDR, capture mapping, wake/power and persisted restoration in P3. Joining viewers must reuse that source; they must not acquire this lease independently.

Independent review of the actual source diff accepted the corrected candidate. It found that the driver's one-second watchdog expires at every tick regardless of ping cadence; acquisition now rejects that setting before display mutation. The reviewed native files remained unchanged when the base advanced to P1a. The combined candidate builds natively and all 38 added fake-boundary cases pass.

## Validation

| Check / environment | Candidate | Result | Evidence / limitation |
|---|---|---|---|
| Author source and pinned-contract inspection | P2a working candidate | Completed | GUID, layouts, IOCTL semantics and watchdog/duplicate behavior inspected at SudoVDA `a4b09fa2`; Apollo reference `adc5c5a0` and current Sunshine display seams inspected. |
| Locked full formatter and outgoing-content checks | P2a working candidate | Passed | No unrelated native changes; dependency pins and licenses unchanged. |
| Windows UCRT64 native configuration/full build | P2a working candidate, then combined with P1a | Passed | GCC 16.2.0, CMake 4.4.4, static Qt 6.12.0; warnings as errors. Documentation/WiX packaging excluded as in P0. Executable dependency inspection found no missing DLLs. |
| Combined identity/driver focused regressions | P2a working candidate on P1a | Passed | 87 passed, zero skips/failures; includes all 38 fake-only `SudoVdaTest.*` cases and the P1a focused cases. |
| Full native suite | P2a working candidate on P1a | Failed with unchanged baseline cases | 926 total: 913 passed, 11 skipped, two inherited external httpbin download failures with CURL code 7. No assertions or skips relaxed. |
| Independent critical review | P2a working candidate | Completed | One-second watchdog defect corrected and re-reviewed; no outstanding actionable findings. Source review does not prove native driver behavior. |
| Installed driver, VM, GPU, streaming, HDR, human acceptance | P2a working candidate | Not run | No driver IOCTL, installation or hardware operation was executed. |

## Delivery

This reviewed and natively tested source foundation awaits integration. It does not activate a production display feature or qualify a driver distribution. P3 must integrate source/application/viewer ownership, persistent policy/identity selection, modern display mapping/restoration and headless/shared-viewer acceptance. Driver provenance, notices, package trust and clean-target qualification remain separate release gates.
