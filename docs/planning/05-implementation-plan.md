# Proposed implementation and validation plan

Updated: 2026-10-23. Initial SudoVDA implementation order and requirements are confirmed in the decision log. The detailed work breakdown remains a proposal. Implementation has not started in this workspace.

This plan covers the focused first release retained by DEC-39. DEC-38 establishes full Apollo/Vibepollo/Vibeshine functional coverage over later releases; see [the later roadmap](10-vibepollo-roadmap-and-questions.md). Additional source features do not enter this v1 plan automatically.

## Readiness assessment

Assessment on 2026-10-23: the scope is sufficiently defined to start foundational development. Another general feature-selection round is unnecessary. This is not a completed technical design, a qualified driver package or release readiness. The workspace currently contains planning documents only; the source checkout, reproducible baseline build and native validation remain to establish.

| Remaining item | When it matters | Recommended handling |
|---|---|---|
| Public release qualification (Q11 resolved by DEC-40) | Before distributing previews or a stable release | Public previews then stable are confirmed. Qualify package provenance, signing, notices and tester setup; distinguish automated checks, installed-package checks and physical streaming acceptance. |
| Installation transition (Q10 direction set by DEC-42/DEC-43) | Before installer/service identity and migration behavior are finalized | Clean Windows VM first. Use Apollo's Sunshine handover as reference; define Miranda's service identity, source-preserving migration and driver ownership explicitly. |
| Upstream update workflow (Q09 resolved by DEC-41) | During baseline and maintenance setup | Pin revisions, periodically integrate selected Sunshine updates and test before adoption. No baseline downgrade is implied. |
| Owner-device permission bootstrap (Q03, resolved) | Apply during pairing/permission implementation | DEC-44 confirms explicit rights granted through the authenticated administration UI for every paired device, including the first owner device. Pairing order grants no authority. |
| Shared-session Stop | Apply during P1/P3 authorization and cleanup | DEC-45 allows devices with launch rights to explicitly stop the application for everyone. Test allowed/denied global Stop separately from non-destructive join and ordinary disconnect. |
| Display policy precedence, shared-session lifetime and migration schema | During P1/P3 design, before implementing the affected state transitions | Record technical decisions and test cases; an active shared session always wins over a joining client's display request. Remaining implementation details do not require reopening feature inclusion. |
| SudoVDA and Nonary gamepad feasibility | Early technical qualification, before promising a distributable v1 | Verify the actual interfaces, available packages, signing, free use, compatibility and recovery. Source availability alone is insufficient. Report concrete gaps before making unsupported delivery claims. |
| Actual client devices, display layouts and old settings (Q08/Q13) | Before migration acceptance and hardware testing | Collect representative fixtures and exact device/build information during the relevant work packages. Record whether old monitor identities/layouts can be preserved. |
| Earlier Gevroska/Apollo changes (Q12) | Before declaring migration/regression coverage complete | Compare relevant prior changes as evidence; do not assume all are absent upstream or automatically transplant them. |
| Later driver-bound, paid-dependent or experimental features | Before scheduling the corresponding later release | Assess free equivalents and adaptation to the existing display shortlist. Full coverage is the goal; feasibility and physical qualification are still engineering work. |

The first development milestone should establish the source/build baseline, map the selected driver interfaces and test constraints, then define the identity/permission/session model needed by P1/P3/P4. Public distribution and installation choices do not prevent read-only source analysis or a local baseline build. No new implementation, driver installation or change to the existing Apollo installation was performed for this readiness assessment.

## Installer reference and test environment

The owner will prepare a clean Windows VM. Fresh installation is the initial test path; a side-by-side Apollo installation is not required to start. Later migration/upgrade tests still need controlled fixtures representing an existing installation.

At examined Apollo commit `adc5c5a0bd80831ce495434bb16aee2cd4175fb8`:

- Packaging uses Apollo's project/package identity and an installation directory named for that package. It does not simply rename the installed Sunshine directory. [A-PACKAGE-ROOT], [A-PACKAGE-COMMON], [A-PACKAGE-WINDOWS]
- The service installer stops and deletes the legacy `SunshineSvc` service, creates or reconfigures `ApolloService`, sets its executable path and starts it. This is a service handover; it is not evidence that all Sunshine files/configuration are removed or imported. [A-SERVICE-INSTALL]
- The NSIS configuration enables uninstall-before-install for previous installations detected by its generated installer. That setting alone does not establish complete cross-product Sunshine removal. [A-PACKAGING]
- `migrate-config.bat` moves legacy files from the current install root into its own `config` subdirectory when destinations are absent. It handles application/config/state files, credentials and covers. It does not search a separate Sunshine installation folder and import its configuration. [A-CONFIG-MIGRATE]

For Miranda, use the service handover pattern as the reference and explicitly design its own package/service identity and supported predecessor detection. Preserve the required apps/settings migration and source backups. Do not copy broad removal of foreign services, shared drivers or configuration directories without ownership and recovery handling. The exact replacement workflow remains an implementation design, not an instruction to alter the owner's current machine.

VM qualification must record Windows build, hypervisor, resources, exposed GPU/encoder and driver packages. Fresh install, administration, pairing, service lifecycle and configuration tests can begin there. Count NVIDIA encoding, HDR, high-rate capture and frame-pacing acceptance only when the tested environment actually exposes the required hardware path and capable client/display; otherwise retain those as outstanding physical acceptance. Creating the VM or configuring GPU access has not been performed in this analysis.

Public preview qualification should verify the tested source revision, produced installer contents, installed service command/path, driver artifacts, checksums and published prerelease identity independently. Preserve the existing separation between build/test success, package behavior and real streaming evidence. Publishing remains a later delivery action.

## Integration principles

1. Keep the selected modern Sunshine baseline and add independently reviewable feature changes.
2. Treat Apollo's behavior and Artemis's client contract as references. Avoid carrying older dependency revisions, entire source files, or historical workarounds into Miranda without a present need.
3. Add UI controls to current Sunshine's Vue single-page application. Apollo's older HTML pages are not drop-in replacements.
4. Keep Windows driver/clipboard implementation behind platform boundaries. Preserve existing Linux/macOS/FreeBSD paths as applicable to the upstream build matrix.
5. Separate persistent client identity, permissions, app identity, display ownership, and individual stream connections. These lifetimes overlap but are not identical.
6. Use a versioned configuration migration with a readable mapping report. Preserve input files and make unsupported/deferred settings visible.
7. Every shipped feature must work without a paid license/subscription. Reuse appropriately licensed components, integrate selected Nonary/libvirtualgamepad and audit inherited purchase/activation paths. Preserve platform support while adapting dependencies.

## Proposed work packages

| Package | Scope | Dependencies | Completion evidence |
|---|---|---|---|
| P0: baseline and build | Pin the initial source, reproduce a clean build, record inherited failures and exact test-host versions; establish the tested-update workflow. | Current Miranda source; DEC-41; available build environment | Pinned revision and reproducible build results, with clean-VM results distinguished from Windows 11 25H2/RTX 3080 physical qualification. |
| P1: client/app identity and permissions | Extend paired-client records, preserve Sunshine enable/disable semantics, add permission enforcement and UI, introduce stable app UUIDs where needed. Every new device, including the first owner device, has no access until explicitly granted rights in the authenticated Web UI. | P0; DEC-44 bootstrap policy | Positive/negative permission tests for Artemis and Moonlight, zero-access default, explicit first-owner grant, active revocation, persistent identity tests. |
| P2: SudoVDA prerequisites | Pin SudoVDA artifacts/API, establish reproducible package acquisition/build, verify required files and installation behavior. | P0; available test host | Complete identified package, API mapping, availability/diagnostic checks and recoverable setup. VDD comparison is not a prerequisite. |
| P3: SudoVDA integration | Implement lifecycle, headless probing, client identity/modes/HDR, global/per-app/per-client forcing, per-device mode overrides, Apollo-style topology choices, native-screen-on behavior, cleanup and reconnect. Preserve the active display mode when another viewer joins. | P2; P1 for persistent paired identities and access checks | Hardware evidence for headless startup, varied modes/rates, HDR, policy precedence, persistence, shared viewing and failure recovery. |
| P4: client protocol adaptation | Artemis capability fields, numeric/UUID app launch, rate conversion, selected extensions; retain Moonlight core compatibility and shared-session behavior. | P1; P3 for display-backed tests | Protocol fixtures plus latest Artemis and Fedora/Windows Moonlight pairing/launch/resume and mixed-client joins. |
| P4a: QR/one-time-code pairing | Add compatible pairing-material generation, QR/code UI, expiration and single-use handling; retain standard PIN pairing and zero-access guest defaults. | P1, P4; current administration UI/authentication | Actual Artemis QR/code pairing succeeds; expired/invalid/reused codes fail; pairing grants no unintended permissions. |
| P4b: Artemis launch shortcuts | Generate application launch links and `.art` exports in the current Web UI, using stable host/application identities and existing launch/resume authorization. | P1, P4; stable host/app IDs | Links and files open the intended app in Artemis; identity survives rename/reorder/restart; denied access and shared-session behavior remain correct. |
| P4c: application management | Persistent custom app ordering and authenticated Web UI launch. Preserve app IDs and supply a bounded display-mode fallback without a client request. | P1, P4; current UI/authentication and session model | Order survives restart; Artemis receives ordering metadata; launch targets remain stable; Web UI launch works and does not silently replace an active shared session. |
| P5: text clipboard | Windows Unicode clipboard, authenticated HTTPS endpoints, separate read/set permissions. | P1, P4 | Actual text transfer in both directions and denied operations verified. |
| P6: Input Only | Preserve main stream/app lifetime; add control-only sessions and cleanup. | P1, P4; session ownership design | Main stream plus secondary input device; standalone mode; disconnect/reconnect and no stuck inputs. |
| P6a: per-application controllers | Inherit, disabled, Auto and supported profile overrides through free controller interfaces. Restore policy at the app boundary and on failures; joining clients do not replace it. | P0, P1; P10 for selected backend/profile qualification | Available Xbox One/Switch Pro hardware passes the client matrix; overrides, mixed controllers, feedback, Input Only and failure cleanup work without paid activation. |
| P7: migration and packaging | Map apps/settings, fresh pairing, Miranda identity and Apollo-like service handover, driver packaging and license notices. Prepare public preview packages and audit inherited paid dependencies/UI. | Chosen schema/backend; DEC-40/DEC-42/DEC-43 | Fresh-VM install, migration fixtures, import report, source preservation and upgrade/uninstall/rollback evidence; all shipped features usable free. |
| P8: release qualification | Complete regression and hardware matrix, fix regressions, verify produced artifacts. | P3-P7, including P4a-P4c and P6a; P10 qualification for shipped controller profiles | Automated results, package checks and human hardware acceptance recorded separately. |
| P9: VDD comparison | Compare a pinned VirtualDrivers/Virtual-Display-Driver build against the working SudoVDA baseline using the same scenarios; implement only the adapter needed to establish equivalent behavior. | Working P3 baseline; recorded SudoVDA results | Evidence-backed comparison and a later decision on VDD production support. Timing relative to the first release remains open. |
| P10: Nonary controller integration and qualification | Implement the selected Nonary/libvirtualgamepad adapter using reusable MIT components where appropriate; map required profiles, retain necessary free compatibility paths and qualify package provenance/signing/lifecycle. See [assessment](08-free-controller-feasibility.md). | P0; available physical controllers and clients | Working integration, qualified profile matrix, distributable artifacts and explicit unsupported capabilities. HIDMaestro is not an implementation target; paid-only delivery is excluded. |

SudoVDA is the first driver implemented. P2 can be researched independently of P1, and clipboard development does not inherently require the virtual display backend. P9 follows a working SudoVDA baseline and does not block beginning that integration. The sequence is about dependencies, not a request for parallel agents or separate chats.

P10 can progress independently of SudoVDA integration. Nonary/libvirtualgamepad and per-application overrides are selected; the modern emulated profile set remains subject to qualification. An unresolved result requires an explicit scope discussion rather than an unannounced feature removal, backend substitution or paid fallback.

## Proposed display backend boundary

The host needs operations to report availability, ensure the chosen render adapter, create/update a monitor with a persistent identity and mode, obtain the capture identifier, release an owned monitor, and recover abandoned monitors after failure. Return structured failures instead of silently falling back to an unrelated physical display.

Keep this small. Do not design a general multi-driver framework before a single driver passes the required scenarios. Monitor ownership and cleanup must accommodate multiple video viewers and secondary Input Only connections. Joining viewers use the existing source display without applying their saved mode over it; ending one connection does not end the shared display session. Maintain separate concepts for requested source mode, actual source mode and each stream's negotiated format.

## Main integration risks

| Risk | Why it matters | Development response |
|---|---|---|
| Different refresh-rate units | Artemis/Apollo thousandths and newer Sunshine fractional handling can produce incorrect display rates, encoding timing or bandwidth. | Explicit boundary conversion and malformed/range tests; compare measured rates. |
| Display lifetime vs. connection lifetime | Disconnecting one viewer or Input Only device must not tear down another's active app/display. | Explicit ownership/state model and multi-client lifecycle tests. |
| Recent Sunshine identity and security changes | Whole-file replacement can revert exact-certificate matching, enable/disable behavior, parsing safeguards or CSRF handling. | Extend current code and preserve existing negative tests. This is an engineering concern, not a completed security audit. |
| Global controller/profile state | Old Apollo per-app changes may conflict with Sunshine's newer controller backends and concurrent inputs. | Use current backend interfaces and explicit restoration. |
| Free modern controller packaging | A permissive source license and an advertised profile do not establish a production-ready signed package or working client/game feedback. | Pin and qualify the chosen free backend and each exposed profile; record source, package and runtime evidence separately. |
| Headless encoder capability detection | Capabilities may be unavailable before a display exists. | Temporary display/probe flow with bounded cleanup; no misleading capability advertisement. |
| Driver package incompleteness | Apollo's checkout contains INF/catalog/certificate/scripts but not all binaries required by its installer. | Reproducible acquisition/build and exact package verification. |
| Physical display disruption | Driver topology changes and upstream display restoration can compete. | Keep native capture's screen on, apply user-selected virtual-display topology, and restore only when appropriate for remaining viewers. Verify recovery after failures. |
| Configuration drift | Apollo app/client schemas differ from current Sunshine; some requested settings relate to deferred features. | Versioned import mapping and explicit unsupported/deferred results. |
| First-client privilege assumptions | Apollo's default first paired device gets full authority. | DEC-44: every newly paired device starts with no access and requires an explicit administrator grant; pairing order grants no authority. |
| Driver update/trust behavior | Current source, latest stable artifacts and installed signer state can differ. | Pin and verify artifacts; test upgrade/rollback separately from source compilation. |

## Minimum first-release acceptance scenarios

| ID | Scenario | Required observation |
|---|---|---|
| T01 | Existing Sunshine functions | Selected baseline tests and ordinary streaming continue to pass. Non-Windows additions do not break their builds. |
| T02 | Artemis and Moonlight pairing | Latest available Artemis plus Fedora/Windows Moonlight pass standard pairing/reconnect and reject unauthorized access. Record exact releases/builds per run. |
| T03 | Virtual display modes | Several resolutions/aspect ratios, including a custom valid mode, work at supported rates including 60, 120 and 165 Hz. Check actual display and stream rates separately. No small hardcoded mode whitelist; reject unsupported combinations explicitly. |
| T04 | Headless boot | Start with no active physical display, connect, stream, disconnect and reconnect. |
| T05 | Persistent client settings | A and B retain separate settings across their own sessions/reconnects and Miranda restart. Joining an already active session does not overwrite its mode or either client's saved preferences. |
| T06 | HDR and SDR | Supported HDR request works; SDR works; unsupported HDR is handled explicitly. Color correctness requires a real display check. |
| T07 | Device permission defaults and restrictions | Every newly paired device, including the first owner device, has no access until explicitly granted rights in the authenticated Web UI. Denied viewing, app listing/launch, inputs and clipboard directions stay denied in Artemis and Moonlight; allowed actions work. |
| T08 | Revocation | Changing or revoking permission during a session takes effect without leaving stuck inputs or stale authorization. |
| T09 | Clipboard | Unicode/plain text transfers both ways; a denied operation does not expose or alter the host clipboard. |
| T10 | Input Only with main stream | Phone/controller input works while a TV or another client receives the stream; no video/audio stream is sent to the Input Only device. |
| T11 | Input Only lifecycle | Standalone mode, reconnect, loss of one client and termination all clean up correctly without ending unrelated sessions. |
| T12 | Recovery | Failed app launch, driver unavailability, network drop and process restart leave a recoverable system and clear diagnostics. |
| T13 | Migration | Representative apps/settings import with a mapping report; originals are retained; commands are not executed by import; fresh pairing works. |
| T14 | Packaging | Clean-VM installation includes the chosen driver assets; service/firewall identities and the installed executable command/path are correct. Separately test predecessor handover, source-preserving migration and upgrade/uninstall/rollback. Verify produced artifacts/checksums and public preview identity independently of CI success. |
| T15 | Frame pacing | Compare sustained native and virtual capture with matching supported client/codec/rate conditions, including 60 Hz and high rates, plus single-viewer versus shared-viewer runs. Correlate repeatable stutter with capture/encoder reinitialization and display events; do not assume a network cause or that old timing workarounds are needed. |
| T16 | Physical-display policy | Native capture keeps the physical screen on. Virtual capture honors keep-active, primary and only-display choices. Verify topology restoration after the appropriate session boundary and after recovery; one departing viewer does not disrupt others. |
| T17 | Shared video session | Artemis/Artemis, Moonlight/Moonlight and mixed viewers join the same app/display. Conflicting resolution/rate/HDR requests leave its mode unchanged. Test both join orders, originating-viewer departure, reconnect, permissions and coexistence with Input Only. Unsupported secondary stream formats return explicit results. |
| T18 | QR/one-time-code pairing | Current Artemis pairs with valid generated QR/code material for the intended host. Expired/invalid codes fail and consumed codes cannot create another pairing. The new guest starts with no access. Standard PIN pairing still works for Artemis and Moonlight. |
| T19 | Artemis launch shortcuts | Generated launch links and `.art` files open the correct host/app. Rename/reorder/restart preserve target identity. Removed/unknown targets fail clearly; unpaired/denied clients cannot bypass authorization. Joining through a shortcut preserves the current app/display session and mode. |
| T20 | Custom application ordering | Order survives restart and reaches Artemis through compatible metadata. Reordering does not change numeric/UUID launch identity or shortcuts. Moonlight app targets remain correct regardless of client-side sorting. |
| T21 | Web UI app launch | Authenticated host administration can launch an app without a video client. Missing client display mode uses a bounded fallback; denied requests fail. Existing shared sessions are not silently replaced. |
| T22 | Per-application controller profiles | Inherit, disabled, Auto and each shipped profile work through the free backend. Test Xbox One/Switch Pro inputs, feedback where available, mixed controllers, viewers and Input Only. Failed launch and app termination restore the expected policy; joining/disconnecting clients leave no stuck input or unwanted profile change. |
| T23 | All features free | A clean install can use every shipped feature without payment, subscription or paid activation. Verify dependency/artifact licenses and notices, package lifecycle and absence of paid-only options. Record unavailable advanced controller/hardware coverage honestly. |
| T24 | Virtual-display forcing and per-device modes | Global, app and paired-device forcing follow a documented precedence policy, including Moonlight clients. Resolution/rate overrides persist, support valid fractional rates, reject invalid modes and can be cleared for automatic selection. A joining client's policy/override leaves the existing session and mode unchanged. |
| T25 | Persistent monitor identity choices | Per-client, per-app and per-client/app identities are deterministic and survive rename, reordering and restart. Separate intended identities retain separate Windows settings; joining viewers use the existing shared target without creating or switching monitors. |

Confirmed baseline: Windows 11 25H2, RTX 3080, latest available Artemis and Moonlight, with Moonlight on Fedora/Windows and varied resolutions at 60/120/165 Hz among other supported rates. Hardware validation is limited to available recent NVIDIA GPUs. Preserve other inherited encoder backends without claiming measured compatibility.

Initial implementation tests will use the owner's planned clean Windows VM. Its actual OS and graphics capabilities must be recorded; do not infer RTX 3080/NVENC availability inside the guest from the host hardware declaration. The VM does not automatically satisfy every hardware acceptance case above.

Available physical controllers: Xbox One and Switch Pro, for Android, Windows and Fedora clients. Record actual USB/Bluetooth paths, firmware and exposed capabilities per run. A selected emulated profile is separate from the physical controller; no physical DualSense acceptance is currently possible with the declared hardware.

At each campaign, record exact OS build, GPU driver, client release/channel, client device/OS, monitor/EDID, driver artifact/hash, requested and actual source mode/HDR, negotiated stream format and observed results. Exact phone/tablet/TV models and physical-monitor arrangements remain to collect. Set measurable duration/latency/frame-drop thresholds after capturing a baseline; no arbitrary performance target has been approved.

## Repository conventions for implementation

Miranda's pinned upstream `AGENTS.md` specifies MSYS2 UCRT64 Windows builds, `cmake-build-` build directories, `test_sunshine`, common test headers, regression tests for changed source behavior, Doxygen documentation, and formatting checks. Respect those instructions when implementation begins. The owner additionally requires English responses, documents, notes and comments. No builds/tests were run merely to validate these planning documents.

## Maintenance policy and remaining release details

The [Vibepollo roadmap](10-vibepollo-roadmap-and-questions.md) concerns later releases only. It is not an extension of this first-release work list. P1/P3 include DEC-33's selected monitor-identity choices as part of persistent display identity handling.

- Periodic integration of selected tested Sunshine revisions is confirmed; choose update timing based on changes and validation capacity.
- Exact phone/tablet/TV device models and the handling of unavailable client-side extension controls; Artemis and Moonlight support are confirmed.
- Clean-VM testing and an Apollo-like installation transition are confirmed directions; define precise migration, predecessor detection and driver ownership during packaging work.
- Public previews followed by stable are confirmed; qualify distributable driver artifacts, document preview limitations and define stable acceptance evidence.
- Whether prior Gevroska/Apollo hardening should be compared as another source of regression cases. Its changes are not automatically missing from current Sunshine.

[A-PACKAGE-ROOT]: https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/CMakeLists.txt
[A-PACKAGE-COMMON]: https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/cmake/packaging/common.cmake
[A-PACKAGE-WINDOWS]: https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/cmake/packaging/windows.cmake
[A-SERVICE-INSTALL]: https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src_assets/windows/misc/service/install-service.bat
[A-CONFIG-MIGRATE]: https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src_assets/windows/misc/migration/migrate-config.bat
[A-PACKAGING]: https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/cmake/packaging/windows_nsis.cmake
