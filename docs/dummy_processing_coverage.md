# Dummy processing GUI coverage

## Scope and evidence rules

`tests/dummy_processing_ui_e2e.cpp` constructs the actual `CommandCenterMainWindow`
and all pages in the compiled CMake application source list. It uses public Qt
controls, ordinary mouse/key events and the real file chooser. It never accesses
private members, injects snippets into a store, substitutes processing algorithms,
or writes expected values into application output.

There are two synthetic inputs:

- The built-in `DummyStreamServer`, restricted to loopback, receives channel-map
  batch writes and a saved quick command through the real SPI queue. Its register
  state is **SIMULATED**, not hardware readback
- A 16,000-frame, 20 kHz, 256-channel raw replay: channels 0 and 2 have an integer
  ADC sine with 2048-code baseline, 40-code amplitude and 1 kHz frequency;
  channels 1 and 3 have 32 negative/biphasic pulses, spaced by 500 source frames;
  other channels have only the DC baseline. The ordinary application replay
  loader decodes the binary bytes

The sine's independent expectations are 0.9 V mean, approximately 12.43 mV AC RMS,
and 1 kHz dominant frequency (finite FFT-bin tolerance). Spike expectations use
injected pulse count/timing, source-frame coordinates, gain ratio, exposure, and
retained-window size. Hardware noise, biological isolation, analog gain accuracy,
DAC output current and real electrode wiring are **UNVERIFIED**.

## Execution status

**PASS (2026-10-02 UTC), final clean aggregate:** all **64/64 tests passed**, with
**zero failures**, normal exit **0**, in **254.02 seconds**. The real-shell processing
test passed **365 assertions** (121.38 seconds), including the new exported
`Manual` classification assertion. The `--map-only` regression passed **67
assertions** (11.18 seconds). The earlier standalone parameter-control path
passed **77 assertions / exit 0**, and the preceding parameter-extended full run
passed 364 assertions before the additive classification assertion.

The final whole-application run includes the auxiliary updater target, which
passed **187 checks / zero failures**. The earlier 57/57 aggregate remains
historical evidence from before the neural archive layer.

The selected-map teardown regression also passed **10/10 separate process
exits**; those repetitions preceded the additional register-field/history/error-
dialog assertions, which passed in the later short and full runs.

Rubber-band selection and Ctrl+wheel up/down passed their exact selection/zoom
oracles in both final runs. The freeze tooltip also now explains that a new
timeline clears stale selections and refreshes the snapshot.

Evidence in the surrounding verification workspace:

- `visual-qa-preview2/ctest-preview2-final-clean.log`: authoritative final 64/64 aggregate pass, zero failures, 254.02 seconds
- `build-preview2/Testing/Temporary/LastTest.log`: integrated detailed processing 365 and map 67 assertions
- `visual-qa-preview2/dummy-processing-parameters-full.log`: preceding parameter-extended full run, 364 assertions, exit 0
- `visual-qa-preview2/dummy-processing-parameters-map.log`: preceding standalone map run, 67 assertions, exit 0
- `visual-qa-preview2/dummy-parameter-controls-fixed.log`: rate-fix short path, 77 assertions, exit 0
- `visual-qa-preview2/dummy-parameter-controls.log`: reproduced pre-fix rate mismatch, exit 1 after 35 assertions
- `visual-qa-preview2/ctest-aggregate.log`: prior 57/57 aggregate pass
- `visual-qa-preview2/dummy-processing-expanded.log`: earlier expanded full run, 289 assertions, exit 0
- `visual-qa-preview2/dummy-processing-run.log`: earlier full run, 236 assertions, exit 0
- `visual-qa-preview2/dummy-map-expanded.log`: earlier expanded short run, 63 assertions, exit 0
- `visual-qa-preview2/dummy-map-fields.log`: earlier short run, 59 assertions, exit 0
- `visual-qa-preview2/dummy-map-exit-repeat.log`: 10 short runs, each exit 0
- `visual-qa-preview2/dummy-processing-teardown.log`: original defect backtrace

The suite found a real selected-map exit crash: destruction of `QGraphicsScene`
emitted `selectionChanged` into `ElectrodeMapView::refreshItemStyles` after the
view's item-index members had been destroyed. `ElectrodeMapView` now disconnects
scene callbacks before member teardown. No temporary backtrace or crash handler
remains in the test. Initial test-only timing assumptions were also corrected:
await the 1-second shadow debounce, settle the ordinary file-dialog model, rerun
a replay after FFT-size changes reset buffers, and repaint before scatter mouse
selection.

Reproduce after building the CMake targets:

```sh
source ../deps/env.sh
export QT_QPA_PLATFORM=offscreen XDG_CACHE_HOME="$PWD/../deps/cache"
../build-preview2/ccv2_dummy_processing_ui_e2e --parameter-controls-only
../build-preview2/ccv2_dummy_processing_ui_e2e --map-only
../build-preview2/ccv2_dummy_processing_ui_e2e
```

The latest extension passed all offered FFT sizes (256 through 262144),
heatmap clicking, analyzer editing/load/zoom, candidate point/Shift/freeze/details,
inspector option persistence, map invalid ranges/local subsets and quick
multi-command/alias execution. A separate 266240-frame long sine input is written
in 4096-frame chunks (272629760 bytes on disk) so large FFT windows are genuinely
filled without an equally large generation-memory allocation. Each FFT runs
through the normal replay pipeline and recovers the independent known 1 kHz tone.

Freeze semantics were tested within one timeline: the displayed snapshot stayed
fixed while the detector advanced; manual refresh replaced it, and unfreezing
resumed automatic updates. A test assumption was corrected after verifying that
new replay epochs intentionally invalidate frozen snapshots for stale-ID safety.

A passing target covers its listed assertions, not every possible parameter
combination. Numerical transfer-function certification remains separate.

## Parameter-control supplement

The real-shell scenario edits Analyzer sampling rate, refresh rate and waveform
points, then opens the ordinary replay loader. A 10 kHz fallback interprets the
20-sample-period fixture as the independently expected 500 Hz tone. The visible
statistics contain exactly 1024 and then 768 samples per output after point
edits. The Analyzer rate is disabled while source-controlled, returns to an
editable configured value on unload, and overview mode temporarily locks
refresh/points while preserving and restoring the user's ordinary values.

Actual repaint events, rather than saved settings alone, verify refresh changes:
over 800 ms the measured Analyzer 5→50 Hz setting produced 4→40 waveform paints,
and Sweep 10→60 Hz produced 8→50 heatmap paints. These checks establish the
control's effect and use broad cadence bounds rather than exact timer deadlines.

This scenario found a real Sweep defect: during a 20 kHz replay, its enabled
local rate editor could change filtering and sweep timing to 12 kHz without
changing the source. The source startup also rounded/clipped rates to the old
integer 2–500 kHz range. Sweep now uses the exact SessionHub rate for processing,
timebase and metric windows, displays a synchronized disabled source readout,
and preserves the saved legacy rate. Unbound legacy widgets retain an editable
integer-rate setting and independently tested persistence.

Three short, 2048-frame manifest fixtures exercise **1000, 20000.125 and
1000000 Hz**. Assertions verify exact source/readout values, filter-limit values,
all input frames consumed, and the rendered cursor's independently predicted
position from sample count, sample rate, time span and plot width. The 20 kHz
normal-source restore is also checked. This is source-rate/timebase coverage,
not numerical certification of each filter's transfer function.

## Implemented test inventory

| Compiled feature | Input/action | Assertion level |
|---|---|---|
| Channel-map select all/clear, block/local subset selection/unselection, global IDs, invalid ranges | Actual map controls and warning dialogs | Exact ADC ID sets; bad block/global input preserves prior selection |
| Channel-map REC/DAC/CT/current tab/write all | Every editable REC/DAC/CT field through real SPI queue to built-in Dummy | Exact final words 0x0F93/0x7D55/0xFD78 on two selected channels; busy gating; write-all command count |
| Channel shadow and mixed fields | Successful queue feedback, selection changes | Exact persisted register words; mixed sentinel retained |
| Map electrode/history/zoom/routed display | Actual electrode mouse click, zoom/fit/routing controls | Selection/history update; fit=100%; exact 1024/512 selectable electrode counts |
| Map rubber-band and Ctrl+wheel | Mouse drag rectangle and Control-modified wheel events | PASS: exact intersected ADC set, ±10 zoom, geometry-scale restoration |
| Quick-command editor | Save custom multi-command and gain alias, run, reset+cancel, reset+save, malformed Save+Cancel | Persisted button/config; two independent register writes and alias applied to all 256 ADCs; invalid input warning and unchanged config |
| Replay load/cancel, repeated run, unload/reopen | Toolbar and file chooser | Hub state, all source frames consumed, detector lifecycle |
| Realtime Origin/Stack | Page and view controls with replay | Actual plots visible and rendered |
| Realtime DC/AC/300 Hz highpass | Signal controls with decoded input | Different waveform/axis pixels, not a transfer-function numerical proof |
| Realtime fixed/adaptive scale | Scale controls | Rendered trace/axis difference |
| Realtime channel/X-axis/refresh controls | Actual edit/apply/map-load buttons | Updated stream runs; map IDs 0,2 loaded exactly; point/refresh settings exercised |
| Realtime heatmap click | Mouse input on ADC57 cell | Exact 32-channel neighborhood 41–72 selected |
| Realtime RMS/p2p/mean/saturation map | All metric selections | Known sine RMS numeric bound; distinct rendered metric states |
| Software reference off/CAR/median | Reference selection and replay | Shared processing mode reaches hub; not full reference numerical equivalence |
| Global TDM off/on and phase A/B | Realtime TDM controls | Other pages mirror state; 512 lanes; exposure=N/4; phase/stride event provenance; 256 lanes when off |
| Analyzer channel groups/map-load/box zoom | Manual 1;3, map-load 0,2, right-drag and double-right-click | Actual source choices/output count; known-tone recovery; waveform and spectrum change and exactly restore rendered views |
| Analyzer fs/refresh/wave-point controls | Real edits, replay, overview and unload | 10 kHz source fallback gives 500 Hz; exact 1024/768 samples; actual repaint cadence; disabled source rate and overview locks; saved/restored values |
| Analyzer two-channel time statistics | Known sine replay | Exact output count, expected DC mean and AC RMS |
| Analyzer FFT | 1 kHz replay, all five windows, every offered point count 256–262144 | Fin within FFT-bin tolerance; finite statistics |
| FFT bins/bandwidth/gain/y-axis/reset-best | Actual controls | Finite recomputation; doubled IRN gain halves input-noise result; visibility/control state |
| Analyzer TDM/statistics source | Both phase pairs plus CH2 statistics selection | Four output lanes and expected 1 kHz frequency; selected source label |
| Analyzer 256-channel overview and exit | Overview button + replay | 256 output count, FFT disabled/locked; restores two-channel view |
| Sweep source-rate readout and refresh | 20 kHz replay; 1 kHz / 20000.125 Hz / 1 MHz manifests; refresh edits | Disabled synchronized exact source value; correct source-based limits/cursor timebase; actual repaint cadence; legacy configuration preserved |
| Sweep wideband/Spike/LFP | Each band with replay | Actual selected lanes; rendered data path (not full filter transfer-function proof) |
| Sweep heatmap metrics and clicking | All three metric modes; ADC57 window and single-toggle mouse input | Distinct labeled renders; exact neighborhood 55–58; only ADC57 removed/added in single-toggle mode |
| Sweep notch and filter order | All available notch/order choices, high/lowpass | Nonzero raw metrics; rendered sweep for each order |
| Sweep threshold/polarity | RMS/absolute and positive/negative controls | Configured pipeline executes; not an independent per-marker count |
| Sweep TDM phase A/B | Both pairs while actual sweep is active | Exact physical electrode lane lists (0,2,..14 and 1,3,..15) |
| Global pause/resume during sweep | Actual toolbar while replay is in progress | Source frame and waveform frozen; source resumes to end |
| Spike detector/count/timing/exposure | Known pulse fixture | 32 events within count bound, pulse-window timing, N observed samples, ADC/gain provenance |
| Hidden and display-paused detection | Switch away with display pause enabled | Full exposure and pulse detection continue |
| Detail inspector | Array mouse selection, close/reopen, scale/auto/fade/overlay edits | Correct lane, one reusable window; exact 350 µV/40/auto-on/fade-off values persist and reopen |
| Per-lane threshold override | Inspector control and mouse drag | 60 µV detector threshold; removal restores 30 µV |
| Candidate workspace/manual labels/unit filter | Real page open, freeze/live/manual-refresh/point/Shift/rectangle/select/clear/assign/unassign | One point labels exactly one event; Shift preserves that identity; real snapshot freezes while detector advances and automatically resumes; empty filter cannot label hidden rows |
| Spike/candidate analysis and quality details | Expand/collapse buttons and actual bare replay | Both detail panels toggle; visible summaries disclose unverified source validity |
| Candidate features and reopen | All X/Y feature choices; close/reopen | Controls exercised; one reusable workspace |
| Retained-event JSON export/cancel | Real chooser and export button | Cancel creates nothing; scope/lane/count/labels/waveform length preserved; bare-source validity remains unverified; labels explicitly not validated neurons |
| Clear/reset and replay reconnect | Clear button and toolbar unload/reload | Empty coherent interval; detector repopulates |
| Spike gain 60/180 | Actual gain control + identical replay | Input waveform amplitude becomes one third; gain provenance=180 |
| Spike pre/post and retention | 0.6 ms/1.5 ms/20 retained | 43 samples per snippet; exactly 20 retained, >=30 detected |
| Spike RMS threshold | Four-RMS mode | Published threshold/noise RMS ratio≈4 |
| Spike notch/highpass/polarity/display | All notch values, HP edit, positive threshold, scale/fade | Nonzero detection, polarity sign, rendered retained events |

## Explicit limits / remaining UNRUN cases

- Hardware-only effects remain UNVERIFIED even if Dummy GUI assertions pass
- Filter frequency-response accuracy belongs to independent numerical tests;
  rendering a filtered trace alone does not establish its transfer function
- Manual candidate labels do not demonstrate biological single-neuron isolation
- Stale frozen selections after eviction/configuration change and extreme/high-load
  GUI combinations remain separate robustness cases supported by isolated tests;
  they are not reclassified as full-shell gesture evidence
- Reference controls reach the global processing mode, but numeric CAR/median
  output equivalence is not asserted through the GUI. DC/AC/HP, heatmap modes
  other than RMS, sweep bands/filter settings and FFT density mode have control/
  rendering coverage, not independent full numerical output oracles
- Candidate feature axes are exercised and real rectangle selection/labels are
  validated, but the exact feature coordinates, raster and ISI plot values are
  not numerically read back from the full-shell widgets
- Map drawing's exact physical electrode geometry and history address text are
  not certified by the click/selection/history-count checks
- Connection settings, live managed-Dummy acquisition, recording/triggered
  auto-stop and MATLAB export are covered (or marked UNRUN) in the separate
  session/acquisition coverage reports, not inferred from this test
- Hidden legacy Sweep start/stop/pause buttons and Analyzer connection-probe
  widgets are intentionally excluded from visible-function coverage; the global
  toolbar is the compiled application's visible acquisition control
- Legacy Sweep command and Analyzer “暂停时继续采集” settings have no operational
  effect in the hub-driven path and are now hidden there; their serialized
  values remain available for legacy compatibility
