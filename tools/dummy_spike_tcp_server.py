# File: dummy_spike_tcp_server.py
# Purpose: Dummy 256-channel TCP stream carrying realistic spiking activity,
#          for exercising the Spike留存 (spike panel) page without hardware.
# Notes:
# - Frame layout matches the real stream: 256 channels x 4 bytes little-endian,
#   12-bit ADC code in bits[11:0] and the 20-bit frame counter in bits[31:12].
# - Each channel gets Poisson-ish spiking at its own rate, with a biphasic
#   waveform, plus baseline noise. Some channels are deliberately silent so the
#   panel's "which sites are active" readout has something to show.
import argparse
import math
import random
import socket
import struct
import threading
import time

CHANNELS_TOTAL = 256
BYTES_PER_POINT = 4
TIMESTAMP_SHIFT = 12
DEFAULT_HOST = "0.0.0.0"
DEFAULT_PORT = 10086
DEFAULT_FS = 20_000
VREF = 1.8
BASELINE_V = 0.9


def spike_shape(fs: int):
    """Biphasic extracellular spike: fast negative trough, slower positive."""
    dur = int(fs * 0.0016)  # 1.6 ms
    shape = []
    for i in range(dur):
        t = i / dur
        v = -math.exp(-((t - 0.18) ** 2) / 0.004) + 0.42 * math.exp(-((t - 0.45) ** 2) / 0.02)
        shape.append(v)
    return shape


class SpikeStreamServer:
    def __init__(self, host=DEFAULT_HOST, port=DEFAULT_PORT, fs=DEFAULT_FS,
                 noise_uv=12.0, active_fraction=0.55):
        self.host = host
        self.port = port
        self.fs = fs
        self.noise_v = noise_uv * 1e-6
        self.stop_event = threading.Event()
        self.shape = spike_shape(fs)
        rng = random.Random(20260726)
        # Per-channel unit: firing rate (Hz) and spike amplitude (volts).
        self.rate = []
        self.amp = []
        for ch in range(CHANNELS_TOTAL):
            if rng.random() > active_fraction:
                self.rate.append(0.0)
                self.amp.append(0.0)
            else:
                self.rate.append(rng.uniform(2.0, 45.0))
                self.amp.append(rng.uniform(80e-6, 420e-6))
        self.countdown = [0] * CHANNELS_TOTAL  # samples into the current spike

    def build_batch(self, start_idx: int, n_frames: int) -> bytes:
        buf = bytearray(n_frames * CHANNELS_TOTAL * BYTES_PER_POINT)
        rng = random.random
        gauss = random.gauss
        shape = self.shape
        slen = len(shape)
        for f in range(n_frames):
            idx = start_idx + f
            ts = (idx & 0xFFFFF) << TIMESTAMP_SHIFT
            base = f * CHANNELS_TOTAL * BYTES_PER_POINT
            for ch in range(CHANNELS_TOTAL):
                v = BASELINE_V + gauss(0.0, self.noise_v)
                c = self.countdown[ch]
                if c > 0:
                    v += self.amp[ch] * shape[slen - c]
                    self.countdown[ch] = c - 1
                elif self.rate[ch] > 0.0 and rng() < self.rate[ch] / self.fs:
                    self.countdown[ch] = slen
                code = int(v / VREF * 4096.0)
                code = 0 if code < 0 else (4095 if code > 4095 else code)
                struct.pack_into('<I', buf, base + ch * BYTES_PER_POINT, ts | code)
        return bytes(buf)

    def serve_client(self, conn: socket.socket, addr):
        print(f"client connected: {addr}")
        try:
            conn.settimeout(2.0)
            try:
                cmd = conn.recv(64)
                print(f"recv cmd: {cmd.decode(errors='ignore').strip()}")
            except socket.timeout:
                pass
            conn.settimeout(None)

            idx = 0
            batch = 200
            interval = batch / self.fs
            next_send = time.perf_counter()
            while not self.stop_event.is_set():
                payload = self.build_batch(idx, batch)
                idx += batch
                conn.sendall(payload)
                next_send += interval
                delay = next_send - time.perf_counter()
                if delay > 0:
                    time.sleep(delay)
                else:
                    next_send = time.perf_counter()  # generator fell behind
        except Exception as exc:
            print(f"client {addr} closed: {exc}")
        finally:
            conn.close()

    def run(self):
        active = sum(1 for r in self.rate if r > 0)
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            s.bind((self.host, self.port))
            s.listen(5)
            print(f"Spike dummy listening on {self.host}:{self.port} "
                  f"(fs={self.fs} Hz, {active}/{CHANNELS_TOTAL} active channels)")
            while not self.stop_event.is_set():
                conn, addr = s.accept()
                threading.Thread(target=self.serve_client, args=(conn, addr), daemon=True).start()


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description="Dummy 256-channel spiking TCP stream")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--fs", type=int, default=DEFAULT_FS)
    ap.add_argument("--noise-uv", type=float, default=12.0)
    ap.add_argument("--active-fraction", type=float, default=0.55)
    args = ap.parse_args()
    SpikeStreamServer(port=args.port, fs=args.fs, noise_uv=args.noise_uv,
                      active_fraction=args.active_fraction).run()
