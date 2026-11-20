# Hold Virtual source authorization through cleanup

This inactive Windows change follows [checked recovery completion](P3e2-completion-release.md). It covers DEC-02, DEC-05, DEC-16, DEC-17, DEC-22, DEC-31 through DEC-33 and DEC-46. The integrated source checkpoint is `7bb4d931ffa36cb3696b1d25aeb927b308f6431a`.

## Behavior

The verifier returns an owning grant for the exact journal, request and generation before the owned-display transaction starts or the driver is called. The backend retains the grant through uncertain journal writes, delayed restoration, heartbeats, retirement, transport close retries and final completion. It releases its reference after verified journal clear and dependent resource teardown. A verified pre-intent failure can release the fresh grant immediately.

Rollover qualifies the new request under the existing grant. The returned grant must point to the same object and share its ownership. Denied and substituted grants leave the existing recovery episode intact. A new episode can obtain a new grant after completion.

Recovery uses its independent qualification entry point. It rejects an ownerless alias before entering the owned-display transaction or calling the driver and can retry with an owning grant under the same startup exclusion. The adopted context retains authority through exact gate release. Native sources use the ordinary scheduler.

## Validation

Independent source review and locked formatting passed. The Windows UCRT64 application, test executable and Web build passed on a verified source overlay. All 352 focused tests passed without skips across the component, startup, backend, owner, managed HTTPS and capture groups.

Six new cases and one renamed case passed. They exercised a verified pre-intent failure, failed manager creation after committed first and rotated intents, delayed restoration, transport and heartbeat teardown, release after verified clear, and a valid recovery retry after ownerless refusal. Existing rollover cases reject both a different object under the same ownership and the original object under different ownership.

The full Windows suite ran 1,410 cases in 420.156 seconds. It recorded 1,395 passes, 12 skips and three failures. The two existing external httpbin downloads failed with CURL7. One managed HTTPS case exceeded its catalog mutation deadline after the test released process preparation. The same case passed in the focused run. All seven new or renamed authorization cases passed in both runs. The catalog delay remains under investigation.

Protected provisioning, proof of exclusive driver control and production registration are prerequisites for activation. The tests inject driver boundaries. Clean-guest and physical streaming qualification remain pending.
