# Decision log and open questions

Updated: 2026-10-23. This file is the authoritative scope record. Confirmed means directly stated or selected by the owner in this chat. Proposed means suggested by the analysis.

## Confirmed decisions

| ID | Decision | Consequence |
|---|---|---|
| DEC-01 | Use the newer Sunshine-based Miranda fork and reimplement selected Apollo functionality. | Behavioral adaptation on Miranda's base, rather than treating Apollo as the new base. |
| DEC-02 | Windows first; preserve other Sunshine platforms. | Windows additions stay isolated; no immediate cross-platform Apollo parity promise. |
| DEC-03 | Start with a small, reliable release containing essential features. | Full Apollo parity is not the first milestone. |
| DEC-04 | Artemis is used and must work. | Real Artemis interoperability is required. |
| DEC-05 | Automatic resolution/refresh rate, HDR and remembered settings per client are required. | Core virtual display work is in the first release. |
| DEC-06 | Family/friends will connect with restricted permissions. | Host-enforced per-device permissions are required. |
| DEC-07 | Intended uses include gaming, remote work, headless PC, TV, phone/tablet and secondary-device input. | Cover representative combinations in acceptance tests; do not infer simultaneous isolated desktops. |
| DEC-08 | Text clipboard in both directions with per-device permissions belongs in the first release. | Implement read/set controls and Windows text transport. |
| DEC-09 | No session automation initially. | Defer added connect/disconnect and pause/resume automation. Existing Sunshine application commands remain baseline behavior. |
| DEC-10 | Input Only mode belongs in the first release. | Test secondary input alongside an active stream and standalone operation. |
| DEC-11 | Apollo-style permissions are sufficient initially. | Per-game guest restrictions are outside the first release. |
| DEC-12 | Preserve applications/settings; re-pairing Artemis is acceptable. | Migration need not retain certificates/pairings. Exact supported settings still need a mapping. |
| DEC-13 | Retain only SudoMaker/SudoVDA and VirtualDrivers/Virtual-Display-Driver; implement SudoVDA first, then compare the two. | Supersedes the earlier comparison-before-selection order. SudoVDA is the initial integration; production support for VDD remains undecided. |
| DEC-14 | Write everything in structured development files. | Maintain this documentation set and update the decisions when the owner answers. |
| DEC-15 | Responses, documentation, notes and comments are in English. | Follow the supplied project instructions. |
| DEC-16 | Preserve Apollo-style display choices. Native capture keeps the captured physical display on; virtual capture lets the user choose physical-display behavior. | Preserve equivalent activation/primary/only-display controls using modern Sunshine display management. Test restoration and native-display power behavior. |
| DEC-17 | A second video client joins the current session. | Share the existing application and display; retain separate client permissions and stream lifetimes. No takeover or separate desktop. |
| DEC-18 | Initial host: Windows 11 25H2 and RTX 3080. Available GPU testing covers recent NVIDIA models only. | Record exact Windows/GPU-driver builds during tests. Preserve inherited support for other vendors, but do not claim hardware validation. |
| DEC-19 | Use the latest available Artemis and Moonlight clients, including Moonlight on Fedora and Windows. | Resolve and record exact client release/build versions per test campaign. The analyzed source snapshot is not a permanent client-version target. |
| DEC-20 | Support all resolutions and varied refresh rates, with tests including 60, 120 and 165 Hz. | No small mode whitelist. Accept arbitrary valid requests within driver/GPU/codec/client capabilities and report unsupported combinations explicitly. Test rates are examples, not limits. |
| DEC-21 | Newly paired family/friend devices get no access until permissions are explicitly granted. | Pairing alone grants no viewing, input, launch or clipboard access. The initial owner-device bootstrap remains a separate question. |
| DEC-22 | A second viewer must not change the active display mode. | Keep its resolution, refresh rate and HDR state while another client joins. Negotiate each stream within capabilities without reconfiguring the shared display. |
| DEC-23 | Include QR/one-time-code pairing in the first release. | Implement the Apollo-compatible Artemis pairing flow and its UI, retaining standard PIN pairing. A successfully paired guest still has no access until permissions are granted. |
| DEC-24 | Include Artemis application launch shortcuts in the first release. | Generate Artemis launch links and `.art` files using stable host/application identities; launches obey the paired client's permissions and current session policy. |
| DEC-25 | Defer manual server commands triggered from Artemis. | C03 is outside the first release, separately from already-deferred automatic session hooks. Do not advertise or execute that extension in v1. |
| DEC-26 | Include custom application ordering and launching applications from Miranda's Web UI. | L02 and L03 enter the first release. Persist order without changing app identity, and add authenticated Web UI launch with a bounded fallback when no client mode is supplied. |
| DEC-27 | Include per-application controller-type overrides in the first release. | L05 is selected despite the earlier recommendation to defer it. Adapt the override to modern Sunshine's available controller profiles/backends; preserve the global/Auto behavior when no override is set. |
| DEC-28 | Available physical controllers: Xbox One and Switch Pro, with Android, Windows and Fedora clients. | Use these as the initial controller test matrix; record exact models, firmware, USB/Bluetooth connection and client versions. DualSense hardware testing is not established. |
| DEC-29 | Every shipped Miranda feature must be available free of charge, without a paid license or subscription. Investigate free ways to provide newer emulated controller profiles. | Supersedes the proposed optional-paid-backend approach. A paid-only dependency cannot satisfy a selected feature. Reuse appropriately licensed code and qualify a free controller backend; the exact modern-profile matrix remains unverified. This does not select previously deferred Apollo features. |
| DEC-30 | Use Nonary/libvirtualgamepad, the controller driver identified by the owner through Vibepollo. | This is the selected Windows modern-controller backend. Integrate and qualify it; HIDMaestro remains a historical comparison, not an additional implementation target. Selection does not establish package or runtime compatibility. |
| DEC-31 | Include forcing a virtual display at all three levels: global, per application and per paired client. | D06 enters the first release, including host-side controls for Moonlight devices. Define and test policy precedence; joining an active session still preserves its display and mode. |
| DEC-32 | Include host-side resolution and refresh-rate overrides per device. | D09 enters the first release alongside automatic selection. Persist overrides, support valid fractional rates and validate requests; a joining viewer's override must not reconfigure the active session. |
| DEC-33 | Also offer persistent virtual-monitor identities per application and per client/application pair. | D07 enters the first release in addition to per-client identity. Keep identity selection explicit, stable across rename/reorder/restart and subordinate to the existing shared-session target when joining. |
| DEC-34 | Review Nonary/Vibepollo using the same feature-by-feature process for later releases, not the next release. | Maintain a separate inventory and question queue. At that point, additions awaited selection; DEC-38 subsequently selected full functional coverage. DEC-30's already-selected controller driver and other existing v1 decisions remain in scope. |
| DEC-35 | Prioritize capture quality, frame pacing and HDR in the later-release review. | Sets the discussion order. DEC-38 subsequently selects full functionality; milestone order and acceptance details remain to define. |
| DEC-36 | Allow Nonary's Moonlight fork as an optional additional client for compatible devices. | Client-specific capabilities may be considered later. Preserve Artemis and standard Moonlight for ordinary streaming. |
| DEC-37 | Include Remote Monitor as an explicit optional mode in a later release. | Stream extra screens in the same Windows desktop while the main session continues. Ordinary second viewers still join the existing stream; monitor count, layout and guest authority remain open. |
| DEC-38 | Miranda should ultimately provide all functionality from the reviewed Apollo, Vibepollo and Vibeshine projects. | Select the remaining real capabilities as long-term targets. Preserve the free-only constraint and explicit Miranda behaviors; paid prerequisites require free equivalents. Prototypes need qualification, and dependencies/defaults are not automatically adopted. This supersedes earlier undecided inclusion statuses for actual reference-project features, not evidence gaps or unrelated imagined capabilities. |
| DEC-39 | Keep the focused first release, then add all remaining features in later releases. | Preserve the established v1 feature list and explicit deferrals. Remaining work concerns implementation order, behavior, dependencies and acceptance, rather than whether to retain each reference feature. |
| DEC-40 | Produce public preview builds, followed by a stable release. | Prepare public packaging, documentation, dependency notices and qualified driver distribution. A preview must state its tested capabilities and limitations; stable release requires the completed acceptance scope. |
| DEC-41 | Periodically integrate selected Sunshine revisions after testing. | Pin the initial Miranda baseline, review upstream changes and qualify deliberate updates. Do not continuously track untested upstream master or restrict updates to stable tags only. |
| DEC-42 | The owner will prepare a clean Windows VM for implementation testing. | Begin with fresh-install validation; side-by-side Apollo installation is not a current test requirement. VM version, hypervisor, resources and exposed GPU remain test-environment metadata to collect. A planned VM is not an available or qualified environment yet. |
| DEC-43 | Use Apollo's handling of Sunshine as the reference for Miranda's eventual installation transition. The owner leans toward replacement rather than a separate test installation. | Adopt an Apollo-like service handover direction, while defining Miranda's own identity and explicit application/settings migration. This does not authorize deleting existing installations or data during analysis, nor establish a fully specified automatic cross-product uninstaller. |

## Resolved questions

| Question | Resolution |
|---|---|
| Q01: physical display behavior | DEC-16: native on; virtual user-selectable, with Apollo-style controls. |
| Q02: second video client | DEC-17 and DEC-22: join the current session and preserve its active display mode. |
| Q06: Moonlight support | DEC-19: required, including Fedora and Windows. |
| Q07: test platform and versions | DEC-18 through DEC-20 settle host/GPU and client-version policy; exact build numbers and device models are test-run metadata still to collect. |
| Q14: conflicting requests from a joining viewer | DEC-22: the active display mode wins. |
| Q04, pairing and shortcuts | DEC-23/DEC-24 include both in v1; other conveniences are later targets under DEC-38/DEC-39. |
| Q04, applications and controls | DEC-25 through DEC-27: manual server commands deferred; custom app order, Web UI launch and per-app controller overrides included. |
| Q15: controller hardware and paid-backend preference | DEC-28/DEC-29: Xbox One/Switch Pro tests on Android/Windows/Fedora; all shipped features free. Free-backend feasibility remains technical work, not an unanswered payment preference. |
| Controller backend | DEC-30 selects Nonary/libvirtualgamepad. Integration, package signing and profile qualification remain engineering work. |
| Q04, virtual-display forcing, modes and identities | DEC-31 through DEC-33 select all three forcing levels, per-device resolution/rate overrides and app/client-app identities for v1. Optional isolated placement is a later target under DEC-38/DEC-39. |
| Later-release review priority, additional client and Remote Monitor | DEC-35 through DEC-37: capture/pacing/HDR first; optional Nonary Moonlight client; explicit Remote Monitor mode selected for later. |
| Full feature coverage and release staging | DEC-38/DEC-39: all reference-project functionality is the long-term target; the focused v1 remains unchanged. |
| Q09: upstream maintenance | DEC-41: periodically integrate selected Sunshine revisions after testing. Exact update cadence follows development needs. |
| Q10: initial test installation and transition direction | DEC-42/DEC-43: clean Windows VM first; use Apollo's Sunshine handover as the later installer reference. Exact service/driver ownership and migration behavior remain engineering work. |
| Q11: release audience | DEC-40: public previews followed by a stable release. |

## Open product choices

| ID | Question | Current recommendation / dependency |
|---|---|---|
| Q03 | How is the owner's first device identified and granted permissions? | Guest defaults are settled by DEC-21. Proposed: explicit grant in the authenticated administration UI; Apollo's first-client-full rule is not selected. |
| Q04 | In which later milestones should the remaining display/rendering conveniences land? | DEC-38 selects full coverage and DEC-39 keeps v1 focused. Use the queue below to settle behavior and delivery order. |
| Q05 | Which Artemis Warp modes and fractional-rate cases should be certified first? | General high-rate support is confirmed, including 60/120/165 Hz test cases. Warp support is a long-term target; specific mode/hardware coverage needs a test plan. |
| Q08 | Which physical/virtual display arrangements and HDR modes are used today? | Establish real mode/topology cases and migration expectations. |
| Q12 | Should the previous Gevroska/Apollo changes be separately reviewed for regression cases or additional requirements? | Earlier work exists, but is not assumed to be additional feature scope or missing from Sunshine. |
| Q13 | Does preserving applications/settings include old Windows per-client monitor arrangements after re-pairing? | Fresh pairing can change display IDs. Current confirmed requirement guarantees remembered settings within Miranda, not automatic transfer of every Apollo display identity. |

## Remaining feature review queue

DEC-38 selects all reference-project functionality over time; DEC-39 preserves the focused first release. The queue now records delivery order and behavioral details for the remaining features. Do not ask again whether to include each one. Owner bootstrap, migration and release policy need concrete choices before their affected work is finalized; they do not block starting the source/build baseline. See [the readiness assessment](05-implementation-plan.md#readiness-assessment).

| Group | Remaining choices | Inventory references | Review status |
|---|---|---|---|
| Applications and controls | Manual commands from Artemis; custom app ordering; launch from Miranda's Web UI; controller type per application. | C03, L02, L03, L05 | Resolved by DEC-25 through DEC-30. The [controller review](08-free-controller-feasibility.md) tracks Nonary integration and hardware qualification. |
| Display preferences | Optional bottom-right virtual-display isolation. | D12 (isolation only) | Later target. Define placement/interaction behavior; existing v1 topology controls remain required. |
| Advanced rendering | Render at a different resolution from the stream; double display refresh rate; additional scaled modes/color-depth options; specific Artemis Warp modes. | D08, D10, D11, V02 | Later targets. Define priority and hardware/client coverage; general v1 mode/HDR interoperability remains required. |
| Pairing and administration | Owner-device permission bootstrap; switch to disable new pairings; dedicated/remembered Web UI login; individual tray, quit and discovery controls. | A04 (owner bootstrap only), A05, A07, V09 | Additional controls are later targets. Owner bootstrap remains a v1 behavioral decision; guest defaults and QR/OTP are settled. |
| Audio, feedback and migration compatibility | Additional audio recapture/default-output controls; host rumble forwarding switch; extra environment variables. | V07, L06, C07 | Later targets, with existing Sunshine behavior preserved. Required migration handling remains v1 work even when execution support is deferred. |
| Experimental/diagnostic options | Encoder-probe bypass and Apollo's GPU-priority experiment. | D16, V06 | Later targets subject to qualification. Keep experimental behavior explicitly optional; measure effects before exposing production defaults. |

Conditional engineering follow-up: D15 (fallback mode) is supporting work for Web UI launch. Previously deferred real features now belong to the long-term target while remaining outside v1. Existing equivalents should be preserved; placeholders do not establish a completed upstream capability. Q08/Q12/Q13 remain environment and migration questions; Q09-Q11 have a confirmed direction. DEC-29 settles the free-only requirement.

## Proposed technical choices, not yet approved

Vibepollo's later-release work is in [the feature inventory](09-vibepollo-feature-review.md) and [the roadmap/questions](10-vibepollo-roadmap-and-questions.md), supplemented by [Vibeshine differences](11-vibeshine-vibepollo-comparison.md). Open delivery/behavior questions do not expand v1 or override the decisions above.

- Keep a small display-backend boundary. SudoVDA-first implementation is confirmed; VDD comparison follows the working baseline and does not require two production backends in the first release.
- Preserve modern Sunshine Web UI/authentication, encoders, build/tests and reusable controller interfaces. Adapt the controller backend and inherited purchase/activation UI to the confirmed all-free product requirement.
- Add protocol compatibility at explicit boundaries instead of globally converting all FPS types or replacing dependency submodules.
- Report imported/transformed/deferred/unsupported settings during migration.
- Use feature-specific changes with acceptance evidence before adding optional conveniences.

## Change discipline

For each new answer, add or revise a dated decision, update the corresponding inventory disposition, and adjust the project brief/acceptance matrix where needed. Keep recommendations distinguishable from decisions. Do not record an unanswered multiple-choice default as the owner's answer.
