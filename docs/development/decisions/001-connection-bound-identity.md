# ADR-001: connection-bound paired-client identity

- Status: accepted
- Date: 2026-10-23
- Relevant requirement IDs: A01-A04, D04, DEC-44; foundation for T07/T08.

## Context

The inherited HTTPS verifier stores the last accepted client's certificate and name in globals. A different client can complete a handshake before an earlier connection launches or resumes. The earlier request then receives the wrong client's metadata. Client-supplied `uniqueid` is a protocol value and cannot establish pairing authority.

The pinned Simple-Web-Server creates a new Request for each keepalive or pipelined message while preserving its Connection. A map keyed by the first Request would lose authentication on later requests. Request connection access is private and grants friendship to the exact `Server<socket_type>` specialization.

## Decision

Retain an owned copy of the original pairing UUID, exact canonical certificate and friendly name on the existing `SunshineHTTPS` socket. Move the existing guarded HTTPS implementation into the application-owned `SimpleWeb::Server<nvhttp::SunshineHTTPS>` specialization, whose friendship allows reading that value through each Request's weak Connection.

Before every configured HTTPS GET dispatch, resolve the original UUID and exact certificate against current enabled pairing state under the existing authorization mutex. Missing, disabled, duplicate, removed or replaced pairings receive the existing unauthorized response and connection closure. Re-pairing the same certificate creates a different UUID and cannot give an old connection the new pairing's authority.

Pass the resolved identity explicitly into launch and resume metadata. Stream allocation retains the trusted UUID alongside its existing paired certificate. Keep the protocol `uniqueid` separate and preserve retained-input behavior in this slice.

## Alternatives and consequences

Globals cannot distinguish interleaved clients. Request-only storage does not survive keepalive. Remote addresses, query fields and headers are untrusted or shared identifiers. A third-party patch or copied base request dispatcher would increase maintenance risk; the application-owned specialization uses the dependency's existing friendship and preserves its dispatch behavior.

The specialization depends on the pinned connection and friendship contract. Dependency updates must revalidate keepalive, pipelines and callback lifetime guards. The identity is copied rather than exposing a database reference or retaining a raw SSL pointer.

Authorization is a dispatch-time snapshot. Its mutex is released before application/display effects. This decision does not make later revocation atomic with those effects and does not revoke already-pending RTSP or active streams/input. Those operations require the subsequent permission and lifecycle implementation. DEC-44's zero-access pairing defaults remain required there.

No persisted or protocol schema changes are needed. The socket implementation is shared across inherited platforms; native Windows execution and platform CI establish their own evidence.

## Validation

The [P1a work item](../work-items/P1a-request-identity.md) records actual review and execution. Required cases include interleaved handshakes in both orders, explicit/default keepalive, pipelines, forged protocol identifiers, disable/unpair/re-pair, duplicate certificates, missing identity/certificate, expired-certificate compatibility and callback shutdown/timeout guards. Verify launch and stream metadata independently of protocol identifiers. Preserve existing pairing/certificate regressions.

## Supersession

None.
