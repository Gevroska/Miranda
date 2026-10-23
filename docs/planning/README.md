# Miranda development planning

Status: scope defined; source/build baseline in progress. Updated: 2026-10-23 (Europe/Paris).

For the development workflow, validation, release process and actual delivery state, see [the development guide](../development/README.md). This directory remains the product-scope and source-analysis reference.

Miranda will retain a recent Sunshine foundation and ultimately provide the functionality of Apollo, Vibepollo and Vibeshine, with Artemis and current Moonlight support. The owner confirmed a focused first release followed by the remaining features in later releases. Baseline development has started; [implementation status](../development/STATUS.md) records actual evidence. These planning documents remain the scope and decision record.

## Reading order

1. [Project brief](01-project-brief.md): objectives, confirmed first-release scope, and boundaries.
2. [Feature inventory](02-feature-inventory.md): Apollo additions, Sunshine overlap, dependencies, and disposition.
3. [Artemis compatibility](03-artemis-compatibility.md): the observed client/server contract that must survive reimplementation.
4. [Virtual display driver comparison](04-display-driver-comparison.md): implement SudoMaker/SudoVDA first, then compare with VirtualDrivers VDD. These are the only two candidates.
5. [Implementation and validation plan](05-implementation-plan.md): readiness assessment, proposed work packages, dependencies, risks, and acceptance criteria.
6. [Decisions and questions](06-decisions-and-questions.md): authoritative user decisions and unresolved choices.
7. [Evidence and analysis limits](07-evidence-and-limits.md): pinned repositories, methods, source map, and what has not been validated.
8. [Free controller feasibility and code reuse](08-free-controller-feasibility.md): all-free requirement, reusable code boundaries, selected Nonary driver and qualification work.
9. [Vibepollo feature review](09-vibepollo-feature-review.md): 48 capability groups for later releases, with overlap, dependency and maturity limits.
10. [Later-release roadmap and questions](10-vibepollo-roadmap-and-questions.md): capture/pacing/HDR review priority, optional Nonary client and selected Remote Monitor direction.
11. [Vibeshine versus Vibepollo](11-vibeshine-vibepollo-comparison.md): verified Sunshine ancestry, shared Nonary additions and differences in Apollo/Artemis extensions.

The decision log takes precedence over proposed recommendations elsewhere. Unanswered questions do not become approved scope. Update affected documents together when a decision changes.

## Main conclusion

The first release centers on Windows virtual displays, Artemis and Moonlight compatibility, per-device permissions, bidirectional text clipboard access, and Input Only mode. It also includes QR/one-time-code pairing, Artemis application launch shortcuts (links and `.art` files), custom app ordering, Web UI launch and per-app controller-type overrides. Application/settings migration is required; preserving existing pairings is not. New session automation and Artemis-triggered manual server commands are deferred. SudoVDA is the first driver to implement; VDD remains the sole comparison alternative.

Native-display streaming must keep that display on. Virtual-display streaming offers Apollo-style choices for keeping physical displays active or disabling them. A second video client joins the existing session without changing its active display mode. Every newly paired device, including the first owner device, receives no access until an administrator explicitly grants permissions in the authenticated Web UI (DEC-44).

Every shipped feature must be available without a paid license or subscription. Nonary/libvirtualgamepad is the selected Windows modern-controller driver; integration and runtime qualification remain to do. Available physical-controller testing covers Xbox One and Switch Pro through Android, Windows and Fedora clients.

Virtual-display controls include forcing a virtual display globally, per application and per device, plus per-device resolution/refresh-rate overrides. Persistent monitor identities can be per client, per application or per client/application pair. These settings preserve an existing shared session when another viewer joins.

Full functionality from all three reference projects is the long-term target (DEC-38), while the focused next-release scope remains unchanged (DEC-39). Later work starts with capture quality, frame pacing and HDR. Nonary's Moonlight fork is an optional additional client, and Remote Monitor is an explicit future mode. Vibeshine's newer Sunshine ancestry is still older than Miranda's current base. Free replacements and qualification are required where upstream features depend on paid software or experimental components.

The initial hardware target is Windows 11 25H2 with an RTX 3080; available GPU testing is limited to recent NVIDIA models. Test the latest available Artemis and Moonlight releases, including Moonlight on Fedora and Windows. Support arbitrary valid resolution/rate requests within the actual pipeline's capabilities; 60, 120 and 165 Hz are representative tests, not a whitelist.

The owner will prepare a clean Windows VM for initial implementation tests. Public preview builds will precede a stable release, and Sunshine updates will be integrated periodically from selected tested revisions. Apollo's service handover from Sunshine is the installation reference; explicit apps/settings migration and driver ownership still need implementation. VM results must be distinguished from actual GPU/HDR/performance qualification.

Miranda and Sunshine both pointed to `d0b4a88ef727f8dabf80cc970a14739b55c885de` during analysis. Apollo's examined head is `adc5c5a0bd80831ce495434bb16aee2cd4175fb8`. Their divergence makes feature-by-feature adaptation preferable to a wholesale Apollo merge.

## Evidence levels

- **Source-confirmed**: the relevant code or configuration was inspected at the pinned revision.
- **Documented**: a maintainer's README, wiki, or release notes describe the behavior.
- **Proposed**: a development recommendation, not a user decision.
- **Unverified at runtime**: no real streaming, driver installation, hardware, performance, or migration test has yet proved the behavior.

The inventory is a comprehensive functional comparison at these revisions. It is not a line-by-line security audit or a claim that all features work on the owner's hardware.
