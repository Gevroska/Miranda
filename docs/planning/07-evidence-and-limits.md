# Evidence, method, and analysis limits

Observed: 2026-10-23 (Europe/Paris).

## Pinned comparison

| Repository | Examined reference | Commit | Observation |
|---|---|---|---|
| Gevroska/Miranda | master | `d0b4a88ef727f8dabf80cc970a14739b55c885de` | Commit date 2026-10-21; identical to Sunshine master at inspection. |
| LizardByte/Sunshine | master | `d0b4a88ef727f8dabf80cc970a14739b55c885de` | Source baseline used for overlap checks. |
| ClassicOldSong/Apollo | master | `adc5c5a0bd80831ce495434bb16aee2cd4175fb8` | Commit date 2026-05-21. This is not the same snapshot as the latest stable installer. |
| ClassicOldSong/moonlight-android | moonlight-noir | `c5cf27f4dc822db0e863c4691e7a70c74bea977a` | Artemis source commit dated 2026-09-09; latest available Artemis is the test target, with exact versions to record at test time. |
| SudoMaker/SudoVDA | master | `a4b09fa2aa731a964d0cb5d139cb1e6240e4da12` | Source/docs reference for the driver comparison. |
| VirtualDrivers/Virtual-Display-Driver | master | `d7244969b2aa8bb38e76d79505eda217996cefea` | Current source/docs may exceed the latest stable release. |
| LizardByte/libvirtualhid | Miranda's pinned submodule | `dfff3ea8c34022c915799740806c17c0ed5c2492` | License map, MIT terms and separate LB-SAL terms checked at the actual baseline dependency. |
| Nonary/libvirtualgamepad | main | `998deebd15d0991562dec812dc62c52853463779` | Selected by DEC-30 after README/license review; no integration or package validation. This analysis pin is not yet a qualified release artifact. |
| hifihedgehog/HIDMaestro | v2.0.3 | `63cc6481f9e82fc8600bda8aaa65cb3271113a19` | Historical README/license comparison; not selected for implementation. |
| Nonary/Vibepollo | master | `8a753af16dcf1d752781ef4e6350da6146553e55` | Source head dated 2026-10-23; later-release feature inventory, not a qualified Miranda package. |
| Nonary/vibeshine | master | `af1a0842048584af90308d86b2a12acb20c5ad1b` | Source head dated 2026-10-23; ancestry and selected functional differences examined. |
| Nonary/vibeshine | canary branch | `abd0b8aa79fd4fcfe08838a91caa030898d5f788` | Targeted follow-up for branch lag; distinguish this branch from the repository's older tag also named canary. |
| Nonary/libvirtualgamepad | Dependency pinned by both Nonary host masters | `7b000a2b060b4daeb0b5c6aa7b77ae1d81857e7a` | Separate from the standalone-driver review pin above; host README and driver README disagree on Xbox One availability. |

Git found the most recent shared ancestor of Apollo and Miranda/Sunshine to be `1a96d135140ddfe5255516971c88f112b9da5beb`, dated 2025-09-26. This is a merge-base observation, not the original date Apollo was created.

Relative to that shared history, Miranda/Sunshine has 874 unique commits and Apollo has 457, including merges. Apollo has 389 unique non-merge commits. The ancestor-to-head file lists contain 218 changed paths on the Apollo side and 551 on the Sunshine side. These counts include maintenance, assets, dependency pointers and branding; they are not feature counts. No changed test paths were found in the ancestor-to-Apollo comparison.

The Apollo release API reported stable `v0.4.6` on 2025-07-13 and prerelease `v0.4.7-alpha.1` on 2025-08-12. Features found in master must not automatically be attributed to a particular installer. Gevroska/Apollo's `v0.4.7-beta.4` prerelease was independently observed as published on 2026-10-18; its cumulative source changes were not analyzed here.

## Method

1. Read repository metadata, Apollo README/wiki and release notes, and the supplied/local upstream instructions.
2. Resolve exact repository heads using public GitHub data and clone reference source into a temporary analysis directory.
3. Determine shared ancestry, enumerate unique commit subjects and changed paths, and inspect the functional additions rather than treating every historical fix as a missing feature.
4. Compare English configuration/UI keys, server endpoints, paired-client structures, app/session structures, Windows display/clipboard/input code, streaming/capture code, and relevant packaging paths.
5. Check current Sunshine equivalents for display management, HDR, client enable/disable, authentication/CSRF, modern UI/controllers, audio, capture timing, intra refresh and log rotation.
6. Cross-check Artemis source for exact fields, endpoints, identities, refresh-rate encoding, clipboard behavior, and Remote Input handling.
7. Compare official driver documentation plus selected control/identity source paths and current release metadata.
8. Record the owner's answers separately from recommendations and open choices.
9. Check the controller dependency's license boundaries and examine two independently licensed modern-controller candidates. Record advertised capabilities separately from tested behavior in [the feasibility review](08-free-controller-feasibility.md).
10. Review Vibepollo's changed paths, commit subjects, configuration, client contracts and selected implementations; record 48 functional groups, experimental status and external dependencies separately from v1.
11. Compare Vibeshine and Vibepollo through Git ancestry, default-branch tree differences, shared dependency pins and targeted canary checks. Distinguish newer upstream ancestry from feature parity and measured stability.

The GitHub connector was initially unavailable, so public GitHub web/API access and local Git inspection were used. It became accessible during the later-release review; subsequent metadata and source checks also used the connector. Partial-clone object retrieval required the appropriate repository remote; no conclusion relies on failed retrievals.

Vibepollo shares Apollo's September 2025 Sunshine ancestor. Vibeshine master and canary share Sunshine commit `2db7176f73e8a6b0eb3fcb6696e28234beb56db9`, dated 2026-05-20 in UTC/Europe/Paris. Miranda remains at October 2026 Sunshine. The default Vibeshine/Vibepollo trees differ in 976 paths, including 182 under `src/`, 123 under the new Web UI and 181 under `tests/`; these include inherited code, branding and maintenance. They are not 976 feature differences. See [the comparison](11-vibeshine-vibepollo-comparison.md) and [sources.json](sources.json) for reproducible pins and counts.

Both Nonary hosts reported latest stable release `2.0.1`, published 2026-10-14. The inspected master commits are newer. Prototype documentation, README claims and source tests are not native acceptance evidence. A newer merge base also does not establish that every intervening Sunshine fix is present, or that an older-base fork lacks fixes it may have adapted separately.

## What this establishes

- A functional inventory of Apollo additions and important overlaps with Miranda's current Sunshine baseline.
- The main observed Artemis contracts relevant to the selected first-release features.
- Source-backed reasons to adapt features individually instead of replacing current Sunshine subsystems wholesale.
- The current scope choices and the work/validation needed to make them implementation-ready.

## What it does not establish

- No line-by-line security audit, vulnerability validation, or guarantee of absence of defects.
- No build, installation, running-stream, hardware performance, acoustic/video-quality, or driver signing test.
- The owner reports Windows 11 25H2 and RTX 3080, with GPU testing limited to recent NVIDIA models. These are declared test targets, not inspected hardware evidence. Exact OS/GPU-driver builds, client versions and current monitor configuration have not been measured. Latest available Artemis and Moonlight (including Fedora/Windows) are confirmed test requirements.
- No proof that a driver README's current capabilities exist in its latest stable artifact.
- Nonary/libvirtualgamepad is selected but not integrated or validated. MIT library reuse is distinct from LB-SAL driver redistribution; the review records published terms and does not determine their legal enforceability. The owner can test physical Xbox One and Switch Pro on Android/Windows/Fedora; advanced features requiring other physical hardware remain unverified.
- No complete comparison of the owner's separate Gevroska/Apollo modifications against modern Sunshine.
- No finished implementation, pushed branch, PR, release, changed service, or changed driver.

The sources below distinguish code observations from project documentation. Wiki/release pages can change; repository code links are pinned. Runtime and package acceptance remain explicit later stages.

## Source map

Source labels used throughout the planning documents resolve to these pinned files or official project pages. The machine-readable revision record is [sources.json](sources.json).

| ID | Source |
|---|---|
| A-DISPLAY | [A-DISPLAY](https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src/platform/windows/virtual_display.cpp) |
| A-DISPLAY-OPTIONS | [A-DISPLAY-OPTIONS](https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src_assets/common/assets/web/configs/tabs/audiovideo/DisplayDeviceOptions.vue) |
| A-PROCESS | [A-PROCESS](https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src/process.cpp) |
| A-MAIN | [A-MAIN](https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src/main.cpp) |
| A-NVHTTP | [A-NVHTTP](https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src/nvhttp.cpp) |
| A-CRYPTO | [A-CRYPTO](https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src/crypto.h) |
| A-CONFIG | [A-CONFIG](https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src/config.cpp) |
| A-INPUT | [A-INPUT](https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src/input.cpp) |
| A-STREAM | [A-STREAM](https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src/stream.cpp) |
| A-CONFIGHTTP | [A-CONFIGHTTP](https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src/confighttp.cpp) |
| A-CLIPBOARD | [A-CLIPBOARD](https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src/platform/windows/misc.cpp) |
| A-PIN | [A-PIN](https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src_assets/common/assets/web/pin.html) |
| A-APPS | [A-APPS](https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src_assets/common/assets/web/apps.html) |
| A-ZWPAD | [A-ZWPAD](https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src/zwpad.h) |
| A-WININPUT | [A-WININPUT](https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src/platform/windows/input.cpp) |
| A-RTSP | [A-RTSP](https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src/rtsp.cpp) |
| A-VIDEO | [A-VIDEO](https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src/video.cpp) |
| A-WGC | [A-WGC](https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src/platform/windows/display_wgc.cpp) |
| A-NVENC | [A-NVENC](https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src/nvenc/nvenc_base.cpp) |
| A-WINCAPTURE | [A-WINCAPTURE](https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src/platform/windows/display_base.cpp) |
| A-AUDIO | [A-AUDIO](https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src/audio.cpp) |
| A-LOGGING | [A-LOGGING](https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src/logging.cpp) |
| A-TRAY | [A-TRAY](https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src/system_tray.cpp) |
| A-SUDOAPI | [A-SUDOAPI](https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/third-party/sudovda/sudovda.h) |
| A-DRIVER-INSTALL | [A-DRIVER-INSTALL](https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src_assets/windows/drivers/sudovda/install.bat) |
| A-PACKAGING | [A-PACKAGING](https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/cmake/packaging/windows_nsis.cmake) |
| A-LINUX-CLIPBOARD | [A-LINUX-CLIPBOARD](https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src/platform/linux/misc.cpp) |
| A-MAC-CLIPBOARD | [A-MAC-CLIPBOARD](https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src/platform/macos/misc.mm) |
| S-DISPLAY | [S-DISPLAY](https://github.com/Gevroska/Miranda/blob/d0b4a88ef727f8dabf80cc970a14739b55c885de/src/display_device.cpp) |
| S-NVHTTP | [S-NVHTTP](https://github.com/Gevroska/Miranda/blob/d0b4a88ef727f8dabf80cc970a14739b55c885de/src/nvhttp.cpp) |
| S-CONFIGHTTP | [S-CONFIGHTTP](https://github.com/Gevroska/Miranda/blob/d0b4a88ef727f8dabf80cc970a14739b55c885de/src/confighttp.cpp) |
| S-CONFIG | [S-CONFIG](https://github.com/Gevroska/Miranda/blob/d0b4a88ef727f8dabf80cc970a14739b55c885de/src/config.h) |
| S-STREAM | [S-STREAM](https://github.com/Gevroska/Miranda/blob/d0b4a88ef727f8dabf80cc970a14739b55c885de/src/stream.cpp) |
| S-VIDEO | [S-VIDEO](https://github.com/Gevroska/Miranda/blob/d0b4a88ef727f8dabf80cc970a14739b55c885de/src/video.cpp) |
| S-RTSP | [S-RTSP](https://github.com/Gevroska/Miranda/blob/d0b4a88ef727f8dabf80cc970a14739b55c885de/src/rtsp.cpp) |
| S-WGC | [S-WGC](https://github.com/Gevroska/Miranda/blob/d0b4a88ef727f8dabf80cc970a14739b55c885de/src/platform/windows/display_wgc.cpp) |
| S-NVENC | [S-NVENC](https://github.com/Gevroska/Miranda/blob/d0b4a88ef727f8dabf80cc970a14739b55c885de/src/nvenc/nvenc_base.cpp) |
| S-WINCAPTURE | [S-WINCAPTURE](https://github.com/Gevroska/Miranda/blob/d0b4a88ef727f8dabf80cc970a14739b55c885de/src/platform/windows/display_base.cpp) |
| S-AUDIO | [S-AUDIO](https://github.com/Gevroska/Miranda/blob/d0b4a88ef727f8dabf80cc970a14739b55c885de/src/platform/windows/audio.cpp) |
| S-LOGGING | [S-LOGGING](https://github.com/Gevroska/Miranda/blob/d0b4a88ef727f8dabf80cc970a14739b55c885de/src/logging.cpp) |
| S-AGENTS | [S-AGENTS](https://github.com/Gevroska/Miranda/blob/d0b4a88ef727f8dabf80cc970a14739b55c885de/AGENTS.md) |
| T-HTTP | [T-HTTP](https://github.com/ClassicOldSong/moonlight-android/blob/c5cf27f4dc822db0e863c4691e7a70c74bea977a/app/src/main/java/com/limelight/nvstream/http/NvHTTP.java) |
| T-DETAILS | [T-DETAILS](https://github.com/ClassicOldSong/moonlight-android/blob/c5cf27f4dc822db0e863c4691e7a70c74bea977a/app/src/main/java/com/limelight/nvstream/http/ComputerDetails.java) |
| T-PAIR | [T-PAIR](https://github.com/ClassicOldSong/moonlight-android/blob/c5cf27f4dc822db0e863c4691e7a70c74bea977a/app/src/main/java/com/limelight/nvstream/http/PairingManager.java) |
| T-CONNECTION | [T-CONNECTION](https://github.com/ClassicOldSong/moonlight-android/blob/c5cf27f4dc822db0e863c4691e7a70c74bea977a/app/src/main/java/com/limelight/nvstream/NvConnection.java) |
| T-GAME | [T-GAME](https://github.com/ClassicOldSong/moonlight-android/blob/c5cf27f4dc822db0e863c4691e7a70c74bea977a/app/src/main/java/com/limelight/Game.java) |
| T-MANIFEST | [T-MANIFEST](https://github.com/ClassicOldSong/moonlight-android/blob/c5cf27f4dc822db0e863c4691e7a70c74bea977a/app/src/main/AndroidManifest.xml) |
| V-SUDO | [V-SUDO](https://github.com/SudoMaker/SudoVDA/blob/a4b09fa2aa731a964d0cb5d139cb1e6240e4da12/README.md) |
| V-VDD | [V-VDD](https://github.com/VirtualDrivers/Virtual-Display-Driver/blob/d7244969b2aa8bb38e76d79505eda217996cefea/README.md) |
| V-VDD-SOURCE | [V-VDD-SOURCE](https://github.com/VirtualDrivers/Virtual-Display-Driver/blob/d7244969b2aa8bb38e76d79505eda217996cefea/Virtual%20Display%20Driver%20%28HDR%29/MttVDD/Driver.cpp) |
| W-DISPLAY | [W-DISPLAY](https://github.com/ClassicOldSong/Apollo/wiki/Display-Mode-Override) |
| W-PERMISSIONS | [W-PERMISSIONS](https://github.com/ClassicOldSong/Apollo/wiki/Permission-System) |
| W-CLIENT-COMMANDS | [W-CLIENT-COMMANDS](https://github.com/ClassicOldSong/Apollo/wiki/Client-Commands) |
| W-PAUSE | [W-PAUSE](https://github.com/ClassicOldSong/Apollo/wiki/Auto-pause-resume-games) |
| W-BEST | [W-BEST](https://github.com/ClassicOldSong/Apollo/wiki/Best-Practices) |
| W-APP-AUDIO | [W-APP-AUDIO](https://github.com/ClassicOldSong/Apollo/wiki/Stream-audio-from-only-one-app) |
| R-030 | [R-030](https://github.com/ClassicOldSong/Apollo/releases/tag/v0.3.0) |
| R-037 | [R-037](https://github.com/ClassicOldSong/Apollo/releases/tag/v0.3.7-hotfix.1) |
| R-046 | [R-046](https://github.com/ClassicOldSong/Apollo/releases/tag/v0.4.6) |
| R-047 | [R-047](https://github.com/ClassicOldSong/Apollo/releases/tag/v0.4.7-alpha.1) |
| V-VDD-RELEASE | [V-VDD-RELEASE](https://github.com/VirtualDrivers/Virtual-Display-Driver/releases/tag/25.7.23) |
| GEVROSKA-APOLLO-RELEASE | [GEVROSKA-APOLLO-RELEASE](https://github.com/Gevroska/Apollo/releases/tag/v0.4.7-beta.4) |
| S-WININPUT | [S-WININPUT](https://github.com/Gevroska/Miranda/blob/d0b4a88ef727f8dabf80cc970a14739b55c885de/src/platform/windows/input.cpp) |
| G-LVH-MAP | [G-LVH-MAP](https://github.com/LizardByte/libvirtualhid/blob/dfff3ea8c34022c915799740806c17c0ed5c2492/LICENSES/license-map.md) |
| G-LVH-MIT | [G-LVH-MIT](https://github.com/LizardByte/libvirtualhid/blob/dfff3ea8c34022c915799740806c17c0ed5c2492/LICENSES/MIT.md) |
| G-LVH-SAL | [G-LVH-SAL](https://github.com/LizardByte/libvirtualhid/blob/dfff3ea8c34022c915799740806c17c0ed5c2492/LICENSES/LicenseRef-LizardByte-SAL-1.0.md) |
| G-NONARY | [G-NONARY](https://github.com/Nonary/libvirtualgamepad/blob/998deebd15d0991562dec812dc62c52853463779/README.md) |
| G-NONARY-LICENSE | [G-NONARY-LICENSE](https://github.com/Nonary/libvirtualgamepad/blob/998deebd15d0991562dec812dc62c52853463779/LICENSE) |
| G-HIDMAESTRO | [G-HIDMAESTRO](https://github.com/hifihedgehog/HIDMaestro/blob/63cc6481f9e82fc8600bda8aaa65cb3271113a19/README.md) |
| G-HIDMAESTRO-LICENSE | [G-HIDMAESTRO-LICENSE](https://github.com/hifihedgehog/HIDMaestro/blob/63cc6481f9e82fc8600bda8aaa65cb3271113a19/LICENSE) |
| B-README | [B-README](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/README.md) |
| B-DISPLAY | [B-DISPLAY](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/platform/windows/virtual_display.cpp) |
| B-RECOVERY | [B-RECOVERY](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/platform/windows/display_helper_v2/state_machine.cpp) |
| B-MAINTENANCE | [B-MAINTENANCE](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src_assets/common/assets/web/views/MaintenanceView.vue) |
| B-CAPTURE | [B-CAPTURE](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/platform/windows/display_base.cpp) |
| B-HDR-PROFILE | [B-HDR-PROFILE](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/platform/windows/virtual_display_sunshine.cpp) |
| B-MODULES | [B-MODULES](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/.gitmodules) |
| B-REMOTE | [B-REMOTE](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/remote_session.cpp) |
| B-REMOTE-DOC | [B-REMOTE-DOC](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/docs/configuration.md) |
| B-TOPOLOGY | [B-TOPOLOGY](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/remote_display_topology.cpp) |
| B-LEASE | [B-LEASE](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/docs/remote-target-lease-prototype.md) |
| B-WGC | [B-WGC](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/platform/windows/display_wgc.cpp) |
| B-WGC-HELPER | [B-WGC-HELPER](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/tools/sunshine_wgc_capture.cpp) |
| B-FRAMEPUSH | [B-FRAMEPUSH](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/platform/windows/display_frame_push.cpp) |
| B-FRAMEGEN | [B-FRAMEGEN](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/framegen_policy.h) |
| B-APP-FRAMEGEN | [B-APP-FRAMEGEN](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/app_framegen_config.cpp) |
| B-LIMITER | [B-LIMITER](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/platform/windows/frame_limiter.cpp) |
| B-RTSS | [B-RTSS](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/platform/windows/rtss_integration.cpp) |
| B-LOSSLESS | [B-LOSSLESS](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/tools/playnite_launcher/lossless_scaling.cpp) |
| B-RTXHDR | [B-RTXHDR](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/platform/windows/rtx_hdr_policy.cpp) |
| B-RTXHDR-LINUX | [B-RTXHDR-LINUX](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/docs/linux/rtx-hdr.md) |
| B-COLOR | [B-COLOR](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/hdr_request_policy.h) |
| B-PYRO | [B-PYRO](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/docs/pyrowave-protocol.md) |
| B-PYRO-HOST | [B-PYRO-HOST](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/pyrowave_host.cpp) |
| B-PROBE | [B-PROBE](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/pyrowave_udp_probe.h) |
| B-BITRATE | [B-BITRATE](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/nvhttp_bitrate_policy.h) |
| B-NVHTTP | [B-NVHTTP](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/nvhttp.cpp) |
| B-AMF | [B-AMF](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/amf/amf_d3d11.cpp) |
| B-ENCODERS | [B-ENCODERS](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/video.cpp) |
| B-PLAYNITE | [B-PLAYNITE](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/plugins/playnite/SunshinePlaynite/README.md) |
| B-PLAYNITE-SYNC | [B-PLAYNITE-SYNC](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/platform/windows/playnite_sync.cpp) |
| B-STEAM | [B-STEAM](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/steam_integration.cpp) |
| B-STEAM-SYNC | [B-STEAM-SYNC](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/steam_sync_policy.cpp) |
| B-LUTRIS | [B-LUTRIS](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/lutris_integration.cpp) |
| B-FOCUS | [B-FOCUS](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/managed_app_focus.cpp) |
| B-PROCESS | [B-PROCESS](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/process.cpp) |
| B-DEVICES | [B-DEVICES](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src_assets/common/assets/web/views/DevicesView.vue) |
| B-GAMEPAD | [B-GAMEPAD](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/platform/windows/vhf_gamepad.cpp) |
| B-GAMEPAD-POLICY | [B-GAMEPAD-POLICY](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/platform/windows/vhf_gamepad_policy.cpp) |
| B-HAPTICS | [B-HAPTICS](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/docs/windows-dualsense-haptics.md) |
| B-SPEAKER | [B-SPEAKER](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/docs/controller_speaker_pcm.md) |
| B-INPUT | [B-INPUT](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/input.cpp) |
| B-BROWSER | [B-BROWSER](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src_assets/common/assets/web/views/BrowserStreamView.vue) |
| B-WEBRTC | [B-WEBRTC](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/webrtc_stream.cpp) |
| B-AUDIO | [B-AUDIO](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/audio_policy.h) |
| B-UI | [B-UI](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src_assets/common/assets/web/router.ts) |
| B-AUTH | [B-AUTH](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/http_auth.cpp) |
| B-API | [B-API](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/confighttp.cpp) |
| B-STATS | [B-STATS](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/session_history.cpp) |
| B-HOST-STATS | [B-HOST-STATS](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/host_stats_service.cpp) |
| B-METRICS | [B-METRICS](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/docs/hardware-metrics.md) |
| B-UPDATES | [B-UPDATES](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/docs/api-only-update-contract.md) |
| B-INSTALLER | [B-INSTALLER](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/packaging/windows/bootstrap_migration.md) |
| B-POWER | [B-POWER](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/docs/power-actions.md) |
| B-PAIR-RECOVERY | [B-PAIR-RECOVERY](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/pairing_recovery.cpp) |
| B-STATE | [B-STATE](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/state_storage_policy.cpp) |
| B-LINUX-INSTALL | [B-LINUX-INSTALL](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/docs/linux/install.md) |
| B-LINUX-DISPLAY | [B-LINUX-DISPLAY](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/platform/linux/private_display.cpp) |
| B-LINUX-LIMITER | [B-LINUX-LIMITER](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/platform/linux/frame_limiter.cpp) |
| B-LINUX-INPUT | [B-LINUX-INPUT](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/docs/linux/playstation-controllers.md) |
| B-STEAMOS | [B-STEAMOS](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/packaging/linux/steamos/AUDIT.md) |
| B-HYPRLAND | [B-HYPRLAND](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/docs/linux/hyprland.md) |
| B-CRYPTO | [B-CRYPTO](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/crypto.h) |
| B-APPS | [B-APPS](https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src_assets/common/assets/web/apps.html) |
| B-GAMEPAD-README | [B-GAMEPAD-README](https://github.com/Nonary/libvirtualgamepad/blob/7b000a2b060b4daeb0b5c6aa7b77ae1d81857e7a/README.md) |
| H-README | [H-README](https://github.com/Nonary/vibeshine/blob/af1a0842048584af90308d86b2a12acb20c5ad1b/README.md) |
| H-NVHTTP | [H-NVHTTP](https://github.com/Nonary/vibeshine/blob/af1a0842048584af90308d86b2a12acb20c5ad1b/src/nvhttp.cpp) |
| H-CRYPTO | [H-CRYPTO](https://github.com/Nonary/vibeshine/blob/af1a0842048584af90308d86b2a12acb20c5ad1b/src/crypto.h) |
| H-DEVICES | [H-DEVICES](https://github.com/Nonary/vibeshine/blob/af1a0842048584af90308d86b2a12acb20c5ad1b/src_assets/common/assets/web/views/DevicesView.vue) |
| H-API | [H-API](https://github.com/Nonary/vibeshine/blob/af1a0842048584af90308d86b2a12acb20c5ad1b/src/confighttp.cpp) |
| H-MODULES | [H-MODULES](https://github.com/Nonary/vibeshine/blob/af1a0842048584af90308d86b2a12acb20c5ad1b/.gitmodules) |
| H-WEB | [H-WEB](https://github.com/Nonary/vibeshine/tree/af1a0842048584af90308d86b2a12acb20c5ad1b/src_assets/common/assets/web) |
| B-RELEASE | [B-RELEASE](https://github.com/Nonary/Vibepollo/releases/tag/2.0.1) |
| H-RELEASE | [H-RELEASE](https://github.com/Nonary/vibeshine/releases/tag/2.0.1) |
| B-SMOOTH | [B-SMOOTH](https://www.nvidia.com/en-us/geforce/news/nvidia-app-global-dlss-overrides-rtx-40-series-smooth-motion/) |
| B-LS-STORE | [B-LS-STORE](https://store.steampowered.com/app/993090/Lossless_Scaling/) |
| A-PACKAGE-ROOT | [A-PACKAGE-ROOT](https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/CMakeLists.txt) |
| A-PACKAGE-COMMON | [A-PACKAGE-COMMON](https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/cmake/packaging/common.cmake) |
| A-PACKAGE-WINDOWS | [A-PACKAGE-WINDOWS](https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/cmake/packaging/windows.cmake) |
| A-SERVICE-INSTALL | [A-SERVICE-INSTALL](https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src_assets/windows/misc/service/install-service.bat) |
| A-CONFIG-MIGRATE | [A-CONFIG-MIGRATE](https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src_assets/windows/misc/migration/migrate-config.bat) |
