# File: dummy_multichannel_tcp_server.py
# Purpose: Dummy multichannel TCP stream server.
# Components:
# - Generate synthetic 256-channel frames
# - Continuously stream frames over TCP
import math
import socket
import struct
import threading
import time

CHANNELS_TOTAL = 256
BYTES_PER_POINT = 4
DEFAULT_HOST = "0.0.0.0"
DEFAULT_PORT = 10086
DEFAULT_FS = 40_000  # each channel sample-rate


class DummyStreamServer:
    def __init__(self, host=DEFAULT_HOST, port=DEFAULT_PORT, fs=DEFAULT_FS):
        self.host = host
        self.port = port
        self.fs = fs
        self.stop_event = threading.Event()

    def build_frame(self, sample_idx: int) -> bytes:
        frame = bytearray(CHANNELS_TOTAL * BYTES_PER_POINT)
        for ch in range(CHANNELS_TOTAL):
            # Baseline + channel-dependent sine, mapped to ADC-like range
            freq = 5.0 + (ch % 16) * 0.5
            amp = 300 + (ch % 8) * 60
            baseline = 2048 + (ch % 4) * 200
            value = baseline + int(amp * math.sin(2 * math.pi * freq * sample_idx / self.fs))
            value = max(0, min(4095, value))
            struct.pack_into('<I', frame, ch * BYTES_PER_POINT, value)
        return bytes(frame)

    def serve_client(self, conn: socket.socket, addr):
        print(f"client connected: {addr}")
        try:
            conn.settimeout(2.0)
            try:
                cmd = conn.recv(64)
                print(f"recv cmd: {cmd.decode(errors='ignore').strip()}")
            except socket.timeout:
                pass

            sample_idx = 0
            send_batch_frames = 128
            target_interval = send_batch_frames / self.fs

            while not self.stop_event.is_set():
                t0 = time.perf_counter()
                payload = bytearray()
                for _ in range(send_batch_frames):
                    payload.extend(self.build_frame(sample_idx))
                    sample_idx += 1
                conn.sendall(payload)
                dt = time.perf_counter() - t0
                if dt < target_interval:
                    time.sleep(target_interval - dt)
        except Exception as exc:
            print(f"client {addr} closed: {exc}")
        finally:
            conn.close()

    def run(self):
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            s.bind((self.host, self.port))
            s.listen(5)
            print(f"Dummy server listening on {self.host}:{self.port}")
            while not self.stop_event.is_set():
                conn, addr = s.accept()
                threading.Thread(target=self.serve_client, args=(conn, addr), daemon=True).start()


if __name__ == "__main__":
    DummyStreamServer().run()
