# Virtual display driver comparison

Updated: 2026-10-23. Confirmed order: implement SudoMaker/SudoVDA first, then compare with VirtualDrivers/Virtual-Display-Driver. These are the only two candidates. No driver was installed or changed.

## Required outcome

The integration must create and remove displays safely, support arbitrary valid requested resolutions/rates and HDR within the pipeline's capabilities, work headlessly, retain a persistent identity for each client's own display, and recover predictably after failures. A joining viewer shares the current session and must not replace its display or change its resolution, refresh rate or HDR state. It must also coexist with modern Sunshine display management. A driver supporting HDR alone does not satisfy this contract.

## Candidates

| Candidate | Evidence available | Fit for Miranda | Remaining proof |
|---|---|---|---|
| **SudoMaker/SudoVDA** | Apollo already calls its add/remove IOCTL interface with a GUID, client metadata and display mode; supports render-adapter selection and a watchdog. README describes HDR and fractional/color options. Repository head `a4b09fa2`; no GitHub releases were returned from the SudoMaker repository. [V-SUDO], [A-DISPLAY], [A-SUDOAPI] | Closest existing functional integration and strongest direct evidence for Apollo-like client identity behavior. | Reproducible binary acquisition/build, exact driver/installer matching, signing/trust, update/removal behavior, compatibility with current Windows/GPU drivers. Apollo's source tree alone lacks `SudoVDA.dll` and `nefconc.exe`. |
| **VirtualDrivers/Virtual-Display-Driver (VDD/MttVDD)** | Official source/docs describe HDR, fractional rates, EDID, GPU selection and a named-pipe control interface. The inspected source includes `SETDISPLAYCOUNT` and `SETGPU`. Current head `d7244969`; latest stable release returned by GitHub is `25.7.23` (2025-07-23), so current-source claims must not be assumed to exist in that release. [V-VDD], [V-VDD-SOURCE], [V-VDD-RELEASE] | Credible alternative with broader documented control/configuration options. Requires a new Miranda adapter rather than Apollo's IOCTL calls. | Prove stable independent identity per Artemis client, exact requested modes without disrupting other monitors, safe repeated add/remove, HDR behavior, service-context access and the chosen release's actual API. A global monitor-count command does not by itself prove per-client lifecycle semantics. |

These are documented/source-level comparisons. No comparative performance, stability, certification, or binary trust verdict has been produced. Source commit activity and release dates describe different things.

## Driver support versus stream support

- A driver may expose 10/12-bit modes or HDR metadata without the entire client/codec path supporting the same format.
- SudoVDA's README describes Windows 11 24H2 as the fully supported HDR target; VDD's README describes HDR on Windows 11 23H2+. Those are project claims to validate on the owner's Windows 11 25H2 host, with its exact build recorded.
- Windows 10 SDR support must not be confused with a promise of virtual-display HDR.
- ARM64 support is not currently a confirmed Miranda requirement. VDD documents an ARM64 signing caveat; validate separately if ARM64 is requested.

## Confirmed implementation order and proposed comparison method

1. Pin a reproducible SudoVDA package and map its actual API to the inspected Apollo integration. Verify required binaries, source/artifact relationship, hashes and signer/trust behavior.
2. Implement the first Windows virtual-display integration with SudoVDA. Establish a working lifecycle, mode/HDR handling, headless operation, persistent identities and shared-viewing baseline.
3. Evaluate VDD against that baseline using one pinned distributable build and its actual API. A limited comparison adapter may be needed; a fully supported second production backend is not yet selected. VDD evaluation does not block beginning SudoVDA integration.
4. Run the same scenarios on Windows 11 25H2 and RTX 3080, recording exact Windows and GPU-driver builds. Additional GPU testing is limited to available recent NVIDIA models. Driver installation is a later implementation/test action, not performed by this analysis.
5. Record pass/fail evidence, frame pacing and recovery observations. Use the results to decide whether to retain SudoVDA alone, replace it, or support VDD too. Initial SudoVDA implementation is already selected; the later production decision remains open.

## Display policy to preserve

Native-display capture must keep the captured physical screen active and on. With virtual-display capture, users choose whether physical displays remain active and which display is primary. Preserve the equivalent Apollo controls through modern Sunshine display management. Apollo's inspected `dd_configuration_option` offers the following values. [A-DISPLAY-OPTIONS]

| Option | Behavior to retain |
|---|---|
| `disabled` | Leave automatic display configuration disabled. This does not override the native-screen-on requirement. |
| `verify_only` | Check that the selected display is enabled. |
| `ensure_active` | Ensure the selected display is active while retaining other active displays. |
| `ensure_primary` | Ensure the selected display is active and primary. |
| `ensure_only_display` | Activate the selected display and deactivate the others. For virtual capture, this allows physical displays to be disabled. |

Restore the previous topology at the appropriate session boundary; disconnecting one of several viewers must not restore/remove a display still in use. Preserve compatible restoration settings such as delay and disconnect behavior, reconciled with that shared-session invariant. Display deactivation and electrical monitor power are distinct: validate the visible screen-on behavior for native capture on real hardware. Bottom-right virtual-screen isolation is a separate optional feature, still open.

## Comparison matrix to execute later

| Check | Success condition |
|---|---|
| No physical monitor | Service startup, encoder probing, first stream and reconnect work. |
| Client mode | Arbitrary valid width/height/rate requests are accepted within capabilities, including portrait/custom modes and supported fractional rates. Test several resolutions at 60, 120 and 165 Hz and independently observe display and stream rates. Unsupported combinations produce explicit results. |
| Stable identity | A client originating its own session gets the same effective Windows monitor identity after reconnect/restart. A second client's own saved arrangement is retained separately; joining an existing session does not apply it over the current arrangement. |
| HDR | Client request, Windows HDR state, encoded format and visible result agree on supported hardware. |
| Cleanup/recovery | Normal close, failed launch, abrupt client loss, Miranda crash/restart and driver failure do not leave a permanently unusable display topology. |
| Existing display setup | Native capture leaves the captured physical screen on. Virtual capture honors keep-active/primary/only-display selections. Physical monitor layout and primary display recover at the appropriate session boundary. |
| Second video client | Joins the same app/display without changing active resolution, refresh rate or HDR. Joining/leaving does not terminate another viewer or bypass per-device permissions. |
| Concurrent input device | Input Only does not create/reconfigure an unnecessary display or disrupt another stream. |
| GPU selection | RTX 3080 renders, captures and encodes successfully on Windows 11 25H2. Exercise additional adapter arrangements only if available; results do not certify AMD/Intel GPUs. |
| Client compatibility | Latest available Artemis and Moonlight, including Fedora/Windows Moonlight, pass their supported streaming paths. Record exact versions and device/OS builds per run. |
| Packaging | Install, upgrade, uninstall and rollback preserve unrelated drivers and settings. |
| Signing/trust | The exact artifact's signatures, hashes and required trust-store changes are understood; a README's signed badge is not binary verification. |

## Decision and remaining design recommendation

SudoVDA is the confirmed first implementation, with VDD as the only alternative to compare afterward. Keep a small replaceable backend boundary as a design recommendation; do not build a general driver framework or claim VDD production support before the comparison establishes its fit.

[A-DISPLAY]: https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src/platform/windows/virtual_display.cpp
[A-SUDOAPI]: https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/third-party/sudovda/sudovda.h
[A-DISPLAY-OPTIONS]: https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src_assets/common/assets/web/configs/tabs/audiovideo/DisplayDeviceOptions.vue
[V-SUDO]: https://github.com/SudoMaker/SudoVDA/blob/a4b09fa2aa731a964d0cb5d139cb1e6240e4da12/README.md
[V-VDD]: https://github.com/VirtualDrivers/Virtual-Display-Driver/blob/d7244969b2aa8bb38e76d79505eda217996cefea/README.md
[V-VDD-RELEASE]: https://github.com/VirtualDrivers/Virtual-Display-Driver/releases/tag/25.7.23
[V-VDD-SOURCE]: https://github.com/VirtualDrivers/Virtual-Display-Driver/blob/d7244969b2aa8bb38e76d79505eda217996cefea/Virtual%20Display%20Driver%20%28HDR%29/MttVDD/Driver.cpp
