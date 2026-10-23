# Development guide

Miranda development proceeds in focused, testable changes on a recent Sunshine foundation. The initial release scope is already defined; remaining reference-project features follow later.

## Read in this order

1. [Planning index](../planning/README.md) and the relevant feature/decision IDs.
2. [Current status](STATUS.md).
3. [Workflow](WORKFLOW.md).
4. [Validation](VALIDATION.md) and, for publication work, [releasing](RELEASING.md).
5. [Architecture decisions](DECISIONS.md) and the relevant source.

## Document ownership

| Record | Canonical content |
|---|---|
| Planning decision log | Product choices and scope changes. |
| Feature inventories | Reference behavior, dependencies and intended milestone. |
| STATUS.md | Delivered state, candidate and supporting evidence. |
| Architecture decision records | Durable technical decisions and tradeoffs. |
| Work item / PR | Bounded change, implementation outcome and actual checks. |
| Release notes | User-visible changes, package requirements and limitations. |

Do not maintain duplicate task ledgers in multiple documents. Link to existing evidence instead of copying command transcripts. Replace obsolete current-state descriptions; preserve useful historical decisions with explicit supersession.

Templates: [work item](templates/WORK_ITEM.md), [architecture decision](templates/DECISION.md), [release notes](templates/RELEASE_NOTES.md).

## Initial development sequence

Establish the source/build baseline and current CI behavior. Qualify the selected driver interfaces and packages early. Design client identity, permissions and session ownership before integrating overlapping display/control features. Follow the dependencies in the [implementation plan](../planning/05-implementation-plan.md).

The source is pinned to the analyzed Sunshine foundation, with the planning and contribution documents integrated into the development candidate. The [P0 work item](work-items/P0-baseline.md) records the baseline scope and evidence. Miranda feature implementation and release qualification remain separate milestones. Preserve upstream source, history, licensing and valid build conventions.
