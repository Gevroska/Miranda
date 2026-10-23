# Later-release roadmap and review questions

Updated: 2026-10-23. Applies to the [Vibepollo inventory](09-vibepollo-feature-review.md), with [Vibeshine comparison](11-vibeshine-vibepollo-comparison.md). It does not expand the next-release work plan.

## Confirmed direction

- DEC-38: full functional coverage of Apollo, Vibepollo and Vibeshine is the long-term target, superseding undecided feature inclusion.
- DEC-39: preserve the focused first release and deliver the remaining features afterward.
- DEC-35: prioritize capture quality, frame pacing and HDR for the later work.
- DEC-36: Nonary's Moonlight fork may be an optional additional client. Artemis and standard Moonlight remain supported for ordinary streaming.
- DEC-37: add Remote Monitor as an explicit optional future mode. Normal viewers continue joining the current stream; remote screens extend the same Windows desktop.
- DEC-29/DEC-30 still apply: all shipped features free; Nonary/libvirtualgamepad already selected for v1. This is not authorization to introduce Nonary's display driver or paid applications.

## Open delivery and behavior decisions

Do not ask whether to include each feature again. The owner has selected full coverage; resolve implementation order, optional modes, compatibility and acceptance instead.

| Question | Choices to resolve | Relevant inventory |
|---|---|---|
| VQ01 | Which capture/pacing changes should lead the first later milestone: WGC/recovery, game frame caps or capture of game-generated frames? | B11-B15 |
| VQ02 | After native HDR correctness, in what order should SDR-to-HDR conversion, per-device calibration profiles and 10-bit SDR controls ship? Define defaults and per-app/client precedence. | B04, B18-B19 |
| VQ03 | How should experimental VRR/PyroWave be exposed and qualified? Record host/client link types, speeds and compatible client hardware. | B20-B22 |
| VQ04 | If a selected feature depends on Nonary/libvirtualdisplay, investigate adaptation to SudoVDA first or explicitly reopen the display-driver shortlist? The existing two-driver decision remains in force until answered. | B05, B13-B14, B20 |
| VQ05 | Which remote-screen count, layouts, audio policy, retention and guest authority are needed? Remote Monitor itself is selected; these details are not. | B06-B10 |
| VQ06 | Which libraries should be tested first? Record Playnite version, Steam/Lutris usage, sync ownership, duplicate handling and manual-app preservation. All three remain long-term targets. | B25-B28 |
| VQ07 | Browser streaming: administrative convenience or guest access too? Define supported browsers and whether remote network access is intended. | B34 |
| VQ08 | Set rollout order, defaults, retention and authorization for API tokens, graphs, history, log bundles, recovery and update notices. | B36-B45 |
| VQ09 | Is future DualSense hardware/client testing available for waveform haptics and speaker audio? The declared Xbox One/Switch Pro hardware does not cover these outcomes. | B31-B33 |
| VQ10 | Schedule native Linux-host and SteamOS as later platform milestones; identify available hardware/distributions and maintainers. Fedora client support is already required and does not establish host coverage. | B46-B48 |

The [authoritative decision log](06-decisions-and-questions.md) tracks the remaining Apollo delivery/behavior questions. Owner bootstrap, migration and release policy still matter for v1; previously unselected additional controls now belong to the later full-coverage backlog.

## Proposed engineering sequence

| Stage | Work | Evidence needed before delivery |
|---|---|---|
| F0: source qualification | Pin the implementation, corresponding driver/client revisions and licenses. Compare with the Miranda baseline at implementation time. Identify free replacements and driver adaptation work. | Feature-specific source delta, compatibility contract and dependency record; no wholesale rollback to an older base. |
| F1: capture/pacing/color | Establish a baseline and adapt WGC/pacing/HDR capabilities as separate changes. | Repeatable native/virtual capture runs at 60/120/165 Hz and other requested valid modes; frame-time distributions, GPU load, image checks and crash/reconnect restoration. |
| F2: optional client experiments | Add VRR/PyroWave negotiation behind explicit capabilities and experimental controls. | Exact host/client/bitstream compatibility, realistic bandwidth/loss tests and preserved standard H.264/HEVC/AV1 paths where hardware supports them. No AV1-encoding promise for the RTX 3080. |
| F3: Remote Monitor | Add independent role/target ownership, layouts and lifecycle to the shared-session architecture. | Main stream plus extra display(s), mixed clients, input routing, permissions, owner departure, app termination, retained resume and topology recovery. |
| F4: libraries, automation and administration | Integrate library/UI/API/diagnostic additions plus deferred Apollo hooks, commands and controls as separate work packages. | Stable app IDs, duplicate/exclusion/removal rules, authenticated operations, bounded resources, command ownership and upgrade/migration behavior. |
| F5: additional hardware/platforms | Qualify modern feedback, multi-GPU, AMD/Intel, Linux-host and SteamOS capabilities. | Actual devices and OS/compositor/client versions. Unsupported hardware remains unverified, not implicitly certified. |

These stages are a proposed order, not a schedule or a requirement to finish every stage before a later release. The long-term feature goal is settled; concrete milestones still need technical decomposition, dependency qualification and acceptance evidence.

## Acceptance principles

- Preserve the v1 Artemis/Moonlight and guest-permission tests through every later change.
- Test source-display mode separately from render, capture, encoded and presented frame rates. Record which quantity a graph reports.
- Capture native HDR and test SDR-to-HDR conversion as separate cases. Check image content, not only HDR metadata or a successful connection.
- Restore frame caps, NVIDIA profiles, audio defaults and display topology without overwriting newer user choices or another session's active settings.
- Compare perceptible improvements with recorded baselines; do not copy README latency or stability claims into Miranda release notes as measured results.
- Keep source tests, native package tests and physical acceptance separate, especially for experimental capture, sleep and controller audio paths.
- An optional client is permitted; silently making it mandatory for ordinary streaming is not.
