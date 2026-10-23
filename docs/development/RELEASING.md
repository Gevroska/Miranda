# Release process

Miranda will publish previews before a stable release. A release describes a specific tested package; a roadmap item or upstream implementation is not a shipped feature.

## Candidate selection

1. Select the integrated revision and milestone.
2. Confirm required checks and reviews for that revision.
3. Verify the produced packages and their source/dependency identity.
4. Complete the relevant installation and runtime acceptance.
5. Write release notes with changes, requirements, migration guidance, known limitations and actual platform coverage.
6. Publish through the authorized maintainer workflow, then verify the tag, release type, assets and checksums from GitHub.

Do not reuse stale CI results after a material candidate change. Use the built artifact that was qualified; a tag alone does not prove which executable was tested.

## Preview versus stable

A preview may expose a documented partial milestone, with an explicit capability matrix and known gaps. It must install and support its advertised workflows; missing essential package/driver requirements are not excused by a preview label.

Stable v1 requires the defined first-release scope and relevant acceptance evidence. If required hardware or signing evidence is unavailable, keep the release at preview or record the concrete blocker; do not silently claim qualification.

Use clear version/channel naming and mark preview releases as prereleases. Do not invent version history, changelog entries or download links before an artifact exists.

## Package checks

- Verify installer contents, executable/service identity, driver packages and applicable licenses/notices.
- Test installation, upgrade, supported migration, uninstall and recovery.
- Preserve existing application/settings sources during migration.
- Record actual signing and package provenance without implying a signature that was not checked.
- Ship no paid-only prerequisite for an advertised Miranda feature.
- Review archives, debug information, logs and build paths for unintended content.
- Keep release notes concise and distinguish tested, inherited and untested platform coverage.

## Public automation

Use minimal workflow permissions and separate untrusted PR validation from privileged publishing. Do not run fork-controlled code with release credentials. Treat downloaded PR artifacts as untrusted. Review reusable workflow references, action pins, caches and artifact boundaries.

Audit inherited workflows before enabling publication: owners, destinations, package registries, update feeds, signing inputs and documentation sites must match the intended Miranda target. Do not accidentally publish to upstream infrastructure.

Keep branch protections and required reviews/checks effective. Automation authority does not justify bypassing them. A local independent review is not a second GitHub account's approval.

Maintain a working private vulnerability-reporting route before advertising one. Retain upstream license/notice files. Add community files only when their contact and maintenance commitments are accurate.

## Evidence and recovery

Retain the candidate SHA, check results, package identifiers and checksums, tested scenarios, release URL and any post-publication correction. Verify the actual published assets. If a release is defective, issue a clear correction or replacement; do not silently move published stable tags.

Template: [release notes](templates/RELEASE_NOTES.md).

References: [GitHub community profiles](https://docs.github.com/en/communities/setting-up-your-project-for-healthy-contributions/about-community-profiles-for-public-repositories), [privileged PR workflow security](https://docs.github.com/en/actions/reference/security/securely-using-pull_request_target).
