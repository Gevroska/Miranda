# Keep a chosen application order

P4c covers persistent application ordering under DEC-26 and T20.

The authenticated Apps page offers **Reorder apps** when the catalog contains at least two entries. The dialog shows the full committed order, with controls to move an app up or down. Search and name sorting remain available for browsing. Save uses the catalog revision captured when the dialog opened. After a conflict or an uncertain response, reload the catalog before making another change.

The host commits the order as a complete list of application UUIDs. Edits retain their positions, new apps append, and deletion preserves the order of surviving apps. GameStream publishes zero-based positions through `IDX` and uses the same application UUIDs and numeric IDs as before. App cards use UUID cover URLs. Existing numeric cover URLs follow the current positions. Reordering preserves the running application's process, evaluated environment and display source.

## Validation

Independent reviews accepted the catalog, HTTPS and Web changes. At `e680659e92fe55fb40b61360a36267669bd98023`, the Windows UCRT64 application, tests and production Web bundle built. All 35 focused native tests passed, including the 13 new or updated cases. They covered committed order and identity after a catalog restart in the fixture, revision conflicts, preservation of the active process, authenticated HTTPS writes, cover images, and GameStream permissions and selectors.

All 334 Web tests passed. An earlier dialog revision also passed keyboard interaction checks against a loopback fixture. Hosted platform checks and real Artemis and Moonlight T20 checks remain pending. T20 includes checking both a freshly opened Artemis grid and a grid that was already open when the order changed.
