# ADR-004: Immutable display policy before source activation

- Status: proposed; implemented candidate awaiting independent review and execution.
- Date: 2026-10-26.
- Relevant requirement IDs: DEC-16/17/22/29/31/32/33/44/45/46; D06/D07/D09.

## Context

Display preparation needs administrator-owned device preferences and stable monitor identities before it can safely acquire a SudoVDA source. P1a supplies the persistent authenticated pairing UUID and P1c supplies stable application UUIDs. A request's `uniqueid`, application name, numeric protocol ID and catalog order are unsuitable monitor identities. A later viewer must retain the existing source even when its saved preferences differ.

The modern Sunshine display manager already owns manual/automatic selection, remapping, topology and restoration delay. Duplicating that selection would risk overriding a host preference or losing fractional rates. Pairing and Web credentials can share one state file; both writers must preserve typed display metadata.

## Decision

`display_policy.h` supplies a platform-neutral typed device preference model and a pure resolver returning `shared_ptr<const source_request_t>`. The caller must revalidate the admission's current authority before calling. A video join returns the exact active object before inspecting fresh mode/preferences; a reserved `no_display` admission returns no source and does not pin an active one. This reserves a future non-video seam without implementing Input Only.

For a fresh video source, global/application/device force-virtual flags and an authorized Artemis opt-in are additive. Lack of a usable physical source also selects virtual preparation. False is absence of an opt-in, not a physical veto. Failure to prepare a requested virtual source must not become physical fallback. This slice does not call acquisition or capture code.

The caller passes the source mode already selected through modern Sunshine's existing manual/automatic/remapping path. Device overrides replace its dimensions and exact rational hertz afterward, so ordinary remapping cannot undo them. Resolution dimensions and device rational components must be positive integers no greater than `INT32_MAX`; no rate whitelist is introduced. The resolver preserves HDR, topology, configured adapter and native selector in the immutable request. Observed display mode and each stream's integer FPS remain separate integration concerns. No millihertz or Artemis boundary conversion occurs here.

Monitor identity encoding version 1 uses namespace `e944e3d0-e64c-49d9-8ef4-2f57df0d8d7b`. Hash these ASCII strings using the existing SHA-256 helper, with LF bytes and no final LF:

| Policy | Exact input after namespace followed by LF |
|---|---|
| Per client | `client` + LF + canonical pairing UUID |
| Per application | `application` + LF + canonical application UUID |
| Per pair | `client-application` + LF + canonical pairing UUID + LF + canonical application UUID |

Take digest bytes 0–15 in network/display order, set octet 6's high nibble to 8 and octet 8's high two bits to binary 10, then render lowercase canonical UUID text. This is a vendor-defined [RFC 9562 UUIDv8](https://www.rfc-editor.org/rfc/rfc9562.html#name-uuid-version-8), not a UUIDv5 encoding. UUID syntax and fixed domain/tuple positions prevent ambiguous concatenation. The namespace is dedicated to this version; a future encoding needs an explicit migration decision rather than silently changing it.

Protocol Desktop substitutes reserved application UUID `3f0c2e18-d464-5c55-8f77-3d6103fefcc2`, which the ordinary catalog allocator rejects. An explicitly configured empty-command Desktop entry keeps its P1c identity, independently of its name. `application_has_lifetime` distinguishes tracked applications from Desktop/placebo entries for retention. Monitor EDID name is `Miranda-` plus the GUID's first five hex characters; serial is the first thirteen hex characters after removing hyphens. These fixed ASCII fields never exceed SudoVDA's thirteen-byte limits and are descriptive metadata; the full GUID remains authoritative.

The existing `dd_config_revert_on_disconnect` and delay persist DEC-46. A source may remain while a tracked application runs by default. Enabling disconnect restoration resolves to restoration/removal after the last viewer, using the existing delay. Protocol Desktop, no-app and catalog placeholders do not gain retention from UUID presence. Actual last-viewer, application-exit and delayed cleanup transitions belong to the integrated lifecycle work.

Global settings use existing configuration parsing/defaults. Application force-virtual lives beside its P1c UUID and passes through existing typed catalog transactions. Device display preferences live inside the UUID-matched paired-device document; only the authenticated, CSRF-protected Web administration update can change them. Every newly paired device still has zero rights. Device-only preference saves keep authorization revision and prepared tokens intact. Requests that also change permissions/enabled state retain P1b's atomic save-before-publication and revocation/reconciliation ordering.

The paired-state writer and credential writer use the established secure byte transaction. They retain unknown JSON types, while authority scalar fields keep their inherited string serialization. Credential updates replace only username/salt/password and keep the existing hash procedure. Parsing reuses the generic loss-preventing parser, without applying the app-catalog schema or allocation normalization. A malformed display subtree is exposed through `display_valid=false`, preserved during unrelated authority edits, and blocks fresh resolution until a valid administrator repair. Successful writes preserve values and types while reserializing formatting; failed writes preserve the exact original file and published state.

## Alternatives and consequences

Copying Apollo's display/session subsystem would replace modern lifetime machinery and reintroduce older assumptions. Names, numeric hashes or XOR-combined identities can collide or drift after edits. Adding Boost.UUID solely for this helper would expand the non-Windows component set; using the existing SHA-256 helper avoids that dependency change. The full 128-bit UUID has 122 digest bits after version/variant masking; abbreviated EDID strings do not replace it.

Continuing property-tree writes would coerce unknown booleans, numbers, nulls and empty arrays/objects into strings, including during a password change. Maintaining the old reader for existing permission scalars while additionally parsing typed display subtrees keeps the authority contract narrow. Duplicate keys, excessive JSON depth and unrepresentable integers are refused by the existing generic parser instead of being rewritten ambiguously.

## Validation

The [P3a work item](../work-items/P3a-display-policy.md) lists the candidate and checks. Added source regressions cover the 32-way opt-in/headless matrix, fixed GUID vectors and domain separation, exact fractional overrides, active-source object preservation, Desktop retention, malformed settings, typed metadata/credential preservation, write rollback, prepared-token continuity, Web authentication/CSRF and settings readback. Independent Python `hashlib`/`uuid` fixture generation supplied the UUID constants; it does not execute or validate the C++ implementation.

No native/Web tests, builds, formatter, CI, driver operations or streaming were run by this worker. Production enabling requires the display transaction, admission/lifetime and capture binding slices to be integrated and independently reviewed together. The dormant adapter and this resolver prove no actual monitor or topology behavior.

## Supersession

None.
