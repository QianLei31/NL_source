# V7 preview validation report

Date: 2026-10-02. Version: **7.0.0-preview.1**. Source baseline:
`f4780565da036695c2521638d9802997b300553c`. The later docs-only upstream commit
`28805257866a4604c223390faa024f4e2bdc11cb` and its recorded-data documentation are
preserved in this preview.

## Build and regression results

Measured in an isolated Linux x86-64 cloud environment:

- GCC 14.2.0, CMake 3.31.6, Qt 6.8.2 and OpenSSL 3.5.7
- Qt/CMake dependencies extracted locally from official Debian packages after
  signature/checksum validation; system package installation was not needed
- Untouched V6.0.4 baseline: application and test targets compiled; **40/40 CTest
  cases passed**, 16.31 seconds
- Final preview: application and all test targets compiled; **49/49 CTest cases
  passed**, 19.79 seconds, including **21 Python fixture assertions/tests**
- `git diff --check` passed
- Main Qt executable remained running during a bounded three-second offscreen
  startup smoke test. Timeout termination was intentional, not a normal app exit
- Actual populated Qt panel/candidate-workspace renders were inspected, including
  labels, rates, validity warnings, plots and controls; see the preview screenshots

Reproduce the normal project build with a supported Qt6/OpenSSL installation:

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/Qt6 \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build -j 4
QT_QPA_PLATFORM=offscreen ctest --test-dir build --output-on-failure -j 1
```

Windows/MinGW remains a supported documented build path, but this preview was
**not rebuilt on Windows**, and no hardware was connected.

The added regressions cover:

1. Source-frame event times, wrap, chunk invariance, TDM physical identity and gain
2. Ring/event pairing, stale-epoch rejection, monotonic identities after configure,
   candidate-label eviction and combined metadata/waveform memory budgets
3. Source exposure, internal gaps, subscriber queue saturation, invalid/unverified
   blocks, pending EOF windows and excluded boundary crossings
4. Atomic retained-window JSON export and refusal to overwrite on invalid snapshot
5. Continuous analysis while hidden/display-paused; source-time rates across replay
   pauses; seek/config/stop/clear/unbind/shutdown lifecycle
6. Manual feature selection/labels, retained raster/ISI, gap-aware intervals,
   bounded model memory and repeated modeless-window open/close
7. Invalid intervals across recorded splits, seek/replay masks, legacy damaged
   input, malformed metadata, truncated parts and final neural-analysis metadata
8. Corrupted first-channel timestamp: live→record→replay source coordinates agree
9. Theme token expansion across all current themes

The first integrated run found one new regression-test sequencing mistake: it
read a subscriber queue after session stop intentionally cleared it. The test was
corrected to snapshot before stop, then the entire 49-case suite was rerun. No
failed final test is being hidden by the aggregate above.

## Published ground-truth detection benchmark

The actual unchanged C++ `SpikeProcessor` was run on the 16-recording core Quiroga
synthetic ground-truth grid: 16 minutes, 24 kHz, one channel, three source units per
recording, and 55,119 truth events in the declared evaluation windows. The
published archive and CC BY 4.0 license were verified through the author's official
University of Leicester/Figshare record; its checksum matched.

| Configuration | Matching tolerance | Precision | Recall | F1 |
|---|---:|---:|---:|---:|
| Default negative polarity | ±2 ms | 99.985% | 48.843% | 65.627% |
| Positive polarity, other settings unchanged | ±2 ms | 97.814% | 81.763% | 89.071% |
| Default negative polarity | ±1 ms | 20.723% | 10.124% | 13.602% |
| Positive polarity | ±1 ms | 67.530% | 56.449% | 61.494% |

**This exposes a limitation, not an accuracy pass.** Polarity and noise materially
affect recall. The dataset's stored injection markers differ from causal filtered
threshold crossings; no fitted time offset or label-tuned threshold was used.
The ±2 ms matching window is permissive and does not establish submillisecond
accuracy. Positive-mode performance must not be advertised as the default result.

Physical amplitude units are absent in this dataset; the declared conversion
1 mV per normalized amplitude unit is a convention, not calibration. A 1000-fold
scale change and batch sizes 1/512/full trace produced identical event indices in
a representative check. The independent matching scorer was validated against
10,000 random dynamic-programming cases and a time-shift negative control.

[Full method, all 96 result rows, source hashes and limitations](validation/quiroga/README.md)
are included, with an optional reproducible harness under `tools/validation/quiroga`.
These are detection tests, **not sorting-accuracy scores**.

## Repository's original recorded ADC sample

The newly published `ADC_DATA.bin` was downloaded without changing it. Its
339,730,028-byte size and SHA-256
`5cf03681e067c27cb5193c590b363bbd4b3f3d4c38a0186157744bd70e47bba0` matched the
repository metadata. There are 331,767 complete 1024-byte frames and a 620-byte tail.

Actual V7 resolver/replay/seek code read the full complete-frame payload without
alteration; the output hash matched. Start, middle, end, near-end seeks and EOF
restart passed. All **84,932,352 timestamp fields are zero** throughout the complete
frames. The strict timestamp analyzer therefore reports repeated timestamps, but
these cannot distinguish dropped or duplicate payload frames. ADC codes range
1640–3252; no 0/4095 saturation code or adjacent identical whole frame was found.

Sampling rate, TDM mode and front-end gain remain unknown. Replay QA used an
explicit test scheduling assumption, not an inferred acquisition rate. No
quantitative Hz/µV, timestamp continuity, biological signal quality or hardware
readiness conclusion is justified from this file alone. It is correctly marked
unverified, and the partial tail is disclosed and ignored frame-aligned.

## Remaining limitations

- No actual FPGA, stimulator, electrode or per-channel gain readback validation
- No sustained full-array real-time throughput, queue-latency or long recording soak
- Subscriber analysis delivery remains potentially lossy; losses are disclosed
- Retained-window labels/exports are not a persistent complete-event archive
- No automatic sorting, PCA, template matching, drift correction or sorter comparison
- No multi-channel truth benchmark or real biological ground-truth validation
- TDM neural analysis remains selected-pair 512/1024; raw capture keeps all phases
- Mixed configuration changes are request-timed, not atomically applied per sample

The preview is suitable for further software evaluation with these limits visible.
It is not a declaration of full Open Ephys parity or validated research acquisition.
