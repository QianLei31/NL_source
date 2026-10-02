# Full-run spike event sidecar, version 1

This sidecar records **direct immutable detector output**, not periodic snapshots
of the bounded waveform display ring. It is software-analysis output and does
not establish biological single-unit isolation or real-hardware validation.
Existing raw recordings, BIN files and `session.json` are never overwritten or
required to contain these new fields. Older sessions remain readable without a
sidecar. Version 1 readers reject unsupported archive versions rather than
misinterpreting them.

## Lifecycle and integration

Use one fresh/empty directory for one run UUID. `begin()` refuses any nonempty
destination. It creates `events.nlsa` and an atomically replaced `archive.json`.
The first framed record contains generic run/source provenance. Before producing
events, enqueue the actual complete detector configuration and saved rule set
with `enqueueRevision(detectorRevision, ruleRevision, metadata)`. A revision
pair is immutable. Identical registration is idempotent; changed metadata under
a registered pair fails the run. Events may refer to **any previously registered
pair**: a crossing pending across a configuration change retains its original
configuration/rule revision.

The producer assigns a nonzero event ID, monotonically increasing within the
run UUID, before independently inserting the same event in the display ring.
The `(runId, eventId)` identity never aliases display sequence numbers or reused
ring slots. Event IDs need not be contiguous. The archive requires source frame,
source sample rate, input gain, physical electrode mapping and a finite waveform.
A batch owns/copies waveform bytes; source buffers can be reused immediately
once enqueue returns. No ring polling is part of this implementation.

`enqueueBatch()` accepts at most 4096 events, 16384 float32 waveform samples per
event, and 16 MiB serialized payload per frame. Smaller batches should be used
for low latency. `enqueueRevision()` and `enqueueManualLabels()` use the same
ordered queue. The queue includes the in-flight record in its byte limit.
Default queue capacity is 32 MiB (configurable from 256 bytes to 1 GiB). A single
record larger than the configured queue fails explicitly rather than deadlocking.
One additional bounded serialization buffer may temporarily exist during enqueue;
the queue limit is not a claim about all application/display/source memory.

- **LiveNonBlocking:** never waits for disk queue space. Exhaustion fails the
  archive visibly; the caller must surface the failed result/status and stop or
  qualify capture. Already accepted records are drained if the device still
  permits writes. A failed/rejected batch is never reported as accepted.
- **OfflineBlocking:** waits for queue space using a cancellable condition wait.
  Backpressure must also be propagated to the offline source/detector pipeline;
  the archive cannot make a dropping upstream pipeline lossless by itself.
  A supplied atomic cancellation flag is checked at most every 10 ms while
  waiting. Cancellation rejects the waiting batch, not previously accepted work.

Call `finish()` followed by `wait()`. A successful finish drains all accepted
records, writes a checksummed terminal record, atomically replaces `archive.json`,
then atomically creates `finish.json` referencing the terminal checksum and final
byte count. `Completed`, `Drained`, and `Cancelled` are separate terminal states.
`Cancelled` still contains every previously accepted event, but is not a full
source run. Destruction requests cancellation and drains accepted work. Queue
exhaustion, invalid metadata/identity, serialization limits, and I/O/commit errors
produce `Failed`; they cannot be presented as a successful complete archive.
A processing/read error can explicitly call `finish(Failed, sourceIntegrity)`;
`sourceIntegrity["error"]` supplies its diagnostic (or a generic processing-failure
reason is used). Accepted records still drain, but no successful terminal record
or finish marker is written. This remains distinct from a user's cancellation.

`sourceIntegrity` is a separate JSON object. Source gaps, invalid samples,
boundary exclusions, missing/pending windows, unverified source validity, and
source cancellation must be supplied by the pipeline. A successfully persisted
archive may have compromised source coverage, and an intact source can have a
failed archive. Do not infer source integrity from a green archive lifecycle.
An empty run is permitted, but only a verified finish marker qualifies its end.

Qt `QSaveFile` uses atomic replacement with direct-write fallback disabled.
Per-frame flush detects ordinary write failures. This is not a claim of
filesystem/power-loss durability equivalent to an OS-specific fsync of all files
and containing directories; recovery always revalidates stored bytes.

## Binary representation

All integers use explicit little-endian widths. Floating point is IEEE float32
(waveforms) or float64 (sample rate and gain), serialized by copying the numeric
bit pattern to the explicitly little-endian integer encoding. No compiler
structure dumps, native padding, implicit endian assumptions or Qt object
serialization are used. Every waveform sample must be finite.

The 32-byte file header is:

| Offset | Size | Meaning |
| --- | --- | --- |
| 0 | 8 | ASCII `NLSPKAR` followed by zero |
| 8 | 2 | major version, 1 |
| 10 | 2 | minor version, 0 |
| 12 | 4 | header length, 32 |
| 16 | 16 | run UUID in RFC 4122 byte order |

Each record is `prefix + payload + checksum`:

| Prefix offset | Size | Meaning |
| --- | --- | --- |
| 0 | 4 | payload byte length, maximum 16 MiB |
| 4 | 2 | kind: run=1, revision=2, events=3, terminal=4, annotation=5 |
| 6 | 2 | record schema version, 1 |
| 8 | 8 | strictly sequential record ordinal, beginning at 1 |
| 16 | length | payload |
| 16 + length | 32 | SHA-256 over the 16-byte prefix and complete payload |

Length-prefixed strings/JSON use uint32 byte length and UTF-8 bytes. JSON must
parse as an object and is capped at 8 MiB. Unknown versions/kinds, malformed
lengths, truncated frames, checksum failures or invalid semantic references stop
the verified prefix. SHA-256 detects accidental corruption; it is not a signature
or proof against deliberate rewriting.

Payloads:

- **Run:** one length-prefixed compact JSON object
- **Revision:** uint64 detector revision, uint64 rule revision, JSON object with
  the complete applied configuration and actual rule-set definition
- **Events:** uint32 count followed by that many explicit event entries
- **Terminal:** JSON object with run UUID, terminal lifecycle, accepted/written
  counts and last event ID as decimal strings, source-integrity object and error
- **Annotation:** UUID bytes, uint64 event ID, int32 unit ID, uint64 annotation
  ID, UTC ISO-8601-with-milliseconds string, note string (maximum 4096 UTF-8 bytes)

Each event entry contains these fields, in this exact order:

1. 16-byte run UUID
2. uint64 event ID, detector revision, rule revision, epoch, continuity segment,
   display sequence
3. int64 unwrapped source frame
4. float64 source frames/second and input gain
5. int32 lane, ADC channel, electrode, TDM phase, source sample stride,
   pre-samples/crossing index, original candidate unit ID
6. uint8 classification (`Unassigned=0`, `Assigned=1`, `Ambiguous=2`,
   `Incompatible=3`, `Manual=4`)
7. uint32 waveform length, then float32 samples in input-referred volts

The initial automatic classification/unit remains immutable even when a human
subsequently labels the same event. ADC mode has TDM phase -1. Source-time
seconds are source frame divided by source sample rate, with no wall-clock or
GUI-frame substitution. Epochs remain distinct; cross-epoch interpretation needs
the source session's explicit timeline mapping.

## Bounded reopen, querying and recovery

`open()` scans sequentially with bounded memory and validates the full prefix,
including identity ordering, registered revisions and terminal counters. It
retains at most 65536 revision-pair hashes and 65536 disjoint accepted-ID ranges;
exceeding either explicit safety bound fails capture/recovery rather than growing
memory without limit. Contiguous event IDs use one range regardless of duration.
The implementation deliberately uses no unbounded in-memory per-event index.
Opening and cold queries are linear in file length; a persistent sparse index is
an optional future format extension, not a current performance claim.

`readPage()` returns up to 256 events (default 128) with at most one decoded
16 MiB frame, and supports electrode, optional epoch, inclusive source-time range,
and a byte-offset/intra-batch cursor. Its default scan-work budget is 64 MiB.
A sparse query can return no events with `atEnd=false`; preserve its continuation
cursor and continue. Result memory is bounded by the page/event limits, independent
of recording duration. Exact-pair metadata lookup scans without retaining a
revision history in memory.

Recovery never modifies the source archive. A partial/corrupt final batch is
excluded as a whole; every previous complete record remains available. Recovery
stops at the **first** invalid frame, including corruption in the middle, and does
not jump over it to invent a coherent later history. Missing/mismatched finish
markers produce an unfinalized prefix, never a completed run. A valid terminal
record alone is insufficient. A stale finish marker from a longer/corrupted file
cannot qualify a shortened prefix. `verifiedBytes`, `recoveredPrefix`,
`finishVerified`, lifecycle and error are exposed separately to the UI.

## Historical manual labels

During capture, use `enqueueManualLabels()` so annotations follow previously
accepted events even if the writer has not flushed them yet. Each call supports
up to 2048 labels. The writer validates exact accepted event membership and
assigns an increasing annotation ID plus UTC timestamp. This operation never
relabels future events or changes any saved rule set.

After a verified terminal state, `appendManualLabels()` validates a bounded batch
against the archive once and appends framed annotations to `annotations.nlsa`,
serialized under `annotations.lock`. This separate file has the same 32-byte
header, its own sequential record ordinals, and annotation IDs continuing the
active archive's history. The original event stream/finish marker is unchanged.
No annotation overwrite occurs. Later labels are later historical observations;
consumers may explicitly display the latest label alongside original assignment.

`readAnnotations()` pages over inline history followed by the post-close sidecar.
Pass its `nextOffset` unchanged; negative offsets identify sidecar continuations.
A truncated/corrupt annotation tail exposes its valid prefix and recovery error.
Appending to a damaged annotation tail is refused; the original bytes are
preserved. Appending via the static reader API before a verified terminal state
is refused; use the active writer queue to avoid accepted-versus-written races.

## Validation target

`ccv2_spike_event_archive_smoke` uses QtCore and the display snippet store only.
It covers 96 exact events against an eight-event display capacity (12× retention),
waveform byte equality, all event provenance/revisions, late old-revision events,
source-time/electrode/epoch paging, active and post-close historical labels,
immutable automated assignments, slow-writer bounded offline backpressure,
cancellable blocked enqueue, live queue exhaustion without silent drops,
deterministic partial disk-write failure, corrupt/truncated/oversized event tails,
missing finish markers, annotation tail recovery, empty qualified runs and
identity/metadata mutation rejection, explicit processing failure, variable TDM
waveform lengths and identities above 2^53. Test-only delay/failure options default off.
Pipeline/source losslessness and GUI preservation require the integrated tests
in addition to this storage-level target.

## Dedicated full-file offline analysis

`SpikeOfflineAnalysisJob` owns a frozen `SpikeOfflineAnalysisRequest`, a private
`ReplayController`, a bounded queue, `SpikeDetectWorker`, independent bounded
retention store, saved immutable rule snapshot and archive writer. It never
subscribes to `SessionHub` or depends on playback speed, page visibility or display
pause. The synchronous input pull reads every validated part in order and pushes
with drop-oldest **disabled**. Full queues wait with cancellation/failure checks.
Block size is capped at 4096 frames, queue depth at 64 blocks, and their combined
payload at 64 MiB; the default is four 1024-frame blocks. Source/detector in-flight
blocks and archive buffers are additional separately bounded allocations.

Source sample rate, frame origin and known TDM mode/pair come from the recording
manifest. Raw BIN input uses the caller's declared source-rate/mode fallback and
a zero recording-frame origin; the hardware counter is used to reconcile gaps,
not invented as a recovered absolute acquisition origin. Raw input's absent
validity evidence remains visible as unverified source coverage. The job records
original source provenance and the full *applied* detector/rule revisions.
Physical-electrode threshold overrides are mapped only to selected detector lanes.

At normal EOF the producer stops, requests detector drain, joins the detector,
checks exact processed-frame count, then finalizes the archive as `Completed`.
The detector accounts incomplete boundary/tail windows explicitly. Cancellation
stops further source/detector work and commits all already accepted archive
records as `Cancelled`. Read, processing and disk failures produce `Failed`.
Finished status reports read/processed/total frames, detected versus retained
counts, source integrity, writer state, path and error independently.

A short final commit phase is deliberately non-cancellable: immediately before
`finish()` the job atomically sets `cancellable=false`, `finalizing=true`, and
emits progress. Later cancellation requests are harmless no-ops while accepted
tail records flush. The UI must disable its Cancel action at this boundary. This
avoids advertising a cancellation that could not safely change an already
committing terminal state. Progress notifications are throttled to 20 Hz so fast
offline runs cannot accumulate an unbounded queue of GUI notifications.

`sourceIntegrity.archive_scope` is `all_input_frames_lossless_offline`.
`incomplete_coverage` and its compatibility alias `incomplete_analysis_coverage`
remain true unless EOF, exact processing, source validity and detector quality
are all verified clean. `whole_source_coverage_claimed` is true only in that
strict case. A successfully stored archive containing gaps/invalid/unverified
input is still `Completed`, with the separate incomplete-source qualification.

`ccv2_spike_offline_analysis_smoke` exercises multipart inputs, physical timestamp
gaps, validity masks, exact source-rate/TDM restoration, complete saved-rule
provenance, byte-exact waveform equivalence across block sizes and slow disk
backpressure, events beyond display retention, cancellation, read/disk failures,
and non-cancellable final accepted-tail flush. It also captures bytes from the
actual built-in `DummyStreamServer`, fixes detector parameters before observation,
discards the first 8000 frames, and analyzes the following 32000 held-out frames.
This is repeatable simulated input, not physical chip/hardware validation.

## Cancellation and annotation semantic recovery

Reader `open()` and post-close `appendManualLabel(s)` accept an optional atomic
cancellation flag as their final argument. Reopen and append verification check
it between records and during checksum reads in 64 KiB chunks. Annotation-lock
waiting is also cancellable. Cancellation before append commit leaves existing
archive and annotation bytes unchanged. The final bounded append itself is not
interrupted midway by cancellation; once committed, the operation reports success.

Reopen validates the annotation sidecar against the event prefix's exact ID
ranges, not just its checksums. Sidecar record ordinals must be sequential,
annotation IDs must strictly increase after inline history, and each referenced
event must exist. Reordered/duplicated intact frames and valid-checksum records
referencing nonexistent events stop annotation recovery at the first violation.
The original event stream can retain a verified successful finish while a
separate label tail is damaged. Annotation pages report that recovery error.

`SpikeArchiveStatus.writerFinished` on an owned writer distinguishes its actual
worker termination from an early `Failed` notification. In particular, a live
queue overflow fails immediately but may still be draining accepted records.
Do not replace/destroy that writer synchronously under a live detection/controller
mutex until `writerFinished` is true. Reader snapshots cannot establish whether
another process owns a still-running writer and do not claim this thread state.
