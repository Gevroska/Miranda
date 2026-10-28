# Architecture decision index

Product decisions remain in the [planning decision log](../planning/06-decisions-and-questions.md). Use architecture records for durable implementation choices, not routine edits or meeting transcripts.

| Record | Status | Scope |
|---|---|---|
| [ADR-001: connection-bound paired-client identity](decisions/001-connection-bound-identity.md) | Accepted | Authenticate each TLS connection once and revalidate its original pairing before HTTPS dispatch. |
| [ADR-002: host-enforced device permissions](decisions/002-device-permission-enforcement.md) | Accepted | Persist explicit grants before publishing authority, and reconcile each device's sessions and input without disturbing other viewers. |
| [ADR-003: stable application identities](decisions/003-stable-application-identities.md) | Accepted | Durable UUID/numeric pairs, typed catalog transactions and bounded selector compatibility. |
| [ADR-004: immutable display policy](decisions/004-immutable-display-policy.md) | Proposed; implemented candidate | Typed saved preferences and versioned monitor identities resolved only for fresh video sources; production lifecycle integration remains separate. |
| [ADR-005: inactive Windows source backends](decisions/005-inactive-windows-source-backends.md) | Proposed; implemented source candidate | Concrete Native/Virtual adapters, copied recovery admission and idle completion; production provisioning and activation remain separate. |

Create records under docs/development/decisions/ using a sequential identifier and the [decision template](templates/DECISION.md). Link accepted/superseded records here. State alternatives, tradeoffs, relevant feature IDs and validation consequences. Supersede an accepted decision explicitly when its rationale changes.
