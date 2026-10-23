# Architecture decision index

Product decisions remain in the [planning decision log](../planning/06-decisions-and-questions.md). Use architecture records for durable implementation choices, not routine edits or meeting transcripts.

| Record | Status | Scope |
|---|---|---|
| [ADR-001: connection-bound paired-client identity](decisions/001-connection-bound-identity.md) | Accepted | Authenticate each TLS connection once and revalidate its original pairing before HTTPS dispatch. |

Create records under docs/development/decisions/ using a sequential identifier and the [decision template](templates/DECISION.md). Link accepted/superseded records here. State alternatives, tradeoffs, relevant feature IDs and validation consequences. Supersede an accepted decision explicitly when its rationale changes.
