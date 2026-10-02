# File: dummy_spike_tcp_server.py
# Purpose: Hardware-free 256-channel spiking stream and deterministic BIN fixtures.
"""Synthetic input-referred spikes, amplified before 12-bit ADC quantization.

The default 60x gain and microvolt units match the built-in C++ Spike source.
The waveform/profile and random sequence are not intended to match C++ byte for
byte. Each channel models at most one synthetic unit, with no overlapping spikes
on that channel. Ground truth describes generated events, not spike sorting.

Wire format is unchanged: 256 little-endian uint32 words per frame; ADC code in
bits [11:0], frame counter modulo 2**20 in bits [31:12]. All truth coordinates
are absolute, zero-based sample positions before that counter wraps.

Examples (no extra Python packages required)::

    python tools/dummy_spike_tcp_server.py --port 10086
    python tools/dummy_spike_tcp_server.py --output-bin fixture.bin --frames 4000 \\
        --truth-jsonl fixture.truth.jsonl --seed 20260726

Listening is loopback-only by default; use --host explicitly for another bind.
Python TCP throughput is machine-dependent; use offline BINs for timing fixtures.
"""
import argparse
from contextlib import ExitStack
from dataclasses import asdict, dataclass
import json
import math
import os
import random
import socket
import struct
import threading
import time

CHANNELS_TOTAL = 256
BYTES_PER_POINT = 4
TIMESTAMP_SHIFT = 12
TIMESTAMP_MASK = (1 << 20) - 1
ADC_COUNTS = 1 << TIMESTAMP_SHIFT
DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 10086
DEFAULT_FS = 20_000
DEFAULT_GAIN = 60.0
DEFAULT_SEED = 20260726
VREF = 1.8
BASELINE_V = 0.9
ADC_LSB_V = VREF / ADC_COUNTS


def _integer(name, value, minimum, maximum=None):
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"{name} must be an integer")
    if value < minimum or (maximum is not None and value > maximum):
        limit = f"{minimum}..{maximum}" if maximum is not None else f">= {minimum}"
        raise ValueError(f"{name} must be {limit}")
    return value


def _finite(name, value, minimum, maximum=None, strict_minimum=False):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f"{name} must be a finite number")
    if (not math.isfinite(value) or value < minimum
            or (strict_minimum and value == minimum)
            or (maximum is not None and value > maximum)):
        lower = ">" if strict_minimum else ">="
        raise ValueError(f"{name} must be finite, {lower} {minimum}"
                         + (f" and <= {maximum}" if maximum is not None else ""))
    return float(value)


def spike_shape(fs: int):
    """Biphasic extracellular spike: fast negative trough, slower positive."""
    _integer("fs", fs, 625)  # at least one sample over a 1.6 ms waveform
    dur = int(fs * 0.0016)
    return [-math.exp(-((i / dur - 0.18) ** 2) / 0.004)
            + 0.42 * math.exp(-((i / dur - 0.45) ** 2) / 0.02)
            for i in range(dur)]


@dataclass(frozen=True)
class SpikeTruth:
    """One generated waveform; unit_id is a synthetic label, not an estimate.

    amplitude_uv is the input-referred waveform multiplier, not the exact
    discrete trough magnitude. The trough offset is the minimum of spike_shape.
    trough_sample/end_sample_exclusive may lie beyond the exported frame range
    when a waveform is cut short at the end of a fixture.
    """
    channel: int
    unit_id: int
    trigger_sample: int
    onset_sample: int
    trough_sample: int
    end_sample_exclusive: int
    amplitude_uv: float
    trough_uv: float


class SpikeStreamServer:
    def __init__(self, host=DEFAULT_HOST, port=DEFAULT_PORT, fs=DEFAULT_FS,
                 noise_uv=12.0, active_fraction=0.55, *, gain=DEFAULT_GAIN,
                 seed=DEFAULT_SEED, collect_truth=False):
        if not isinstance(host, str) or not host.strip():
            raise ValueError("host must be a nonempty string")
        self.host = host
        self.port = _integer("port", port, 0, 65535)
        self.fs = _integer("fs", fs, 625)
        self.noise_uv = _finite("noise_uv", noise_uv, 0.0)
        self.noise_v = self.noise_uv * 1e-6  # input-referred volts
        self.active_fraction = _finite("active_fraction", active_fraction, 0.0, 1.0)
        self.gain = _finite("gain", gain, 0.0, strict_minimum=True)
        self.seed = _integer("seed", seed, 0)
        if not isinstance(collect_truth, bool):
            raise ValueError("collect_truth must be a boolean")
        self.collect_truth = collect_truth
        self.stop_event = threading.Event()
        self.shape = spike_shape(fs)
        self._trough_offset = min(range(len(self.shape)), key=self.shape.__getitem__)
        self.reset_stream()

    def reset_stream(self, start_idx=None, *, seed=None):
        """Reset profiles, random state, pending waveforms, and collected truth.

        With the same seed, output is reproducible regardless of batch size.
        start_idx is an absolute label, not a request to simulate skipped data.
        If omitted, the first build_batch chooses the starting label. Later
        batches must be contiguous; reset explicitly before changing timelines.
        """
        if start_idx is not None:
            _integer("start_idx", start_idx, 0)
        if seed is not None:
            self.seed = _integer("seed", seed, 0)
        profile_rng = random.Random(self.seed)
        self._rng = random.Random(self.seed)
        # Preserve the previous default seeded population and input amplitudes.
        self.rate = []
        self.amp = []
        for _ in range(CHANNELS_TOTAL):
            if profile_rng.random() >= self.active_fraction:
                self.rate.append(0.0)
                self.amp.append(0.0)
            else:
                self.rate.append(profile_rng.uniform(2.0, 45.0))
                self.amp.append(profile_rng.uniform(80e-6, 420e-6))
        self.countdown = [0] * CHANNELS_TOTAL
        self.truth_events = []
        self._next_sample_idx = start_idx

    def _new_stream(self, *, collect_truth=False):
        return SpikeStreamServer(self.host, self.port, self.fs, self.noise_uv,
                                 self.active_fraction, gain=self.gain,
                                 seed=self.seed, collect_truth=collect_truth)

    def input_v_to_code(self, input_v):
        """Apply gain, add 0.9 V baseline, truncate, and clip to 12 ADC bits."""
        code = int((BASELINE_V + input_v * self.gain) / ADC_LSB_V)
        return max(0, min(ADC_COUNTS - 1, code))

    def build_batch(self, start_idx: int, n_frames: int) -> bytes:
        _integer("start_idx", start_idx, 0)
        _integer("n_frames", n_frames, 0)
        if self._next_sample_idx is not None and start_idx != self._next_sample_idx:
            raise ValueError("batches must be contiguous; call reset_stream for a new timeline")
        if n_frames == 0:
            return b""
        buf = bytearray(n_frames * CHANNELS_TOTAL * BYTES_PER_POINT)
        rng = self._rng.random
        gauss = self._rng.gauss
        shape = self.shape
        slen = len(shape)
        gain, noise_v = self.gain, self.noise_v
        baseline_v, adc_lsb_v = BASELINE_V, ADC_LSB_V
        pack_into = struct.pack_into
        for f in range(n_frames):
            idx = start_idx + f
            ts = (idx & TIMESTAMP_MASK) << TIMESTAMP_SHIFT
            base = f * CHANNELS_TOTAL * BYTES_PER_POINT
            for ch in range(CHANNELS_TOTAL):
                input_v = gauss(0.0, noise_v)
                c = self.countdown[ch]
                if c > 0:
                    if c == slen and self.collect_truth:
                        self.truth_events.append(SpikeTruth(
                            channel=ch, unit_id=ch, trigger_sample=idx - 1,
                            onset_sample=idx, trough_sample=idx + self._trough_offset,
                            end_sample_exclusive=idx + slen,
                            amplitude_uv=self.amp[ch] * 1e6,
                            trough_uv=self.amp[ch] * shape[self._trough_offset] * 1e6))
                    input_v += self.amp[ch] * shape[slen - c]
                    self.countdown[ch] = c - 1
                elif self.rate[ch] > 0.0 and rng() < self.rate[ch] / self.fs:
                    # Preserve the original trigger convention: waveform starts
                    # on the following sample, including across batch boundaries.
                    self.countdown[ch] = slen
                # Inline the same conversion as input_v_to_code in this hot loop.
                code = int((baseline_v + input_v * gain) / adc_lsb_v)
                code = 0 if code < 0 else (4095 if code > 4095 else code)
                pack_into('<I', buf, base + ch * BYTES_PER_POINT, ts | code)
        self._next_sample_idx = start_idx + n_frames
        return bytes(buf)

    def drain_truth(self):
        """Return collected onsets and clear the buffer; disabled by default."""
        events, self.truth_events = self.truth_events, []
        return events

    def truth_metadata(self, start_idx, n_frames):
        return {
            "type": "metadata", "schema": "nl-source-spike-truth-v1",
            "sample_rate_hz": self.fs, "channels": CHANNELS_TOTAL,
            "sample_coordinates": "absolute_zero_based_before_20bit_wrap",
            "start_sample": start_idx, "end_sample_exclusive": start_idx + n_frames,
            "front_end_gain": self.gain, "noise_input_rms_uv": self.noise_uv,
            "adc_bits": TIMESTAMP_SHIFT, "adc_full_scale_v": VREF,
            "adc_baseline_v": BASELINE_V, "seed": self.seed,
            "unit_labels": "synthetic_one_unit_per_channel_not_sorting_results",
            "events": "emitted_at_waveform_onset; tail_may_extend_beyond_export",
            "channel_profiles": [
                {"channel": ch, "unit_id": ch, "rate_hz": self.rate[ch],
                 "amplitude_uv": self.amp[ch] * 1e6}
                for ch in range(CHANNELS_TOTAL)],
        }

    def export_fixture(self, output_path, n_frames, *, start_idx=0,
                       truth_path=None, batch_frames=200):
        """Write a fresh deterministic raw BIN and optional JSONL ground truth.

        No sockets are opened. This does not alter the current generator state.
        JSONL starts with metadata; subsequent records have type='spike'. Events
        are recorded only once their first waveform sample appears in the BIN.
        """
        _integer("n_frames", n_frames, 0)
        _integer("start_idx", start_idx, 0)
        _integer("batch_frames", batch_frames, 1)
        if truth_path is not None:
            if os.path.realpath(output_path) == os.path.realpath(truth_path):
                raise ValueError("BIN and truth paths must differ")
            if (os.path.exists(output_path) and os.path.exists(truth_path)
                    and os.path.samefile(output_path, truth_path)):
                raise ValueError("BIN and truth paths must differ")
        stream = self._new_stream(collect_truth=truth_path is not None)
        with ExitStack() as stack:
            output = stack.enter_context(open(output_path, "wb"))
            truth = (stack.enter_context(open(truth_path, "w", encoding="utf-8"))
                     if truth_path is not None else None)
            if truth is not None:
                truth.write(json.dumps(stream.truth_metadata(start_idx, n_frames)) + "\n")
            end_idx = start_idx + n_frames
            for idx in range(start_idx, end_idx, batch_frames):
                output.write(stream.build_batch(idx, min(batch_frames, end_idx - idx)))
                if truth is not None:
                    for event in stream.drain_truth():
                        truth.write(json.dumps({"type": "spike", **asdict(event)}) + "\n")

    def serve_client(self, conn: socket.socket, addr):
        print(f"client connected: {addr}")
        # A connection owns its RNG/countdowns. Concurrent clients and reconnects
        # cannot perturb one another's stream, and each starts from sample zero.
        stream = self._new_stream()
        try:
            conn.settimeout(2.0)
            try:
                cmd = conn.recv(64)
                print(f"recv cmd: {cmd.decode(errors='ignore').strip()}")
            except socket.timeout:
                pass

            idx = 0
            batch = 200
            interval = batch / self.fs
            next_send = time.perf_counter()
            while not self.stop_event.is_set():
                payload = stream.build_batch(idx, batch)
                idx += batch
                conn.sendall(payload)
                next_send += interval
                delay = next_send - time.perf_counter()
                if delay > 0:
                    self.stop_event.wait(delay)
                else:
                    next_send = time.perf_counter()  # generator fell behind
        except OSError as exc:
            print(f"client {addr} closed: {exc}")
        finally:
            conn.close()

    def run(self):
        active = sum(1 for r in self.rate if r > 0)
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            s.bind((self.host, self.port))
            s.listen(5)
            s.settimeout(0.25)
            print(f"Spike dummy listening on {self.host}:{s.getsockname()[1]} "
                  f"(fs={self.fs} Hz, gain={self.gain:g}x, "
                  f"{active}/{CHANNELS_TOTAL} active channels)")
            while not self.stop_event.is_set():
                try:
                    conn, addr = s.accept()
                except socket.timeout:
                    continue
                threading.Thread(target=self.serve_client, args=(conn, addr), daemon=True).start()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--fs", type=int, default=DEFAULT_FS)
    ap.add_argument("--noise-uv", type=float, default=12.0, help="input-referred RMS noise in microvolts")
    ap.add_argument("--active-fraction", type=float, default=0.55)
    ap.add_argument("--gain", type=float, default=DEFAULT_GAIN, help="front-end voltage gain (default: 60)")
    ap.add_argument("--seed", type=int, default=DEFAULT_SEED)
    ap.add_argument("--output-bin", help="write an offline fixture instead of opening a socket")
    ap.add_argument("--frames", type=int, help="number of frames for --output-bin")
    ap.add_argument("--start-idx", type=int, default=0, help="absolute starting sample of an offline fixture")
    ap.add_argument("--truth-jsonl", help="optional generated-event truth for --output-bin")
    args = ap.parse_args(argv)
    if args.output_bin is None and (args.frames is not None or args.truth_jsonl is not None or args.start_idx != 0):
        ap.error("--frames, --truth-jsonl and --start-idx require --output-bin")
    if args.output_bin is not None and args.frames is None:
        ap.error("--output-bin requires --frames")
    try:
        server = SpikeStreamServer(host=args.host, port=args.port, fs=args.fs,
                                   noise_uv=args.noise_uv, active_fraction=args.active_fraction,
                                   gain=args.gain, seed=args.seed)
        if args.output_bin is not None:
            server.export_fixture(args.output_bin, args.frames, start_idx=args.start_idx,
                                  truth_path=args.truth_jsonl)
        else:
            server.run()
    except ValueError as exc:
        ap.error(str(exc))
    except KeyboardInterrupt:
        server.stop_event.set()


if __name__ == "__main__":
    main()
