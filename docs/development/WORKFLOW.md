# Development workflow

## A bounded change

Define one observable outcome and its acceptance criteria. Record relevant feature/decision IDs, affected interfaces and risks in a work item or PR. Start with source evidence and the current repository state.

Select a pinned base and use a focused branch such as codex/display-lifecycle. Preserve unrelated work. Independent work may use separate worktrees; tightly coupled edits need a single owner or an agreed interface.

## Implementation and review

The coordinator remains accountable for architecture and integration. Delegate when a task benefits from separate attention, an independent review or isolated execution. Every worker receives an explicit scope and returns changed paths, checks, results and unresolved issues.

A reviewer examines the actual candidate diff and relevant behavior. Significant permission, pairing, session, driver, installer, migration and CI changes need an independent reading. Do not equate a review by the author with independent review, or a local agent review with a GitHub approval by a different account.

After corrections, renew the affected review and checks. Resolve conflicts and incompatible assumptions before integrating. Do not broaden a task into a framework rewrite when a targeted change delivers the required behavior.

## Failure handling

Classify failures as product defects, test defects, environment/tool failures, baseline failures or missing evidence. Diagnose before retrying. Verify a correction with the smallest meaningful check, then resume the relevant campaign.

One operator owns each shared VM, installed driver, active stream or release campaign. Other workers can analyze source and prepare isolated changes without modifying that environment. Keep handoffs explicit when ownership changes.

## Pull requests and merges

Use the PR template. Summarize behavior, validation and material limitations without dumping internal coordination history. Verify the current head, required checks, review state and actual mergeability.

Respect repository protections. Never bypass a required review or CI check, force-push protected history, or mark missing validation as passed. If external review or another permission is genuinely required, report the exact outstanding condition.

A merge completes integration only. Update the status with the merged revision and evidence. Publication follows the separate release criteria.

## Maintenance

Periodically compare selected Sunshine revisions with the Miranda baseline. Record the chosen update, compatibility impact and validation. Preserve inherited platform support and avoid reintroducing obsolete fork-specific workarounds.

Use current relevant evidence. Re-run checks when code, dependencies, configuration or coverage changes invalidate it; do not repeat a passing campaign only to reformat its report. Measure efficiency through completed verified work, including rework, rather than activity or agent count.

Stop a work item once its criteria are met. Record the next coherent item and any genuine blocker. An unanswered implementation detail is not automatically a reason to reopen confirmed product scope.
