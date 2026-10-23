# Contributing to Miranda

Miranda is establishing its source/build baseline before feature implementation. Start with the [project scope](docs/planning/README.md) and [status register](docs/development/STATUS.md) so a proposal does not duplicate planned or active work.

## Propose a change

Use a feature request to describe the desired behavior and client/host scenario. For a bug, provide the Miranda revision or package version, reproduction steps, expected result, actual result and relevant environment. Share a minimal sanitized log excerpt rather than a full configuration or system dump.

Discuss substantial architecture changes before implementation. Maintain focused branches and pull requests targeting the repository's actual default branch. Preserve unrelated changes, upstream notices and component licenses.

## Prepare a pull request

1. State the observable behavior and the scope of the change.
2. Implement it using existing interfaces where practical.
3. Run the checks appropriate to its risk and affected paths.
4. Update user/developer documentation and the status register when delivery evidence changes.
5. Complete the PR template with actual results and any missing coverage.

Use English for repository prose and comments. Follow [AGENTS.md](AGENTS.md) and the [development workflow](docs/development/WORKFLOW.md) for source conventions.

## Validation

The [validation guide](docs/development/VALIDATION.md) defines local, integration and release checks. A passing unit test does not qualify a driver or a real stream. Do not claim hardware support from an untested source path.

Do not remove assertions, skip required cases or change production behavior solely to obtain a green result. Explain baseline failures separately from regressions.

## Licensing and collaboration

Contributions must be compatible with the repository's existing license and applicable dependency licenses. Preserve notices and attribution required by those licenses. Review code and assumptions respectfully, describe concrete problems and keep discussions focused on the change.

Do not include credentials, pairing state or private diagnostic data in issues, PRs or artifacts. For a suspected vulnerability, use a private reporting channel offered in the repository's Security tab when available; do not open a public issue containing exploit details.
