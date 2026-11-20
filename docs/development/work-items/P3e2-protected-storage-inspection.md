# Inspect protected Windows display recovery storage

This inactive Windows prerequisite follows [held Virtual authorization](P3e2-held-virtual-authorization.md). It covers DEC-02, DEC-05, DEC-16, DEC-17, DEC-22, DEC-31 through DEC-33 and DEC-46. The integrated source checkpoint is `4d63aadac0007f2b859e612195004f5db9dba75e`.

## Behavior

The reader checks a bounded binary installation record in the 64-bit machine registry and the fixed recovery directory below the Windows ProgramData known folder. The record identifies Native-only or recovery-managed deployment mode and binds an opaque deployment ID to the volume and directory file ID. Missing, incomplete, changed, unsafe and unreadable records produce distinct results.

The scan checks trusted owners, access control lists, registry links, canonical paths and usable local NTFS directory handles. It compares two observations and holds the verified handles for subsequent rechecks. The result exposes a read-only installation observation and the bound journal path. The existing recovery transaction remains responsible for journal contents and cleanup.

## Validation

Independent source review and locked formatting passed. The Windows UCRT64 application and test executable built successfully. A test-only rebuild verified the same application bytes. The two corrected fixture cases exercise a refused registry read that leaves the denied access controls intact and a permitted directory access-control change that invalidates an earlier observation.

All 11 executable storage inspection cases passed. The directory-symlink case was skipped because the local test account could not create the link. The remaining cases exercised real temporary NTFS directories and private per-user registry keys, including manifest parsing, registry links, protected handle lifetime and detection of replacement and access-control changes. The initial fixture run recorded two failures that were corrected before the focused pass.

The neighboring Windows checks passed 32 of 32 startup cases, 13 of 15 backend cases and 212 of 213 component cases. One backend case finished Native restoration before its next Virtual preparation returned 503. The other reached its callback deadline before triggering the explicit retry. The component case failed its startup reconstruction prerequisite before reaching the owner offer. The exact causes remain open. These paths use the existing startup or fake backend fixtures independently of the new reader.

Production acceptance still requires an authenticated installer and a real machine installation with verified registry and directory protections, journal ownership and service identity. Driver exclusion, clean-guest operation and physical streaming require separate qualification.
