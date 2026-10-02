# Dummy neural archive integration coverage

## Scope

`tests/dummy_neural_archive_e2e.cpp` uses the compiled `SpikePanel`, a real
`SessionHub`, and the application's built-in `DummyStreamServer` over a fresh
loopback TCP port. It records actual raw Dummy bytes into a split session, then
replays and independently analyzes those files. It does not inject detected
outputs or replace processing implementations.

The new archive/rule/offline widgets are children of the actual panel. The test
uses their ordinary named controls and file choosers to request operations.
Service lifecycle methods are used for the surrounding acquisition, recording
and replay source. This supplements the separate full-shell processing and
session suites rather than claiming that a standalone panel is a main-window
end-to-end test.

## Execution status

**PASS (2026-10-02 UTC):** the final clean aggregate passed **64/64 tests**, with
**zero failures**, normal exit **0**, in **254.02 seconds**. In that run,
`dummy_neural_archive_e2e` passed **1,631 assertions in 12.51 seconds**. The preceding
standalone run exited **0**, with **1,635 assertions**. Counts include bounded
reader-page checks and can vary slightly with live acquisition timing. The actual
Dummy recording contained **64,000 frames in 16 raw parts**, with a complete
manifest and zero ingress drops, recording drops, upstream gaps, repeated
frames, irregular jumps or intra-frame mismatches.

The updated `continuous_neural_analysis_smoke` passed in **2.49 seconds** in the final aggregate. Its
normal-stop assertion now requires monotonic final exposure/counts, zero pending
window bookkeeping and `stoppedEarly=false`; raw-source uncertainty remains
explicit. Unbind and immediate shutdown retain interrupted-state assertions.

Evidence in the surrounding verification workspace:

- `visual-qa-preview2/ctest-preview2-final-clean.log`: authoritative final 64/64 aggregate pass, zero failures, 254.02 seconds
- `build-preview2/Testing/Temporary/LastTest.log`: detailed final integrated 1,631-assertion neural pass
- `visual-qa-preview2/dummy-neural-archive-origin.log`: preceding 1,635-assertion terminal pass
- `visual-qa-preview2/ctest-neural-integrated-first.log`: lifecycle regression
  pass and the initial new integration failure; it is not a whole-suite pass
- `visual-qa-preview2/dummy-neural-archive-run.log`: intermediate UI-state timing
  correction; terminal exit 1
- `visual-qa-preview2/dummy-neural-archive-bounded.log`: intermediate source-origin
  assumption correction; terminal exit 1
- `visual-qa-preview2/neural-archive/`: six populated dark/light PNGs, covering
  analysis controls, rule editor and archive browser

All six screenshots were inspected: readable text, opaque palette-consistent
surfaces, visible event rows/waveforms and no overlapping controls in the tested
sizes. These checks preserve the existing design; they do not certify arbitrary
window sizes or DPI settings.

The intermediate failures were test assumptions, not production changes:
await the panel's 250 ms archive-status refresh before clicking Start; explicitly
record at least 64,000 frames instead of inferring duration from event counts;
and preserve the recorded source-frame origin instead of assuming the first
file frame has source coordinate zero. The final pending-rule assertion checks
unchanged source position and zero analyzed samples while the editor continues
to display the prior applied revision.

The final whole-suite run includes the auxiliary updater target, which passed
**187 checks / zero failures**. Subsequent changes require their own regression
evidence.

## Passed scenarios and independent assertions

| Area | Actual input/action | Required observation |
|---|---|---|
| Live archive | Built-in spike Dummy, real TCP acquisition and archive Start button | Immutable event writer accepts actual detector output |
| Visibility and display pause | Hide/deactivate the panel, then pause its display | Written-event count continues increasing in both states |
| Bounded retention | 20 retained snippets per electrode, longer actual acquisition | Reopened selected-electrode archive exceeds 20 events |
| Rule capture | Capture current applied detector context through the editor | Exact electrode, gain, source sample rate and snippet context |
| Rule persistence | Ordinary Save dialog and independent rule loader | Readable definitions with the captured compatibility context |
| Future-event assignment | Apply a broad diagnostic waveform box | Later source frames/IDs receive candidate 7 and a nonzero applied revision; earlier records stay unassigned |
| Ambiguity | Add a second overlapping candidate and apply | Later events are explicitly ambiguous with no chosen unit |
| Incompatibility | Change the detector gain while retaining the old rule context | Later events report incompatible, with no chosen unit |
| Configuration boundary | Detector gain change during archiving | Prior archive closes with an interrupted/reconfigured reason |
| Archive-only stop | Actual Stop-and-drain button while Dummy is still running | Asynchronous writer terminal verified before reopen; acquisition and detection continue |
| Normal stop | Stop actual live source while recording and analyzing | Detector drains; pending windows settle; archive has verified Drained terminal |
| Persistent identity | Independent sidecar reader and revision lookups | Stable increasing IDs, run UUID, revision metadata, finite 33-sample paired waveforms |
| Manual annotation | Archive browser label button | Independent reopen sees append-only label; automated labels/waveforms remain unchanged |
| Requested versus applied rule | Submit a saved rule before replay consumes samples, then close/reopen editor | Actual prior revision remains displayed until a processing block applies the pending rule |
| Replay seek | Archive real recorded data, then seek | Prior archive closes with old-timeline boundary and explicit early qualification |
| Replay EOF | Archive the whole recording from its start | Completed terminal after producer/detector drain and zero pending windows |
| Dedicated offline analysis | Actual offline Start button on the recorded session | Read and processed counts equal all recorded frames; persistent events exceed display retention |
| Offline losslessness | Independent readback of replay and offline archives | Every physical electrode/source-frame event, float waveform and automated label agrees under the same configuration |
| Cancellation | Actual Cancel button after a nonempty archive prefix exists | Cancelled terminal; every accepted event can be independently reopened; prefix is not called a complete recording |
| Input preservation | Hash each raw recording file and manifest | All hashes unchanged after archive, replay, labels and offline jobs |
| Theme | Actual populated controls, rule editor and browser in dark/light themes | Opaque palette-matching surfaces; screenshots optionally saved for visual review |

The classification box is deliberately broad and is a diagnostic fixture. A
passing assignment check establishes persistence and future-event execution; it
does not establish biological single-neuron isolation or classifier accuracy.
Source integrity and archive lifecycle are asserted separately. Dummy data does
not validate hardware gain, ADC noise, electrode routing or stimulation safety.

## Reproduction

After building the targets:

```sh
source ../deps/env.sh
export QT_QPA_PLATFORM=offscreen XDG_CACHE_HOME="$PWD/../deps/cache"
export CCV2_NEURAL_ARCHIVE_QA_DIR="$PWD/../visual-qa-preview2/neural-archive"
../build-preview2/ccv2_dummy_neural_archive_e2e
../build-preview2/ccv2_continuous_neural_analysis_smoke
```

The standalone archive, rule-classifier, offline-job and widget tests cover
additional bounded-queue/failure/recovery/geometry cases. Their unit-level
results remain distinct from the actual Dummy integration evidence here.
