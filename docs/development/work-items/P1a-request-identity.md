# P1a: preserve the authenticated client across HTTPS requests

- Identifier and milestone: P1a, identity foundation for P1.
- Requirements: A01-A04, D04, DEC-44; preparation for T07/T08.
- Base: `ced606d172a0910d6a8d5294f0e5db86244110e1`; runtime source is unchanged from the reviewed `bd7edbd382e989b9e115b5b56b9725d25cccbd25` base.
- Scope: HTTPS socket identity, current pairing revalidation, explicit launch/resume metadata and trusted stream UUID.
- Interfaces: existing TLS verifier, Simple-Web-Server specialization, launch session and stream allocation.
- Invariants: exact enabled pairing authority, existing callback lifetime guards, certificate compatibility and inherited platform support.
- Exclusions: permission schema/UI/defaults, pending RTSP and active-input revocation, display/application lifetime changes, client interoperability and release qualification.
- Dependencies: P0 foundation; one coordinating operator owns native execution.

## Acceptance

Each request retains the identity authenticated by its own connection across other clients' handshakes, keepalive and pipelines. A forged protocol identifier cannot replace that identity. Disable, unpair, duplicate records or re-pairing reject a later request from the old connection. Fresh valid connections retain existing certificate compatibility. Missing/expired connection context never dispatches the protected handler.

Launch/resume and stream allocation carry the trusted pairing UUID and certificate separately from protocol metadata. Callback cancellation, errors and timeouts retain their server lifetime guards.

## Implementation and review

[ADR-001](../decisions/001-connection-bound-identity.md) records the connection-bound design and alternatives. The existing HTTPS implementation is rehomed into the exact server specialization, using its existing Request friendship. No third-party source, copied base dispatch, persisted schema or wire format changes are introduced.

Independent review of the seven source/test files found no actionable defect. Review established source behavior and coverage; it does not substitute for compilation or test execution. Authorization remains a dispatch-time snapshot; full permission and active-session revocation are subsequent work.

## Validation

| Check / environment | Candidate | Result | Evidence / limitation |
|---|---|---|---|
| Independent source review | Seven-file P1a candidate on the stated base | Accepted | Connection ownership, exact pairing checks, all configured protected GET routes, metadata propagation and test cleanup inspected. |
| Formatting / whitespace | Same source/test candidate | Pass | Locked Python sync, `uv run --locked lb-update-clang-format` and whitespace checks pass. The seven reviewed source/test hashes remain identical; no unrelated native changes. |
| Native Windows build | P1a working candidate | Pass | UCRT64 GCC 16.2.0/CMake 4.4.4/static Qt 6.12.0, warnings as errors; executable inspection found no missing DLLs. Local documentation and WiX packaging excluded as in P0. |
| Focused native regressions | Same P1a source/test manifest | Pass | 49 identity, certificate, pairing, callback lifetime and stream-allocation tests pass, including all 12 added cases. Real loopback TLS is used; no physical streaming. |
| Full native suite | Same P1a source/test manifest | Fail; no additional failures | 888 total: 875 passed, 11 skipped, 2 inherited httpbin download failures with CURL code 7, matching P0. Tests remain intact; hardware/external-command skips remain untested. |
| Other platforms / clients / VM | P1a | Not run | Source review does not establish interoperability, hardware behavior or installation. |

## Delivery

The candidate is unintegrated and unpublished. Native evidence applies to the reviewed source/test manifest; bringing forward the CI/documentation-only base correction does not change those files. Next: publish the bounded candidate for applicable platform validation. This work item does not deliver device permission enforcement, a preview or a stable release.
