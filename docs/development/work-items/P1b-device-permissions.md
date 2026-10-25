# P1b: enforce explicit per-device rights on the host

- Identifier and milestone: P1b, implemented permission enforcement for P1.
- Requirements: A01-A04, DEC-21, DEC-22, DEC-44, DEC-45; T07/T08/T17.
- Base: `7836bdcf0f2f29f88e78e29288e951c4a704131f`.
- Scope: explicit persisted rights, existing authenticated client administration, GameStream actions, RTSP admission/revocation and all implemented input categories.
- Interfaces: paired-client records, shared credential/state persistence, existing RTSP owner, stream input lifecycle, Windows controller feedback and Troubleshooting UI.
- Exclusions: stable application UUIDs, display backend selection, clipboard, server commands, file transfer, QR pairing, migration and standalone Input Only.

## Acceptance and behavior

Every pairing starts with zero grants, including the first device. Missing or invalid persisted masks also require an explicit administration grant. Enabled remains a separate gate; updating rights without enabled cannot re-enable a disabled device. Only implemented bits are accepted. Failed persistence reports failure and preserves published authorization.

List, View and Launch retain the reference inclusion semantics. List protects application names/artwork; View or Launch permits joining the selected target; starting another target requires Launch and is busy while other viewers remain. A joining client does not execute the application or reconfigure its active display. Launch-authorized explicit Stop and authenticated administration Stop terminate the shared application and all sessions. Ordinary disconnect/revocation isolates the affected principal and preserves other clients.

Completed revocation invalidates pending and accepted RTSP tokens and waits for the applicable session/input cleanup. Regrant cannot revive stale tokens, queued packets or delayed callbacks. Per-connection mapped key/mouse contributions protect another client's held state. Reconnects receive fresh input state while idle controller devices retain their IDs and move to the correct feedback destination.

[ADR-002](../decisions/002-device-permission-enforcement.md) records ordering, persistence and platform tradeoffs. POSIX extended ACLs that cannot be preserved cause saves to fail before bytes or replacement. Windows captures modern inherited ACEs and the original inheritance policy, restores and verifies the exact policy on an empty temporary, and refuses EFS before creating a plaintext temporary. A separate restoration descriptor requests automatic inheritance only when the captured policy already records that model, preserving repeated-save readback without relaxing any security comparison. Partial replacement recovery is explicit; an unrecoverable restore retains secured recovery files and blocks subsequent writes.

## Prepared regressions

- Existing authorization/configuration/HTTPS fixtures: zero defaults, action hierarchy, invalid/missing masks, omitted enabled, restart, failed saves and recovery, credential/permission write interleaving, real protected route denial, pending reservation contention/rollback, one ANNOUNCE claim, accepted-token revoke/regrant and owner shutdown ordering.
- Existing input/keyboard fixtures: every named packet category, mixed-category dispatch ordering, ingress/dispatch barriers, delayed mouse/repeat/controller work, shared mapped keys and all five mouse buttons, platform-equivalent Right Control/flag aliases, equivalent US and different native-layout scans, selected HID/SendInput direct-VK behavior, synthetic modifiers, Windows extended Enter, stale reset versus reconnect and stable idle controller ownership. Feedback dequeue remains nonblocking if revocation drains the mailbox, and a dequeued event cannot cross revoke/regrant.
- Existing Windows input fixture: queued feedback rejected after rebind/removal and correct current client-relative destination, without a live driver.
- Existing state-file fixture: secured empty temporary, preserved mode/uid/gid or modern Windows owner/group/ordered ACEs and protection controls, repeated protected and explicit/inherited unprotected saves with strict empty-file readback, later parent revocation against an untouched sibling followed by further saves, native security-set and verification failure recovery with exact retry-policy checks, attribute-injected EFS refusal without changing encryption settings, actual sharing-denied replacement, Windows partial replacement restoration, blocked failed recovery and Linux named/inherited ACL refusal.
- Existing web fixture: eight implemented grants, zero defaults, rights-only updates preserving disabled state, explicit rejection and authoritative readback after a saved update/unpair whose response was lost. Failed HTTP/status/body/JSON readback reports an unknown outcome without falsely claiming unchanged authority.

## Evidence and remaining work

The final source and correction diffs received independent review. Locked formatting passed, dependency pins are unchanged, and both the Windows application and native test targets built successfully under MSYS2 UCRT64. The final native campaign used the reviewed source without further source changes:

| Check | Result |
|---|---|
| Focused identity, permission, input, RTSP and adapter cases | 284 total: 283 passed, one inherited licensed-driver case skipped, zero failures. |
| Targeted repeated-save, authorization and administration cases | All 28 passed, including the previously failing security-policy and recovery paths. |
| HTTPS identity and callback lifetime cases | All 17 passed after correcting shutdown ordering in the fixture's externally owned event loop. |
| Full local native suite | 976 total: 963 passed, 11 skipped, two failed. Both failures are the unchanged external `httpbin.org` download cases with CURL code 7, also present in the baseline. No new failure remains. |
| Web tests | 224 passed across 12 files; the three affected Web source/test hashes remain unchanged from that run. |
| Web production build | Passed, including the final combined native build. |

Separate synthetic Windows diagnostics reproduced the repeated-save marker loss and verified a request-flag variant and a modern-setter variant through three protected or inherited saves. The inherited cases retained exact policy against an untouched sibling after parent grant revocation. The candidate uses the narrower request-flag correction and retains strict readback before bytes. The production-seam regressions also passed; these checks do not establish a general Windows filesystem guarantee.

macOS/FreeBSD ACL branches are checked against primary APIs but await platform execution. Native ViGEm notification thread/target lifetime inside the pinned dependency remains upstream behavior; the callback generation regression exercises the application-owned seam without a live driver. GPU/display mode preservation, active media workers, Artemis/Moonlight interoperability and human acceptance require their own campaigns.

## Delivery

Reviewed source with local native and Web evidence, awaiting exact-candidate hosted validation and integration. VM installation, live clients, physical streaming and human acceptance remain separate gates. No preview or stable release is established by this work item.
