# Persistent waveform-box candidate rules (version 1)

These deterministic rules label **future detected waveform events**. They do not
perform clustering, principal-component analysis, template sorting, drift
correction, or demonstrate biological single-neuron isolation. Candidate IDs
1–32 are user-defined labels scoped to a physical electrode and acquisition
context. They are independent of display-ring row numbers or lane assignments.

## Coordinates and classification

A `SpikeWaveformBox` has increasing finite `timeMinMs`, `timeMaxMs`,
`voltageMinUv`, and `voltageMaxUv`. Time is milliseconds relative to the **causal
threshold-crossing sample**, not a peak-aligned coordinate. Voltages are
input-referred microvolts. Stored waveform samples passed to the classifier are
input-referred **volts**; the classifier multiplies by 1,000,000 and never applies
input gain a second time.

For waveform sample index `i`, the time coordinate is:

```
time_ms = (i - preSamples) * 1000 * sampleStride / sourceSampleRate
```

`sourceSampleRate` describes source frames before TDM demultiplexing. ADC stride
is 1; TDM stride is 4. The classifier linearly interpolates between successive
samples, clips each segment to the box's time interval, and checks whether its
voltage range overlaps the box's voltage interval. All four rectangle boundaries
are inclusive. A segment can hit a box even when neither sampled endpoint is
inside it. A one-sample waveform can match a box containing its single point.
Zero-area/inverted boxes are invalid; boxes outside the retained time window
simply do not match. Every sample must be finite, even samples occurring after
an earlier matching segment.

A candidate matches only if its waveform intersects **every** box in that
candidate's rule (logical AND). Different boxes may intersect different segments.
Classification outcomes are:

- No matching candidate: `Unassigned`, `unitId = -1`
- Exactly one matching candidate: `Assigned`, with that candidate ID
- Multiple matching candidates: `Ambiguous`, `unitId = -1`, and sorted matching
  IDs in the transient `SpikeRuleClassification` result; no first-match priority
- Invalid waveform, mismatched event metadata, or incompatible configured rules:
  `Incompatible`, `unitId = -1`, and an explanatory reason
- `Manual` is an event annotation status owned by the manual-label workflow, not
  an output from this classifier

Existing event labels are not consulted. Classifying a preview does not itself
change, archive, or relabel an event.

## Immutable snapshots and revision boundaries

`SpikeRuleSet::Snapshot` is `std::shared_ptr<const SpikeRuleSet>`. Use
`SpikeRuleSet::create(revision, electrodeRules, &error)` to validate and freeze
editor values. Values are copied; subsequent changes to the editor do not mutate
a snapshot. Electrode groups and candidates are sorted canonically.

A persisted revision is a nonzero `quint64`. Revision 0 is reserved for events
that captured no rule snapshot. A GUI can use revision 1 for a private draft, but
the acquisition service owns the actual monotonically advanced apply revision.
Loading a file does not authorize silently replacing that service's current
revision: load its definitions, validate compatibility, and apply through the
same revision boundary used for an editor change.

At a causal crossing, capture **both** the immutable rule snapshot and the
crossing's complete `SpikeRuleContext`. Retain these in the pending-window state
until post-samples arrive. Classify the completed window against those captured
values; copy the returned `ruleRevision`, `status`, and `unitId` to the event.
Never look up the current mutable GUI rules or current controls when completing
an old crossing. This preserves the meaning of edits made while windows are
pending. Clearing rules uses an empty electrode list in a new nonzero revision.

The caller owns thread-safe snapshot exchange (for example, a lock or an atomic
shared-pointer exchange), event/archive identity, and detector revision changes.
The classifier has no mutable classification state and no service dependency.
The test holds one snapshot across a simulated edit and proves the old and new
classifications remain distinct.

## Physical context and compatibility

Each electrode group stores its complete `SpikeRuleContext` and a SHA-256
compatibility fingerprint. The identity includes acquisition mode and physical
mapping:

- ADC: `tdmPhase = -1`, `sampleStride = 1`, `electrode = adcChannel`
- TDM: `tdmPhase = 0..3`, `sampleStride = 4`,
  `electrode = adcChannel * 4 + tdmPhase`

A single revision may contain ADC and TDM groups, including groups with the same
numeric electrode index in different modes. The selected TDM display pair does
not enter the fingerprint: moving an unchanged physical electrode to another
lane does not alter its waveform semantics. A rule for another mode must never
be silently reused.

The fingerprint includes all context fields in the header:

- Source sampling rate, stride, input gain, ADC/electrode/phase mapping
- Software reference mode (off/common-average/median)
- Spike-band filter order, notch frequency, high-pass and low-pass settings
- Absolute/RMS threshold mode and values, negative polarity, refractory interval,
  and per-electrode threshold override enable/value
- Causal crossing alignment identifier and pre/post window sample counts

These are the configured values captured for the crossing. Calibration is
conservatively exact; there is no epsilon-based acceptance. Even a stored
currently inactive threshold parameter changing invalidates compatibility. The
changing observed RMS estimate is not a context field: the configured RMS
multiplier is. Displayed band, LFP cutoff, display lane, session/run ID, epoch,
event ID, and detector/rule revisions do not change waveform-box coordinates and
are not fingerprint fields.

The fingerprint input is a fixed Qt 6.0 `QDataStream` in big-endian order,
double-precision floating point, with the domain separator
`nl-spike-rule-context-v1`. The field order is defined in
`SpikeRuleSet::compatibilityFingerprint`. Floating-point signed zero is
normalized. Invalid contexts have no fingerprint. Fingerprints are deterministic
compatibility checks, **not signatures or evidence of file authenticity**.

At classification time the event's physical mapping, source rate, gain,
pre-sample index, and waveform length must agree with its captured context. The
saved rule fingerprint must then match that context. A mismatched rule is
retained but disabled for that event, with an explanation (calibration, rate,
reference, filter, threshold, or window). No configured rule for the electrode
means unassigned; a same-index saved rule from another ADC/TDM mode produces an
explicit incompatibility reason.

## JSON persistence

`toJson()` emits compact deterministic JSON. `fromJson()` validates the entire
file and returns an immutable snapshot or a null pointer plus error.
`save(path)` uses `QSaveFile` atomic commit; `load(path)` performs a bounded read.
Neither function applies the rules to acquisition.

Root structure:

```json
{
  "schema": "nl_spike_waveform_rules",
  "schema_version": 1,
  "rule_revision": "1",
  "classification_semantics": "candidate_labels_not_validated_neuron_isolation",
  "electrodes": [
    {
      "context": { "...": "all fields emitted by contextJson, no omissions" },
      "compatibility_sha256": "64 lowercase hexadecimal characters",
      "candidates": [
        {
          "unit_id": 1,
          "boxes": [
            {
              "time_min_ms": -0.05,
              "time_max_ms": 0.05,
              "voltage_min_uv": -120,
              "voltage_max_uv": -80
            }
          ]
        }
      ]
    }
  ]
}
```

The structure above is illustrative: use the serializer to produce the complete
context and its matching fingerprint. The exact context JSON keys are
`adc_channel`, `electrode`, `tdm_phase`, `sample_stride`,
`source_sample_rate_hz`, `input_gain`, `reference_mode`, `filter_order`,
`notch_hz`, `highpass_hz`, `spike_lowpass_hz`, `threshold_absolute`,
`threshold_volts`, `rms_multiple`, `negative_polarity`, `refractory_ms`,
`threshold_override_enabled`, `threshold_override_volts`, `alignment`,
`pre_samples`, and `post_samples`.

The uint64 revision is a canonical decimal **string**, preserving integers above
JSON's exact floating-point range. Numeric IDs and counts must be finite
integers. Booleans must be JSON booleans, not numbers or strings. JSON `null`
is never a valid numeric placeholder. Unknown/missing fields, unsupported schema
versions, duplicate keys (including escaped duplicate spellings), duplicate
physical contexts, duplicate candidate IDs, and fingerprint tampering all fail
closed. These checks do not partially install otherwise valid groups.

## Resource and numeric limits

- File/document size: at most 4 MiB; preflight depth at most 12
- Strings: at most 128 decoded characters and 768 encoded bytes per token;
  at most 32 fields in any object
- At most 1,024 electrode groups per revision, 32 candidates per group,
  16 boxes per candidate, and 8,192 total boxes
- A group has at least one candidate; a candidate has at least one box;
  an empty root electrode array is valid for clearing rules
- ADC channel 0–255; physical electrode 0–1023 with exact mode mapping
- Source rate 1–1,000,000,000 Hz; finite positive normal input gain at most 1e9
- Reference 0/1/2; filter order 2/4/6/8; notch 0/50/60 Hz; configured spike filter
  frequencies 0–1e9 Hz (the detector owns any effective cutoff clamping)
- Absolute/override threshold within ±1,000 V; RMS multiplier 0–1,000;
  refractory 0–1,000,000 ms
- Nonnegative pre/post counts, total waveform length 1–8,192 samples
- Box time coordinates within ±1,000,000 ms, voltage coordinates within ±1e9 µV;
  each minimum must be strictly smaller than its maximum

These defensive input limits are not recommendations for scientifically useful
settings. Costs are bounded by the configured rule count and retained waveform
length; no training model or third-party numerical dependency is loaded.

## Detection worker API and archive hand-off

`SpikeDetectWorker` preserves its existing constructor and default no-callback
operation. The additional API is:

- `setEventCallbacks(runId, revisionCallback, eventCallback)` before first start
  installs synchronous worker-thread callbacks; callbacks must not throw or wait
  for the GUI thread. The revision callback receives detector revision, rule
  revision, and complete `QJsonObject` metadata. The event callback receives a
  const reference to a batch of owned `SpikeArchiveRecord` values
- `setRuleSet(snapshot)` requests application at the next nonempty processing
  flush. New rule revisions must increase within this worker; clearing uses an
  empty, nonzero revision, not a null pointer
- `appliedRuleContext(lane, &detectorRevision)` captures the complete currently
  applied context and its detector revision under one lock. GUI previews must
  use this atomic pair rather than fetching the revision separately
- `appliedRuleSet()`, `detectorRevision()`, and `runId()` expose applied state and
  the stable worker run identity
- `requestDrain()` requests graceful EOF processing; `drained()` distinguishes a
  finished EOF drain from immediate cancellation

The worker assigns the run UUID and monotonically increasing event IDs before
archive callbacks and before display-ring insertion. Callback records own their
waveform vectors; they are never reconstructed by polling the bounded ring.
Their ring sequence is not their archive identity. The full metadata for every
referenced detector/rule revision pair is delivered before any corresponding
event batch, including when an old pending crossing completes after a newer
revision has already been registered.

Threshold override values and their requested/applied counters are copied under
one configuration lock. Intermediate UI values that never reach a processing
boundary do not create fictitious detector revisions. At each crossing, the
worker retains the immutable rule snapshot, complete context, and exact detector
revision. It classifies the completed waveform before inserting it into the
ring. Rule/threshold edits therefore affect future crossings and cannot relabel
older pending or retained events. Reference changes exclude pending windows and
reset the detector so no waveform mixes two reference schemes. Epoch resets also
exclude old pending state; it is never carried into another timeline.

### Applied boundary coordinates

Revision metadata has schema `nl_spike_detector_revision` version 1, with
`analysis` containing the complete configured detector/reference/gain/threshold
settings and `rule_set` containing the full immutable rule JSON or null. The
root additionally records the following lossless decimal strings:

- `applied_epoch`: the source epoch in which this configuration became effective
- `applied_source_frame`: the boundary in unwrapped source-frame coordinates
- `applied_continuity_segment`: the new effective continuity segment

`boundary_semantics` explains how to interpret that coordinate:

- `subscription_origin`: the startup configuration was initialized at the
  requested subscription origin. This is an initialization marker, **not** a
  claim that any frame at that origin has been observed or processed
- `first_input_frame_of_processing_flush`: the first valid input frame actually
  fed to a lane in the flush that applied a requested configuration change.
  It is not the last decoded frame, a spike crossing/completion time, or the time
  the GUI requested a change
- `epoch_reset_at_first_processed_input_frame`: a new detector revision becomes
  effective at the first valid fed input after an epoch/filter reset, even when
  the configured parameter values and rules have not changed

Empty, paused, all-invalid, and excluded-TDM-phase-only intervals do not fabricate
processing application coordinates. A requested change or pending epoch-reset
revision waits for a nonempty valid processing flush. In TDM mode, the shared
boundary is the first included physical-phase sample; each lane uses that
configuration at its first available sample at or after that boundary. The
reference-change boundary records the incremented continuity segment, not the
previous one. A quiet interval can therefore record an exact configuration
boundary without generating a spike event.

The coordinate identifies configuration application for the selected flush's
first valid included input. Known cancellation is checked before announcing it;
interruption later in the flush can still limit how much input is processed.
Observed exposure, completed events, and cancellation/coverage status are
reported separately and must not be inferred from a boundary marker alone.
These coordinates make applied configuration provenance explicit. They do not
implement or claim automatic reproduction of arbitrary GUI editing histories.

### Bounded ordering and termination

Completed events from all lanes in a processing flush are merged by source frame
and lane before IDs are assigned. Transport is then sliced into batches of at
most 256 records. The raw-frame flush size is chosen using waveform length,
lane count, and record size, targeting roughly 8 MiB of merge-buffer record data
with a minimum of one raw frame and a maximum of 256. A transport batch contains
at most 8 MiB of waveform samples at the 8,192-sample classifier window limit.
These are record-data bounds, not a claim that the process's total resident
memory, container allocation overhead, raw input queues, and filter history fit
inside 8 MiB. Dense multi-lane fixtures verify that the merge and ID ordering do
not change with caller chunk partitioning.

Call `requestDrain()` only after the producer has stopped adding data. The worker
consumes every queued block, publishes final progress/statistics, and explicitly
counts pending windows missing their post-samples as boundary exclusions. A
truncated final raw frame marks coverage incomplete. Immediate shared-stop
cancellation takes precedence over drain, leaves `drained()` false, and preserves
the legacy pending-window visibility; it never claims to have consumed queued
input. Archive writer completion/failure and source-coverage quality remain
separate responsibilities of the owning controller.

## Verification

`spike_rule_classifier_smoke` exercises exact clipped-segment geometry,
between-sample hits, inclusive edges, AND and ambiguity semantics, invalid/NaN
samples and degenerate boxes, ADC/TDM physical mapping, every compatibility field,
immutable pending snapshots, atomic file persistence, deterministic JSON and
uint64 revision round-tripping, malicious/oversized JSON, duplicate keys, and
local/global rule count limits.

A separate section saves rules, destroys the original snapshot, reloads it, and
classifies 240 later held-out synthetic events with amplitude/noise variation:
160 expected candidate events and 80 expected unassigned events. Event labels
are deliberately initialized incorrectly; only computed classifier output is
recorded and compared to generator truth. This proves deterministic persistence
and inference mechanics on that fixture. It is not a biological isolation,
recorded-neural-data accuracy, or false-positive-rate claim.

Focused cloud verification (2026-10-02): the QtCore-only test passed 116
assertions and all 240 held-out fixture events. A separate build with
`-Wall -Wextra -Wpedantic` produced no compiler warnings. The same test passed
AddressSanitizer and UndefinedBehaviorSanitizer with leak detection disabled;
LeakSanitizer cannot run under this environment's ptrace instrumentation.
These focused checks do not substitute for the integrated acquisition/UI suite.


Worker integration verification initially passed 371 assertions in ten consecutive
runs, including 768 synthetic events across all 256 ADC lanes, one-row display
rings, and exact event ID/source-frame/lane/waveform equality between large and
17-frame input chunks. AddressSanitizer and UndefinedBehaviorSanitizer also passed
that integration suite (leak detection disabled for the ptrace environment).
The final provenance additions separately test a quiet nonzero subscription
origin, a paused rule edit, exact first-processing-frame application, a reference
switch, an epoch seek starting with an invalid block, and an offset TDM stream
whose first raw frames are excluded or invalid. All fixtures exercise manually
defined waveform-box candidate assignment mechanics, not biological spike
isolation or neural-data classification accuracy.

The final applied-boundary revision of `spike_rule_worker_smoke` passed **398
assertions**, including the new provenance cases, with
`-Wall -Wextra -Wpedantic` producing no warnings. The final code also contains the
reviewed early-cancellation guard before boundary announcement. This final
focused result is separate from the earlier 371-assertion sanitizer run and from
the aggregate application's integration tests.
