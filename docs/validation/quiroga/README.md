# NL_source standard ground truth detection validation

## Main conclusion

The actual NL_source C++ `SpikeProcessor` was run on a bounded, published ground-truth benchmark: 16 complete Quiroga recordings totaling 16 minutes. The default negative-polarity configuration had high event precision but missed about half of the labeled spikes at the declared ±2 ms matching tolerance. A separately evaluated positive-polarity configuration improved aggregate recall, with a precision trade-off. This validates an executable detection path and exposes configuration limits; it does not establish general sorting quality or acquisition-hardware readiness.

## Dataset provenance

- Author: Rodrigo Quian Quiroga
- Citation: Quiroga, Rodrigo Quian (2020). *Simulated dataset*. University of Leicester. Dataset. https://doi.org/10.25392/leicester.data.11897595.v1
- Landing page: https://figshare.le.ac.uk/articles/dataset/Simulated_dataset/11897595
- Official public metadata: https://api.figshare.com/v2/articles/11897595
- Official download returned by that API: https://ndownloader.figshare.com/files/21819066
- License verified in the API: [Creative Commons Attribution 4.0](https://creativecommons.org/licenses/by/4.0/)
- Original archive: `Simulator.zip`, 257,950,406 bytes
- Published and locally verified MD5: `52aafe7f04ec4d65ca383536970310cc`

These are published synthetic recordings assembled from spike templates and background noise, not data generated specifically to satisfy this implementation. They are also not real acquisition recordings. Dataset credit and license remain with the author. The flat traces and CSV ground-truth files here are format conversions made for this validation on 2026-10-02; no waveform synthesis or noise adjustment was applied.

The subset is the complete 4-by-4 core grid: `Easy1`, `Easy2`, `Difficult1`, and `Difficult2`, each at filename noise levels `005`, `01`, `015`, and `02`. The archive's additional high-noise, burst, drift, LFP-correlation, short, and precomputed Wave_clus output files were not scored.

Each selected MAT file was directly checked: one channel, 1,440,000 float64 samples, 60 seconds, and three labeled source units. Sample rate is 24,000 Hz: each file's `samplingInterval=0.041666666666666664` is interpreted in milliseconds and independently corroborated by `par.sr=24000` in the archive's example output. Across the 16 files there are 55,330 original truth events and 55,119 inside the evaluation window.

Physical voltage calibration is not specified by these MAT fields. The harness applies a declared convention of 0.001 V per normalized source amplitude unit. This is not a claim about actual microvolt amplitudes. RMS-multiple detection is scale invariant; a 1,000-fold scale check below confirms identical event indices on one complete representative recording.

## Executed algorithm and frozen protocol

`detector_harness.cpp` compiles and links the repository's actual `src/signal/spike_filter.cpp`, using Qt6 Core. It calls `configure(1, cfg)` and then `processChannel(0, input, display, flags)` in 512-sample batches. It records each flagged sample's global zero-based index. It does not reimplement the detector.

The repository HEAD at validation was `f4780565da036695c2521638d9802997b300553c`. This was a working-tree validation, not a claim that all other upgrade files had been committed. Exact hashes of the tested detector source are in `results.json` and `validation_input_sha256.txt`; the source hashes were checked again after the run and had not changed.

Parameters use `SpikeProcConfig` defaults except the verified 24 kHz sample rate: 50 Hz notch, 250–6,000 Hz spike band, second-order filters, 4× running RMS threshold, 1 ms refractory period, and negative polarity. A separate test changes only polarity to positive. The archive's example processing metadata declares positive detection; that motivated this explicit configuration comparison. No threshold, filtering, refractory period, or timing offset was optimized against labels. Positive-mode scores must not be reported as default scores.

Truth comes from each raw recording's `spike_times` and the first cell of `spike_class`, not the archive's previously detected/sorted `times_*.mat` output. Original indices are preserved and interpreted as MATLAB one-based sample indices, then converted by subtracting one. As an index-convention sensitivity check, retaining original indices changes none of the aggregate ±2 ms counts.

The evaluation window is [0.252 s, 59.998 s): first 250 ms RMS warm-up plus a 2 ms guard, and a 2 ms end guard. Both truth and detected events are restricted to that window. Matching is time-ordered, one-to-one maximum-cardinality matching within a symmetric tolerance. A detected event and a truth event each contribute at most one match. Precision is TP/(TP+FP), recall is TP/(TP+FN), and F1 is 2TP/(2TP+FP+FN). Aggregates pool event counts across files.

The primary tolerance was declared as ±2 ms before scoring outputs. Sensitivity results at ±1 ms and ±0.5 ms are retained. No fitted shift or peak realignment is applied: source markers precede prominent waveform extrema, while this causal detector reports threshold crossings. At ±2 ms the per-file median detection-minus-marker offsets range approximately 1.08–1.54 ms for negative polarity and 0.92–1.13 ms for positive polarity. Thus these scores are an event-detection check, not a submillisecond timing-accuracy claim.

## Aggregate results

| Polarity | Tolerance | TP | FP | FN | Precision | Recall | F1 |
|---|---:|---:|---:|---:|---:|---:|---:|
| Default negative | ±2 ms | 26,922 | 4 | 28,197 | 99.985% | 48.843% | 65.627% |
| Positive configuration | ±2 ms | 45,067 | 1,007 | 10,052 | 97.814% | 81.763% | 89.071% |
| Default negative | ±1 ms | 5,580 | 21,346 | 49,539 | 20.723% | 10.124% | 13.602% |
| Positive configuration | ±1 ms | 31,114 | 14,960 | 24,005 | 67.530% | 56.449% | 61.494% |
| Default negative | ±0.5 ms | 1,101 | 25,825 | 54,018 | 4.089% | 1.997% | 2.684% |
| Positive configuration | ±0.5 ms | 1,876 | 44,198 | 53,243 | 4.072% | 3.404% | 3.708% |

Polarity is consequential: on `C_Easy2_noise01`, default negative recall is 11.29%, versus 95.87% for positive mode at ±2 ms. Positive mode is not uniformly better: on `C_Easy1_noise005`, default negative F1 is 97.35%, versus 90.45% for positive mode. Noise also materially reduces recall. `results.csv` contains all 96 dataset/polarity/tolerance rows; `results.json` includes counts, provenance, source hashes, timing summaries, and diagnostic per-source-unit recalls.

## Correctness checks

- Every run consumed all 1,440,000 samples, finished with the RMS detector ready, and produced no nonfinite display values
- On the complete `C_Easy1_noise01` default-negative trace, batches of 1, 512, and 1,440,000 samples yielded exactly the same 3,251 event indices
- Changing that trace's input amplitude multiplier from 0.001 to 1.0 yielded exactly the same indices
- An independent scorer recomputed all 96 result rows and checked metric arithmetic
- The time-matching algorithm agreed with a dynamic-programming maximum-cardinality oracle in 10,000 seeded random small cases
- A circular 0.5-second detection-shift negative control produced only 20.94% precision / 13.75% F1 for negative polarity and 20.30% precision / 18.48% F1 for positive polarity at ±2 ms. This sanity check shows that the main high precision is not explained solely by the permissive window, although close overlaps remain ambiguous

## Reproduction and file layout

From the repository root, run `tools/validation/quiroga/reproduce.sh` with `NL_DEPS_PREFIX` set to a Qt6 installation prefix and Python with NumPy/SciPy. The script is a Linux GCC/Qt6 harness and writes generated fixtures/results under its own directory. It compiles the harness, downloads missing source data through the official public API URL, verifies the published checksum, materializes flat traces and truth CSVs, runs both polarity modes and the invariant checks, and independently checks the metrics. `NL_SOURCE_REPO` can select another checkout; the detected source hashes will be recorded again. The script does not edit the repository.

- `detector_harness.cpp`: actual-detector executable driver
- `run_validation.py`: checksum verification, conversion, execution, and scoring
- `check_metrics.py`: independent scoring and sanity checks
- `reproduce.sh`: exact build and run workflow
- `figshare_article.json`: saved official source metadata and license
- `results.json`, `results.csv`, `metric_checks.json`, `run_validation.log`: reproducible results
- `truth/`: original and zero-based truth markers plus source-unit labels
- `events/`: actual flagged sample indices
- `traces/`: little-endian float64 source amplitudes before the harness's scale multiplier

The compact evidence ZIP excludes the 258 MB original archive, derived raw traces, and compiled executable. Running the reproduction script recreates them from the official source.

## Limits and next validation need

This tests single-channel event detection, not unit assignment, manual cluster quality, automatic sorting, simultaneous multichannel operation, drift robustness, TCP/ADC decoding, packet loss, queue behavior, GUI integration, actual acquisition calibration, or hardware operation. Per-unit recalls in JSON are time-match diagnostics, not sorting scores. The permissive ±2 ms window can associate close overlapping source events ambiguously. No comparison to another sorter was run.

Do not ship a general accuracy claim based on these numbers. The actionable finding is that polarity and noise conditions substantially affect detection recall. Before declaring acquisition readiness, run calibrated representative recordings through the full pipeline, confirm timestamp conventions, and evaluate the intended polarity and threshold configuration with separately defined held-out acceptance criteria.
