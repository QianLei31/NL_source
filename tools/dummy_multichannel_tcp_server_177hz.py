import socket
import threading
import time

import numpy as np

CHANNELS = 256
FRAME_BYTES = CHANNELS * 4
HOST = "0.0.0.0"
PORT = 10086
SAMPLE_RATE = 40_000
BASE_FREQUENCY_HZ = 177.0
FREQUENCY_STEP_HZ = 0.5
BATCH_FRAMES = 128


class DummyStreamServer:
    def __init__(self):
        self.stop_event = threading.Event()

    @staticmethod
    def build_batch(sample_index):
        sample = np.arange(
            sample_index, sample_index + BATCH_FRAMES, dtype=np.float64
        )[:, None]
        channel = np.arange(CHANNELS, dtype=np.float64)[None, :]
        frequency = BASE_FREQUENCY_HZ + np.mod(channel, 16.0) * FREQUENCY_STEP_HZ
        amplitude = 300.0 + np.mod(channel, 8.0) * 60.0
        baseline = 2048.0 + np.mod(channel, 4.0) * 200.0
        values = baseline + amplitude * np.sin(
            2.0 * np.pi * frequency * sample / SAMPLE_RATE
        )
        return np.clip(values, 0, 4095).astype("<u4", copy=False).tobytes()

    def serve_client(self, connection, address):
        print(f"client connected: {address}", flush=True)
        try:
            connection.settimeout(2.0)
            try:
                command = connection.recv(64)
                print(f"recv command: {command!r}", flush=True)
            except socket.timeout:
                pass

            sample_index = 0
            next_send = time.perf_counter()
            batch_seconds = BATCH_FRAMES / SAMPLE_RATE
            while not self.stop_event.is_set():
                connection.sendall(self.build_batch(sample_index))
                sample_index += BATCH_FRAMES
                next_send += batch_seconds
                delay = next_send - time.perf_counter()
                if delay > 0:
                    time.sleep(delay)
                elif delay < -0.1:
                    next_send = time.perf_counter()
        except (ConnectionError, OSError) as error:
            print(f"client closed: {address}: {error}", flush=True)
        finally:
            connection.close()

    def run(self):
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
            server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            server.bind((HOST, PORT))
            server.listen(5)
            print(
                f"Dummy server listening on {HOST}:{PORT}, "
                f"{CHANNELS} channels, {SAMPLE_RATE} S/s, CH0={BASE_FREQUENCY_HZ} Hz",
                flush=True,
            )
            while not self.stop_event.is_set():
                connection, address = server.accept()
                threading.Thread(
                    target=self.serve_client,
                    args=(connection, address),
                    daemon=True,
                ).start()


if __name__ == "__main__":
    DummyStreamServer().run()
