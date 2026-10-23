# Validation strategy

The acceptance scenarios T01-T25 in the [implementation plan](../planning/05-implementation-plan.md) define the first release's required behavior. This guide explains when and how to run them.

## Evidence levels

| Level | What it establishes |
|---|---|
| Source inspection | A path or contract exists in a specified revision. |
| Unit/component checks | Behavior within the stated fixtures and mocks. |
| Native build/integration | The candidate builds and executes the checked native paths. |
| Clean Windows VM | Actual installer, service, configuration and available runtime behavior in that guest. |
| Physical streaming | The tested GPU, display, driver and client combination works in the recorded scenario. |
| Human acceptance | An explicitly observed user outcome, including perceptual checks where required. |

Never substitute one level for another. A VM inherits no hardware qualification merely because its host contains a particular GPU.

## Proportionate checks

| Change | Minimum useful checks |
|---|---|
| Documentation/templates | Links, schema/syntax and consistency; no native rebuild solely for prose. |
| Localized source behavior | Relevant regressions, formatting and affected build/tests. |
| Client protocol/permissions | Positive and denied operations, revocation and current client interoperability. |
| Session/display lifecycle | Shared-viewer rules, disconnect/resume, failed creation, recovery and topology restoration. |
| Driver/controller integration | Interface/package checks, real install/lifecycle and each advertised profile. |
| Installer/migration | Fresh install, supported predecessors, source preservation, failure recovery and uninstall/upgrade. |
| CI/dependency/shared build changes | Affected platforms and complete required pipelines; preserve existing coverage. |
| Preview/stable candidate | Exact-candidate CI and package checks, plus the hardware evidence required for claims and release scope. |

Use the simplest test that can detect the relevant defect. Preserve useful negative and concurrent cases. A required skipped or unexecuted case remains outstanding. Existing unrelated failures must be recorded without disguising regressions or bypassing protected checks.

## Native conventions

Follow AGENTS.md and the actual source revision's build tooling. Preserve the common GoogleTest header, platform guards, lockfiles, formatting and Doxygen requirements. Discover existing commands before inventing a parallel test runner.

## Campaign records

Record the candidate, scope, environment, exact OS/client/driver versions, requested and observed modes, executed checks, results and limitations. Include only information needed to reproduce the result.

Keep detailed operational output separate from concise public evidence. Use synthetic fixtures where possible. Remove credentials and unrelated data before attaching diagnostics.

For streaming, distinguish source refresh, rendered frames, encoded frames and client presentation. Evaluate HDR content as well as signaling. Establish measured baselines before claiming performance improvements.

One operator monitors each campaign. Prefer completion events and meaningful status changes. Diagnose failed checks before retrying; do not hide flakes through arbitrary waits or a pass-on-retry policy.
