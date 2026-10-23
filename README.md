# Miranda

Miranda is a Sunshine-based streaming host for Artemis and Moonlight. It keeps a recent Sunshine foundation while adding integrated virtual displays, device permissions and other streaming capabilities.

**Development status:** the source/build baseline is in progress. The Miranda-specific functionality described below is planned; it has not yet been implemented or qualified. See the [implementation status](docs/development/STATUS.md) for actual validation and delivery evidence.

## First milestone

- Windows virtual displays through SudoVDA, including automatic modes, HDR and persistent monitor identities.
- Artemis and standard Moonlight interoperability.
- Per-device permissions, bidirectional text clipboard and Input Only sessions.
- QR/one-time-code pairing, Artemis launch shortcuts and application controls.
- Free modern-controller support through Nonary/libvirtualgamepad.
- Application/settings migration and public preview packages.

A second video viewer will join the active session without changing its display mode. Remote Monitor and the remaining reference-project capabilities follow in later milestones. Every shipped Miranda feature must be usable without a paid license or subscription.

## Documentation

- [Project scope and source analysis](docs/planning/README.md)
- [Development guide](docs/development/README.md)
- [Implementation status](docs/development/STATUS.md)
- [Validation strategy](docs/development/VALIDATION.md)
- [Release process](docs/development/RELEASING.md)
- [Contributing](CONTRIBUTING.md)

Public previews will precede a stable release. Installation instructions and download links will be added when a Miranda package has been built and verified. Planned support must not be interpreted as a tested platform or hardware matrix.

## Project foundations

Miranda builds on [Sunshine](https://github.com/LizardByte/Sunshine). [Apollo](https://github.com/ClassicOldSong/Apollo), [Vibepollo](https://github.com/Nonary/Vibepollo) and [Vibeshine](https://github.com/Nonary/vibeshine) are functional and implementation references.

Retain Sunshine's license, notices and contributor history, and the licenses of incorporated components. Dependency-specific distribution requirements remain part of release qualification. The existing repository's [license](https://github.com/Gevroska/Miranda/blob/master/LICENSE) governs its inherited source.
