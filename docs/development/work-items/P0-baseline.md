# P0: source and validation baseline

- Milestone: P0, before feature implementation.
- Requirements: DEC-01, DEC-02, DEC-40, DEC-41; acceptance T01 and preparation for T14.
- Base: `d0b4a88ef727f8dabf80cc970a14739b55c885de` on `codex/p0-baseline`.
- Scope: integrate the planning/contribution overlay, establish the pinned source and tooling, adapt inherited CI for validation, and record actual baseline failures.
- Interfaces: inherited build matrices, reusable workflows, package-generation tests and Web UI tests.
- Invariants: preserve source history, licenses, dependency pins and platform support; do not publish inherited upstream packages or claim Miranda features from upstream behavior.
- Exclusions: feature implementation, driver installation, service replacement and release publication.

## Acceptance

Establish an inspectable source candidate with its pinned submodules and Python lockfile intact. Run relevant local checks and qualify native/platform validation on the same candidate. Audit inherited publication destinations before any push. A missing platform, driver, VM or GPU result remains outstanding.

## Implementation and review

Main CI is adapted to validation with read-only repository access and no forwarded signing, registry or bot secrets. Upstream-specific publishers are guarded by their exact upstream repository identity. A local Docker workflow retains the inherited Bake graph without registry publication or Docker Hub description updates. Direct external action/workflow references are pinned to inspected commits. Copr and Cloudsmith service integrations remain inactive until Miranda destinations are qualified.

The native platform matrices remain present. Homebrew validation explicitly enables its existing test, fixture and coverage paths for validation rows; ordinary release-formula generation does not opt in. This preserves the actual repository identity rather than pretending to be upstream. Build artifacts retain inherited product identity during P0 and are validation outputs, not Miranda release packages. Signing, release automation, migration and artifact distribution require their own qualification. Some inherited transitive tooling and OS package sources remain mutable; direct pins alone do not make the build hermetic.

The Web UI documentation/shortcut regression originally failed with Windows CRLF checkouts while its source document matched the base after line-ending normalization. The test now accepts and explicitly checks both LF and CRLF while preserving its content and shortcut assertions. Independent review of the workflow/build/test diff is complete; subsequent hosted failures and their corrections receive separate review and affected validation.

## Validation

| Check / environment | Candidate | Result | Evidence / limitation |
|---|---|---|---|
| Source and submodules | Pinned base | Established | Source head matches the analyzed revision; recursive pinned checkouts initialized. |
| Python tooling | P0 candidate, unchanged `uv.lock` | Pass | `uv sync --locked --python 3.14`; CPython 3.14.8. |
| Web UI build | P0 candidate | Pass | Native Windows Node.js 24.22.1; `npm ci --ignore-scripts` and `npm run build`. |
| Web UI tests | P0 candidate with line-ending correction | Pass | `npm test`: 12 files, 213 tests. These are component tests, not client interoperability. |
| Workflow syntax/source | CI candidate | Pass; independently reviewed | actionlint 1.7.12 and yamllint 1.38.0; local actionlint excludes unavailable ShellCheck/Pyflakes subprocesses. No hosted execution claimed. |
| Homebrew formula | P0 candidate | Template checks pass | Ruby 4.0.7 syntax, CMake 4.4.4 ON/OFF/unset expansions and 12 repository/flag cases. No installed Homebrew runtime claimed. |
| Windows native configuration | P0 working candidate | Pass | MSYS2 UCRT64, GCC 16.2.0, CMake 4.4.4, static Qt 6.12.0; pinned FFmpeg asset checksum verified. Documentation and WiX packaging excluded locally; the installed .NET runtime has no SDK. |
| Native Windows build | `bd7edbd382e989b9e115b5b56b9725d25cccbd25` | Pass | UCRT64 RelWithDebInfo build with warnings as errors; executable dependency inspection found no missing DLLs. Documentation and WiX remain excluded locally. |
| Native Windows suite | `bd7edbd382e989b9e115b5b56b9725d25cccbd25` | Fail | 876 total: 863 passed, 11 skipped, 2 failed. Both failures are inherited httpbin download cases with CURL code 7 in this environment. Tests remain intact. Hardware/external-command skips are untested cases. |
| Hosted platform CI | `ced606d172a0910d6a8d5294f0e5db86244110e1` | Pass | All 26 main validation/build jobs, lint and CodeQL passed. Windows AMD64: 876 total, 864 passed, 12 skipped, zero failures. macOS x86: 766 total, 757 passed, 9 skipped, zero failures, including completed coverage generation. Guarded publication and notarization were skipped. |
| Local package assembly | Corrected P0 workflow | Pass | `npm pack --dry-run --json` checks local package contents without publication/version policy; 79 files including 76 built Web UI files. Package metadata/lockfile unchanged. |
| Clean VM and physical streaming | No candidate qualified | Not run | VM preparation pending; no GPU, HDR, controller or stream acceptance claimed. |

## Delivery

[PR #1](https://github.com/Gevroska/Miranda/pull/1) merged normally as `837f1fecb3187f985c570caa80e2b0f7a787d944` after independent review and exact-candidate hosted validation. Its tree matches the reviewed `ced606d172a0910d6a8d5294f0e5db86244110e1` candidate. The baseline work item is complete; no Miranda feature, preview package or stable release is delivered by it. [P1a](P1a-request-identity.md) adds request-bound identity; permission/display integration follows separately. DEC-44's explicit grants are not implemented by this baseline.
