# Spike archive and rule workspace UI

These widgets preserve the existing application theme and `page-control-panel`,
`title`, and `caption` roles. They own opaque top-level surfaces, with plot colors
provided by the existing `setWaveTheme` palette. The service owns capture,
processing, persistent run identity, revisions, and offline analysis.

## Integration contract

- `SpikeAnalysisControls` emits start/stop/archive-open/offline/cancel/editor
  requests. `setArchiveStatus` and `setOfflineProgress` are authoritative; clicking
  an action never claims successful completion. The optional `finalizing` argument
  locks source/output/new-run controls while Cancel is disabled. A failed archive
  remains restart-disabled until its writer has actually finished. Terminal status
  refreshes preserve a user-edited next archive destination. The destination is a new sidecar
  directory, not an existing recording directory. Opening the control is inert.
- `SpikeArchiveBrowser::openArchive` accepts an asynchronous request and uses a worker-local archive reader and retains only a
  page of up to 128 events. Queries use physical electrode, exact 64-bit epoch,
  and inclusive source-time bounds. The applied query remains stable while the
  user edits fields. Empty nonterminal pages explicitly permit continued scanning.
  Back navigation stores at most 128 cursors. No display-ring total is substituted
  for the archive total.
- The archive table preserves stable run/event identity, source timestamps,
  physical mapping, epoch/continuity segment, detector/rule revisions, original
  automatic candidate and classification. A selected waveform uses ms and
  input-referred µV. Source-integrity information is separate from the archive's
  completed/drained/cancelled/failed status. Unknown source quality is explicit.
- Manual annotations are append-only, enabled only for a verified terminal
  archive. Annotation acceptance is followed by an independent reader reopen and
  stable-identity verification, scanning bounded pages without accumulating
  history. The original automated classification is not changed. Active-run
  annotation must use the service's ordered writer API.
- `SpikeRuleEditor` requests context/waveform capture by lane. The service calls
  `setCapturedContext` with the effective configured context and copied events.
  At most 80 waveforms of the core's bounded maximum snippet size are retained.
  Rectangle drawing and accessible numeric inputs produce the same canonical
  time-ms/input-µV definitions. All boxes for one candidate use AND. Cross-candidate
  matches remain ambiguous. The preview calls the core classifier on captured
  data only; it never mutates historical events.
- `applyRulesRequested` contains definitions, never a claimed applied revision.
  The service creates its immutable snapshot at its actual crossing boundary and
  calls `setAppliedRules`/`setServiceStatus` with the result. A matching acknowledged
  definition clears dirty state; an unrelated service update preserves edits.
  `setRequestedRules` separately preserves service-retained desired definitions,
  including while stopped and no applied snapshot exists. On open, call
  `setAppliedRules(current acknowledgement or null)` then `setRequestedRules(desired)`;
  also update requested state after an accepted submission. Pending/desired and
  actually applied revisions are visibly distinct; dirty local edits survive both.
  Draft JSON uses valid interchange revision 1 and is visibly distinguished from
  the service's actual applied revision. Archive revisions remain the historical
  source of truth. An empty ruleset is a valid explicit future-event clear.
- A context mismatch disables new-box editing until the user explicitly rebinds
  or clears that electrode. Loaded definitions stay drafts. Acquisition is never
  started by showing or closing these modeless windows.

## Asynchronous lifecycle

Opening/verifying a sidecar, paging events, reading annotation history, and
append/reopen verification run through QtConcurrent with worker-local value
objects. Workers never touch widgets or share a QFile with the GUI. The return
value of `openArchive`/`appendSelectedLabel` acknowledges request acceptance;
`archiveOpened(bool)`, `pageReady()`, and `annotationFinished(bool)` report results.
`busy()`/`busyChanged(bool)` expose outstanding work, including the initial page
and annotation lookup after an open. Tests wait until the browser is idle before
checking visible rows. Cancelled generations do not apply their stale results.

The visible Cancel Read button and close request cancellation. Destruction sets a
shared cancellation token and disconnects the parent-owned watcher without
waiting for a scan. Reader-open and pre-append validation check that token; page
and annotation reads are bounded calls. Cancellation after an append commits
cannot undo that immutable label, and the UI explicitly tells users to reopen to
check an uncertain result. At most one current generation can update the view.

Completed is displayed as archive-write completion only. Always-visible source
counts include missing/dropped/invalid/unverified frames, pending windows and
boundary exclusions. The archive browser separately identifies live after-start
scope, a dedicated offline source pass, or unknown scope. Unknown counters are
shown as `?`, never invented zeros; the details panel contains complete metadata.

## GUI test target

`tests/spike_archive_rule_widgets_smoke.cpp` requires Qt6 Core + Widgets + Concurrent, AUTOMOC,
`src/theme/theme_manager.cpp`, `src/ui/theme/theme.qrc`,
`src/io/spike_event_archive.cpp`, `src/signal/spike_rule_classifier.cpp`, and the
three widget `.cpp` files. Use `QT_QPA_PLATFORM=offscreen`.

Set `CCV2_ARCHIVE_RULE_QA_DIR` to save actual QtWidget renders for dark, light,
soft-dark, midnight-indigo, and compact layouts. The test uses real ThemeManager
QSS/qrc, synthetic archived events, an independent reopened reader, and the real
rule classifier. It checks action signals, output data, pagination/filtering,
append-only persistence, ambiguity/incompatibility, drag/numeric editing,
service-owned revisions, worker cancellation/destruction and stale-result suppression, modeless close/open and opaque
surfaces. Pixel inspection is required after the first target run; synthetic
fixtures do not establish hardware performance or neuronal isolation.

## Actual application-shell validation

The additive section in `tests/full_app_visual_smoke.cpp` runs the real
`CommandCenterMainWindow`, global toolbar replay signals, `SessionHub`, detector
worker, and constructed Spike page. It starts the event archive through the
actual visible archive control before replay, verifies the populated terminal
sidecar, opens the real asynchronous archive browser, then replays again and
pauses before EOF to capture the worker's current context/waveforms. A diagnostic
box is added through numeric controls and submitted, and subsequent replay blocks
establish the actual service acknowledgement before the screenshots are taken.
No detected-event store, rule result, substitute stylesheet, or rendered UI is
injected. Only known local fixture paths bypass native file chooser dialogs.

A focused run on 2026-10-02 completed with 3,758 archived events and actual rule
revision 1. The raw synthetic fixture's unverified source validity and excluded
boundary windows remain visible in the archive/controls. This is proof of the
UI, pipeline and persistence workflow on that fixture, not biological isolation
or a physical-hardware accuracy claim.

Existing `full-app-*` and `candidate-workspace-*` captures remain. New outputs are
`neural-full-app-*`, `neural-rule-editor-*`, and `neural-archive-browser-*` for all
four application themes, plus a compact expanded-tools shell. The isolated
widget target passed 212 checks after asynchronous lifecycle, finalization,
repeat-destination, and stopped-rule-retention regressions were added.
