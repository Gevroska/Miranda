# Pair Artemis with a QR code or one-time credentials

This P4a slice implements host and Web support for DEC-21, DEC-23 and DEC-44. The integrated source checkpoint is `42dec07ca5484e297de36790167ebe1b270dea68`.

## Pairing flow

An administrator can generate or withdraw a one-time invitation from the PIN page. The host keeps one unclaimed invitation in memory for up to 180 seconds. The invitation uses a four-digit PIN and a random passphrase. The Web UI displays both values for manual entry and builds one Artemis URI for the link and its locally generated QR code. The URI uses a validated reachable address and an explicit GameStream HTTP port. The page clears the credentials, link and image when their display lifetime ends.

Invitation creation and withdrawal require authenticated administration and a valid CSRF token. The first valid claim consumes the invitation inside the pairing registry transaction. Artemis and manual PIN clients share the existing bounded registry and certificate handshake. Every new device, including the first device, receives zero explicit permissions. An administrator can grant rights through the existing client settings.

## Validation

Independent reviews accepted the host and Web diffs. The Windows UCRT64 application, test executable and production Web bundle built successfully. All 310 Web tests passed, and the bundled QR package notice matched the reviewed MIT license text.

All 56 Windows pairing and administration cases and 31 neighboring HTTPS cases passed without skips. The signed loopback fixture persisted two distinct client certificates with zero initial permissions, verified the protected HTTPS challenge and exercised an explicit later grant. Other focused cases covered conventional PIN, one-time claim/withdrawal races, expiration, shutdown, malformed requests, sensitive logging and cleanup of a failed reply. The delivery failure fixture exercises the actual response continuation through an injected failure.

Real Artemis QR scanning, link opening and typed entry remain part of T18, alongside conventional Artemis and Moonlight PIN pairing on the selected clients.
