# Artemis and Moonlight compatibility contract

Updated: 2026-10-23. This is an observed source contract, not a runtime certification.

The latest available Artemis and Moonlight clients are required, including Moonlight on Fedora and Windows. Resolve and record exact release/build versions at the start of each validation campaign; the source snapshot below is analysis evidence, not a permanent version target. Record whether a selected build is a stable release or prerelease rather than silently assuming a channel.

The examined Android client is `ClassicOldSong/moonlight-android`, branch `moonlight-noir`, commit `c5cf27f4dc822db0e863c4691e7a70c74bea977a`. Apollo and this Artemis source both reference `moonlight-common-c` commit `c999436858471dfefa7617af3b7dc03ec1644ce4`; Miranda references a newer, different upstream revision. That is a dependency to reconcile, not a reason to replace Miranda's submodule wholesale. Moonlight's core pairing/streaming/input path is required; do not assume it exposes Artemis-only clipboard, Input Only or virtual-display request controls.

## Protocol surface

| Surface | Observed contract | Miranda handling |
|---|---|---|
| Basic discovery/pair/launch/resume | GameStream-compatible endpoints and existing encryption/certificate negotiation remain the base. | Preserve current Sunshine behavior and verify both Artemis and Moonlight. |
| Server capability response | Artemis reads `VirtualDisplayCapable`, `VirtualDisplayDriverReady`, `Permission`, `ServerCommand`, and `currentgameuuid`. | Advertise only implemented/available capabilities. Return permission data for the authenticated identity. |
| Virtual display launch | Artemis sends `virtualDisplay` and `scaleFactor` on launch/resume. | Honor requests plus global/per-app/per-client forcing and per-device resolution/rate overrides. A joining viewer preserves the current session. Define unsupported scaling behavior until D08 is selected. |
| Mode and refresh rate | `mode` contains width, height and an integer representation of FPS. Artemis multiplies non-integer FPS by 1,000; Apollo normalizes its launch rate into thousandths. | Introduce explicit, bounded conversion at the compatibility boundary. Do not pass `59940` through as ordinary 59,940 FPS. |
| Stream rate/Warp | Artemis can multiply the rate negotiated with its streaming core. Apollo separates display/encode/negotiated rates and adjusts bitrate handling. | Compare against Sunshine's `framerateX100` path. Test agreed modes; no claim that all Warp modes work until measured. |
| App list | Apollo adds `UUID` and `IDX` alongside numeric `ID`; Artemis uses them for identity and ordering. | First-release requirement: persistent custom ordering in the host UI and compatible ordering metadata. Keep numeric IDs and stable UUIDs; reordering must not change launch targets. |
| Current application | Artemis compares `currentgameuuid` and the numeric current-game ID when deciding launch/resume. | Maintain consistent identity through rename, reorder, migration, and reconnect. |
| UUID launch | Artemis can send `appuuid` alongside `appid`. | Resolve both consistently; do not authorize by an untrusted supplied UUID alone. |
| Remote Input | Artemis recognizes UUID `8CB5C136-DA67-4F99-B4A1-F9CD35005CF4` and avoids cancelling the current app when selecting it. | Preserve this wire-level identity even when branding changes. |
| Clipboard read | HTTPS `GET /actions/clipboard?type=text`. | Enforce read permission and return the expected text response. |
| Clipboard write | HTTPS `POST /actions/clipboard?type=text`, body `text/plain`. Artemis treats an empty successful response as success. | Preserve the client-visible contract; separately verify the Windows clipboard actually changed. |
| OTP pairing | Artemis's pairing code adds `otpauth` when a passphrase is supplied; Apollo verifies expiring pairing material. | First-release requirement: implement compatible QR/one-time-code pairing, including the host UI. Retain standard PIN pairing and the zero-access guest default. |
| Server commands | Server info exposes names; Artemis calls its native control path with a command index. Apollo uses packet type `0x3000`. | Deferred from the first release. Do not advertise commands or execute these requests. Preserve the wire definition for future compatibility. |
| Deep links/files | Artemis registers the `art` URL scheme and `.art` files. | First-release requirement: generate compatible application launch links/files using stable host/application IDs. Enforce existing permissions and session rules. |
| Host version | Artemis parses a four-part numerical protocol `appversion`. | Keep protocol version fields compatible. Miranda marketing version and branding are separate. |

Sources: [T-HTTP], [T-PAIR], [T-CONNECTION], [T-GAME], [T-MANIFEST], [A-NVHTTP], [A-RTSP], [A-PROCESS], [A-STREAM], [S-RTSP].

## Permission bits

These values are consumed by Artemis and should remain wire-compatible if Miranda implements the Apollo permission model. Internal names and storage may differ.

| Permission | Value |
|---|---|
| Controller input | `0x00000100` |
| Touch input | `0x00000200` |
| Pen input | `0x00000400` |
| Mouse input | `0x00000800` |
| Keyboard input | `0x00001000` |
| Set host clipboard | `0x00010000` |
| Read host clipboard | `0x00020000` |
| Server command | `0x00100000` |
| List apps | `0x01000000` |
| View stream | `0x02000000` |
| Launch apps | `0x04000000` |

The file-upload/download bits `0x00040000` and `0x00080000` are reserved by Apollo's model but do not establish working file transfer. Do not enable or advertise them as implemented capabilities. Apollo's action hierarchy makes higher-level permissions include related lower-level actions; reproduce the tested semantics rather than treating all bits as unrelated switches. [A-CRYPTO], [T-DETAILS]

Server commands are also deferred: retain knowledge of their permission bit but do not grant or advertise a functioning command feature in the first release.

Newly paired family/friend devices must start with no access until the owner grants permissions. Enforce this on the host for both client families, including clients that do not show Apollo's permission UI. Pairing is not itself a grant to list/launch applications, view, send input or access clipboard contents. Owner-device bootstrap remains an open policy detail.

## Shared video session contract

- A second authorized video client joins the existing application/display session. It does not create an independent desktop or replace the running application.
- Its requested resolution, refresh rate or HDR state must not change the active source display. Preserve the originating client's mode while the shared session remains active.
- Per-client stream parameters may differ only where the capture/encoder/client pipeline supports this without changing that source display. Unsupported combinations need explicit negotiation or failure; stable display mode does not guarantee arbitrary simultaneous SDR/HDR or codec combinations.
- Permissions remain attached to each paired identity. Joining/leaving one stream or an Input Only connection must not destroy another client's app/display. A joining client's saved display settings are retained for its future own session, not applied over the current one.
- Validate Artemis/Artemis, Moonlight/Moonlight and mixed-client joins, including disconnecting the originating viewer while another remains.

## Important adaptation points

1. Current Sunshine stores enabled paired identities and validates exact certificate identity. Extend that model rather than accepting a client-provided `uniqueid` as authority.
2. Current Sunshine's control message array has additional controller feedback types. Apollo's old internal array indices differ even where packet values match. Add named mappings and tests; do not copy numerical indices wholesale.
3. Apollo declares control types `0x3001` (clipboard) and `0x3002` (transfer nonce), but their examined handlers do not perform the advertised operation. Working text clipboard transport is HTTPS. Implementing placeholder packets is not necessary for the observed Artemis clipboard path.
4. Input Only must skip normal audio/video capture, retain the permitted control connection, and leave the main app/display alive. Audio/video configuration layout and shared-state assumptions must be rebuilt against current Sunshine.
5. Keep the upstream packet validation, certificate handling, encryption, and Web UI CSRF behavior when extending endpoints. A feature port must not revert newer protections.
6. The host cannot promise automatic background clipboard mirroring independently of Artemis and Android behavior. The initial requirement is successful bidirectional text transfer through the client's supported actions.
7. Per-application controller overrides must integrate the selected Nonary/libvirtualgamepad backend and preserve current client input/feedback interfaces and required free compatibility paths. A physical controller model does not guarantee the same emulated profile or all advanced features; see the [controller feasibility review](08-free-controller-feasibility.md).

## Required compatibility checks

- Standard PIN pairing, reconnect, certificate rejection, and client disable/re-enable.
- QR and one-time-code pairing with current Artemis: generated material opens the intended host's pairing flow; a valid code works, expired/invalid codes fail and a consumed code cannot establish another pairing. Verify the newly paired guest still has zero access until granted permissions.
- Artemis launch shortcuts: both generated links and `.art` files open the intended host/application. Rename/reorder/restart preserve the target identity; removed/unknown applications fail clearly without selecting a different app. Unpaired or unauthorized clients cannot bypass pairing/permissions through a shortcut.
- Shortcut launch/resume into an active session follows the same shared-session policy, preserving the current display mode. A shortcut requesting another application must not silently replace the session as a side effect of joining.
- Capability response with driver absent, ready, failed, and unavailable on another platform.
- Global/per-app/per-client virtual-display forcing works with Artemis and Moonlight under a documented precedence policy. Per-device resolution/rate overrides persist, validate supported/fractional modes and can be cleared to resume automatic selection. Neither setting reconfigures an existing session when joining.
- Per-client, per-app and per-client/app monitor identities remain stable across rename/reorder/restart. Joining viewers keep the current target regardless of their configured identity policy.
- Multiple resolutions/aspect ratios, custom valid modes, 60/120/165 Hz, 59.94/119.88-style fractional rates where supported, and explicit rejection of invalid/oversized rates. Display rate and encoded stream rate are checked separately.
- Launch/resume by both numeric ID and UUID; app rename and order changes do not select a different app.
- Custom order persists and Artemis receives matching ordering metadata. Moonlight clients retain correct app identities even if they choose their own display order.
- Authenticated Web UI app launch works without a streaming client, with a bounded fallback display mode when needed. Existing sessions are not silently replaced, and administrator access remains protected.
- Per-application controller inheritance and overrides work with the available Xbox One/Switch Pro hardware, including Auto, mixed clients, Input Only and restoration after application termination/failure. Verify each shipped profile without paid activation.
- Ordinary stream plus a second Input Only device; standalone Input Only; disconnecting either client; no stuck keyboard/gamepad state.
- New guest with zero access, then each granted/denied permission, including revocation during an active stream and each clipboard direction. Respect the documented permission hierarchy when constructing combinations.
- UTF-8/Windows Unicode clipboard conversion, empty text, non-ASCII text, large bounded text, and clipboard contention.
- Current Moonlight on Fedora and Windows: pairing, app list/launch with appropriate permissions, streaming, input, reconnect and joining an existing video session.
- Two video clients requesting different modes: active display resolution/rate/HDR remain unchanged and supported streams continue. Test join/leave and permission denial in mixed Artemis/Moonlight sessions.

No protocol exchange or installed Artemis session has been tested during this analysis.

[A-CRYPTO]: https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src/crypto.h
[A-NVHTTP]: https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src/nvhttp.cpp
[A-PROCESS]: https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src/process.cpp
[A-RTSP]: https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src/rtsp.cpp
[A-STREAM]: https://github.com/ClassicOldSong/Apollo/blob/adc5c5a0bd80831ce495434bb16aee2cd4175fb8/src/stream.cpp
[S-RTSP]: https://github.com/Gevroska/Miranda/blob/d0b4a88ef727f8dabf80cc970a14739b55c885de/src/rtsp.cpp
[T-CONNECTION]: https://github.com/ClassicOldSong/moonlight-android/blob/c5cf27f4dc822db0e863c4691e7a70c74bea977a/app/src/main/java/com/limelight/nvstream/NvConnection.java
[T-DETAILS]: https://github.com/ClassicOldSong/moonlight-android/blob/c5cf27f4dc822db0e863c4691e7a70c74bea977a/app/src/main/java/com/limelight/nvstream/http/ComputerDetails.java
[T-GAME]: https://github.com/ClassicOldSong/moonlight-android/blob/c5cf27f4dc822db0e863c4691e7a70c74bea977a/app/src/main/java/com/limelight/Game.java
[T-HTTP]: https://github.com/ClassicOldSong/moonlight-android/blob/c5cf27f4dc822db0e863c4691e7a70c74bea977a/app/src/main/java/com/limelight/nvstream/http/NvHTTP.java
[T-MANIFEST]: https://github.com/ClassicOldSong/moonlight-android/blob/c5cf27f4dc822db0e863c4691e7a70c74bea977a/app/src/main/AndroidManifest.xml
[T-PAIR]: https://github.com/ClassicOldSong/moonlight-android/blob/c5cf27f4dc822db0e863c4691e7a70c74bea977a/app/src/main/java/com/limelight/nvstream/http/PairingManager.java
