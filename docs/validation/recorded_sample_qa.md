# Recorded ADC sample QA

Checked 2026-10-02 UTC. Scope: file-format, parser, transport, seek, EOF, and quality/provenance bookkeeping. No physiological accuracy or acquisition-setting claims.

## Input identity

Public source: https://github.com/QianLei31/NL_source/releases/download/sample-adc-data-20261001/ADC_DATA.bin

- Original filename: `ADC_DATA.bin`
- Bytes: **339,730,028**
- SHA-256: **5cf03681e067c27cb5193c590b363bbd4b3f3d4c38a0186157744bd70e47bba0**
- Metadata taken from remote main commit `28805257866a4604c223390faa024f4e2bdc11cb`
- Original bytes preserved outside the repository. No upload or redistribution performed.
- No original session manifest. Sampling rate, TDM mode, acquisition date, and front-end gain remain unknown.

## Sources exercised

The external C++ harness directly compiled the **V7 working-tree** implementations of `SessionManifest`, `ReplayController`, `TimestampContinuityAnalyzer`, `FrameTimestampReconciler`, and subsequently `SpikeDetectWorker`/`SpikeSnippetStore`/`SpikeProcessor`. This is not a clean build of the commit named by HEAD.

The exercised source hashes are recorded in [recorded_sample_sources.sha256](recorded_sample_sources.sha256) and were rechecked after both harnesses. The harness does not modify application source or input data.

## Full-file findings

- **331,767 complete 1,024-byte frames**, containing **84,932,352 channel words**
- **620 trailing bytes**, containing 155 complete words but not a complete frame; replay excludes them and warns
- Every high-20-bit timestamp field is **zero**, including all 155 tail words
- No intra-frame timestamp mismatches anywhere
- The actual analyzer, both fixed step 1 and auto mode, reports 331,766 checked transitions, 331,766 repeated timestamps/discontinuities, zero estimated missing frames and zero irregular jumps
- Auto mode selects fallback step 1 and sets its `calibrated` field; this does **not** establish a valid hardware clock
- The timestamp reconciler stays inactive and preserves sequential software indices 0 through 331,766; no timestamp-derived gaps inferred
- ADC low-12-bit codes span **1640–3252**, with **1611 distinct codes**, no 0/4095 codes, no all-zero frames, and no consecutive identical complete payload frames
- Refeeding the entire file to the actual analyzer in deliberately fragmented 65,537-byte input chunks produced identical statistics

Repeated zero timestamps must not be reported as proof of lost frames or duplicate recorded payloads. This sample cannot validate hardware timestamp continuity. ADC code range is a format observation, not proof of correct gain, voltage calibration, biological content, or signal fidelity.

## Parser and replay: passed

- Resolver and replay open paused, with 331,767 total frames and the exact 620-byte-tail warning
- Start, middle (frame 165,883), end (331,767), and near-end (331,640) seeks completed and produced exact expected file payloads/source indices where data exists
- Cold middle/end indexing took 34/33 ms on this run; these are observations, not performance guarantees
- Full replay delivered **all 331,767 frames**, 82 blocks, with strictly correct source indices and exact complete-frame payload SHA-256:
  `c01aceadd192f60e62606178dbcbac2f4923472f2a5ce553d02815d58fd507b2`
- EOF emitted `finished`, stopped playback, and a subsequent play restarted at source frame 0 without stale payload
- Full replay used an explicitly artificial **1,000,000 frames/s transport setting** to bound the test, completing in 2.707 s including consumer hashing and queue work. This is not an acquisition-rate inference or 1x timing validation
- Open/seek checks used an assumed 20,000 Hz setting solely to exercise the transport

## Unknown/unverified status: passed

Source provenance reports `has_manifest=false`, `frame_validity_known=false`, `integrity_complete=false`, `original_complete=false`, `ignored_tail_bytes="620"`, plus the raw-BIN integrity-unknown and ignored-tail warning. `tdmKnown=false`. No validity mask is invented.

`integrityUnknown=false` is the V7 deliberate raw-BIN behavior: absent metadata is distinguished from known-damaged legacy data that must be blocked. It does not mean verified clean.

A real first replay block of 4,096 recorded frames was fed into the actual detector worker and store. Result: **4,096 unverified frames**, zero marked invalid frames, all 256 lanes accounted for 4,096 processed samples, and **quality.incomplete()=true**. The block's validity mask remained empty. Processing rate 20,000 Hz, gain 1 and non-TDM were explicitly arbitrary test settings. No spike counts, amplitudes, timing accuracy or biological conclusions are claimed.

## Reproduce

The standalone CMake harnesses are in
[`tools/validation/recorded`](../../tools/validation/recorded/README.md).
Supply the checksum-verified original sample and a supported Qt6 installation.
Both checked executables exited 0. The test harness has one harmless Qt hashing
API deprecation warning; this is separate from application compilation.
