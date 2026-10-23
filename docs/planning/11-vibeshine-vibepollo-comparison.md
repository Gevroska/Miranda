# Vibeshine versus Vibepollo

Observed: 2026-10-23, Europe/Paris. This comparison uses pinned source and Git history. No build, driver installation or streaming test was performed.

## Main finding

Vibeshine has a newer shared Sunshine foundation than Apollo and Vibepollo, while Miranda's current Sunshine foundation is newer still. The two Nonary forks carry many of the same streaming additions, but they are not interchangeable: Vibepollo retains Apollo extensions that are absent from the examined Vibeshine implementation.

The owner subsequently selected full functional coverage across these references as Miranda's overall goal (DEC-38). That selection does not make their experimental paths tested, remove the free-only requirement or automatically choose their underlying dependencies.

## Verified Sunshine ancestry

The comparison resolves each head against Miranda/Sunshine `d0b4a88ef727f8dabf80cc970a14739b55c885de` using Git's most recent common ancestor.

| Project | Examined head | Most recent shared Sunshine ancestor | Ancestor date, UTC/Paris |
|---|---|---|---|
| Apollo | `adc5c5a0bd80831ce495434bb16aee2cd4175fb8` | `1a96d135140ddfe5255516971c88f112b9da5beb` | 2025-09-26 |
| Vibepollo | `8a753af16dcf1d752781ef4e6350da6146553e55` | `1a96d135140ddfe5255516971c88f112b9da5beb` | 2025-09-26 |
| Vibeshine | `af1a0842048584af90308d86b2a12acb20c5ad1b` | `2db7176f73e8a6b0eb3fcb6696e28234beb56db9` | 2026-05-20 |
| Miranda | `d0b4a88ef727f8dabf80cc970a14739b55c885de` | Same as its head | 2026-10-21 |

These are upstream ancestry dates, not installer version numbers or the dates the forks were created. The May commit is dated May 19 in its original US timezone and May 20 in UTC/Paris. Sources: [September ancestor](https://github.com/LizardByte/Sunshine/commit/1a96d135140ddfe5255516971c88f112b9da5beb), [May ancestor](https://github.com/LizardByte/Sunshine/commit/2db7176f73e8a6b0eb3fcb6696e28234beb56db9), [Miranda head](https://github.com/Gevroska/Miranda/commit/d0b4a88ef727f8dabf80cc970a14739b55c885de).

Sunshine has 874 commits outside the Apollo/Vibepollo shared history and 490 outside Vibeshine's shared history. Those numbers include merges and maintenance; they do not count missing fixes or features. A fork can independently adapt or cherry-pick later changes without extending its common ancestry.

## Shared Nonary capabilities

Both forks document or implement the same broad groups: virtual displays and recovery, WGC helper capture, frame pacing and HDR controls, Remote Monitor/Remote Input, PyroWave and VRR-oriented streaming, Nonary gamepad integration, Playnite/Steam/Lutris integration, browser streaming, diagnostics and administration workflows. This establishes broad overlap, not identical behavior or measured performance. [B-README], [H-README]

Their examined master branches pin the same key dependencies:

| Component | Shared pin |
|---|---|
| libdisplaydevice | `18bdadbac2b21f35fe166aa1177db5c4e75829ae` |
| Nonary/libvirtualdisplay | `b2946d02a09bc453d9e6d287623390ca1dedf964` |
| Nonary/libvirtualgamepad | `7b000a2b060b4daeb0b5c6aa7b77ae1d81857e7a` |
| Nonary/moonlight-common-c | `4ea115e8d23d1ca055a2aa8ddcfb601b705b3323` |

The controller dependency is already selected for Miranda. The Nonary display dependency is separate from the owner's existing SudoVDA/VDD shortlist; functional scope does not silently select a third driver. Dependency locations are defined by the pinned module manifests. [B-MODULES], [H-MODULES]

## Functional differences relevant to Miranda

| Area | Vibepollo at the examined head | Vibeshine at the examined head | Development consequence |
|---|---|---|---|
| Paired-device permissions | Apollo permission bitmask, with separate viewing, launch, input and clipboard controls. | Device enabled/disabled state and separate specific controls such as sleep permission; no equivalent Apollo permission model in the examined certificate structure/UI. | Use Apollo/Vibepollo as the behavioral reference for Miranda's mandatory restricted guests. [B-CRYPTO], [B-DEVICES], [H-NVHTTP], [H-DEVICES] |
| Text clipboard | Authenticated GET/POST `/actions/clipboard` routes with separate read/set permissions. | Those routes are absent in master and the examined canary branch. | Preserve Miranda's selected bidirectional Artemis clipboard contract separately. [B-NVHTTP], [H-NVHTTP] |
| QR/one-time-code pairing | OTP generation endpoint `/api/otp` and `otpauth` pairing handling. | These endpoints/handling are absent in master and examined canary; standard PIN pairing remains. | Apollo-compatible QR/OTP pairing needs explicit implementation in Miranda. [B-API], [B-NVHTTP], [H-API], [H-NVHTTP] |
| Apollo server commands and client hooks | Server-command advertisement, Apollo permission fields and client do/undo settings retained. | Corresponding Apollo fields and per-client do/undo controls absent in the compared paths. | Maintain these as distinct functional requirements; they were deferred from the original v1 scope. [B-NVHTTP], [B-DEVICES], [H-NVHTTP], [H-DEVICES] |
| Artemis launch conveniences | Apollo launch-link and `.art` export code retained in the compared legacy app UI. | The legacy apps.html file containing that exporter is absent from the examined tree. | Miranda requires explicit shortcut acceptance tests. This finding alone is not an exhaustive claim about every alternative new-UI workflow. [B-APPS], [H-WEB] |
| Other Artemis-related fields | Virtual-display capability and UUID-oriented app/session handling. | Still has `VirtualDisplayCapable`, `currentgameuuid` and `appuuid` handling, plus Remote Input/Monitor functionality. | Absence of the extensions above does not establish that ordinary Artemis streaming is incompatible. Actual interoperability remains to test. [B-NVHTTP], [H-NVHTTP] |
| Recently added fixes/features | Includes controller-speaker PCM source/documentation at this head. | Missing from master at this head but present in the examined canary branch. | Some differences are branch timing rather than permanent project exclusions. Native playback remains unverified. [B-SPEAKER] |

Configuration overrides also differ structurally. Removing Apollo's dedicated per-app controller field does not prove the behavior is wholly unavailable: Vibeshine has broader configuration overrides. Map the actual effective setting before declaring a feature missing or copying a second override mechanism.

## Revision and maturity limits

Both projects reported stable release `2.0.1`, published 2026-10-14; the inspected master heads are newer. Identical release numbers do not imply identical code. [B-RELEASE], [H-RELEASE]

The default-branch trees differ in 976 paths, including 182 under `src/`, 123 under `src_assets/common/assets/web/` and 181 under `tests/`. These counts mix inherited upstream code, project identity, implementation, tests and maintenance. Their shared ancestor is `8e6915634a0724b47552924737bf94ad842b93e5`; the histories contain 2,086 Vibeshine-only and 2,528 Vibepollo-only commits relative to each other.

Vibeshine's examined canary branch is `abd0b8aa79fd4fcfe08838a91caa030898d5f788`, with the same May Sunshine ancestor. Master and canary have 3 and 62 unique commits respectively. This comparison used the branch's full reference, not the older tag also named `canary`.

Newer ancestry is a maintenance advantage to investigate, not proof of better stability, lower latency or complete upstream parity. Dependency-specific and experimental features remain subject to the limitations in [the 48-group inventory](09-vibepollo-feature-review.md).

## Recommendation for Miranda

Keep Miranda's newer Sunshine foundation. Use Vibeshine as an additional implementation reference for Nonary's capture, pacing, library and administration additions, and Apollo/Vibepollo for the detailed Artemis extensions and permission behavior. Compare the actual feature delta against the current Miranda baseline before adapting it; do not assume whole-fork merging is easier because one merge base is newer.

Full functional coverage means a unified implementation with documented capabilities and conflict resolution. Preserve explicit user choices: ordinary viewers join the active display without changing its mode, Remote Monitor is an explicit independent-screen role, and all delivered features work without paid licenses. Conflicting defaults, experimental paths and driver-bound features need deliberate integration and acceptance criteria.

The revision record and source paths are in [sources.json](sources.json); confirmed scope is in [the decision log](06-decisions-and-questions.md). No base switch or source port was performed during this analysis.

[B-API]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/confighttp.cpp
[B-APPS]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src_assets/common/assets/web/apps.html
[B-CRYPTO]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/crypto.h
[B-DEVICES]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src_assets/common/assets/web/views/DevicesView.vue
[B-MODULES]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/.gitmodules
[B-NVHTTP]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/src/nvhttp.cpp
[B-README]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/README.md
[B-RELEASE]: https://github.com/Nonary/Vibepollo/releases/tag/2.0.1
[B-SPEAKER]: https://github.com/Nonary/Vibepollo/blob/8a753af16dcf1d752781ef4e6350da6146553e55/docs/controller_speaker_pcm.md
[H-API]: https://github.com/Nonary/vibeshine/blob/af1a0842048584af90308d86b2a12acb20c5ad1b/src/confighttp.cpp
[H-WEB]: https://github.com/Nonary/vibeshine/tree/af1a0842048584af90308d86b2a12acb20c5ad1b/src_assets/common/assets/web
[H-DEVICES]: https://github.com/Nonary/vibeshine/blob/af1a0842048584af90308d86b2a12acb20c5ad1b/src_assets/common/assets/web/views/DevicesView.vue
[H-MODULES]: https://github.com/Nonary/vibeshine/blob/af1a0842048584af90308d86b2a12acb20c5ad1b/.gitmodules
[H-NVHTTP]: https://github.com/Nonary/vibeshine/blob/af1a0842048584af90308d86b2a12acb20c5ad1b/src/nvhttp.cpp
[H-README]: https://github.com/Nonary/vibeshine/blob/af1a0842048584af90308d86b2a12acb20c5ad1b/README.md
[H-RELEASE]: https://github.com/Nonary/vibeshine/releases/tag/2.0.1
