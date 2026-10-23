# Apollo feature inventory and Miranda disposition

Updated: 2026-10-23. Source revisions and links are in [the evidence map](07-evidence-and-limits.md).

This inventory combines Apollo release history, its English UI/configuration surface, source paths, and matching Sunshine/Artemis code. A historical Apollo addition is not necessarily missing from current Sunshine.

DEC-38 selects full reference-project functionality as a long-term goal; DEC-39 preserves the focused first release. Disposition: **First release** = confirmed v1 scope; **Supporting work** = dependency of that scope; **Later target** = selected for a subsequent release; **Deferred** = explicitly outside v1, now a later target; **Preserve baseline** = retain current Sunshine behavior; **External/stub** = an external recipe or unimplemented placeholder, not proof of a completed host feature. Open behavioral questions are distinct from feature inclusion. Effort is relative engineering complexity, not a schedule estimate.

## Virtual displays and rendering

| ID | Capability | Apollo behavior and current Sunshine comparison | Miranda disposition / effort |
|---|---|---|---|
| D01 | Virtual display lifecycle | Apollo integrates SudoVDA creation/removal and driver health handling. Current Sunshine has display configuration but no equivalent built-in SudoVDA lifecycle in the examined source. Display lifetime can extend across a stream disconnect until the application session ends. [A-DISPLAY], [A-PROCESS] | **First release**, high. Implement SudoVDA first; compare only VDD afterward. Separate app/display lifetime from individual viewers. |
| D02 | Automatic client resolution and refresh rate | Apollo creates the requested mode. Sunshine already configures modes on existing displays; it does not provide the Apollo-created monitor. [A-PROCESS], [S-DISPLAY] | **First release**, high. Accept arbitrary supported modes; test multiple resolutions and 60/120/165 Hz. Joining viewers preserve the active display mode. |
| D03 | HDR on the virtual display | Apollo combines a capable driver with client HDR requests. Sunshine already has HDR streaming and display HDR policy. Driver support is the added dependency. [A-PROCESS], [S-DISPLAY] | **First release**, high. Preserve current encoding/color handling. |
| D04 | Persistent identity per client | Apollo uses the paired client's stored UUID for monitor identity, allowing Windows to remember layout/settings. [A-NVHTTP], [A-PROCESS] | **First release**, high. Identity must survive Miranda restart and reconnect. |
| D05 | Headless startup and encoder probing | Apollo creates temporary displays for probing and offers headless operation without a dummy plug. [A-PROCESS], [A-MAIN] | **First release / supporting work**, high. Test startup with zero active displays. |
| D06 | Display selection policy | Apollo accepts Artemis's virtual-display request and can force a virtual display globally, per app, or per client. [A-NVHTTP], [A-PROCESS], [A-CRYPTO] | **First release**, medium. Include all three forcing levels, including host controls for Moonlight devices. Define policy precedence and preserve an existing shared session. |
| D07 | Identity per application or per client/application pair | Apollo can use a dedicated app identity or a combined identity rather than only a client identity. [A-PROCESS] | **First release**, medium, explicitly selected by DEC-33. Offer these alongside per-client identity; retain IDs through app rename/reorder and keep the existing target when joining. |
| D08 | Render resolution scaling | A client or app can request a render size different from the encoded stream size; an app override takes precedence over a non-default client factor. This is not Windows DPI scaling. [A-PROCESS], [T-HTTP] | **Later target**, medium/high. Requires explicit coordinate/capture validation. |
| D09 | Per-client mode override | Apollo stores a width/height/refresh override, including fractional rates such as 59.94 Hz. Sunshine has global display mode remapping, not this paired-client field. [A-CRYPTO], [A-NVHTTP], [W-DISPLAY] | **First release**, medium. Host-side per-device resolution/rate controls alongside automatic selection. Persist and validate overrides; joining viewers cannot change the active display mode. |
| D10 | Double display refresh rate | The virtual display can run at twice the requested rate while the stream retains its intended rate. [A-PROCESS], [A-CONFIG] | **Later target**, medium/high. Benefit must be measured on target hardware. |
| D11 | Additional scaled modes and driver color options | SudoVDA supplies extra modes useful for scaling tools and exposes SDR/HDR bit-depth settings. These are driver capabilities, not additional stream-codec guarantees. [R-030], [V-SUDO] | **Later target**, driver-dependent. Do not promise 12-bit streaming from a 12-bit display option. |
| D12 | Primary-display/topology management and isolation | Sunshine already provides display activation, primary selection, restoration, and HDR policy. Apollo exposes configuration/activation/primary/only-display controls and additionally offers bottom-right placement to constrain pointer movement. [S-DISPLAY], [A-DISPLAY], [A-DISPLAY-OPTIONS] | Apollo-style topology controls **first release**: native capture keeps its screen on; virtual capture's physical-display behavior is user-selectable. Isolated placement is a **later target**. High interaction risk. |
| D13 | Preserve chosen capture display on resume | Apollo remembers its selected output instead of unexpectedly switching to primary; newer Sunshine also handles capture/device changes. Exact reconnect behavior needs comparison in hardware tests. [A-NVHTTP], [A-PROCESS], [S-NVHTTP] | **Supporting work**, medium/high. Test the outcome rather than transplanting old workarounds. |
| D14 | Select rendering GPU for the virtual monitor | Apollo passes the configured adapter to SudoVDA, especially useful for dual-GPU/headless hosts. [A-DISPLAY], [A-PROCESS] | **Supporting work**, high. Initial validation: Windows 11 25H2/RTX 3080; additional GPU hardware testing limited to recent NVIDIA models. Multi-adapter coverage depends on availability. |
| D15 | Fallback display mode | Apollo supplies a fallback when launch mode is absent, including Web UI launches. [A-NVHTTP], [A-CONFIG] | **Supporting work**, low/medium, for selected Web UI launch. Define a bounded fallback when no client mode is supplied; preserve an already active session's mode. |
| D16 | Ignore encoder probe failure | Apollo exposes an override allowing operation after failed initial probing. This is a troubleshooting bypass, not a reliability improvement by itself. [A-CONFIG], [A-MAIN] | **Later target**, after normal headless probing works; retain an explicit diagnostic control. |

## Client identity, pairing, and access

| ID | Capability | Apollo behavior and current Sunshine comparison | Miranda disposition / effort |
|---|---|---|---|
| A01 | Input permissions | Independent controller, touch, pen, mouse, and keyboard permissions, enforced in the host input path. Sunshine has global input settings but not these per-client permission bits. [A-CRYPTO], [A-INPUT] | **First release**, high. UI switches alone are insufficient. |
| A02 | Application/stream permissions | List apps, view streams, and launch/terminate apps; permissions have inclusion relationships. This is not a per-game allowlist or an OS sandbox. [A-CRYPTO], [A-NVHTTP] | **First release**, high. Guest app-specific restrictions are explicitly not required initially. |
| A03 | Manage and revoke active client access | Apollo updates active session permission state and disconnects when viewing rights are revoked. Sunshine already supports paired-client enable/disable and unpairing. [A-STREAM], [S-NVHTTP], [S-CONFIGHTTP] | **First release / supporting work**, high. Extend current identity and revocation behavior. |
| A04 | Restricted defaults for newly paired clients | Apollo gives the first paired client full permissions; later clients receive list/view only. [A-CRYPTO], [W-PERMISSIONS] | **First release**: new family/friend devices receive no access until explicitly granted. Owner-device bootstrap remains **open**; do not silently adopt Apollo's pairing-order defaults. |
| A05 | Disable new pairings | Apollo has a global pairing switch. This is different from disabling an already-paired client. [A-CONFIG], [A-NVHTTP] | **Later target**, low/medium. |
| A06 | OTP, QR pairing, and pairing deep links | Apollo generates expiring one-time pairing material; Artemis handles the extended pairing flow and `art://` links. Standard PIN pairing remains available. [A-NVHTTP], [A-PIN], [T-PAIR] | **First release**, medium/high. Include QR/one-time-code pairing and its UI; retain PIN pairing and zero-access guest defaults. |
| A07 | Dedicated Web UI login and remembered login | Apollo replaces browser Basic authentication with a login/cookie flow. Current Sunshine has its own authentication and CSRF handling. [A-CONFIGHTTP], [S-CONFIGHTTP] | **Later target**, high sensitivity. Keep current protections when adding UI features. |

## Interaction and automation

| ID | Capability | Apollo behavior and current Sunshine comparison | Miranda disposition / effort |
|---|---|---|---|
| C01 | Bidirectional text clipboard | Artemis uses HTTPS `GET`/`POST /actions/clipboard?type=text`; Apollo has separate read/set permissions and Windows Unicode clipboard implementation. Its Linux/macOS implementations are placeholders. [A-NVHTTP], [A-CLIPBOARD], [T-HTTP] | **First release on Windows**, medium/high. No images/files or full cross-platform parity implied. |
| C02 | Input Only / Remote Input | A device sends controls without receiving normal video/audio streams; it can coexist with another stream. There are dedicated launch/resume/termination semantics and a stable built-in app UUID. [A-PROCESS], [A-STREAM], [T-CONNECTION] | **First release**, high. Avoid terminating or reconfiguring the main stream. |
| C03 | Server commands from Artemis | The host publishes configured command names; Artemis sends a control-channel command index. Access is governed by a server-command permission. [A-NVHTTP], [A-STREAM], [T-GAME] | **Deferred** by user, separately from automatic session hooks. Do not advertise/execute the command extension in v1. |
| C04 | Per-client connect/disconnect commands | Host-configured commands run on individual client connection transitions; apps can opt out. [A-PROCESS], [A-CRYPTO], [W-CLIENT-COMMANDS] | **Deferred** by user. |
| C05 | Global/per-app resume/pause commands | Commands run when the first viewer arrives or last viewer leaves; game suspension itself requires extra scripts/tools. [A-PROCESS], [R-037] | **Deferred** by user. |
| C06 | Terminate application on last disconnect | Apollo can end an application when all clients disconnect. This is separate from optional pause commands and from Input Only session cleanup. [A-PROCESS] | User-facing app policy **deferred** with automation; Input Only cleanup remains **supporting work**. |
| C07 | Extra environment variables and compatibility mode | Apollo adds app/client UUIDs, state, scaling and floating-point FPS variables, while optionally preserving integer `SUNSHINE_CLIENT_FPS`. [A-PROCESS] | **Later target** for extended variable compatibility; lifecycle extensions remain outside v1. Importing or reporting affected settings is v1 work. Preserve existing Sunshine variables. |

## Applications and controls

| ID | Capability | Apollo behavior and current Sunshine comparison | Miranda disposition / effort |
|---|---|---|---|
| L01 | Persistent application UUID and UUID launch | Apollo exposes UUIDs alongside numeric app IDs and reports the current app UUID. Artemis uses these for resume and Remote Input. Sunshine's examined app flow uses numeric IDs. [A-NVHTTP], [A-PROCESS], [T-HTTP], [T-CONNECTION] | **Supporting work**, medium/high. Preserve numeric IDs for existing clients. |
| L02 | Manual application order | Apollo publishes `IDX`; its UI supports reorder/alphabetize. A legacy workaround uses invisible Unicode prefixes in names. [A-NVHTTP], [A-APPS], [A-ZWPAD] | **First release**, medium. Persist custom order and expose it to Artemis without changing identity. Legacy name rewriting is a later compatibility target if needed by a supported client; prefer explicit ordering metadata. |
| L03 | Launch applications from Web UI | Apollo adds host-side launch and can hand off launch to Artemis on the local device. Sunshine already has application management and a close endpoint, not this same launch flow. [A-CONFIGHTTP], [A-APPS], [S-CONFIGHTTP] | **First release**, medium. Use current Web UI authentication/CSRF protection; handle missing client mode and already-running apps explicitly. |
| L04 | App deep links and `.art` launcher export | Apollo creates Artemis launch links/files for shortcuts and frontends. [A-APPS], [T-MANIFEST] | **First release**, low/medium after stable host/app identities. Generate links/files and validate actual Artemis launch, permissions and shared-session behavior. |
| L05 | Per-app controller profile | Apollo offers global/default, disabled, Auto, Xbox 360 and DS4 overrides on Windows, applied for the application session and restored afterward. Current Sunshine has newer backends/profiles and global Auto selection. [A-APPS], [A-PROCESS], [S-CONFIG] | **First release**, medium/high. Use selected Nonary/libvirtualgamepad for modern profiles and retain required free compatibility paths. Test Xbox One/Switch Pro hardware on Android/Windows/Fedora. [The controller review](08-free-controller-feasibility.md) tracks integration/qualification; paid-only delivery is excluded by DEC-29. |
| L06 | Host-side rumble forwarding switch | Apollo adds a global forwarding control in its Windows gamepad path. Existing feedback capabilities must remain intact. [A-WININPUT], [S-STREAM] | **Later target**, low/medium; validate all active controller backends. |

## Capture, performance, audio, and administration

| ID | Capability | Apollo behavior and current Sunshine comparison | Miranda disposition / effort |
|---|---|---|---|
| V01 | Capture/encode frame limiting and pacing | Apollo separates encoding rate from negotiated rates and adds pacing changes. Current Sunshine has evolved timing/rate handling. [A-RTSP], [A-VIDEO], [S-VIDEO] | Functional target is **supporting work**; old algorithm is **not selected**, high. |
| V02 | Artemis fractional rates and Warp compatibility | Artemis can encode fractional rates in thousandths and multiply the negotiated rate for Warp. Apollo decodes these and adjusts rate/bitrate handling. Sunshine has a different fractional-rate path. [A-RTSP], [T-HTTP], [T-GAME], [S-RTSP] | **Supporting compatibility analysis**, high. Warp is a **later target**; exact modes and hardware coverage to certify remain open. |
| V03 | Automatic minimum FPS target | The auto target is present in current Sunshine too. [S-VIDEO] | **Preserve baseline**; no separate port required for the documented auto behavior. |
| V04 | WGC capture above 60 FPS | Both branches set `MinUpdateInterval`, with different values/policies. [A-WGC], [S-WGC] | **Preserve baseline and validate**. No automatic transplant of Apollo's historical fix. |
| V05 | NVENC intra refresh | Current Sunshine implements client-requested intra refresh. Apollo also exposes an `nvenc_intra_refresh` config member, but the examined active encoder path uses the client request; a working independent UI override was not established. [A-NVENC], [S-NVENC] | **Preserve baseline**; do not count the Apollo setting as proven additional behavior. |
| V06 | GPU scheduling priority experiment | Apollo's alpha changes GPU thread priority to `0x4000001E` with fallback. Sunshine uses its existing priority policy. [A-WINCAPTURE], [S-WINCAPTURE], [R-047] | **Later experimental target**, subject to measurement and fallback. No guaranteed latency/stutter improvement or commitment to copy the same scheduling constant. |
| V07 | Audio endpoint policy options | Apollo exposes automatic recapture and virtual-sink-default switches. Sunshine already handles endpoint changes and has newer audio code; added controls and behavior need separate mapping. [A-AUDIO], [S-AUDIO] | **Later target**, medium. Headless audio reliability remains an acceptance requirement. |
| V08 | Preserve previous log on restart | Apollo adds a `.backup` log; current Sunshine already has log rotation. [A-LOGGING], [S-LOGGING] | **Preserve baseline**. |
| V09 | Tray/admin conveniences and discovery switch | Apollo adds tray information/control changes, hidden tray controls, Web UI quit, and an mDNS enable switch. These are separate from streaming features; some administration already exists upstream. [A-TRAY], [A-CONFIGHTTP], [A-MAIN] | **Later target**, low/medium. Evaluate each option separately. |

## External tools, placeholders, and scope traps

| ID | Item | Finding | Disposition |
|---|---|---|---|
| E01 | Apollo Profile Manager | Separate tool for per-app/per-client settings, mods, or save files; not implemented inside Apollo's host. [R-046] | **External / later target** for the documented integration outcome; define application/ownership boundaries. |
| E02 | Automatic game suspension and game FPS limits | Recipes depend on scripts and tools such as process suspension utilities, Special K, or RTSS. Hooks and game-level behavior are different deliverables. [W-PAUSE], [W-BEST] | **External / deferred**. |
| E03 | Capture only one app's audio | Apollo's wiki describes a setup recipe using Windows audio routing and commands; it does not establish a native app-isolated audio subsystem. [W-APP-AUDIO] | **External / later target** for the documented outcome; a native implementation needs separate design and testing. |
| E04 | File transfer | Permission bits and packet identifiers exist, but the examined transfer handler is a placeholder and no complete transfer endpoints were found. The control-channel clipboard placeholder is also distinct from working HTTPS text clipboard access. [A-CRYPTO], [A-STREAM] | **Stub**, not an Apollo parity requirement. |
| E05 | Separate concurrent Windows desktops | Multiple clients, monitor identities, or multiple processes do not provide separate Windows user sessions or isolated concurrent games. Apollo's main process manager tracks one running app session. [A-PROCESS] | **Not established**, separate project scope. |
| E06 | Client microphone return | No completed microphone-return feature was identified in the compared Apollo additions. Audio output streaming is not microphone forwarding. | **Not established**, not selected. |
| E07 | Native non-Windows virtual displays and clipboard | Apollo describes Linux virtual displays as planned; Linux/macOS clipboard functions in the inspected host source are placeholders. [A-LINUX-CLIPBOARD], [A-MAC-CLIPBOARD] | **Not implemented there**. The later Nonary Linux-display implementations are separately inventoried in B46-B48; they do not prove cross-platform clipboard support. |

## Miranda-specific enabling work

| ID | Item | Requirement | Disposition |
|---|---|---|---|
| N01 | Apollo configuration migration | Import apps and compatible settings, report unmapped/deferred fields, allow fresh pairing, and protect originals. | **First release**, high. |
| N02 | Miranda packaging and identity | Define executable/service/install/config/firewall/update identities; preserve protocol identifiers required by Artemis. | **Supporting work**, medium/high. Public previews then stable; clean-VM testing first and Apollo-like installation transition. Exact migration/service/driver ownership remains engineering work. |
| N03 | Reliable build/release validation | Keep modern Sunshine tooling and test coverage, validate packaging and real streaming separately, and preserve other platforms. Apollo has no test-file delta against the common ancestor in the compared range. | **Supporting work**, high across the project. |

## Selection summary

Confirmed capabilities: D01-D07, D09, D12 topology controls (isolation later), A01-A04, A06, C01-C02, L02-L05, N01, with the supporting client identity/protocol and release work required to deliver them. These include QR/one-time-code pairing, Artemis shortcuts, custom app ordering, Web UI launch, per-app controller overrides, virtual-display forcing at all three levels, app/client-app identities and per-client mode overrides. D15 is supporting work for Web UI launch; C03 manual commands are deferred. Latest Artemis and Moonlight support, including Fedora/Windows Moonlight, is required. A second video viewer joins the existing session without changing its display mode; newly paired guests have no access until granted. SudoVDA is the initial display driver, VDD the sole display-driver comparison alternative. Nonary/libvirtualgamepad is the selected modern-controller driver. Other real reference capabilities are later targets under DEC-38/DEC-39. Open implementation details and unimplemented upstream placeholders are not equivalent to delivered features.

Shared-viewing policy and default guest permissions are Miranda requirements, not claims that every Apollo default should be copied. Validate them with T07/T17 in the [implementation plan](05-implementation-plan.md).

DEC-29 applies across the inventory: all shipped features must work free of charge. DEC-38 selects full functional coverage over time; DEC-39 keeps later targets outside the focused v1. Preserve useful upstream interfaces without inheriting paid-only delivery paths, and qualify actual capabilities before declaring them delivered.

[W-APP-AUDIO]: https://github.com/ClassicOldSong/Apollo/wiki/Stream-audio-from-only-one-app
[A-LINUX-CLIPBOARD]: https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src/platform/linux/misc.cpp
[A-MAC-CLIPBOARD]: https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src/platform/macos/misc.mm

[A-APPS]: https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src_assets/common/assets/web/apps.html
[A-AUDIO]: https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src/audio.cpp
[A-CLIPBOARD]: https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src/platform/windows/misc.cpp
[A-CONFIG]: https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src/config.cpp
[A-CONFIGHTTP]: https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src/confighttp.cpp
[A-CRYPTO]: https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src/crypto.h
[A-DISPLAY]: https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src/platform/windows/virtual_display.cpp
[A-DISPLAY-OPTIONS]: https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src_assets/common/assets/web/configs/tabs/audiovideo/DisplayDeviceOptions.vue
[A-INPUT]: https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src/input.cpp
[A-LOGGING]: https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src/logging.cpp
[A-MAIN]: https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src/main.cpp
[A-NVENC]: https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src/nvenc/nvenc_base.cpp
[A-NVHTTP]: https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src/nvhttp.cpp
[A-PIN]: https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src_assets/common/assets/web/pin.html
[A-PROCESS]: https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src/process.cpp
[A-RTSP]: https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src/rtsp.cpp
[A-STREAM]: https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src/stream.cpp
[A-TRAY]: https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src/system_tray.cpp
[A-VIDEO]: https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src/video.cpp
[A-WGC]: https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src/platform/windows/display_wgc.cpp
[A-WINCAPTURE]: https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src/platform/windows/display_base.cpp
[A-WININPUT]: https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src/platform/windows/input.cpp
[A-ZWPAD]: https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src/zwpad.h
[R-030]: https://github.com/ClassicOldSong/Apollo/releases/tag/v0.3.0
[R-037]: https://github.com/ClassicOldSong/Apollo/releases/tag/v0.3.7-hotfix.1
[R-046]: https://github.com/ClassicOldSong/Apollo/releases/tag/v0.4.6
[R-047]: https://github.com/ClassicOldSong/Apollo/releases/tag/v0.4.7-alpha.1
[S-AUDIO]: https://github.com/Gevroska/Miranda/blob/d0b4a88ef727f8dabf80cc970a14739b55c885de/src/platform/windows/audio.cpp
[S-CONFIG]: https://github.com/Gevroska/Miranda/blob/d0b4a88ef727f8dabf80cc970a14739b55c885de/src/config.h
[S-CONFIGHTTP]: https://github.com/Gevroska/Miranda/blob/d0b4a88ef727f8dabf80cc970a14739b55c885de/src/confighttp.cpp
[S-DISPLAY]: https://github.com/Gevroska/Miranda/blob/d0b4a88ef727f8dabf80cc970a14739b55c885de/src/display_device.cpp
[S-LOGGING]: https://github.com/Gevroska/Miranda/blob/d0b4a88ef727f8dabf80cc970a14739b55c885de/src/logging.cpp
[S-NVENC]: https://github.com/Gevroska/Miranda/blob/d0b4a88ef727f8dabf80cc970a14739b55c885de/src/nvenc/nvenc_base.cpp
[S-NVHTTP]: https://github.com/Gevroska/Miranda/blob/d0b4a88ef727f8dabf80cc970a14739b55c885de/src/nvhttp.cpp
[S-RTSP]: https://github.com/Gevroska/Miranda/blob/d0b4a88ef727f8dabf80cc970a14739b55c885de/src/rtsp.cpp
[S-STREAM]: https://github.com/Gevroska/Miranda/blob/d0b4a88ef727f8dabf80cc970a14739b55c885de/src/stream.cpp
[S-VIDEO]: https://github.com/Gevroska/Miranda/blob/d0b4a88ef727f8dabf80cc970a14739b55c885de/src/video.cpp
[S-WGC]: https://github.com/Gevroska/Miranda/blob/d0b4a88ef727f8dabf80cc970a14739b55c885de/src/platform/windows/display_wgc.cpp
[S-WINCAPTURE]: https://github.com/Gevroska/Miranda/blob/d0b4a88ef727f8dabf80cc970a14739b55c885de/src/platform/windows/display_base.cpp
[T-CONNECTION]: https://github.com/ClassicOldSong/moonlight-android/blob/c5cf27f4dc822db0e863c4691e7a70c74bea977a/app/src/main/java/com/limelight/nvstream/NvConnection.java
[T-GAME]: https://github.com/ClassicOldSong/moonlight-android/blob/c5cf27f4dc822db0e863c4691e7a70c74bea977a/app/src/main/java/com/limelight/Game.java
[T-HTTP]: https://github.com/ClassicOldSong/moonlight-android/blob/c5cf27f4dc822db0e863c4691e7a70c74bea977a/app/src/main/java/com/limelight/nvstream/http/NvHTTP.java
[T-MANIFEST]: https://github.com/ClassicOldSong/moonlight-android/blob/c5cf27f4dc822db0e863c4691e7a70c74bea977a/app/src/main/AndroidManifest.xml
[T-PAIR]: https://github.com/ClassicOldSong/moonlight-android/blob/c5cf27f4dc822db0e863c4691e7a70c74bea977a/app/src/main/java/com/limelight/nvstream/http/PairingManager.java
[V-SUDO]: https://github.com/SudoMaker/SudoVDA/blob/a4b09fa2aa731a964d0cb5d139cb1e6240e4da12/README.md
[W-BEST]: https://github.com/ClassicOldSong/Apollo/wiki/Best-Practices
[W-CLIENT-COMMANDS]: https://github.com/ClassicOldSong/Apollo/wiki/Client-Commands
[W-DISPLAY]: https://github.com/ClassicOldSong/Apollo/wiki/Display-Mode-Override
[W-PAUSE]: https://github.com/ClassicOldSong/Apollo/wiki/Auto-pause-resume-games
[W-PERMISSIONS]: https://github.com/ClassicOldSong/Apollo/wiki/Permission-System
