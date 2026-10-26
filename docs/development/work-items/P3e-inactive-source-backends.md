# Work item: inactive concrete Windows source backends

- Identifier and milestone: P3E1, source prerequisite for Windows display integration.
- Requirements: DEC-02, DEC-05, DEC-16, DEC-17, DEC-22, DEC-31 through DEC-33 and DEC-46.
- Base: local checkpoint `0f8dcaf06151ec28c256226232ab5e9167c96603`, containing accepted B/C/D source prerequisites; no publication or release is implied.
- Observable outcome: an inert factory prepares and manages one exact Native or SudoVDA Virtual source through the existing owner, display and capture interfaces, with copied cross-kind recovery admission.
- Scope: Windows backend adapter, narrow ordinary DD progress facade, frozen native configuration and read-only availability preflight, owner admission/idle recovery/generation seams, Windows build registration and focused regressions.
- Interfaces and decision: [ADR-005](../decisions/005-inactive-windows-source-backends.md).
- Exclusions: production factory registration, main/startup/shutdown wiring, production provisioning verifier or receipt creation, driver installation, hardware campaigns, dependencies, publishing and release qualification.

## Implementation

Native wraps the ordinary DD_DATA scheduler and persistence. Its copied state is generation fenced and status never enters the scheduler. The full ordinary parser action and optional resolution/rate/HDR fields survive source resolution. Configuration errors remain tolerant; an explicit disabled action schedules ordinary revert. Exact capture metadata comes from a capturable selector, Windows target/path/library ID and D observation, with actual dimensions/rational refresh and optional observed HDR. Native wake remains on the operation lane and actual capture keeps D's existing native display-awake behavior. Logical retirement follows successful delayed revert scheduling while physical recovery remains separately pending. Completion only checks recovery; it never resets persistence.

Virtual uses the existing B transaction and SudoVDA lease, then D verification. Independent authority injection defaults absent and refuses Virtual before B intent or driver access. The included interface has no production implementation and supplies no configuration switch that confers authority. Clean pre-intent denials retire as NoEffects. Entered or uncertain durable intent stays owned; actual adapter-issued receipts are required after acquisition. Acquisition, cleanup and liveness errors remain separate. Heartbeat remains owned through restoration, failure and retained application lifetime. Reconnect revalidates exact identity with a new epoch; probing passes the owner's existing pin into the managed D entry point.

If B can have created durable intent but returns no transaction object, the adapter conservatively blocks cleanup and retains its logical generation. It cannot reconstruct that episode through an unreviewed production startup path in E1. E2 recovered-episode adoption remains necessary for this failure boundary. A new rollover intent followed by B manager reconstruction failure likewise cannot be presented as a successful recovery.

Copied admission rejects an opposite source kind before a new owner record, generation increment or process claim. Same-kind Virtual recovery uses B rollover with the actual next GUID. A separate idle operation continues retired recovery and completes only after persistence is empty, retaining its generation and fencing admissions during completion. A quiescent generation-seeding seam is available for E2 recovery integration. No production caller installs the factory.

Independent source review requested corrections for slow progress starving ready/failure publication and public retry failing to restart stopped retired restoration. The owner now consumes completed observations before another refresh and starts its next progress interval at completion. Native and B copied status distinguish stopped failed restoration from scheduled unavailable-device work. A generation-fenced public retry is retained across in-flight observation, then restarts the existing failed restoration on the serialized operation lane. It preserves the manager, settings, journal and issued receipt; it does not repeat display removal or rotate the episode. B's restoration algorithm is unchanged apart from publishing its existing stop decisions. Renewed review must include those B status changes.

## Acceptance and evidence

All cases below are authored source regressions and remain unexecuted in this worker candidate.

| Area | Source cases | Remaining evidence |
|---|---|---|
| Inert construction and authority | Factory/status/admission invoke no native/driver callback; absent authority has clean NoEffects retirement and Native remains available | Coordinator formatting/build/execution and independent review |
| Native | Exact actual binding without UUID; optional disabled fields; tolerated configuration failure; disabled-action revert; delayed logical retirement with pending recovery; exact retained epoch/selector; read-only query error versus checked empty baseline | Scheduler regression execution and physical display-awake/topology behavior |
| Virtual | Exact B/D binding; retained epoch; separate explicit/observed HDR; armed failed add and heartbeat; unarmed receipt; sticky liveness plus separate cleanup failure; actual next-GUID rollover | Component execution, independent receipt/recovery review and later real driver qualification |
| Owner | First Native/force-Virtual join remains exact; both opposite-kind recovery directions reject before generation mutation; idle completion does not repeat removal; recovered generation seed is quiescent; inherited pin-drain and authority regressions retained | Exact integrated owner suite and source review |
| Slow progress and retry corrections | Three barrier/virtual-clock owner cases cover slow ready, terminal failure and cancellation without changing admission deadlines; two actual concrete-backend/owner cases use the pinned settings manager and inert Windows/driver/codec boundaries to verify repaired fault plus public retry, retained settings until real restoration, both cross-kind admissions and one Virtual removal | All five new cases are authored and unexecuted; renewed review and exact native execution remain required |
| HTTPS and policy | Full parser snapshot and copied availability result; query runs on the existing off-owner refresh lane; fresh failure is unavailable, joins preserve their request; managed fixture installs and clears its fake availability callback while retaining the coordinator's corrected cleanup/diagnostics | Route execution; the separate intermittent first permitted /applist stall remains unresolved |

Only read-only source/diff inspection was performed by the author. No formatter, compiler, native test, VM, driver, capture, encoder, streaming, commit or publication operation was run. Source inspection does not establish runtime qualification.

## Delivery

Coordinator integration, formatting/native checks and independent review are next. E2 must implement real protected provisioning, recovered-episode adoption, startup/headless ordering and shutdown drain before considering production factory registration. This work does not change `STATUS.md` delivery claims or the product decision log.
