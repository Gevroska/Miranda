# Free controller support and code reuse

Updated: 2026-10-23. Nonary/libvirtualgamepad is selected for implementation (DEC-30). This remains a source and licensing assessment; no controller driver has been built, installed or tested for Miranda.

## Confirmed requirements

- Every shipped Miranda feature must be available without a paid license or subscription (DEC-29). This constraint also applies to inherited dependencies; it does not bring previously deferred Apollo features into scope.
- Per-application controller overrides belong in the first release (DEC-27). Preserve the global setting, including Auto, and allow an application to inherit it or select another supported profile.
- Available physical controllers are Xbox One and Switch Pro, with Android, Windows and Fedora clients (DEC-28). USB/Bluetooth arrangements and exact client builds remain to record. Physical DualSense testing is not currently available.
- Use Nonary/libvirtualgamepad as the Windows modern-controller backend (DEC-30). Qualify its integration and distributable package. HIDMaestro is retained below only as comparison evidence, not a second implementation target.

A physical controller, the information transported by Artemis/Moonlight, and the controller emulated on the Windows host are separate parts of the path. Using a physical Xbox One does not require the host to emulate Xbox One specifically. Conversely, selecting a DualSense profile cannot supply sensors or feedback absent from the physical device or client transport. Qualification must cover the whole path.

## Existing behavior to preserve and adapt

Sunshine's global Auto setting chooses an emulated controller using client-reported information and configured heuristics. Apollo adds an application override with inherit, disabled, Auto, Xbox 360 and DS4 choices; its process code saves and restores global input configuration around the application lifetime. Miranda must adapt this behavior to its newer controller interfaces and shared-session model. A joining viewer or Input Only device must not replace the running application's profile policy. [S-WININPUT], [A-APPS], [A-PROCESS]

## Which LizardByte code can be reused?

The actual libvirtualhid dependency pinned by the examined Miranda baseline is `dfff3ea8c34022c915799740806c17c0ed5c2492`. Its license map separates the reusable library from restricted driver components. [G-LVH-MAP]

| Component | Published terms | Consequence for Miranda |
|---|---|---|
| Shared library, public headers and other files covered by MIT | MIT permits modification and redistribution while retaining the required copyright and license notices. | Reuse these components where technically useful. Check individual file exceptions. |
| Windows driver, broker, specified shared entitlement/evaluation files and generated driver packages | LB-SAL limits modifications to upstream contributions and does not grant general redistribution rights, including for free distributions. | A freely redistributed derivative requires separate permission or different applicable licensing. A public source repository alone is insufficient. |

Sources: [G-LVH-MAP], [G-LVH-MIT], [G-LVH-SAL]. This records the published permission boundary, not a legal enforceability opinion. No freely licensed historical version of the modern LizardByte driver has been established by this review.

The proposed technical route is to retain useful MIT interfaces and implement an adapter to an independently licensed driver. This is an integration proposal, not evidence of binary compatibility. Avoid copying restricted driver code or treating removal of payment checks as a free backend implementation.

## Selected backend and prior comparison

These are controller drivers. The separate display-driver shortlist remains SudoVDA and VirtualDrivers/Virtual-Display-Driver.

| Candidate | Evidence | Gaps before a Miranda release |
|---|---|---|
| Existing ViGEmBus path | The examined Sunshine Windows input code retains the Xbox 360/DS4 fallback. [S-WININPUT] | Useful compatibility baseline; it does not establish modern emulated profiles. Verify package provenance, redistribution terms and installation on the target Windows build. |
| **Nonary/libvirtualgamepad: selected** | MIT; native Windows UMDF/VHF driver with an IOCTL interface. README lists Xbox Series, DS4, DualSense and Switch Pro. Xbox One is not implemented, and Xbox 360 is not supported through this VHF path. Producer packages require downstream signing. [G-NONARY], [G-NONARY-LICENSE] | Implement a native adapter and qualify game recognition, feedback, lifecycle and a distributable signed package. Physical Xbox One testing remains useful through other emulated profiles. |
| HIDMaestro v2.0.3: historical comparison | MIT; .NET SDK and native driver, with modern controller profiles. Documentation describes local certificate trust/elevated setup; some composite USB paths use usbip-win2. [G-HIDMAESTRO], [G-HIDMAESTRO-LICENSE] | Not selected for implementation. Integration would require assessment of a native bridge or companion process, runtimes and profile-specific dependencies. |

The owner's selection settles the backend choice: proceed with Nonary/libvirtualgamepad. Preserve the existing free compatibility path where required for inherited profiles, and make profile availability explicit. No reported latency figures are adopted as Miranda results. Free use by end users does not imply that producing and maintaining signed packages has no cost.

The later [Vibepollo review](09-vibepollo-feature-review.md) found that Vibepollo and Vibeshine both pin this driver at `7b000a2b060b4daeb0b5c6aa7b77ae1d81857e7a`, whereas this standalone review examined `998deebd15d0991562dec812dc62c52853463779`. The pinned driver README also marks Xbox One unimplemented despite the host's advertised profile and selector. Qualify actual package capabilities before exposing a profile; neither analysis pin is automatically the Miranda release pin.

## Required feasibility and acceptance evidence

1. Pin source, dependencies and distributable artifacts; verify their licenses, notices and source-to-package provenance. Identify any account, paid activation, expiration or service dependency.
2. Map profiles and capabilities to current Sunshine input interfaces. Advertise only working profiles; preserve non-Windows paths and meaningful Auto behavior.
3. Test physical Xbox One and Switch Pro through current Artemis and Windows/Fedora Moonlight. Record connection type, OS, firmware and client build. Check buttons, axes, triggers, game recognition and available rumble/motion transport. Mark unavailable physical-device coverage explicitly.
4. Test application inheritance/override, failed launch, termination/restoration, mixed controllers, multiple viewers, Input Only, permission revocation and disconnect. No stuck inputs or unintended changes to another active session.
5. Verify install, reboot, upgrade, service/process failure, uninstall and recovery on Windows 11 25H2. A development test certificate is not production package qualification.
6. Verify a clean installation can use every shipped feature without purchasing anything. Imported unsupported profiles must produce an explicit migration result. Do not release an unqualified modern profile merely to fill the menu.

Open technical result: qualify Nonary's production package and compatibility in Miranda. If a required capability cannot be delivered through the selected backend or existing free compatibility path, report the concrete gap and discuss its effect on scope; do not silently substitute another backend, a paid requirement or an untested support claim.

[S-WININPUT]: https://github.com/Gevroska/Miranda/blob/d0b4a88ef727f8dabf80cc970a14739b55c885de/src/platform/windows/input.cpp
[A-APPS]: https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src_assets/common/assets/web/apps.html
[A-PROCESS]: https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src/process.cpp
[G-LVH-MAP]: https://github.com/LizardByte/libvirtualhid/blob/dfff3ea8c34022c915799740806c17c0ed5c2492/LICENSES/license-map.md
[G-LVH-MIT]: https://github.com/LizardByte/libvirtualhid/blob/dfff3ea8c34022c915799740806c17c0ed5c2492/LICENSES/MIT.md
[G-LVH-SAL]: https://github.com/LizardByte/libvirtualhid/blob/dfff3ea8c34022c915799740806c17c0ed5c2492/LICENSES/LicenseRef-LizardByte-SAL-1.0.md
[G-NONARY]: https://github.com/Nonary/libvirtualgamepad/blob/998deebd15d0991562dec812dc62c52853463779/README.md
[G-NONARY-LICENSE]: https://github.com/Nonary/libvirtualgamepad/blob/998deebd15d0991562dec812dc62c52853463779/LICENSE
[G-HIDMAESTRO]: https://github.com/hifihedgehog/HIDMaestro/blob/63cc6481f9e82fc8600bda8aaa65cb3271113a19/README.md
[G-HIDMAESTRO-LICENSE]: https://github.com/hifihedgehog/HIDMaestro/blob/63cc6481f9e82fc8600bda8aaa65cb3271113a19/LICENSE
