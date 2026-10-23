# Project brief

Updated: 2026-10-23.

## Objective

Build Miranda on recent Sunshine and ultimately provide all functionality from Apollo, Vibepollo and Vibeshine. Keep the newer Sunshine foundations and adapt overlapping implementations into one coherent product. Artemis and Moonlight are required clients; Nonary's Moonlight fork is accepted as an optional additional client for extensions.

The owner confirmed a small, reliable first release with the scope already defined, followed by the remaining features in later releases (DEC-38/DEC-39). The full-coverage goal does not make every feature a v1 requirement. Remaining collaboration concerns delivery order, behavior, dependency choices and testing.

## Confirmed requirements

| Requirement | Meaning for development |
|---|---|
| Windows first | Deliver and validate the new host features on Windows first. |
| All features free | Every shipped Miranda feature must work without a paid license or subscription. Paid-only dependencies do not satisfy the requirement. Preserve appropriately licensed reusable code. |
| Preserve other Sunshine platforms | Keep existing non-Windows functionality and build paths; Windows additions must not make those platforms depend on Windows components. |
| Client compatibility | Test the latest available Artemis and Moonlight releases. Artemis must support the selected Apollo extensions; Moonlight is required on platforms such as Fedora and Windows. Record exact versions at each test campaign. |
| QR/one-time-code pairing | Include Apollo-compatible Artemis QR and one-time-code pairing in the first release. Preserve standard PIN pairing for Artemis and Moonlight. Pairing does not grant guest permissions. |
| Artemis launch shortcuts | Include generation of application launch links and `.art` files in the first release. Use stable host/application identities and enforce the paired client's existing permissions. |
| Application management | Include custom ordering of apps shown in Artemis and launching apps directly from Miranda's authenticated Web UI. Preserve app identity through reordering and define a bounded display mode when no client supplies one. |
| Controller override per application | Include per-app emulated controller-type selection, with global/default inheritance and Auto available. Use profiles actually supported by the selected free backend. Test physical Xbox One and Switch Pro controllers on Android, Windows and Fedora clients; modern emulated-profile qualification remains technical work. |
| Controller driver | Use Nonary/libvirtualgamepad as the Windows modern-controller backend. Integration, packaging and supported profiles require qualification; HIDMaestro is not an implementation target. |
| Automatic virtual display | Match the originating client's supported resolution and refresh rate; support HDR and remember settings per client. Accept arbitrary valid modes within driver/GPU/codec/client limits, without a small hardcoded whitelist. Test multiple resolutions and 60, 120 and 165 Hz, among other supported rates. |
| Virtual-display controls | Include forcing a virtual display globally, per application and per paired device. Include per-device resolution/refresh-rate overrides alongside automatic mode selection. Neither policy may replace or reconfigure a session when another viewer joins. |
| Virtual-monitor identities | Offer persistent identity per client, per application and per client/application pair. Identity changes must not disrupt an active shared session; preserve identity across app renaming/reordering. |
| Physical display policy | Native-display streaming keeps the captured display active and on. Virtual-display streaming provides Apollo-style activation/primary/only-display options so the user chooses whether physical displays stay active. Restore the prior layout as appropriate after the session. |
| Multiple video clients | A second viewer joins the current application/display session. Keep the active display resolution, refresh rate and HDR state unchanged when a joining viewer requests different settings. Each client retains its own permissions. |
| Headless use | Support use without a physical display as part of the intended use cases. |
| Shared access | Family and friends may connect with restricted permissions. Every newly paired device, including the first owner device, has no access until an administrator explicitly grants rights in the authenticated Web UI (DEC-44). Apollo-style permissions are sufficient initially; per-game restrictions are outside the first release. |
| Text clipboard | Support PC-to-Artemis and Artemis-to-PC text transfer with separate per-device permissions. |
| Input Only mode | Allow a phone or other device to supply controls while another client receives the stream. Include this in the first release. |
| No new session automation initially | Defer Apollo connect/disconnect hooks, pause/resume commands, and automatic game suspension. Preserve Sunshine's existing application functionality. |
| No manual server commands initially | Defer Artemis-triggered host command buttons, separately from session automation. This does not remove existing Sunshine application launch/preparation functionality. |
| Migration | Preserve applications and supported settings from the existing Apollo setup. Re-pairing Artemis is acceptable. |
| Driver order | Implement SudoMaker/SudoVDA first, then compare it with VirtualDrivers/Virtual-Display-Driver. Retain only these two candidates. Supporting VDD as a production backend is not yet selected. |
| Initial hardware | Windows 11 25H2 and RTX 3080. Hardware validation is limited to recent NVIDIA GPUs; preserve inherited support for other GPU vendors without claiming it was tested. |
| Initial test environment | The owner will prepare a clean Windows VM. Test fresh installation there first; record the VM's actual Windows build and exposed graphics/encoding capabilities before interpreting results as RTX 3080 hardware coverage. |
| Release audience | Public preview builds followed by a stable release. Record package provenance, compatibility and limitations for testers. |
| Upstream maintenance | Periodically integrate selected Sunshine revisions after testing, starting from a pinned Miranda baseline. |
| Installation transition | Use Apollo's handling of Sunshine as the reference. The owner leans toward replacing the previous host; exact service handover, driver ownership and configuration migration remain to design. No initial side-by-side testing requirement in the clean VM. |
| Development documentation | Keep the analysis, decisions, requirements, and follow-up questions in structured English files. |

Intended uses include gaming, remote desktop/work, headless operation, TV streaming, phones/tablets, and one device acting as a controller for another. These are use cases, not promises of separate concurrent Windows desktops.

## Proposed first-release outcome

A user installs Miranda, pairs Artemis or Moonlight, and explicitly grants that device permissions in the authenticated Web UI before connecting from a phone, tablet, TV or desktop. Miranda captures the selected native or virtual display, streams with a supported mode, and restores/cleans up predictably. Native capture keeps its physical screen on; virtual capture follows the user's display policy. A second permitted video client joins the same session without changing the active display mode. An Input Only device can send controls without replacing either stream. The first owner device follows the same explicit-grant rule as every newly paired device. Artemis text clipboard access works in each permitted direction. Supported Apollo applications/settings can be imported with a clear record of unsupported fields.

Artemis users can pair through QR/one-time-code pairing, choose app order and open an application through a generated launch link or `.art` file. Administrators can launch apps through Miranda's Web UI. Apps can override the emulated controller type using the selected free backend. Those convenience paths preserve the appropriate authentication, permissions and shared-session rules.

The above is a proposed acceptance scenario assembled from confirmed choices. The initial driver, shared-viewing behavior, display policy, host/GPU family, QR/one-time-code pairing and launch shortcuts are settled. Exact client devices, installed build numbers, physical monitor arrangements and some optional features remain to be specified. Source display mode and each client's encoded stream parameters are separate: keeping the display stable does not promise every requested secondary stream format can be produced.

## Boundaries

- Full reference-project functionality is the long-term target. Existing v1 selections remain the next-release scope; other real capabilities are later targets.
- New Vibepollo/Vibeshine additions remain after v1. Capture quality, frame pacing and HDR lead the later review; Remote Monitor is an explicit future mode alongside ordinary shared viewers.
- Feature coverage does not require identical algorithms, duplicate interfaces or conflicting defaults. Experimental implementations need qualification; paid dependencies need free equivalents. The current two-display-driver shortlist remains in force while driver-bound features are assessed.
- The first release does not include new session automation, Artemis-triggered manual server commands or per-game guest allowlists.
- File transfer, separate concurrent Windows user sessions, and client microphone return are not established Apollo host features in this comparison.
- Native Linux/macOS virtual displays and clipboard implementations are not implied by preserving Sunshine platform support.
- Driver replacement, driver installation, deployment, publishing, and changes to an installed Apollo service are not part of this analysis.
- Standard Moonlight compatibility is required. Artemis-only features are not automatically available through Moonlight; test each client's supported controls and negotiate unsupported requests explicitly.
- A selected modern-controller feature cannot be declared delivered through a paid-only backend. Free-backend source availability, installation, runtime compatibility and client feedback are separate checks; the [feasibility review](08-free-controller-feasibility.md) records the unresolved work.

## Maintenance and release direction

The owner selected public previews followed by a stable release, and periodic integration of selected Sunshine revisions after testing (DEC-40/DEC-41). Start from the pinned Miranda baseline and update deliberately. The initial test environment will be a clean Windows VM (DEC-42). Apollo's Sunshine transition is the installer reference (DEC-43); see [the implementation plan](05-implementation-plan.md#installer-reference-and-test-environment) for source-confirmed behavior and remaining technical work. A small downstream patch set and isolated Windows display integration remain architectural recommendations.

## Migration implications

Application/settings migration and client identity migration are different tasks. Fresh Artemis pairing can assign new persistent client IDs, which can also produce new Windows display identities. Remembering settings after pairing is required; automatically preserving every old Apollo monitor arrangement is not yet promised.

Some Apollo settings depend on deferred features. The importer must classify those settings as imported, transformed, deferred, or unsupported; it must not silently imply full compatibility or execute application commands during import.
