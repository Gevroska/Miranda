# Work item: durable application identities and safe catalog editing

- Identifier and milestone: P1c, application identity prerequisite
- Requirements / feature IDs: DEC-24, DEC-26, DEC-33; identity portions of L01/D07/T19/T20/T25; preserve DEC-44/45
- Candidate/base revision: locally validated source candidate based on P1b checkpoint `732a97ee04ccb5fe47b4f9016eaf2f6221124611`; final P1b ancestry and hosted integration pending
- Scope and owned paths: typed catalog helper; process, shared HTTP writer, editor endpoints/UI, protocol and owner wrapper; existing fixtures
- Interfaces and invariants: immutable UUID/numeric pair, typed preservation, safe native persistence, original paired-client authority and copied active context
- Explicit exclusions: shortcut generation, custom ordering controls, Web UI launch, monitor/display policy integration, full installation migration, clipboard, manual commands, files/QR, Input Only and controller backend
- Dependencies / shared environment owner: coordinator owns native/Web/CI execution, environment operations and publication

## Acceptance

Every configured application retains its UUID and positive numeric ID through restart, rename, artwork and stored-position changes. Legacy identities commit before advertising; malformed/conflicting or unwritable inputs retain bytes and the last good runtime. The permanent allocation ledger is bijective and prevents numeric reuse after deletion. Runtime publication is prepared before replacement. Unknown top-level/application/nested scalar and container types survive supported edits; ambiguous nested loss is rejected.

Editor operations require an exact content revision and UUID target, returning explicit 428/409/404/400/500 failures without closing a failed form. Concurrent stale saves/deletes cannot silently retarget another array entry. Same-mtime updates and different configured paths refresh by content. Native security/read/write/recovery failure coverage must retain P1b authority guarantees.

Listing emits UUID/ID/IDX; restricted currentgameuuid is hidden exactly like currentgame. Numeric, UUID-only, positive agreeing/conflicting pairs, exact UUID/-1 syntax, empty UUID compatibility and selector-free resume obey the captured active pair and current View/Launch authority. Removed active apps remain joinable until stopped, without executing edited commands. Joins to an existing media source preserve display mode; a running app with zero media sessions retains fresh-source preparation. Rejected preparation leaves no usable admission. Stale certificates, revocation and shutdown retain their original admission safeguards.

## Implementation and review

[ADR-003](../decisions/003-stable-application-identities.md) records the schema, selector, lock and publication contracts. The writer extraction retains the existing native platform path and property_tree credentials/permissions adapter. Review found and corrected a byte-read contract gap: iterator reads can conceal native read errors on some standard libraries. The shared core now checks binary fread/ferror/fclose before preparation, using wide Windows paths. The first whole-diff review also found a malformed local environment expression, a catalog/cleanup environment lifetime race, accidental native-parent environment mutation and loss of assignment order from sorted JSON keys. Corrections add an already-held process guard through publication, owned environment evaluation and insertion-preserving typed JSON, with real-child cleanup, failure/no-op parent-environment and dependent-variable restart regressions. Renewed source review identified Windows owned-key replacement casing, configured PATH consumption and POSIX fixture quoting follow-ups. Corrections reuse existing Windows key spelling, carry prepared PATH through directory and actual basename lookup, and use temporary POSIX child/undo scripts. Configured-only and same-named native candidates, explicit working directories and quoted arguments are covered by prepared synthetic regressions. The corrected integrated source diff received independent review and native/Web execution. The subsequent Windows workflow change remains a separate review and hosted-validation gate.

Source regressions are prepared in existing process, HTTP writer, editor HTTPS, GameStream HTTPS, RTSP and Apps fixtures. They include deterministic allocation seams, ledger conflicts, duplicate-key/overflow refusal, nested foreign metadata, stale revisions, active edit/delete/resume, metadata visibility, original pairing authority and failed persistence. Existing P1b platform failure/recovery cases remain intact.

## Validation

| Check / environment | Candidate | Result | Evidence / limitation |
|---|---|---|---|
| Integrated source and secured byte extraction review | Frozen 26-path candidate based on `732a97ee` | Independent review accepted, including renewed environment, concurrency and byte-read corrections | Source review does not establish driver/client acceptance |
| Locked dependency preparation and formatter | Same frozen source | Passed | Dependency manifests and lockfiles unchanged |
| Windows UCRT64 application/test build and linkage | Same frozen source | Passed; no missing DLLs | Local native build; packaging, documentation and installer qualification remain separate |
| Focused native regressions | Same frozen source | 377 total: 376 passed, one inherited licensed-driver skip, zero failures | Includes real child execution/cleanup, authenticated HTTPS, catalog transactions and inherited permission/driver component cases |
| Full native suite | Same frozen source | 1,014 total: 1,001 passed, 11 skipped, two baseline external-download failures | Both failures are the inherited httpbin base64/redirect cases with CURL code 7 |
| Web tests and production bundle | Same unchanged Web inputs | 230 tests passed in 12 files; production build passed | No physical/client acceptance implied |
| Windows CI native test step | Subsequent workflow-only change | Independent review, YAML and PowerShell syntax passed; exact extracted step passed both real-child cases locally | Exact-candidate hosted validation remains required |
| Current Artemis/Moonlight and physical streaming | Not selected | Not run | Pinned client source establishes syntax only |
| VM/driver/release qualification | Not selected | Not run | Outside this source mission |

## Delivery

Source implementation is independently reviewed and locally validated. Two real-child regressions initially failed with Windows error 5 under the local MSYS2 launcher; a bounded probe showed that launcher's job disallowed the existing production breakaway flag. Both regressions and the complete focused/native suites were subsequently executed directly in a native Windows context, with the results above. Production launch flags, assertions and host policy were unchanged. The CI test step selects the native Windows shell and adds the configured MSYS2 runtime directory; build, linkage and publication gates remain intact. Hosted behavior must still be verified on the final candidate.

Final P1b ancestry, committed provenance, hosted validation and integration remain pending. No P1c publication, VM operation, driver operation or release is qualified. Full T19/T20/T25, application shortcuts, monitor identity policies and whole-installation migration remain separate work.
