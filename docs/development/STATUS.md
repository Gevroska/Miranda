# Implementation status

Updated: 2026-10-23. Candidate: [draft PR #1](https://github.com/Gevroska/Miranda/pull/1), branch `codex/p0-baseline`, based on `d0b4a88ef727f8dabf80cc970a14739b55c885de`. Upstream functionality is not counted as a Miranda feature implementation.

| Work | State | Evidence / next action |
|---|---|---|
| Product scope and source review | Documented | [Planning index](../planning/README.md); no runtime qualification. |
| Contribution and development structure | Published for review; not merged | Guides and templates preserve upstream source/history/license; draft PR #1. |
| P0 source/build baseline | In progress | Local Windows native build passes. Native suite: 863 passed, 11 skipped and 2 external-download failures. Web UI build and 213 tests pass locally and hosted; remaining platform CI is under validation. See [P0 evidence](work-items/P0-baseline.md). |
| P1 identity/permissions | Identity foundation implemented and reviewed; not merged | [P1a](work-items/P1a-request-identity.md): native build and 49 focused regressions pass; full suite has the same two baseline external-download failures. Permission defaults/UI/enforcement and active revocation remain unimplemented. DEC-44/45 define explicit grants and launch-authorized global Stop. |
| P2/P3 SudoVDA and displays | Prerequisites inspected; integration planned | A complete reference driver payload exists; source provenance, package distribution and clean-target qualification remain pending. |
| P4/P4a-P4c client/pairing/apps | Planned | Preserve Artemis/Moonlight contracts. |
| P5/P6 clipboard and Input Only | Planned | Host enforcement and lifecycle acceptance pending. |
| P6a/P10 controllers | Prerequisites inspected; integration planned | Nonary beta9 package contents/hashes checked; unsigned producer artifacts and runtime profile qualification remain release gates. |
| P7/P8 migration, packaging and release | Planned | Clean-VM and public preview acceptance pending. |
| P9 VDD comparison | Later comparison | Begins after a working SudoVDA baseline. |
| Remaining reference features | Later milestones | [Later roadmap](../planning/10-vibepollo-roadmap-and-questions.md). |

Use precise states: planned, in progress, implemented, reviewed, verified, merged or released. Report these separately when only some are established. Attach a revision and evidence when a state changes. Do not mark an entire feature complete because one subtask passed.
