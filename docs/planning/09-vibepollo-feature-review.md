# Vibepollo feature review for later Miranda releases

Observed: 2026-10-23, Europe/Paris. Reference: Nonary/Vibepollo `8a753af16dcf1d752781ef4e6350da6146553e55`. This is a functional source review, not a build, runtime certification or security audit.

## Scope and evidence

DEC-34 initially placed Vibepollo additions in a later-release review. DEC-38 now selects all reference-project functionality as the long-term target; DEC-39 preserves the already-defined focused v1. Capture quality, frame pacing and HDR remain the first priority (DEC-35). Nonary's Moonlight fork is an optional additional client (DEC-36), and Remote Monitor is an explicit future mode (DEC-37). Selection is distinct from implementation, qualification and release readiness.

The review covers the README, release metadata, unique commit subjects, changed-file inventory, configuration, routes, UI views, client contracts and selected implementation paths. It groups related fixes into capabilities rather than treating every commit as a feature. Relative to the examined Apollo head, Vibepollo has 2,512 unique commits, including 2,430 non-merge commits; Apollo has 22 commits absent from Vibepollo's ancestry. Their merge base is `bf47fca9e498cd0654950fb713208b220dcc8a76`. The merge-base-to-Vibepollo comparison has 2,546 changed paths, including 433 under `tests/`; these are not test results or feature counts.

The latest stable release observed was `2.0.1`, published 2026-10-14. The observed newest prerelease was `2.0.2-nightly.d20261022.83bb31680419`. Master includes later changes; an implementation present in this snapshot is not automatically available or qualified in either installer. [B-RELEASE]

Vibeshine carries many of the same Nonary additions on a newer Sunshine foundation. See [the comparison](11-vibeshine-vibepollo-comparison.md) before choosing source changes to adapt. The 48 entries below form the functional backlog. Overlaps should be unified; paid dependencies need free replacements, and experimental paths require qualification before production delivery.

Status meanings: **Existing v1 overlap** preserves a v1 outcome; **Future selected** and **Later target** identify confirmed long-term scope; **Prototype** remains a development target with explicit maturity limitations; **Constraint conflict** requires an adapted implementation or dependency decision. None of these labels asserts Miranda implementation. Preserve the two-driver shortlist until a separate decision changes it.

## Displays, recovery and session roles

| ID | Capability and observed implementation | Miranda disposition and dependency |
|---|---|---|
| B01 | Automatic virtual displays, client modes/HDR, persistent identity and headless probing. [B-DISPLAY] | **Existing v1 overlap.** Use the source as an engineering reference; it does not add a second automatic-display project. |
| B02 | Extended layout recovery: saved physical snapshots, a known-good layout, helper/watchdog recovery and returning-monitor handling. [B-RECOVERY], [B-MAINTENANCE] | **Later target** for additional recovery tools. V1 already requires reliable cleanup/restoration; that obligation is not deferred. Validate crash/reboot/partial-failure behavior and respect user changes. |
| B03 | Stable GPU/output selection and hybrid-GPU display routing. [B-CAPTURE], [B-DISPLAY] | **Later target** for expanded selection and diagnostics. Basic correct adapter selection remains v1 work. Multi-GPU/hybrid claims need hardware beyond the declared RTX 3080 setup. |
| B04 | Per-device Windows HDR calibration-profile association and luminance handling. [B-HDR-PROFILE] | **Later target.** Distinct from v1's HDR on/off and remembered modes. Verify profile ownership, color correctness and restoration on physical outputs. |
| B05 | Nonary's bundled virtual-display driver with SudoVDA fallback, mode catalog and capacity management. [B-MODULES], [B-DISPLAY] | **Constraint conflict with the present display shortlist.** It is a third display driver, separate from the selected Nonary controller driver. Do not replace SudoVDA or add this backend without an explicit later decision. |
| B06 | Remote Monitor streams an additional private display alongside the running game/desktop; Remote Input has a separate control-only role. [B-REMOTE], [B-REMOTE-DOC] | **Future selected** for Remote Monitor. Keep ordinary viewers joining the existing session. Extra displays extend the same Windows desktop; they are not isolated Windows users. Input Only is already selected for v1. |
| B07 | Saved relative arrangements of client displays: anchor, direction, spacing, alignment and primary status. [B-TOPOLOGY] | **Later target**, supporting Remote Monitor. Do not infer a maximum number of Miranda monitors from Vibepollo's documented limit of eight. |
| B08 | Remote Monitor profiles and retention/resume/disconnect policies, audio muting and guarded replacement/termination controls. [B-REMOTE], [B-REMOTE-DOC] | **Later target details** within the selected Remote Monitor direction. Decide ownership and guest authority explicitly. One retained profile per client is not an unlimited number of screens per device. |
| B09 | Synthetic launch tiles for Resume, Remote Input, Remote Monitor, Disconnect and Terminate, including standard-Moonlight interaction. [B-REMOTE] | **Partial v1 overlap / later target.** Preserve Artemis's selected Input Only identity. Evaluate additional tiles and their permissions separately. |
| B10 | Exact-target borrowing for existing physical/shared virtual displays. [B-LEASE] | **Prototype.** Explicitly disabled by default and not a completed normal Remote Monitor target selector. Do not advertise as ready or silently add to v1. |

## Capture, pacing, color and transport

| ID | Capability and observed implementation | Miranda disposition and dependency |
|---|---|---|
| B11 | WGC helper/service capture, desktop transitions and fallback handling for login/UAC situations. [B-WGC], [B-WGC-HELPER] | **Later target, priority group.** Modern Sunshine already has capture infrastructure; compare actual lifecycle behavior before porting. Frame-generation or VRAM claims require measured games and failure cases. |
| B12 | Reduced-copy WGC handoff and direct GPU encoder-input options. [B-WGC], [B-WGC-HELPER] | **Later target.** Benchmark capture-to-encode latency, sustained pacing, memory pressure and repeated/keyframe behavior; a code path alone is not a performance result. |
| B13 | IddCx frame-push capture from Nonary's display driver. [B-FRAMEPUSH], [B-CAPTURE] | **Prototype / driver-dependent target.** Opt-in build/runtime path tied to a matching producer protocol. Compatibility with SudoVDA has not been established. |
| B14 | Capture policy for game-provided frame generation, with promoted display refresh distinct from stream rate. [B-FRAMEGEN], [B-APP-FRAMEGEN] | **Later target, priority group.** Do not equate a 4x display mode with 4x encoded FPS. Measure supported modes, duplicates, latency and GPU load. |
| B15 | Automatic game FPS limits through RTSS or NVIDIA settings, V-Sync-related changes and restoration. [B-LIMITER], [B-RTSS] | **Later target, priority group.** External-tool presence, per-app scope, shared viewers and restoring pre-existing settings need clear rules. This is additional automation, not part of v1's existing prep-command preservation. |
| B16 | NVIDIA Smooth Motion integration. [B-APP-FRAMEGEN], [B-SMOOTH] | **Later target with hardware gap.** NVIDIA documents RTX 40/50 support; the declared RTX 3080 does not establish a test path. Preserve the global free-feature requirement. |
| B17 | Lossless Scaling setup/launch/frame-generation integration. [B-LOSSLESS], [B-LS-STORE] | **Constraint conflict.** The external application is paid. Its integration code does not make that dependency free; replace the paid prerequisite with a free implementation or alternative; feasibility remains unresolved. |
| B18 | RTX HDR/TrueHDR conversion of SDR capture, app/global tuning and live adjustment. [B-RTXHDR], [B-RTXHDR-LINUX] | **Later target, priority group.** Distinct from streaming native HDR. Check NVIDIA runtime redistribution, supported hardware/capture paths and real colors; do not infer compatibility from RTX branding alone. |
| B19 | Prefer 10-bit SDR for selected devices/apps and additional color-policy controls. [B-COLOR] | **Later target.** Negotiate codec/client capabilities and distinguish HDR signaling from bit depth. |
| B20 | VRR-oriented capture/playback: Windows fixed high-refresh virtual mode and a compatible client pacing path. [B-FRAMEGEN], [B-PYRO] | **Later target.** Selected as a later feature under DEC-38; requires a compatible optional client and validation. A 1000 Hz virtual mode is not a 1000 FPS stream or physical display promise. |
| B21 | PyroWave GPU wavelet streaming, with independent frames and negotiated color formats. [B-PYRO], [B-PYRO-HOST] | **Later target.** Requires compatible client/bitstream support, substantial bandwidth and GPU qualification. Stock Moonlight support does not imply PyroWave support in Artemis. |
| B22 | PyroWave-specific UDP link calibration, transport budgeting, partial recovery and adaptive parity. [B-PYRO], [B-PROBE] | **Later target coupled to B21.** Test realistic network loss, overhead and mixed traffic. A link-speed value is not a measured path capacity. |
| B23 | Runtime bitrate control and capability negotiation for participating clients. [B-BITRATE], [B-NVHTTP] | **Later target.** Host controls alone do not create a working adaptive client. Define authority, limits and shared-stream effects; compare the newer Sunshine transport before adapting. |
| B24 | Encoder/backend changes and tuning, including native AMD AMF and Linux NVENC paths. [B-AMF], [B-ENCODERS] | **Engineering targets**, not wholesale ports. Reconcile against modern Sunshine equivalents; AMD/Intel performance cannot be qualified with the declared NVIDIA-only hardware. |

## Libraries and application management

| ID | Capability and observed implementation | Miranda disposition and dependency |
|---|---|---|
| B25 | Playnite library sync, recent-game/category/exclusion policies, artwork and compiled connector. [B-PLAYNITE], [B-PLAYNITE-SYNC] | **Later target.** The examined connector targets Playnite 10; its documentation explicitly calls for a separate Playnite 11 port. Confirm the user's actual version before implementation. |
| B26 | Native Steam discovery/sync, recent/all-installed policies, exclusions, covers and launch support. [B-STEAM], [B-STEAM-SYNC] | **Later target.** Preserve manually configured apps and stable IDs; decide whether Steam or Playnite owns duplicate entries. |
| B27 | Lutris discovery/sync and provider-aware launch/artwork. [B-LUTRIS] | **Later target for a Linux host.** Fedora as a streaming client does not make Linux-host library sync part of the Windows-first scope. |
| B28 | Managed game focus, process tracking and termination, including launcher/emulator cases. [B-FOCUS], [B-PROCESS] | **Later target.** Define which launched processes are owned and what can be closed; distinguish managed libraries from arbitrary manual commands. |
| B29 | Broad app/client configuration overrides and richer editing workflows. [B-PROCESS], [B-DEVICES] | **Partial v1 overlap / later target.** V1 already selects controller/display overrides. Do not silently expose every global setting as an override. Establish precedence and scope per setting. |

## Controllers, browser access and audio

| ID | Capability and observed implementation | Miranda disposition and dependency |
|---|---|---|
| B30 | Native integration of Nonary/libvirtualgamepad, profile selection and device recovery. [B-GAMEPAD], [B-GAMEPAD-POLICY] | **Existing v1 selection (DEC-30).** Useful integration reference. Shared driver does not mean identical client capabilities or already validated profiles. |
| B31 | Composite USB DualSense waveform haptics and gain control. [B-HAPTICS] | **Later target.** Additional USB/IP transport and a capable client/physical DualSense are required. Ordinary HID rumble/adaptive-trigger messages and waveform audio are distinct. |
| B32 | Separate DualSense controller-speaker PCM transport. [B-SPEAKER] | **Source-stage target.** Its document explicitly leaves native playback/client validation outstanding. It is separate from actuator haptics and does not establish microphone return. |
| B33 | Bind a client keyboard key to that client's controller Guide/Home button, with per-session selection. [B-INPUT] | **Later target.** Test ownership, simultaneous clients, release/reconnect and both keyboard/controller permissions. |
| B34 | Browser streaming through WebRTC, with capability-based codec/HDR controls and browser input. [B-BROWSER], [B-WEBRTC] | **Later target.** A separate authenticated client path, not automatic Artemis feature parity. Check browser codec support, access scope, network reachability and coexistence with native streams. |
| B35 | Capture a chosen audio sink without changing Windows defaults, with role-aware recovery/routing refinements. [B-AUDIO] | **Later target beyond v1 audio reliability.** Test host playback, stream audio, endpoint removal, user changes and concurrent sessions. |

## Administration, diagnostics and packaging

| ID | Capability and observed implementation | Miranda disposition and dependency |
|---|---|---|
| B36 | Task-oriented configuration UI, library/devices pages, grouped settings and maintenance. [B-UI] | **Later design target.** Miranda already inherits a modern Vue UI; adapt selected workflows instead of replacing it wholesale or keeping two complete interfaces by default. |
| B37 | Session login, remembered login and session revocation. [B-AUTH], [B-API] | **Later target overlapping Apollo administration.** Preserve modern Sunshine authentication/CSRF and define the desired UX in the existing behavior queue. |
| B38 | API tokens scoped to registered routes and methods, with revocation. [B-AUTH], [B-API] | **Later target.** Tokens are administration/automation credentials, distinct from paired-client streaming permissions. |
| B39 | Live host/session charts plus persistent session history and retention limits. [B-STATS], [B-HOST-STATS] | **Later target.** Choose useful metrics, overhead and retention. Do not call host-side timings end-to-end latency. |
| B40 | Optional hardware metrics for an authenticated client that owns an active stream. [B-METRICS] | **Later target requiring a client renderer.** Source documents provider limitations and separate native acceptance; do not promise all GPUs or multiple-GPU enumeration. |
| B41 | Filtered logs, support exports, crash bundle manifests and redaction work. [B-API], [B-MAINTENANCE] | **Later target.** Keep diagnostic export separate from automatic upload. Inspect for credentials, client identifiers and dump contents before defining shareable output. |
| B42 | Update notices, channel selection and user-initiated installer downloads. [B-UPDATES] | **Later target.** Miranda must use its own release feed and channel policy. Update notifications are not automatic installation. |
| B43 | Recovery/maintenance actions, Windows service-startup controls, migration-aware installer and portable package. [B-MAINTENANCE], [B-INSTALLER] | **Partial v1 packaging overlap / future conveniences.** Port only needed behavior and retain Miranda-specific identity/driver ownership. A portable host may still depend on installed drivers. |
| B44 | Explicitly granted paired-client idle-only sleep action. [B-POWER] | **Later target requiring compatible client UI.** Separate opt-in from streaming permission; source documents outstanding native sleep/wake acceptance. V1 session automation remains deferred. |
| B45 | State/paired-client recovery proposals and explicit repair paths. [B-PAIR-RECOVERY], [B-STATE] | **Later target / engineering reference.** Inspect what a tool actually restores and what needs operator review. Do not infer preservation of Apollo identities from backup availability. |

## Linux and experimental platform expansion

| ID | Capability and observed implementation | Miranda disposition and dependency |
|---|---|---|
| B46 | Machine-host Linux services, private virtual displays, HDR and login-session integration. [B-LINUX-INSTALL], [B-LINUX-DISPLAY] | **Later platform target.** The documented native beta targets Arch/CachyOS with specific kernel/compositor requirements; Fedora clients remain a separate, already-selected requirement. |
| B47 | Linux library/Proton pacing, MangoHud, HDR compatibility, NVIDIA TrueHDR and PlayStation controller extensions. [B-LINUX-LIMITER], [B-RTXHDR-LINUX], [B-LINUX-INPUT] | **Later platform targets.** Review independently with actual Linux-host hardware; do not imply that preserving upstream Linux builds delivers all these additions. |
| B48 | SteamOS Gaming Mode/user bundle and additional compositor paths, including experimental HDR work. [B-STEAMOS], [B-HYPRLAND] | **Experimental later targets.** Read the platform-specific limitations and distinguish SDR capture, patched HDR and independent-monitor capabilities. |

## Cross-cutting findings for Miranda

1. **Keep the current base.** Feature adaptation must preserve modern Sunshine interfaces, current authentication and the selected Artemis contract. Vibeshine can be a useful second implementation reference, but its May 2026 common Sunshine baseline is older than Miranda's October baseline.
2. **Shared viewers and Remote Monitor are different roles.** Add explicit target ownership and authorization for independent screens; an ordinary join must continue preserving the current display mode.
3. **Free features remain mandatory.** Lossless Scaling cannot be adopted unchanged as a paid prerequisite. Audit all optional SDK/runtime/driver distributions individually; the root GPL license does not grant rights to every external component.
4. **The display shortlist remains unchanged.** Selecting Nonary's gamepad driver does not select Nonary/libvirtualdisplay. Driver-bound capture, calibration and pacing need an adaptation assessment before claiming SudoVDA support.
5. **Source and marketing can disagree.** Vibepollo advertises Xbox One emulation and has a host selector, while the README at its pinned gamepad dependency `7b000a2b060b4daeb0b5c6aa7b77ae1d81857e7a` marks Xbox One unimplemented. Resolve profile availability against the actual runtime/package. The earlier standalone-driver review used `998deebd...`; neither is automatically the Miranda release pin. [B-GAMEPAD-POLICY], [B-GAMEPAD-README]
6. **No performance guarantee follows from a feature name.** Establish capture, encode, network and client measurements independently; keep image-quality checks and user acceptance distinct from automated tests.

No drivers were installed, no streams launched and no source changes ported during this review. The next decisions and proposed acceptance work are in [the later-release roadmap](10-vibepollo-roadmap-and-questions.md). Revision/source mappings are recorded in [sources.json](sources.json).

[B-AMF]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/amf/amf_d3d11.cpp
[B-API]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/confighttp.cpp
[B-APP-FRAMEGEN]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/app_framegen_config.cpp
[B-AUDIO]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/audio_policy.h
[B-AUTH]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/http_auth.cpp
[B-BITRATE]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/nvhttp_bitrate_policy.h
[B-BROWSER]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src_assets/common/assets/web/views/BrowserStreamView.vue
[B-CAPTURE]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/platform/windows/display_base.cpp
[B-COLOR]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/hdr_request_policy.h
[B-DEVICES]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src_assets/common/assets/web/views/DevicesView.vue
[B-DISPLAY]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/platform/windows/virtual_display.cpp
[B-ENCODERS]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/video.cpp
[B-FOCUS]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/managed_app_focus.cpp
[B-FRAMEGEN]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/framegen_policy.h
[B-FRAMEPUSH]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/platform/windows/display_frame_push.cpp
[B-GAMEPAD]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/platform/windows/vhf_gamepad.cpp
[B-GAMEPAD-POLICY]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/platform/windows/vhf_gamepad_policy.cpp
[B-GAMEPAD-README]: https://github.com/Nonary/libvirtualgamepad/blob/7b000a2b060b4daeb0b5c6aa7b77ae1d81857e7a/README.md
[B-HAPTICS]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/docs/windows-dualsense-haptics.md
[B-HDR-PROFILE]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/platform/windows/virtual_display_sunshine.cpp
[B-HOST-STATS]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/host_stats_service.cpp
[B-HYPRLAND]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/docs/linux/hyprland.md
[B-INPUT]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/input.cpp
[B-INSTALLER]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/packaging/windows/bootstrap_migration.md
[B-LEASE]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/docs/remote-target-lease-prototype.md
[B-LIMITER]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/platform/windows/frame_limiter.cpp
[B-LINUX-DISPLAY]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/platform/linux/private_display.cpp
[B-LINUX-INPUT]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/docs/linux/playstation-controllers.md
[B-LINUX-INSTALL]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/docs/linux/install.md
[B-LINUX-LIMITER]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/platform/linux/frame_limiter.cpp
[B-LOSSLESS]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/tools/playnite_launcher/lossless_scaling.cpp
[B-LS-STORE]: https://store.steampowered.com/app/993090/Lossless_Scaling/
[B-LUTRIS]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/lutris_integration.cpp
[B-MAINTENANCE]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src_assets/common/assets/web/views/MaintenanceView.vue
[B-METRICS]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/docs/hardware-metrics.md
[B-MODULES]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/.gitmodules
[B-NVHTTP]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/nvhttp.cpp
[B-PAIR-RECOVERY]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/pairing_recovery.cpp
[B-PLAYNITE]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/plugins/playnite/SunshinePlaynite/README.md
[B-PLAYNITE-SYNC]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/platform/windows/playnite_sync.cpp
[B-POWER]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/docs/power-actions.md
[B-PROBE]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/pyrowave_udp_probe.h
[B-PROCESS]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/process.cpp
[B-PYRO]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/docs/pyrowave-protocol.md
[B-PYRO-HOST]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/pyrowave_host.cpp
[B-RECOVERY]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/platform/windows/display_helper_v2/state_machine.cpp
[B-RELEASE]: https://github.com/Nonary/Vibepollo/releases/tag/2.0.1
[B-REMOTE]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/remote_session.cpp
[B-REMOTE-DOC]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/docs/configuration.md
[B-RTSS]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/platform/windows/rtss_integration.cpp
[B-RTXHDR]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/platform/windows/rtx_hdr_policy.cpp
[B-RTXHDR-LINUX]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/docs/linux/rtx-hdr.md
[B-SMOOTH]: https://www.nvidia.com/en-us/geforce/news/nvidia-app-global-dlss-overrides-rtx-40-series-smooth-motion/
[B-SPEAKER]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/docs/controller_speaker_pcm.md
[B-STATE]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/state_storage_policy.cpp
[B-STATS]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/session_history.cpp
[B-STEAM]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/steam_integration.cpp
[B-STEAM-SYNC]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/steam_sync_policy.cpp
[B-STEAMOS]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/packaging/linux/steamos/AUDIT.md
[B-TOPOLOGY]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/remote_display_topology.cpp
[B-UI]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src_assets/common/assets/web/router.ts
[B-UPDATES]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/docs/api-only-update-contract.md
[B-WEBRTC]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/webrtc_stream.cpp
[B-WGC]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/platform/windows/display_wgc.cpp
[B-WGC-HELPER]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/tools/sunshine_wgc_capture.cpp
