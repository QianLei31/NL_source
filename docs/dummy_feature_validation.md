# Dummy functional validation inventory

## Scope and acceptance rule

Baseline: published `387c36ce57baa9e85fa787ffef50da16ece800f6`. Inventory is derived from the sources actually included in `CMakeLists.txt` and connected by `CommandCenterMainWindow`, not from all files present in `src/`. In particular `src/service/spi_command_service.*`, `safety_interlock.*`, prototype pages, and legacy model/pipeline files are not evidence about this application. Existing unit/smoke tests are supporting evidence, not a substitute for the acceptance requirement that every implemented GUI function be exercised with Dummy data.

Status vocabulary:
- **PASS**: scenario has executed and assertions/evidence are recorded
- **SYNTHETIC WIDGET PASS**: the actual compiled widget action passed with deterministic synthetic records written/read by the real application services; this is Dummy functional evidence, with its narrower integration scope stated
- **SERVICE PASS / GUI UNRUN**: the real backend path passed but the visible GUI action has not yet been exercised
- **PARTIAL**: action ran and stated state/rendering assertions passed, but the row's stronger numerical or remaining-variant oracle was not exercised
- **FAIL**: executed scenario failed its required assertion
- **UNRUN**: test not executed, or no evidence supplied yet
- **BLOCKED**: cannot be claimed with the current test environment/model; exact reason is stated
- **N/A hidden**: compiled helper/control deliberately hidden in this version; not a reachable current GUI function

A test that only clicks a control, changes an input, takes a screenshot, or does not crash cannot establish data/configuration/output correctness. Required oracles are state, exact wire payload, numerical samples, persisted configuration, file contents, and timeline identity where relevant. Visual evidence supplements these assertions.

## Evidence producers

Final integrated gate: **64/64 passed, zero failures, normal exit 0, 254.02 seconds** (2026-10-02 UTC). Summary: `visual-qa-preview2/ctest-preview2-final-clean.log`; complete target output: `build-preview2/Testing/Temporary/LastTest.log`. This run includes the final source-boundary metadata checks, all literal GUI control extensions and the corrected version-relative updater fixture. No aggregate result erases the external, licensing, numerical or robustness limits below.

Historical evidence: the earlier baseline passed 57/57 in 201.41 seconds. The first expanded run passed 63/64 in 263.90 seconds (`ctest-preview2-final.log`), failing only the stale AUX04 preview-version test expectation. Its test-only correction passed a focused 187-assertion rerun and the final 64/64 gate; the earlier failing log is retained as history.

- `tests/dummy_acquisition_e2e.cpp`: actual `SpiControlPanel` send paths against a capturing loopback peer (C01–C07) and built-in Dummy state (C08); actual built-in `DummyStreamServer` → TCP → `SessionHub` → `RecordingWorker` → `SessionRecorder` (A01–A03); same captured bytes through replay (R01–R04); every exported sample in all formats (E01–E04); live threshold-trigger callback → raw recording (T01)
- `tests/dummy_session_ui_e2e.cpp`: real main-window/settings/toolbar/file-dialog/recording/trigger/export wiring (SUI01–SUI11), terminal PASS; `tests/dummy_processing_ui_e2e.cpp`: processing/map/candidate GUI suite, PASS 365 assertions in the final 64/64 run (364 in the preceding focused run); exact assertion levels and limits in `docs/dummy_processing_coverage.md`
- `tests/dummy_protocol_model_smoke.cpp`: actual TCP framing/register model/gain/OFF/clipping/TDM/lifecycle, PASS 10/10 runs (`dummy-protocol-repeat.log`); limits in `docs/dummy_protocol_model.md`
- `tests/dummy_auxiliary_ui_e2e.cpp`: actual updater/settings and negative license UI against local HTTP fixtures, PASS 187 assertions in two full runs; exact cases and signature restrictions in `docs/dummy_auxiliary_coverage.md`
- `tests/dummy_neural_archive_e2e.cpp`: actual built-in Dummy → recorded raw session → live/replay archive, future-event rules, independent offline analysis and immutable-file assertions; final aggregate PASS 1,631 assertions; earlier focused/aggregate passes had 1,635/1,637 assertions, with live-timing-dependent reader-page counts; exact scope in `docs/dummy_neural_archive_coverage.md`
- `tests/spike_archive_rule_widgets_smoke.cpp`: actual archive browser/rule editor/analysis controls against real sidecars populated with deterministic synthetic events; PASS 323 assertions with 15 screenshots, plus three final repeated 308-assertion passes without screenshot saves; distinct from the live-Dummy integration
- `tests/spike_rule_worker_smoke.cpp`: final PASS 398 assertions, including exact applied source-frame/epoch/continuity metadata, pending-window edits, source/reference boundaries and paused requests without fabricated applied coordinates; supporting worker-level synthetic evidence
- Existing 49-test regression suite: supporting regression evidence only, never a blanket feature pass

Current acquisition-suite status (2026-10-02 UTC): **PASS**, three focused complete runs plus the final integrated CTest run with exit 0 after fixing the recorder lifetime bug described below (`dummy-acquisition-final.log` plus `dummy-acquisition-full-repeat.log`). Log: `visual-qa-preview2/dummy-acquisition-final.log` in the verification workspace. The targeted immediate-failure/recovery scenario also passed **10/10 repeated runs** (`dummy-acquisition-errors-repeat.log`). C01–C08, A01–A03, R01–R04, E01–E04 and T01 all executed.

Measured evidence:
- Single/dual live captures: each 1,280 frames across 20 split parts; stalled display drops never corrupted recording
- Replay and re-recording: 1,310,720 raw bytes matched exactly; source provenance and restored TDM/configuration history checked
- ADC 12/raw32/TDM exports: each 327,680 samples compared exactly across 256/256/1,024 output files; all four TDM source-index files matched
- Live trigger: six rising/falling/TDM-phase recording scenarios; no duplicate start while recording
- Actual `SpiControlPanel` → built-in Dummy: 1,039 supported commands, all 256 gain states, global flags, REC/DAC/CT/stimulation fields and unchanged neighboring channel verified
- Refused connection, failed recording path, immediate valid restart, source disconnect, incomplete-session finalization, missing/malformed replay and cancelled export tested

Application bug found and fixed: after a failed recording path, a deleted recording object's queued error could match a new recorder allocated at the same memory address. The valid recording then stopped at zero bytes with the prior error. The failing narrow diagnostic is retained in `dummy-acquisition-errors-diagnostic.log`; the production callback now uses `QPointer` lifetime identity, and immediate recovery passes without an event-loop delay. The test retains this regression.

Test correction: the export API's sample-rate argument is an explicit override, while the GUI prefills it from the manifest. An initial test incorrectly treated it as a fallback; the oracle was corrected after inspecting the UI/API semantics. No application change was used to satisfy that incorrect assumption.

Processing GUI suite: **PASS**, exit 0 with **365 assertions** in the final 64/64 aggregate (the preceding focused run had 364); map-only PASS with 67 assertions. The parameter-only supplement passed **77 assertions**. Evidence: `build-preview2/Testing/Temporary/LastTest.log`, `visual-qa-preview2/dummy-processing-parameters-full.log`, `dummy-processing-parameters-map.log`, and `dummy-parameter-controls-fixed.log`. The prior 57-target aggregate had 293 processing assertions; earlier 289/63 and 236/59 runs also passed. It found and fixed a selected-map teardown crash: scene destruction emitted selection callbacks after the view's member indexes were already destroyed. The lifetime fix also passed the selected-map exit regression 10/10 times. Known sine input established 0.9 V mean, approximately 12.43 mV AC RMS and 1 kHz FFT peak; all five FFT windows and every offered FFT size (256–262144) were exercised through actual replay. The known pulse fixture established spike count, timing, 16,000-frame exposure, fs/4 TDM lane exposure and candidate-label/export continuity.

Analyzer sample-rate/refresh/wave-point editors now have measured functional evidence: a 10 kHz fallback produces the independently expected 500 Hz tone, point changes produce exactly 1,024 then 768 samples, repaint counts change from 4 to 40 over 800 ms for 5→50 Hz refresh, and overview/source locks preserve and restore configured values. Sweep refresh changes produce 8→50 heatmap paints over 800 ms for 10→60 Hz; broad cadence bounds avoid exact timer assumptions. A real source-rate mismatch was fixed: hub-bound Sweep now uses and displays the exact source rate, with the local editor disabled. Recorded 1,000 / 20,000.125 / 1,000,000 Hz fixtures prove exact readout, source-based limits and cursor timebase; unbound legacy rate edits still persist. These tests close the previously UNRUN parameter functions, without claiming filter-transfer-function certification.

**Function exercise and exhaustive correctness are different claims.** The rows below distinguish actual GUI actions with output/configuration assertions, synthetic-widget actions, service-only paths, numerical limits and genuinely unexercised controls. An untested numerical edge is not described as a wholly unrun function.

Session GUI suite: **PASS**, exit 0 (`visual-qa-preview2/dummy-session-ui-literal-final.log`), expanded SUI01–SUI11 scenarios. The preceding final11 and earlier SUI01–SUI09 runs also passed. The earlier final11 toolbar recording captured 5,120 frames; the expanded run independently validates its own recorded manifest and source-frame origin. All five pages were visited while recording continued; all three export modes used actual file dialogs and asynchronous GUI completion. Directory-picker cancel, auto-generated session name, all four theme buttons and persisted/reopened settings, refused manual connection/recovery, remembered endpoint, close while recording and close during export were exercised. Actual jump-start/end, ±2.5-second skip and mouse-drag slider controls verify asynchronous relative/original-source positions, queue clearing and post-seek raw-byte identity. The short fixture exercises skip bounds; R02 independently covers a non-clamped 200-frame skip. Actual trigger editors select channel 5, falling edge, both TDM-slot choices and 1,050 mV; 1,800 mV suppresses capture before the live threshold edit starts recording. Trigger auto-stop/rearm produced two starts and two stops; reopening restores the edited values. The invalid-destination GUI recovered immediately to a valid capture, the configured reserve-space limit safely stopped recording without filling the disk, and external-source selection connected an independent Dummy without owning or destroying its listener.

Additional application issues found and fixed: editing only auto-stop seconds failed to persist; pair1/3 metadata selected a nonexistent index in the all-four-phase export selector; saving a manual endpoint could be overwritten by a synchronous local-test toggle callback; and hidden storage panes incorrectly suppressed the selected TDM trigger slot. The last issue has a separate failing-then-passing narrow regression. The auto-name assertion was corrected to match the documented `v6_` folder prefix; no production naming behavior was changed for that test.

Auxiliary GUI suite: earlier **PASS**, two complete runs each with 187 assertions and zero failures (`dummy-auxiliary-version-final.log`, `dummy-auxiliary-version-repeat.log`). Actual updater UI made 21 local fixture requests while managed Dummy acquisition kept advancing. Release-button clicks were intercepted by a registered URL handler and compared exactly, so no real browser/download was started. Empty/invalid/transport/malformed/mismatched/unsigned license cases, actual production timeouts, fingerprint copy and idle-dialog Exit/Escape/close were exercised without any real key, signed allow response or activation cache.

Two updater defects were reproduced and fixed: aborting a pending reply could synchronously clear the reply pointer and crash replacement/destruction; and numeric-only version comparison incorrectly ranked final 7.0.0 below shipped 7.0.0-preview.1. Both pending-request subprocess cases now pass 10/10 each, and final/next-preview update scenarios pass. No production endpoint, license policy, signature verification or security permission was changed.

The later 64-target run exposed a stale version fixture in AUX04. The corrected version-relative fixture now passes **187 assertions, zero failures** in `dummy-aux-preview-version.log`; this closes the focused auxiliary failure without changing production behavior. The corrected fixture also passed in the final 64/64 aggregate.

Signed successful activation, signed authoritative denial and valid signed offline-cache success/expiry/migration remain **BLOCKED without an authorized signed fixture**. Ordinary startup is currently in its shipped grace period through 2026-12-21; negative dialog tests instantiate the real compiled dialog directly. This distinction is not a successful license assertion.

Reproduce the acquisition suite from the checkout after the integrated CMake targets are built:

```sh
source ../deps/env.sh
QT_QPA_PLATFORM=offscreen XDG_CACHE_HOME="$PWD/../deps/cache" ../build-preview2/ccv2_dummy_acquisition_e2e
QT_QPA_PLATFORM=offscreen XDG_CACHE_HOME="$PWD/../deps/cache" ../build-preview2/ccv2_dummy_acquisition_e2e --errors-only
```

## 1. Application shell, settings and session controls

| Implemented function | Dummy scenario and expected result | Evidence/status |
|---|---|---|
| Open application; close while idle/receiving/recording/exporting | Start real main window, close in each state; background threads stop safely, final files remain consistent | SUI07–SUI09 PASS receiving/recording/export shutdown; complete recorder manifest and bounded export drain/cancel |
| Five page tabs and hardware/storage side-pane tabs | Switch repeatedly while receiving; each visible page updates, hidden continuous spike analysis remains running | SUI02 PASS: all five tabs during live recording; hidden detector remains running |
| Settings open/close/apply | Open, edit, apply, reopen; endpoint/theme configuration persists; close/cancel does not make unintended changes | SUI01/SUI07/SUI08 PASS connect/save/close/manual endpoint application and reopen persistence |
| Manual host/control/data-port editing | Use loopback source; verify endpoint change is applied to next connection and persisted | SUI07/SUI08 PASS manual endpoint save, refusal/recovery, remembered endpoint and reopened values; C07 PASS queued-command cancellation |
| Settings control-link probe | Probe reachable control port and refused port; separate control/data link badges correct | SUI01/SUI07 PASS reachable/refused connection feedback and recovery |
| Built-in/local external Dummy selection | Switch managed sine/spike and external-source modes; start/stop managed server and restore remembered manual endpoint | SUI01/SUI07/SUI08/SUI11 PASS managed sine/spike, manual endpoint restoration, external dropdown/config and independent listener surviving GUI close |
| Dummy single/dual-port selection | Live data with same port and distinct ports; correct sample stream and control response | A01/A02 PASS |
| Dummy sine/spike waveform selection | Deterministic sinusoid frequency/ADC range and active-vs-silent spikes; UI choice changes generated data | SUI01/SUI07 PASS managed source selection/persistence/connect; waveform numerical checks in Dummy regression/model suites |
| Theme selection buttons and persistence | Exercise all listed themes; visible plots/status/controls receive palette and reload keeps selection | SUI07/SUI08 PASS all four theme buttons, exact manager/config theme and restart restoration; full visual QA separate |
| Pending update close/retry lifecycle | Close application with pending local HTTP fixture; no abort callback lifetime crash, duplicate completion or stale result | AUX09–AUX11 PASS actual pending close/reopen, replacement and destruction; each crash-regression mode 10/10 clean exits |
| Update check and open release link | Show checking/success/no-update/error; open verified release target | AUX01–AUX11 PASS actual UI/request/parser/status/timeout/retry and sanitized URL capture; live deployment/download/browser behavior UNRUN |
| Live start/stop toolbar | Live state/data then Idle; duplicate start rejected; stop preserves valid final recording | SUI01–SUI03/SUI06 PASS GUI; A01/A02 PASS raw/timeline oracles |
| Live/replay view selector | Selector changes visible controls without silently stopping or replacing source | SUI04/SUI06 PASS actual selector use; active-source-preservation variant UNRUN |
| Link, data rate, drops, time/recording status displays | Compare labels/badges to hub counters, source state and storage status | SUI01 PASS connection feedback; all rate/drop/time-label numerical comparisons UNRUN |
| Refused/disconnected data source and reconnect | Error and cleared running state; next valid start receives; epoch changes | A03/A01/A02 PASS; SUI07 PASS refused manual connection/retry GUI; midstream-disconnect GUI status variant UNRUN |
| Endpoint change during queued SPI work | Old endpoint's remaining commands cancelled; new endpoint command succeeds | C07 PASS |
| Config restoration across restart | Edit one setting in each active page, close, reopen isolated config; compare values | SUI08 PASS theme, Dummy profile, manual endpoint and recording/trigger settings; all plot-page setting fields not individually round-tripped |
| License cached/offline authorization and renewal | Valid signed cache honors expiry/grace/device/product; authorized renewal/migration retains integrity | BLOCKED valid signed fixture absent; AUX12 PASS empty cache refusal; no policy bypass |
| License device fingerprint display/copy | Isolated fixture identity appears and Copy button sends exactly that text to the local clipboard | AUX13 PASS exact synthetic identity copied by actual button; no real credential/device identity used |
| License entry/verification/retry/cancel | Exercise empty/invalid/success/error with service-owned authorized fixture | AUX12–AUX16 PASS negative/timeout/retry/idle Exit-Escape-close; signed allow/authoritative denial BLOCKED; current ordinary-startup licensing N/A during grace |

## 2. Hardware control and channel map

All hardware effects in this section are **simulated**. TCP success, a zero reply, or a software shadow update does not prove a physical write, real gain, charge/current output, safe stimulation, or device readback.

| Implemented function | Dummy scenario and expected result | Evidence/status |
|---|---|---|
| Direct 32-bit SPI send and persisted input | Spaces/underscores normalize to exact `spi`+8 hex bytes; saved original field text | C02 PASS |
| Invalid direct SPI length/nonbinary text | No bytes transmitted; explicit error; no shadow-applied signal | C02 PASS |
| Global analog reset/release | Click both buttons; exact opcodes `0x07` / `0x08` | C01 PASS |
| Global DAC on/off | Exact opcodes `0x09` / `0x0d` | C01 PASS |
| Global CBOK low and Dummy | Exact `0x12` / `0x00` commands and reply handling | C01 PASS |
| High/low gain all channels | Exactly 256 REC writes plus 256 zero spacer commands per button; block/channel/data ordering correct | C03/C08 PASS (wire + simulated state); physical gain BLOCKED |
| Quick-command editor save/custom multi-command/aliases | Save custom valid commands, execute exact sequence; alias expands both gains; reopen persisted definition | PUI PASS custom multi-command Save/execute/persist (two independent register writes); gain alias executes all 256 writes; C03 full built-in gain sequences |
| Quick-command editor invalid/reset/cancel | Invalid entry rejected; reset previews defaults; cancel does not persist; valid save updates buttons | Map GUI PASS malformed Save warning/Cancel preserves config; Reset+Cancel and Reset+Save both verified |
| Stimulation block/outer channel/electrode/amplitude/polarity/step/compensation | Configure values; output/off emits zero preamble then exact independent address/data bits; config persists | C04/C08 PASS (wire + simulated state); physical stimulation BLOCKED |
| Stimulation on/off | Expected OFF_STIM bit switches; response and status shown | C04/C08 PASS simulated-model state |
| SPI control console clear | TX/RX/errors visible; clear empties console | C07 PASS |
| Short/incomplete reply | Command already sent still emits applied-shadow event; link status becomes degraded, not fully confirmed | C06 PASS |
| Unreachable control/retry | No applied-shadow event for unsent command; next successful send recovers | C06 PASS |
| Map rubber-band multi-selection and Ctrl+wheel zoom | Drag across electrodes, add selection, scroll and modified wheel; selected IDs/zoom match gesture | PUI PASS actual rectangle selects exact intersected ADC set; Ctrl+wheel +10 geometric zoom and reverse restores exact scale |
| Map electrode click and address details/history | Click routed electrode; block/local/global/SPI address and selection agree | Map GUI PASS mouse click adds history and selects one ADC; exact displayed address strings/physical geometry UNRUN |
| Map zoom slider, +/- buttons, fit-to-window and routed-only filter | Zoom changes geometry; fit restores chip extent; filter restricts displayed electrode set without corrupting selection | Map GUI PASS +/-/fit 100% and 1,024 vs 512 selectable electrodes; manual slider-drag UNRUN |
| Block-range/local-channel batch select/unselect | Valid ranges select exactly expected channel set; invalid input reports error and preserves set | PUI PASS block/local-subset exact selected IDs and removal; invalid block/global warnings preserve prior selection |
| Select-all, clear selection and global-channel apply | All 256 routed channels or explicit list/ranges selected; correct deduplication/bounds | PUI PASS all 256/clear/explicit selected ID set and invalid global-range rejection |
| Legacy SPI address-preview slot | No address-preview widget or connected action is constructed in the active SPI panel | N/A unreachable helper |
| REC fields: reference/low-LP/high-power/gain/high-pass/trim/off/reset | Each selected field changes only its protocol bits for selected channels; unselected and mixed fields preserved | Map GUI PASS every editable field, exact selected-register word 0x0F93; mixed gain sentinel and shadow persistence |
| DAC fields: amplitude/polarity/compensation/step/electrode/off | Each field correct, electrode remains independent of outer channel address | Map GUI PASS every editable field, exact selected-register word 0x7D55; C04/C08 independent electrode packing |
| DAC_CT fields: positive/negative duration/global/local frequency/polarity-first/amp×20/on | Each field correct in chosen channels | Map GUI PASS every editable field, exact selected-register word 0xFD78 |
| Write current register tab | Correct per-channel read-modify-write sequence with zero spacers and selected inter-command delay | Map GUI PASS each REC/DAC/CT tab into actual Dummy state; C05 PASS byte-exact zero-spacer sequence |
| Write all register sections | Correct REC/DAC/CT sequence per selected channel; no duplicate or omitted writes | Map GUI PASS 3 registers × 2 channels + 6 dummy commands and target register state |
| Busy disable/repeated batch click prevention | In-flight batch disables writes; drain reenables; cannot snapshot half-updated shadows | Map GUI PASS write disabled while queue busy and reenabled on completion |
| Successful-send shadow feedback, mixed indicators and persistence | Successful/sent-but-unconfirmed writes update software shadow, mixed indicators and persisted snapshots; unsent writes do not | Map GUI PASS exact persisted REC/DAC/CT shadow words and mixed gain sentinel; C06 PASS sent/unconfirmed vs unsent distinction |

## 3. Raw recording, trigger and offline exports

| Implemented function | Dummy scenario and expected result | Evidence/status |
|---|---|---|
| Save directory text/browse, session name/automatic name | Chosen path used; browse cancel preserves path; empty name creates legal time-based folder | SUI02/SUI03 PASS directory accept/cancel, named and auto-generated sessions |
| Split selection: none/1/2/5 GB | UI writes exact byte options; bounded test limit exercises actual split mechanics without multi-GB artifact | SUI02 PASS all four UI byte mappings; A01/A02 PASS bounded 64-frame actual split (multi-GB file size not exercised) |
| Start record while live; repeated click/start guard | Raw bytes match live subscriber payload exactly; second start does not replace active path | SUI02 PASS actual toolbar; A01/A02 PASS exact bytes and idempotent service start |
| Stop record while acquisition continues | Manifest finalizes and raw closes; live frame count continues increasing | SUI02/A01/A02 PASS |
| Stop/restart recording; stop whole source during record | New session receives raw; whole-session stop drains producer/distributor and finalizes recorder | SUI03 PASS GUI stop/reconnect; A01/A02 PASS drain/finalization |
| Record immediately from stopped/live-start state | Main-window pending-record path begins only when connection ready; failure clears pending state | SUI03 PASS idle-record auto-connect/start/stop; GUI pending connection-failure variant UNRUN |
| Recording status/edit lock/free-space warning | Inputs disabled while recording; duration/bytes/free space correct; limits stop safely | SUI02 PASS edit lock/unlock; SUI10 PASS configured reserve-space stop with finalized raw frames and live stream continuing; actual disk-full I/O fault UNRUN |
| Raw split manifest and validity/provenance | Every file frame-aligned; totals match; live endpoint/rate/TDM/analysis config changes persisted; no losses falsely marked clean | A01/A02 and SUI02 PASS |
| Recording directory failure | File-as-parent path fails without active recorder; later valid directory succeeds | A03 and SUI10 PASS invalid destination, explicit GUI failure and immediate valid retry after lifetime fix |
| Disconnect during recording | Recorder stops; inactive/incomplete manifest with stop reason, never false completion | A03 PASS after lifetime fix |
| Trigger enable/channel/threshold/rising/falling | Live sine crosses configured level; callback epoch matches source; starts exactly one recording while active | T01 PASS all backend directions; SUI06/SUI08 PASS actual channel 5, falling edge, 1,800 mV suppression then 1,050 mV capture, exact saved conversion/configuration and reopen |
| TDM trigger electrode slot in 0/2 and 1/3 pairs | All four physical phase selections produce real source threshold recording, metadata consistent | T01 PASS all four backend phase selections; SUI06 PASS both actual slot editor values and persistence, live capture with pair 1/3 slot 1; hidden-pane TDM slot regression PASS |
| Trigger auto-stop and rearm/manual stop | Positive duration stops at selected time and rearms; zero requires manual stop; stale callback cannot start wrong session | SUI06 PASS: isolated auto-stop edit persisted, two captures and stops, rearm, manual disarm |
| Load BIN for export; cancel dialog | Selected source recognized; manifest sample rate/TDM defaults applied; cancel preserves prior selection | SUI05 PASS actual source/cancel chooser, TDM metadata and all-four selector |
| Raw ADC export | 256 int32 little-endian channel files, every sample exactly equals source low 12 bits; explicit export rate equals GUI-prefilled manifest rate | E01 PASS every numerical sample; SUI05 PASS actual GUI mode selection and 256-file export |
| Raw 32-bit export | 256 files, every complete word including timestamp matches source | E02 PASS every source word; SUI05 PASS actual GUI mode selection and 256-file export |
| Four-phase TDM export | 1024 files, all samples routed by source frame modulo four, all four int64 source timeline files exact | E03 PASS all source samples/timeline; SUI05 PASS actual GUI chooser and asynchronous 1,024-file export |
| MATLAB loading helpers and metadata | Correct scripts, filenames, sample rate and counts emitted; helpers are inspected as text | E01–E03 PASS; executing MATLAB/Octave BLOCKED unless runtime available |
| Export source error/cancel/shutdown | Missing source/cancel flag produce explicit error; GUI shutdown waits for export safely | E04 PASS source/cancel errors; SUI05/SUI09 PASS chooser cancellation and active-export window destruction without hang |

## 4. Replay and timeline

| Implemented function | Dummy scenario and expected result | Evidence/status |
|---|---|---|
| Open recorded BIN/session part | Full multipart session loads paused; source rate/TDМ/analysis provenance restored | SUI04/SUI05 and R01 PASS |
| Replay play/pause/resume/end | Raw replay bytes identical to live capture; pause stops new distribution; end emits finished | R01/R02 and SUI04 PASS actual play/pause and source/byte oracles |
| Jump start/end and ±skip | Exact position and source-rate conversion; bounds clamp | SUI04 PASS actual start/end/±2.5-second buttons, exact file/source coordinates, boundary clamps and paused state; R02 also proves non-clamped skip arithmetic |
| Slider seek including paused/playing state | New epoch; queues cleared; first sample belongs to selected source offset; no stale pre-seek data | SUI04 PASS real mouse-drag/release, exact asynchronous fraction/source position, paused state, epoch/queue reset and post-seek raw bytes; R02 service oracle supplementary; playing-state gesture variant UNRUN |
| Replay unload/file-chip close | Source stops and returns to Idle; controls reset | SUI04 PASS actual chip; R02 PASS backend |
| Missing BIN/malformed manifest recovery | Explicit Error, no silent split-session truncation fallback; valid next file recovers | R03 PASS |
| Replay backpressure | Full queue does not advance source; draining resumes with exact source prefix | R04 PASS |
| Missing split/truncated raw/invalid frame regions | No falsely valid analysis; explicit failure/warning | UNRUN E2E; existing session/validity tests supplementary |
| Record while replaying | Replay bytes retained, source provenance remains replay; clean end finalizes | R01 PASS backend capability; SUI04 PASS GUI deliberately disables recording during replay |

## 5. Array realtime page

| Implemented function | Dummy scenario and expected result | Evidence/status |
|---|---|---|
| Edit/apply channel ranges; load channel-map selection | Correct visible channels, invalid input rejected, state persists | PUI PASS channel/apply controls, exact map-loaded IDs 0,2 and source run; realtime invalid-input dialog variant UNRUN |
| Origin/analyzer waveform box zoom and reset | Right-button box selection maps to visible linear/log ranges; short click/outside click/right double-click resets | PUI PASS actual analyzer waveform and spectrum right-drag zoom and exact rendered restoration on double-right-click; shared WaveformWidget; Origin-specific gesture/short/outside-click variants UNRUN |
| Origin/Stack modes | Identical source sample identity across layouts | PUI PASS both visible rendered views; exact cross-view sample readback not asserted |
| DC/AC/high-pass display | DC includes baseline; AC removes it; high-pass signal matches configured processing | PUI PARTIAL: all modes change actual signal/axis pixels; independent transfer-function values UNRUN |
| Reference off/CAR/median | Actual channel numerical outputs reflect reference, constant common-mode removed; setting global/persisted | PUI PARTIAL: all choices reach shared hub mode; GUI numerical output equivalence UNRUN |
| Fixed/adaptive stack Y scale | Correct displayed limits and no rescaling discontinuity in data | PUI PASS control/rendered trace/axis change; exact axis values not independently read back |
| Points/apply X axis/refresh rate | Correct sample window and time hint at source rate; settings persist | PUI PASS edits/apply and updated stream; exhaustive window/axis numbers/persistence variants UNRUN |
| Global TDM on/off and 0/2 vs 1/3 phase pair | Correct physical lane identity, fs/4 lane sample rate, linked pages and recording metadata | PUI PASS shared pages, 512vs256 lanes, N/4 exposure, phase/stride provenance; SUI02/R01 PASS metadata |
| Heatmap AC RMS/P2P/mean/saturation | Metrics numerically consistent with Dummy source, selected-channel highlights | PUI PASS analytic sine RMS bound; PARTIAL rendered distinct other metric modes without numerical readback |
| Heatmap channel-window click | Correct plotted neighborhood/selection and stable layout | PUI PASS ADC 57 click selects exact 32-channel neighborhood 41–72 |
| Start/stop/page activation/seek resets | No new receiver per view; clear stale timeline on seek, hold final frame on stop | PUI/SUI PASS hidden detector, unload/reopen and source pause; all rendered-view stale-frame identities not independently asserted |

## 6. Analyzer page

| Implemented function | Dummy scenario and expected result | Evidence/status |
|---|---|---|
| Channel edit/group selection; load from map | Up to two normal channels/groups; numerical output and labels match selection | PUI PASS manual 1;3 source groups and exact map-loaded 0,2 selection with known-tone recovery |
| 256-channel overview toggle | Exactly all channels; 0.5 Hz/1024 window/FFT-off local limits; returning restores ordinary settings | PUI PASS 256 outputs, FFT disabled/locked, return to two-channel ordinary mode |
| Global TDM/pair and stats-source selection | Correct lanes, effective rate and selected statistics; overview's local TDM-off does not corrupt global state | PUI PASS both pair modes/four outputs/1kHz frequency; statistics source label switches CH2 |
| Refresh rate/sample rate/wave points | Buffer/time axes and persisted parameters correct | PUI PASS actual editors: 10 kHz fallback → expected 500 Hz tone; exact 1,024/768 sample windows; measured repaint cadence; saved/restored parameters and overview/source locks |
| FFT enable/point counts | Spectrum computed only when enabled; expected size/frequency for sine | PUI PASS enable/off and all 11 offered sizes 256–262144; each genuinely filled from 266,240-frame sine source and recovers 1 kHz Fin |
| FFT windows Rect/Hann/Blackman/Blackman-Harris/Kaiser | Actual backend calculation changes; known sine peak retains correct frequency | PUI PASS all five windows preserve known 1 kHz Fin within bin tolerance |
| DC exclusion/signal bins/bandwidth | Statistics calculation honors configured bands/bins | PUI PARTIAL changed controls and finite recomputation; independent full bin/bandwidth numerical oracle UNRUN |
| IRN gain | Numerical input-referred noise scales inversely with selected gain | PUI PASS doubling IRN gain halves measured input-noise result |
| Spectrum power dB/noise-density view | Correct quantity, unit and axis label/scale | PUI PARTIAL control/rendering states; independent density-spectrum numerical readback UNRUN |
| Time statistics and FFT/SNR/SNDR/ENOB/noise tables | Known sine statistics plausible; no NaN/stale values; chosen output matches stats | PUI PASS two-channel DC mean/AC RMS/Fin and finite statistics; independent all-derived-metric oracle remains separate numerical tests |
| Reset best values | Max/min/best history resets without losing active stream | PUI PASS reset control leaves valid recomputed FFT statistics; full best-history tracking oracle UNRUN |
| Legacy pause-keep-capture setting | Retain compatibility configuration without presenting an inactive acquisition control | N/A hidden in current hub mode; full_app_visual_smoke verifies hidden widget |
| Hidden connectivity-probe controls | Deliberately hidden in current page; use Settings probe instead | N/A hidden |

## 7. Sweep page

| Implemented function | Dummy scenario and expected result | Evidence/status |
|---|---|---|
| Channels/window/Y range/refresh and source-rate readout | Correct lanes, span, units, repaint rate and source-controlled timebase | PUI PASS channel/time-window/Y edits, measured refresh paints, exact 1,000 / 20,000.125 / 1,000,000 Hz source readout/limits/cursor; source fs is deliberately noneditable; legacy unbound fs editor/persistence also passes; exhaustive parameter combinations UNRUN |
| Wideband/Spike/LFP | Actual filtered outputs appropriate to selected band | PUI PARTIAL all three actual filtered views render selected channels; independent transfer-function proof UNRUN |
| Notch off/50/60 Hz; high-pass; Spike low-pass; order2/4/6/8 | Numerical processing responds to each configuration; finite output; persisted values | PUI PARTIAL all notch/order paths and HP/LP controls run with nonzero metrics/rendered outputs; independent filter-response proof UNRUN |
| RMS/absolute threshold and polarity | Displayed crossing/detection thresholds consistent with units/direction | PUI PARTIAL configured sweep pipeline executes; independent crossing-marker count UNRUN |
| Raw heatmap RMS/P2P/mean | Metric based on raw all-channel source, not only filtered visible lanes | PUI PASS all 3 labeled metric rendering states; independent all-metric numerical equivalence UNRUN |
| Heatmap click window/single-channel modes | Chooses correct neighborhood or sole channel | PUI PASS ADC 57 click selects 55–58 neighborhood; single mode removes/adds only ADC 57 preserving others |
| TDM/pair | Correct four-phase lane identity and fs/4 processing | PUI PASS exact physical sweep electrode lane lists for both 0/2 and 1/3 pairs |
| Hidden page-local start/stop/pause buttons | Deliberately hidden; global toolbar owns session | N/A hidden |

## 8. Continuous spike analysis, inspector and candidate sorting

| Implemented function | Dummy scenario and expected result | Evidence/status |
|---|---|---|
| Continuous session detector | Built-in spike source yields active-lane events; silent lanes low; continues with page hidden | PUI PASS32-pulse fixture count/timing/exposure; SUI02 PASS hidden analysis during live Dummy record |
| Gain60/180 input-referred scaling | Peak amplitude and thresholds convert as configured; provenance updated | PUI PASS identical input yields one third amplitude at 180x and gain provenance=180; physical gain UNVERIFIED |
| RMS/absolute µV threshold; polarity | Actual event count/threshold follows mode, magnitude and direction | PUI PASS absolute thresholds,4× RMS/noise ratio and positive/negative configured threshold sign |
| High-pass and notch off/50/60 | Detector restart/configuration update reflected in analysis provenance/output | PUI PASS configuration paths/nonzero detection; independent filter-transfer numerical proof UNRUN |
| Pre/post window and retained count | Waveform sample length and bounded retention match requested units/count | PUI PASS: 0.6 ms / 1.5 ms gives 43-sample snippets; exactly 20 retained and≥30 detected |
| Y scale/auto-range/fade/columns | Rendering and current data labels consistent; parameters persist | PUI PASS scale/auto/fade rendered settings and actual 16-column layout verified by ADC 1 mouse hit; all display persistence variants UNRUN |
| Pause display/resume | Rendering paused only; acquisition/recording/detection counters continue | PUI PASS hidden+display-paused detector retains full 16,000 source-sample exposure and pulses |
| Clear retained events | New analysis interval/retention starts; source stays running | PUI PASS coherent empty/new interval while source lifecycle preserved |
| Analysis details expand/collapse and quality summary | Visible source-time coverage, stale/partial/replay state truthfully shown | PUI PASS actual expand/collapse; bare-source validity uncertainty retained in summaries/JSON |
| Click lane -> reusable detail window | Correct lane/timing/threshold/rate and snippets, no extra receiver | PUI PASS array click selects ADC 1, correct lane inspector, close/reopen reuses one window |
| Inspector per-lane threshold override | Only selected physical electrode overridden; disable restores global threshold; TDM pair identity preserved | PUI PASS editor and actual threshold drag reach 60 µV; removal restores 30 µV; cross-pair override persistence UNRUN |
| Inspector Y/auto/fade/overlay count | Controls affect detail rendering and are reused consistently | PUI PASS exact 350 µV / 40 overlays/auto-on/fade-off saved and restored on reused-window reopen |
| Selected-lane export | CSV events/waveforms + JSON metadata contain retained snapshot, source frames/times, candidate labels and parameters | PUI PASS actual export/cancel; JSON scope/lane/count/source-validity/candidate labels/waveform lengths; not complete-event recording |
| Candidate workspace open/change lane/close/reopen | Snapshot corresponds to selected existing store lane; no extra detection thread | PUI PASS real page open/reusable workspace and actual lane-spin selection of ADC 1; all 1,024 lane values not individually enumerated |
| Candidate freeze/unfreeze/refresh | Frozen IDs stable; refresh/import updates current retained window | PUI PASS same-epoch frozen snapshot stable while detector advances; manual refresh and unfreeze resume; new epoch invalidates stale frozen snapshot |
| X/Y feature axes | Each axis value matches event feature; scatter changes without changing assignments | PUI PARTIAL all axis choices exercised; exact GUI feature coordinates UNRUN |
| Click/drag selection; append; select-all-visible; clear | Exact IDs selected, filters respected | PUI PASS point selects exactly one event; Shift preserves prior identity; rectangle/select-all/clear and assignment gating |
| Candidate unit1–32 assign/unassign/filter | Existing events get requested labels; all/unassigned/unit filter consistent | PUI PASS selected unit assign/remove, real store labels, empty-filter guard and exported Manual classification distinct from automated rules; all 32 values not individually enumerated |
| Candidate waveform/feature/source-time raster/ISI | Same event IDs across plots; ISI excludes cross-boundary/gapped invalid segments | PUI PARTIAL workspace renders real events; exact GUI raster/ISI/feature coordinates UNRUN (separate model tests supporting only) |
| Quality details expand/collapse and coverage | Retained-window restriction and validity/label statistics accurate | PUI PASS actual expand/collapse and explicit retained-window/unverified-source-validity summary |
| Epoch/reset invalidates stale selections/labels | Seek/restart/config interval do not relabel unrelated new events | PUI PASS new epoch replaces frozen snapshot on refresh/unfreeze; stale selection after eviction/configuration-change stress remains UNRUN |

## 9. Persistent event archives, waveform rules and whole-recording analysis

NAR denotes `dummy_neural_archive_e2e`: actual built-in spike Dummy, real TCP/SessionHub/raw recorder and real `SpikePanel` controls. The focused **1,635-assertion PASS** recorded **64,000 frames in 16 raw parts**, a complete manifest and zero ingress/recording drops, upstream gaps, repeats, irregular jumps or intra-frame mismatches. Every accepted archive event was independently reopened using bounded reader pages; the count includes repeated page-bound checks and is not a count of distinct functions. The final clean aggregate target passed **1,631 assertions in 12.51 seconds** with the same 64,000-frame/16-part clean recording; the earlier expanded aggregate had 1,637. These counts vary slightly with live timing and bounded reader-page checks, not the scenario inventory. AW denotes `spike_archive_rule_widgets_smoke`: **323 passing assertions with 15 screenshots**, followed by **three final repeated 308-assertion passes** without screenshot saves (`widget-literal-controls-final.log`, `widget-literal-controls-final-repeat-1.log` through `-3.log`), using deterministic synthetic event sidecars produced by the real archive writer. The earlier aggregate had 212 assertions before the literal-button/dialog extension. AW is valid Dummy widget evidence; it does not claim the same source/detector integration as NAR.

| Implemented function | Dummy scenario and expected result | Evidence/status |
|---|---|---|
| Expand archive tools; launch rule editor/archive browser | Actual Spike panel buttons expose reusable real widgets | NAR PASS; archive browser opened by ordinary directory chooser |
| Start live archive and edit destination text | Real detector output is accepted/written with immutable run/event identity | NAR PASS live Start and actual destination text; independent readback |
| Archive destination chooser | Accepted/cancelled dialog changes/preserves the next new directory | AW SYNTHETIC WIDGET PASS actual chooser accept/cancel and exact destination/preserved text; NAR text/service destination path also passes |
| Hide page / pause display during archive | Written-event counts continue while display is inactive | NAR PASS counters advance in both cases |
| Archive exceeds display retention | Event archive remains complete beyond 20 retained snippets per electrode | NAR PASS selected-electrode archive contains more than 20 events |
| Archive Stop-and-drain / restart | Writer finalizes asynchronously while acquisition/detection continue; next archive allowed only after writer terminal | NAR PASS live counters continue; exact drained status and user-stop emission-boundary reason |
| Normal live stop, reconfiguration, replay seek and EOF | Drain pending detector windows and writer; distinguish complete EOF from qualified partial boundaries | NAR PASS live normal drain, config-change interrupted/reconfigured reason, old-timeline seek boundary, EOF Completed with producer/detector drained and zero pending windows |
| Archive status, run identity, revisions and source integrity | No false complete state; all accepted events accounted for independently | NAR PASS increasing IDs, run UUID, detector/rule revision metadata and finite paired 33-sample waveforms; AW PASS unfinalized/Drained/failed-but-draining UI distinctions |
| Rule electrode/candidate/context capture and numeric waveform boxes | Actual detector gain/rate/snippet context captured; edited boxes are explicit drafts | NAR PASS ADC 234, gain 60, source 20 kHz, pre=8 samples and numeric candidate box; AW PASS actual electrode spin 0→1→0 emits exact capture requests, invalidates stale context without applying rules, plus explicit recapture/unapplied draft/six-waveform preview |
| Save/load rules | Ordinary dialogs preserve readable definitions and compatibility context | NAR PASS real Save/Load dialogs and independent rule loader; AW SYNTHETIC WIDGET PASS actual Save/Load accept/cancel preserves correct drafts; invalid-file variants separate |
| Apply rule to future events | Later source frames/IDs receive candidate 7 and an applied revision; earlier archive remains unchanged | NAR PASS future assignment and independent immutable-history comparison |
| Overlapping rules and incompatible context | Ambiguity chooses no candidate; changed gain rejects prior context | NAR PASS candidate overlap → Ambiguous/-1, gain change → Incompatible/-1 |
| Requested vs applied revision, close/reopen | Paused source must not advertise a merely submitted rule as already applied | NAR PASS unchanged source position/zero analyzed samples and prior applied revision after editor reopen |
| Applied rule source-boundary metadata | Persist the first input frame of the applying processing flush, with epoch/continuity identity; never substitute request, crossing or completion time | `spike_rule_worker_smoke` PASS 398 assertions in final aggregate, including pending-window edits, paused/no-input request, startup, reference/timeline change and TDM source-frame cases; this precise oracle is synthetic worker-level evidence, supplementing NAR GUI revision checks |
| Rule mouse-drag box, delete selected box, clear electrode, bind captured context | Canonical box created; deletion/clear affect draft only; incompatible context needs explicit rebind | AW SYNTHETIC WIDGET PASS actual drag/delete/clear/bind, preview and empty-rule submission; these actions are not claimed as live-Dummy service integration |
| Rule compatibility-details expand/collapse | Actual toggle exposes and hides compatibility information | AW SYNTHETIC WIDGET PASS actual toggle opens/closes compatibility-details viewport |
| Archive browser own Browse / Open-revalidate | Chosen/typed sidecar reopens independently and updates current path/status | NAR PASS panel launcher directory chooser; AW SYNTHETIC WIDGET PASS browser's own Browse accept/cancel and Open/revalidate, exact selected path/page; missing-path recovery also passes |
| Archive previous/next pages and electrode/time/epoch filters | Bounded cursor pages preserve IDs; edited-but-unapplied filters cannot change current query | AW SYNTHETIC WIDGET PASS 128/128/14 pages, exact selected IDs, physical electrode, inclusive source times, epoch and overflow rejection |
| Select archive event and inspect waveform/metadata | Selected stable ID controls waveform/revision context | NAR/AW PASS initial and paged selection/rendering; AW actual viewport pointer-click selects exact event 7 and refreshes its empty annotation history |
| Append manual archive label / unlabel | Append-only annotation survives independent reopen without modifying automated label or waveform | NAR PASS actual label button, candidate 11 and run/event identity; AW SYNTHETIC WIDGET PASS label and unlabel history, automated column unchanged |
| Read further annotation history | Bounded subsequent annotation page preserves selected event identity | AW SYNTHETIC WIDGET PASS actual More Annotations over 142 persisted labels; final record #142 and independent 128+14 page readback agree |
| Archive run/source-details expand/collapse | Actual toggle reveals lifecycle and source-integrity details | AW SYNTHETIC WIDGET PASS actual toggle opens/closes run/source-details viewport; quality text checked separately |
| Cancel archive read / close or destroy during read | UI returns promptly; stale asynchronous completion cannot replace current page | AW SYNTHETIC WIDGET PASS actual Cancel Read immediately clears busy state, ignores stale completion, and allows successful new-generation reopen; destruction remains bounded |
| Expand whole-recording analysis; edit paths | Source/output reach dedicated job with separate lifecycle | NAR PASS expansion and text fields via public setters, actual Start/Cancel; AW exact requested source/output and finalizing locks |
| Offline source/output choosers | Source directory and new output destination dialog accept/cancel wire correctly | AW SYNTHETIC WIDGET PASS actual source-directory and new-output chooser accept/cancel, exact text/request paths and cancellation preservation |
| Analyze complete recorded session | Read/processed counts equal all recorded frames; persistent events exceed bounded retained display | NAR PASS actual Start; completed whole-file job, input EOF and all-frame counts |
| Offline/replay agreement and shared-session independence | Every electrode/source-frame event, float waveform and automated label agrees; job leaves current replay unchanged | NAR PASS independent archive comparison, source position unchanged and no acquisition restart |
| Cancel offline job after nonempty prefix | Every accepted event remains readable; explicit Cancelled terminal never claims full-session completion | NAR PASS actual Cancel and independently reopened accepted prefix |
| Immutable raw input | Archive, replay, labels and offline processing do not modify source recording | NAR PASS hashes of every raw part and manifest unchanged |
| Dark/light populated archive/rule/offline surfaces | Readable opaque themed windows and visible event rows/waveforms | NAR PASS six inspected PNGs; AW also renders all four themes and compact layout; arbitrary DPI/window geometry not certified |

The broad waveform box is a diagnostic classifier fixture. Assignment persistence and future-event execution are proved; biological single-neuron isolation and scientific classifier accuracy are not. Source integrity, archive completeness and classification quality remain separate claims.

## Known limitations and exact stopping condition

The Analyzer/Sweep parameter functions previously listed as UNRUN are now exercised with numerical, timing and persistence assertions. The replay slider/jump-end/±skip controls and nondefault trigger channel/threshold/polarity/TDM-slot editor wiring also pass the expanded full SUI run. The archive/rule/offline literal-button extension now passes all previously identified missing controls, including every listed chooser, revalidation, annotation continuation, row selection, details toggle and Cancel Read. **No identified locally testable GUI function in this inventory remains wholly unexercised with Dummy data.** This is an evidence-based function inventory, not a claim that every input combination or numerical quantity has been exhaustively validated. Synthetic widget actions marked PASS already satisfy a narrower Dummy functional-exercise claim; their missing full live-flow variants are integration limits, not wholly unrun functions.

Other remaining variants do not mean the function itself never ran: slider vs button ways to change map zoom; Origin-specific use of the already-tested waveform box zoom; invalid channel-input variants; every channel/candidate number; all setting round trips; stale-selection eviction/configuration stress; active-source view switching; missing split/truncated source, actual disk-full and extreme-load combinations. The individual rows retain these distinctions.

Numerical-proof limits remain for GUI readback of CAR/median, display/filter transfer functions, non-RMS heatmaps, density spectra and candidate feature/raster/ISI values. The relevant controls ran, and independent model/unit checks are supporting evidence, but rendering/configuration assertions are not described as full numerical equivalence.

External or fixture limits: genuine signed license allow/authoritative-denial/cache cases; physical gain/stimulation/filter/wiring behavior; executing generated MATLAB scripts in a real MATLAB/Octave runtime; live updater deployment/browser/download/install behavior. Signed licensing positive/cache cases remain **BLOCKED without an authorized signed fixture**. No simulation result certifies those cases.

Acceptance remains conditional while a reachable implemented action has no Dummy exercise or a current required assertion fails. Functional exercise does not require pretending every parameter combination or hardware/scientific validity claim is proven. External signed-license and hardware boundaries must remain explicit scope exclusions rather than silently relabelled PASS. The final whole-suite gate is now PASS 64/64 with zero failures; the limitations above remain explicit and are not promoted by that result.

The current built-in Dummy models documented register writes, global flags, REC gain/OFF effects and optional synthetic TDM profiles. It still returns twelve zero bytes as a legacy compatibility reply, **not device-register readback**. No undocumented response semantics, physical stimulation effect or hardware safety certification is inferred.

The sweep TDM tooltip/comment incorrectly claimed two-way/fs/2 processing. It was corrected during this validation to describe four phases and fs/4; numerical tests retain the four-phase oracle. The later Sweep source-rate fix keeps filter/timebase calculations synchronized to the exact SessionHub rate.

Legacy compatibility controls: the Sweep command field/label and Analyzer pause-keep-capture checkbox are explicitly hidden when attached to the shared SessionHub. Their configuration remains available for legacy standalone use. `full_app_visual_smoke` asserts all three hidden widget IDs; they are excluded from current visible-function acceptance.
