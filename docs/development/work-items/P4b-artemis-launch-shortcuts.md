# Open an application from Artemis

P4b covers application launch links and `.art` files under DEC-24 and T19.

Each application card in the authenticated Web UI can open an Artemis launch link or download a corresponding `.art` file. Both use the stable application UUID and the host's durable UUID. Names help identify the target. Renaming an app leaves its shortcut identity intact.

The host preserves the spelling of an existing durable UUID and can establish a missing UUID through protected state storage. The Web UI offers shortcuts after an authenticated catalog read confirms that durable identity. Fresh-state hosts and hosts whose identity could not be verified show the shortcut as unavailable.

Artemis uses its existing pairing and launch flow. The host checks the current certificate and grants for each action. Joining an active stream preserves its app, display and mode. An authorized stale application UUID returns a specific unavailable result.

## Validation

Independent source reviews accepted the host and Web changes. At `f2d7469f5aaa6f4e8531aeddf0cc2a3afe221cee`, the Windows UCRT64 application, native tests and production Web bundle built successfully. All 130 focused native tests passed, covering host identity, authenticated catalog metadata, HTTPS launch and resume, and existing pairing. All 319 Web tests passed. The production Apps bundle included the reviewed shortcut code.

Hosted platform checks and T19 on a real Artemis client remain pending. T19 includes opening the link, importing the file, testing an app rename and host restart, and verifying the behavior of granted, denied and stale shortcuts. T20 covers ordering separately.
