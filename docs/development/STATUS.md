# Implementation status

Updated: 2026-10-26. [P0 baseline PR #1](https://github.com/Gevroska/Miranda/pull/1), [P1a identity PR #2](https://github.com/Gevroska/Miranda/pull/2) and [P2a adapter PR #3](https://github.com/Gevroska/Miranda/pull/3) are merged. The integrated revision is `d172333b1f9072523b26fb660047e02676db3e20`, on the Sunshine foundation `d0b4a88ef727f8dabf80cc970a14739b55c885de`. Upstream functionality is not counted as a Miranda feature implementation. No release is qualified.

| Work | State | Evidence / next action |
|---|---|---|
| Product scope and source review | Documented | [Planning index](../planning/README.md); no runtime qualification. |
| Contribution and development structure | Merged | Guides and templates preserve upstream source/history/license; PR #1. |
| P0 source/build baseline | Reviewed, validated and merged | Local Windows native build passes; its two external-download failures remain recorded. Exact-candidate hosted validation, lint and CodeQL passed. See [P0 evidence](work-items/P0-baseline.md). |
| P1 identity/permissions | Identity foundation merged; permission enforcement reviewed and locally validated | [P1a](work-items/P1a-request-identity.md): native build and 49 focused regressions pass; exact-candidate hosted validation passed. [P1b](work-items/P1b-device-permissions.md): native build passes, 283 focused tests pass with one inherited licensed-driver skip, and Web validation passes. The full local suite retains only the two baseline external-download failures. Exact-candidate hosted validation and integration remain pending. DEC-44/45 define zero access for every new pairing and launch-authorized global Stop. |
| P1c application identities | Independently reviewed and locally validated; integration pending | [P1c](work-items/P1c-stable-application-identities.md): native build, 376 focused tests and 230 Web tests pass; one focused inherited licensed-driver case is skipped. The full local suite retains two baseline external-download failures. Final P1b ancestry and exact-candidate hosted validation remain pending. Shortcuts, display policy and installation migration remain separate work. |
| P2/P3 SudoVDA and displays | Inactive control foundation reviewed, validated and merged | [P2a](work-items/P2a-sudovda-adapter.md): native build and 87 combined focused regressions pass, including 38 fake-only driver cases; exact-candidate hosted validation passed. Production display integration, driver provenance/distribution and clean-target qualification remain pending. |
| P3a display preferences and identities | Independently reviewed and locally validated; integration pending | [P3a](work-items/P3a-display-policy.md): native build, 1,025 full-suite tests and 248 Web tests pass. Eleven inherited cases are skipped; the two baseline external-download failures remain. Final P1b ancestry and hosted validation remain pending. The resolver and driver acquisition remain inactive in production. |
| P4/P4a-P4c client/pairing/apps | Planned | Preserve Artemis/Moonlight contracts. |
| P5/P6 clipboard and Input Only | Planned | Host enforcement and lifecycle acceptance pending. |
| P6a/P10 controllers | Prerequisites inspected; integration planned | Nonary beta9 package contents/hashes checked; unsigned producer artifacts and runtime profile qualification remain release gates. |
| P7/P8 migration, packaging and release | Planned | Clean-VM and public preview acceptance pending. |
| P9 VDD comparison | Later comparison | Begins after a working SudoVDA baseline. |
| Remaining reference features | Later milestones | [Later roadmap](../planning/10-vibepollo-roadmap-and-questions.md). |

Use precise states: planned, in progress, implemented, reviewed, verified, merged or released. Report these separately when only some are established. Attach a revision and evidence when a state changes. Do not mark an entire feature complete because one subtask passed.
