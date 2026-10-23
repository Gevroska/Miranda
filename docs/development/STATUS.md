# Implementation status

Updated: 2026-10-23. Candidate branch: `codex/p0-baseline`, based on `d0b4a88ef727f8dabf80cc970a14739b55c885de`. Upstream functionality is not counted as a Miranda feature implementation.

| Work | State | Evidence / next action |
|---|---|---|
| Product scope and source review | Documented | [Planning index](../planning/README.md); no runtime qualification. |
| Contribution and development structure | Integrated in candidate | Guides and templates preserve upstream source/history/license; publication pending. |
| P0 source/build baseline | In progress | Source pin and recursive submodules established; Web UI build and 213 tests pass. Native and hosted CI results pending. See [P0 evidence](work-items/P0-baseline.md). |
| P1 identity/permissions | Planned | [Implementation plan](../planning/05-implementation-plan.md). |
| P2/P3 SudoVDA and displays | Prerequisites inspected; integration planned | A complete reference driver payload exists; source provenance, package distribution and clean-target qualification remain pending. |
| P4/P4a-P4c client/pairing/apps | Planned | Preserve Artemis/Moonlight contracts. |
| P5/P6 clipboard and Input Only | Planned | Host enforcement and lifecycle acceptance pending. |
| P6a/P10 controllers | Prerequisites inspected; integration planned | Nonary beta9 package contents/hashes checked; unsigned producer artifacts and runtime profile qualification remain release gates. |
| P7/P8 migration, packaging and release | Planned | Clean-VM and public preview acceptance pending. |
| P9 VDD comparison | Later comparison | Begins after a working SudoVDA baseline. |
| Remaining reference features | Later milestones | [Later roadmap](../planning/10-vibepollo-roadmap-and-questions.md). |

Use precise states: planned, in progress, implemented, reviewed, verified, merged or released. Report these separately when only some are established. Attach a revision and evidence when a state changes. Do not mark an entire feature complete because one subtask passed.
