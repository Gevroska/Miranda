# Honor explicit Artemis virtual-display requests

This is the first [Artemis protocol](../../planning/03-artemis-compatibility.md) slice for P4. It covers DEC-04, DEC-17, DEC-19, DEC-22, DEC-44 and DEC-45. The integrated source checkpoint is `1bef4fc578afe49ca8731514b877323bd88e4286`.

## Protocol behavior

HTTPS launch and resume accept the decoded Artemis `virtualDisplay` values `0` and `1`. An omitted option or `0` delegates source selection to the existing host, application and device policy. A single `1` requests a Virtual source for fresh preparation. An unavailable Virtual backend returns protocol status 503 before altering host audio, the application or the display. A failed Virtual acquisition grants no RTSP admission.

An authorized client joining an existing source keeps that application's display and mode. An invalid or repeated option returns protocol status 400 when the existing authorization, target, conflict and encryption checks succeed. The XML error response keeps the standard launch or resume result and omits the session URL. The advertised capability fields remain reserved for production backend readiness.

## Validation

Independent review and locked formatting passed. The incremental Windows UCRT64 application, test executable and Web build passed on a sealed source overlay. All 31 HTTPS cases passed with no skips. These include the five new and two extended regressions for the explicit option, older Moonlight-shaped requests, malformed and duplicate parameters, protected denial responses, absent backend, failed acquisition, and ready and retained source reuse. Tests exercised the production HTTPS routes over loopback with injected backend boundaries.

The same build passed the separate native recovery test whose callback now keeps its signal alive through delayed dispatch. The earlier recovery failures remain recorded with their original evidence.

The full Windows run completed 1,427 cases with 1,409 passes, 13 skips and five failures. All five new and two expanded HTTPS cases, all nine managed HTTPS cases and both concrete owner cases passed. The five failures were two existing httpbin CURL7 download failures and three recovery adoption cases. One adoption case repeated a startup reconstruction refusal. Two others exceeded their cleanup or shutdown waits. The causes of the three adoption failures remain open.

Current Artemis and Moonlight application acceptance, measured refresh rates, driver-backed Virtual operation and mixed-client streaming remain pending. Fractional-rate conversion, QR pairing, shortcuts and application management have separate acceptance criteria.
