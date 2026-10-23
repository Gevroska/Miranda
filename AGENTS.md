# Miranda working instructions

## Read first

- Write responses, documentation, notes and comments in English.
- Read the planning index, the development guide and the implementation status. Then read only the scope, evidence and source sections relevant to the task.
- Product scope is recorded in docs/planning/06-decisions-and-questions.md. docs/development/STATUS.md records delivery evidence; it does not change requirements.
- Follow WORKFLOW.md, VALIDATION.md and RELEASING.md under docs/development.
- Use MCP Context7 for library documentation when useful. Verify version-sensitive behavior against primary documentation and the actual pinned dependency.
- Inspect repository state and applicable nested instructions before editing. Preserve unrelated work and upstream licenses/notices.

## Responsibility and coordination

The coordinating agent owns the result, including architecture, integration, validation and accurate reporting. A worker's completion message is evidence to inspect, not acceptance of its patch.

For substantial work, identify the observable outcome, scope, affected interfaces, risks and completion criteria first. Delegate coherent tasks when this improves focus, independent review or overall efficiency. Small tasks may remain with one agent.

Give each worker a bounded mission: candidate revision, owned paths, interfaces, neighboring work, invariants, acceptance cases, commands and expected report. Use fresh task-focused context for a new mission. Retain the same worker for related corrections; use a separate reviewer for significant or critical changes.

Use the models and reasoning levels assigned by the launching task. Explain material escalation by complexity, uncertainty or risk. Do not fill every available slot by default.

Avoid concurrent writes to the same paths. Use separate branches/worktrees for independent changes when appropriate. Assign one operator to any shared VM, driver installation, capture campaign, CI run or release operation. Do not overlap campaigns that share mutable hardware state.

Optimize the total work required for a verified result: context, implementation, tests, review and rework. Reuse relevant passing evidence when its inputs and scope still match. Diagnose failures before retrying; do not hide defects with repeated retries or relaxed assertions.

## Miranda invariants

- Keep the recent Sunshine foundation and preserve inherited platform support.
- Every delivered feature must work without a paid license or subscription.
- SudoVDA is the initial display backend. Compare VDD afterward; the selected Nonary controller driver is not a selection of another display driver.
- A joining video client must not replace the active application/display or change its mode.
- Newly paired guest devices receive no access until permissions are granted. Enforce rights on the host.
- Preserve native-display power behavior, user-selected virtual-display topology and recoverable cleanup.
- Preserve applications/settings during migration and report unsupported fields explicitly.
- Treat source/display lifetime, application lifetime and individual client connections separately.
- Deliver the defined first release before the later feature backlog.

## Implementation conventions

Before introducing a new abstraction, check modern Sunshine's existing implementation. Adapt behavior at clear interfaces; do not transplant an older subsystem wholesale without a present requirement. Record significant architecture decisions with their tradeoffs and validation plan.

For Windows native builds, use MSYS2 UCRT64 and build directories beginning with cmake-build-. The pinned baseline uses the launcher C:\msys64\msys2_shell.cmd with -defterm -here -no-start -ucrt64 -c. Verify that the toolchain exists before running it; the prepared documentation workspace is not yet a source checkout.

The native test executable is test_sunshine under the build directory's tests folder. Use tests/tests_common.h through the correct relative path, not direct GoogleTest includes. Preserve the shared test fixture and required include order. Group test includes, standard includes, library includes and local includes; omit empty groups and respect platform guards.

Add or update regressions for changed source behavior; target coverage of all changed behavior. Validate build-system edits with configuration/build/lint checks, without introducing dedicated build-system test projects merely to mirror the implementation.

Preserve the pinned Python toolchain and lockfile workflow. At the examined baseline, use uv sync --locked, refresh the lock only when dependency declarations require it, and run uv run --locked lb-update-clang-format after native edits. Inspect formatter output for unrelated changes. Reconcile these commands with the checked-out revision before use.

Follow .clang-format and Doxygen conventions. Document changed APIs with primary block comments and inline member documentation using ///<. Add localization only to en unless project scope explicitly requires another locale. Do not add decorative comment banners.

## Review and evidence

Critical permission, pairing, session, driver, installer, migration and shared-state changes require independent review of the actual diff. Reviewers report concrete triggers, impacts and corrections, not speculative finding lists. Review relevant test coverage and failure recovery.

After integration, validate the resulting candidate and account for changed dependencies or stale results. Keep source inspection, mocks, native execution, VM checks, physical streaming and human acceptance distinct. An unavailable GPU or client is an untested case, not a pass.

Use repository PR and issue templates. PRs explain the resulting behavior, relevant validation and material limitations. Pending CI is workflow status, not a code defect. Verify the current head and mergeability; an unknown mergeability state is not a confirmed conflict.

## Delivery and stopping

Follow applicable publication authority and repository protections. Do not bypass required reviews/checks, rewrite protected history, or manufacture passing statuses. A merge is not a verified release.

Record the delivered revision, relevant evidence, unresolved limitations and next action. Stop a completed task when its criteria are met. Ask about unresolved product tradeoffs or actions outside the mission; do not repeatedly seek approval for routine work already authorized.

Keep secrets, local operational state and unsanitized diagnostics out of repository changes and public artifacts. Inspect the actual diff and artifact contents before publication.
