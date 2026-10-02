"""Standard-library tests; no hardware, sockets, or third-party packages.

Run from the repository root:
    python -m unittest discover -s tests -p 'test_dummy_spike_server.py' -v
"""
from contextlib import redirect_stderr, redirect_stdout
import io
import json
import math
from pathlib import Path
import random
import statistics
import struct
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools import dummy_spike_tcp_server as dummy


FRAME_BYTES = dummy.CHANNELS_TOTAL * dummy.BYTES_PER_POINT


def channel_codes(payload, channel):
    return [struct.unpack_from('<I', payload, offset + channel * 4)[0] & 0xFFF
            for offset in range(0, len(payload), FRAME_BYTES)]


def controlled_stream(**kwargs):
    stream = dummy.SpikeStreamServer(noise_uv=0, active_fraction=0,
                                     collect_truth=True, **kwargs)
    # Public rate/amp arrays retain their existing input-referred units.
    stream.rate[7] = stream.fs  # always trigger when no waveform is pending
    stream.amp[7] = 200e-6
    return stream


class DummySpikeServerTests(unittest.TestCase):
    def test_wire_layout_baseline_and_20_bit_timestamp_wrap(self):
        stream = dummy.SpikeStreamServer(noise_uv=0, active_fraction=0)
        start = (1 << 20) - 2
        payload = stream.build_batch(start, 5)
        self.assertEqual(len(payload), 5 * FRAME_BYTES)
        for frame in range(5):
            timestamp = (start + frame) & dummy.TIMESTAMP_MASK
            expected = struct.pack('<I', (timestamp << 12) | 2048)
            self.assertEqual(payload[frame * FRAME_BYTES:(frame + 1) * FRAME_BYTES],
                             expected * dummy.CHANNELS_TOTAL)

    def test_saturated_adc_does_not_corrupt_timestamp_bits(self):
        for input_v, expected_code in ((-1.0, 0), (1.0, 4095)):
            with self.subTest(input_v=input_v):
                stream = dummy.SpikeStreamServer(active_fraction=0)
                with mock.patch.object(stream._rng, 'gauss', return_value=input_v):
                    payload = stream.build_batch(dummy.TIMESTAMP_MASK, 2)
                for frame, timestamp in enumerate((dummy.TIMESTAMP_MASK, 0)):
                    expected = struct.pack('<I', (timestamp << 12) | expected_code)
                    self.assertEqual(payload[frame * FRAME_BYTES:(frame + 1) * FRAME_BYTES],
                                     expected * dummy.CHANNELS_TOTAL)

    def test_adc_gain_quantization_and_clipping(self):
        stream = dummy.SpikeStreamServer()
        self.assertEqual(stream.gain, 60.0)
        input_lsb = dummy.ADC_LSB_V / stream.gain
        self.assertAlmostEqual(input_lsb * 1e6, 7.32421875)
        for input_v, expected in ((0, 2048), (0.25 * input_lsb, 2048),
                                  (-0.25 * input_lsb, 2047),
                                  (1.25 * input_lsb, 2049),
                                  (-1.25 * input_lsb, 2046), (-1, 0), (1, 4095)):
            with self.subTest(input_v=input_v):
                self.assertEqual(stream.input_v_to_code(input_v), expected)

    def test_default_gain_preserves_observable_input_spikes(self):
        for amplitude_uv in (80.0, 420.0):
            stream = controlled_stream()
            stream.amp[7] = amplitude_uv * 1e-6
            payload = stream.build_batch(0, len(stream.shape) + 1)
            actual = channel_codes(payload, 7)
            expected = [2048] + [
                max(0, min(4095, int((0.9 + amplitude_uv * 1e-6 * value * 60.0)
                                    / (1.8 / 4096))))
                for value in stream.shape]
            self.assertEqual(actual, expected)
            self.assertGreaterEqual(2048 - min(actual), 10)
            unity_gain = controlled_stream(gain=1.0)
            unity_gain.amp[7] = amplitude_uv * 1e-6
            legacy_codes = channel_codes(unity_gain.build_batch(0, len(stream.shape) + 1), 7)
            self.assertLessEqual(max(legacy_codes) - min(legacy_codes), 1)

    def test_noise_is_input_referred_before_gain(self):
        stream = dummy.SpikeStreamServer(noise_uv=12, active_fraction=0)
        with mock.patch.object(stream._rng, 'gauss', return_value=12e-6) as noise:
            payload = stream.build_batch(0, 1)
        self.assertEqual(noise.call_count, dummy.CHANNELS_TOTAL)
        noise.assert_called_with(0.0, 12e-6)
        self.assertEqual(channel_codes(payload, 0), [2049])
        higher_gain = dummy.SpikeStreamServer(noise_uv=12, active_fraction=0, gain=180)
        with mock.patch.object(higher_gain._rng, 'gauss', return_value=12e-6):
            payload = higher_gain.build_batch(0, 1)
        self.assertEqual(channel_codes(payload, 0), [2052])

    def test_seeded_noise_rms_after_quantization(self):
        stream = dummy.SpikeStreamServer(noise_uv=12, active_fraction=0, seed=91)
        payload = stream.build_batch(0, 32)
        decoded_uv = [(word[0] & 0xFFF) * dummy.ADC_LSB_V / stream.gain * 1e6
                      for word in struct.iter_unpack('<I', payload)]
        # 8192 samples. Quantization adds a bounded error and the legacy
        # truncation introduces roughly a half-LSB offset; stddev removes DC.
        measured_rms = statistics.pstdev(decoded_uv)
        self.assertAlmostEqual(measured_rms, 12.0, delta=0.7)
        baseline_uv = dummy.BASELINE_V / stream.gain * 1e6
        self.assertLess(abs(statistics.mean(decoded_uv) - baseline_uv), 5.0)

    def test_reset_seed_and_global_random_state_are_independent(self):
        stream = dummy.SpikeStreamServer(seed=123, collect_truth=True)
        global_state = random.getstate()
        first = stream.build_batch(0, 200)
        first_truth = stream.truth_events[:]
        self.assertEqual(random.getstate(), global_state)
        stream.reset_stream()
        self.assertEqual(stream.truth_events, [])
        self.assertTrue(all(count == 0 for count in stream.countdown))
        self.assertEqual(stream.build_batch(0, 200), first)
        self.assertEqual(stream.truth_events, first_truth)
        stream.reset_stream(seed=124)
        self.assertNotEqual(stream.build_batch(0, 200), first)
        stream.reset_stream(seed=123)
        self.assertEqual(stream.build_batch(0, 200), first)

    def test_arbitrary_batch_boundaries_preserve_bytes_and_truth(self):
        whole = dummy.SpikeStreamServer(seed=17, collect_truth=True)
        split = dummy.SpikeStreamServer(seed=17, collect_truth=True)
        start = (1 << 20) - 101
        expected = whole.build_batch(start, 427)
        actual = []
        offset = start
        for count in (1, 7, 32, 200, 187):
            actual.append(split.build_batch(offset, count))
            offset += count
        self.assertEqual(b''.join(actual), expected)
        self.assertEqual(split.truth_events, whole.truth_events)
        self.assertGreater(len(whole.truth_events), 0)

    def test_zero_length_batch_and_timeline_reset(self):
        stream = dummy.SpikeStreamServer(seed=99)
        self.assertEqual(stream.build_batch(99, 0), b'')
        original = stream.build_batch(20, 2)
        self.assertEqual(stream.build_batch(22, 0), b'')
        with self.assertRaisesRegex(ValueError, 'contiguous'):
            stream.build_batch(0, 1)
        stream.reset_stream(20)
        self.assertEqual(stream.build_batch(20, 2), original)
        self.assertEqual(stream.build_batch(22, 1),
                         dummy.SpikeStreamServer(seed=99).build_batch(20, 3)[-FRAME_BYTES:])

    def test_silent_and_active_channel_profiles(self):
        silent = dummy.SpikeStreamServer(active_fraction=0, noise_uv=0, collect_truth=True)
        self.assertTrue(all(rate == 0 for rate in silent.rate))
        self.assertTrue(all(amp == 0 for amp in silent.amp))
        payload = silent.build_batch(0, 40)
        self.assertTrue(all((word[0] & 0xFFF) == 2048
                            for word in struct.iter_unpack('<I', payload)))
        self.assertEqual(silent.truth_events, [])
        active = dummy.SpikeStreamServer(active_fraction=1)
        self.assertTrue(all(2 <= rate <= 45 for rate in active.rate))
        self.assertTrue(all(80e-6 <= amp <= 420e-6 for amp in active.amp))
        mixed = dummy.SpikeStreamServer(noise_uv=0, collect_truth=True)
        self.assertGreater(sum(rate > 0 for rate in mixed.rate), 0)
        self.assertLess(sum(rate > 0 for rate in mixed.rate), dummy.CHANNELS_TOTAL)
        payload = mixed.build_batch(0, 500)
        self.assertGreater(len(mixed.truth_events), 0)
        for event in mixed.truth_events:
            self.assertGreater(mixed.rate[event.channel], 0)
            self.assertEqual(event.unit_id, event.channel)
        for channel, rate in enumerate(mixed.rate):
            if rate == 0:
                self.assertEqual(set(channel_codes(payload, channel)), {2048})

    def test_truth_onset_trough_and_tail_positions_across_wrap(self):
        stream = controlled_stream()
        start = (1 << 20) - 2
        trigger = stream.build_batch(start, 1)
        self.assertEqual(channel_codes(trigger, 7), [2048])
        self.assertEqual(stream.truth_events, [])  # scheduled, not yet emitted
        payload = trigger + stream.build_batch(start + 1, 70)
        events = stream.drain_truth()
        self.assertEqual(stream.truth_events, [])
        self.assertEqual([event.onset_sample for event in events],
                         [start + 1, start + 34, start + 67])
        trough_offset = min(range(len(stream.shape)), key=stream.shape.__getitem__)
        codes = channel_codes(payload, 7)
        for event in events:
            self.assertEqual(event.channel, 7)
            self.assertEqual(event.unit_id, 7)
            self.assertEqual(event.trigger_sample, event.onset_sample - 1)
            self.assertEqual(event.trough_sample, event.onset_sample + trough_offset)
            self.assertEqual(event.end_sample_exclusive, event.onset_sample + 32)
            self.assertAlmostEqual(event.amplitude_uv, 200.0)
            self.assertAlmostEqual(event.trough_uv, 200.0 * min(stream.shape))
            if event.end_sample_exclusive <= start + len(codes):
                window = codes[event.onset_sample - start:event.end_sample_exclusive - start]
                self.assertEqual(codes[event.trough_sample - start], min(window))
        self.assertGreater(events[-1].trough_sample, start + 70)
        self.assertEqual(channel_codes(payload, 8), [2048] * 71)

    def test_truth_collection_is_optional_and_does_not_change_stream(self):
        plain = dummy.SpikeStreamServer(seed=5)
        truth = dummy.SpikeStreamServer(seed=5, collect_truth=True)
        self.assertEqual(plain.build_batch(0, 200), truth.build_batch(0, 200))
        self.assertEqual(plain.drain_truth(), [])
        self.assertGreater(len(truth.drain_truth()), 0)
        self.assertEqual(truth.drain_truth(), [])

    def test_fixture_export_matches_in_memory_and_is_chunk_invariant(self):
        stream = dummy.SpikeStreamServer(seed=72)
        stream.build_batch(0, 1)
        expected_next = dummy.SpikeStreamServer(seed=72).build_batch(0, 2)[FRAME_BYTES:]
        reference = dummy.SpikeStreamServer(seed=72, collect_truth=True)
        start = (1 << 20) - 10
        expected = reference.build_batch(start, 350)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            stream.export_fixture(root / 'a.bin', 350, start_idx=start,
                                  truth_path=root / 'a.jsonl', batch_frames=1)
            stream.export_fixture(root / 'b.bin', 350, start_idx=start,
                                  truth_path=root / 'b.jsonl', batch_frames=200)
            self.assertEqual((root / 'a.bin').read_bytes(), expected)
            self.assertEqual((root / 'b.bin').read_bytes(), expected)
            self.assertEqual((root / 'a.jsonl').read_bytes(), (root / 'b.jsonl').read_bytes())
            records = [json.loads(line) for line in (root / 'a.jsonl').read_text().splitlines()]
            metadata = records[0]
            self.assertEqual(metadata['schema'], 'nl-source-spike-truth-v1')
            self.assertEqual(metadata['front_end_gain'], 60.0)
            self.assertEqual(metadata['noise_input_rms_uv'], 12.0)
            self.assertEqual(metadata['start_sample'], start)
            self.assertEqual(metadata['end_sample_exclusive'], start + 350)
            self.assertEqual(len(metadata['channel_profiles']), 256)
            self.assertIn('not_sorting_results', metadata['unit_labels'])
            self.assertEqual(len(records) - 1, len(reference.truth_events))
            self.assertGreater(len(records), 1)
            for record, event in zip(records[1:], reference.truth_events):
                self.assertEqual(record.pop('type'), 'spike')
                self.assertEqual(record, dummy.asdict(event))
        self.assertEqual(stream.build_batch(1, 1), expected_next)

    def test_empty_fixture_and_identical_path_rejected(self):
        stream = dummy.SpikeStreamServer()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'empty.bin'
            truth_path = Path(directory) / 'empty.jsonl'
            stream.export_fixture(path, 0, start_idx=5, truth_path=truth_path)
            self.assertEqual(path.read_bytes(), b'')
            self.assertEqual(len(truth_path.read_text().splitlines()), 1)
            with self.assertRaisesRegex(ValueError, 'paths must differ'):
                stream.export_fixture(path, 1, truth_path=path)
            self.assertEqual(path.read_bytes(), b'')

    def test_constructor_parameter_validation(self):
        cases = {
            'host': ('', '  ', None), 'port': (-1, 65536, 2.5, True),
            'fs': (0, 624, -1, 20_000.0, True),
            'noise_uv': (-1, math.inf, math.nan, True, '12'),
            'active_fraction': (-0.01, 1.01, math.inf, math.nan, True),
            'gain': (0, -1, math.inf, math.nan, True),
            'seed': (-1, 1.2, None, True), 'collect_truth': (0, 1, None),
        }
        for parameter, values in cases.items():
            for value in values:
                with self.subTest(parameter=parameter, value=value):
                    with self.assertRaises(ValueError):
                        dummy.SpikeStreamServer(**{parameter: value})
        self.assertEqual(len(dummy.spike_shape(625)), 1)
        self.assertEqual(dummy.SpikeStreamServer(port=0).port, 0)

    def test_batch_reset_and_export_parameter_validation(self):
        stream = dummy.SpikeStreamServer()
        for start, count in ((-1, 0), (0, -1), (0.5, 1), (0, 1.5), (True, 1), (0, False)):
            with self.subTest(start=start, count=count), self.assertRaises(ValueError):
                stream.build_batch(start, count)
        with self.assertRaises(ValueError):
            stream.reset_stream(-1)
        with self.assertRaises(ValueError):
            stream.reset_stream(seed=-1)
        for kwargs in ({'n_frames': -1}, {'n_frames': 1, 'batch_frames': 0},
                       {'n_frames': 1, 'start_idx': -1}):
            with self.subTest(kwargs=kwargs), self.assertRaises(ValueError):
                stream.export_fixture('not-created.bin', **kwargs)

    def test_cli_offline_mode_opens_no_socket(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'cli.bin'
            truth = Path(directory) / 'cli.jsonl'
            with mock.patch.object(dummy.socket, 'socket', side_effect=AssertionError('socket opened')):
                dummy.main(['--output-bin', str(path), '--frames', '12', '--truth-jsonl', str(truth),
                            '--seed', '7', '--start-idx', str((1 << 20) - 1)])
            self.assertEqual(path.stat().st_size, 12 * FRAME_BYTES)
            self.assertEqual(json.loads(truth.read_text().splitlines()[0])['seed'], 7)

    def test_existing_cli_flags_and_explicit_host_remain_supported(self):
        with mock.patch.object(dummy.SpikeStreamServer, 'run', autospec=True) as run:
            dummy.main(['--port', '12345', '--fs', '20000', '--noise-uv', '8',
                        '--active-fraction', '0.4'])
        stream = run.call_args.args[0]
        self.assertEqual(stream.host, '127.0.0.1')
        self.assertEqual(stream.port, 12345)
        self.assertEqual(stream.noise_uv, 8.0)
        self.assertEqual(stream.active_fraction, 0.4)
        with mock.patch.object(dummy.SpikeStreamServer, 'run', autospec=True) as run:
            dummy.main(['--host', '0.0.0.0', '--gain', '180'])
        self.assertEqual(run.call_args.args[0].host, '0.0.0.0')
        self.assertEqual(run.call_args.args[0].gain, 180.0)

    def test_cli_rejects_incomplete_or_invalid_fixture_arguments(self):
        for args in (['--frames', '1'], ['--truth-jsonl', 'truth.jsonl'],
                     ['--start-idx', '1'], ['--output-bin', 'test.bin'],
                     ['--gain', 'nan'], ['--fs', '0']):
            with self.subTest(args=args), redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as error:
                    dummy.main(args)
                self.assertEqual(error.exception.code, 2)

    def test_connections_use_independent_seeded_streams_without_network(self):
        server = dummy.SpikeStreamServer(seed=41)
        server.build_batch(0, 1)  # must not leak server-generator state to clients
        connections = [mock.Mock(), mock.Mock()]
        for connection in connections:
            connection.recv.return_value = b'ctre\n'
            connection.sendall.side_effect = OSError('end fake connection')
            with redirect_stdout(io.StringIO()):
                server.serve_client(connection, ('127.0.0.1', 12345))
            connection.close.assert_called_once()
        first = connections[0].sendall.call_args.args[0]
        second = connections[1].sendall.call_args.args[0]
        self.assertEqual(first, second)
        self.assertEqual(first, dummy.SpikeStreamServer(seed=41).build_batch(0, 200))

    def test_listener_binds_loopback_and_stops_without_network(self):
        server = dummy.SpikeStreamServer(port=0)
        listener = mock.MagicMock()
        listener.getsockname.return_value = ('127.0.0.1', 12345)

        def stop_accepting():
            server.stop_event.set()
            raise dummy.socket.timeout()

        listener.accept.side_effect = stop_accepting
        with mock.patch.object(dummy.socket, 'socket') as socket_factory:
            socket_factory.return_value.__enter__.return_value = listener
            with redirect_stdout(io.StringIO()):
                server.run()
        listener.bind.assert_called_once_with(('127.0.0.1', 0))
        listener.settimeout.assert_called_once_with(0.25)


if __name__ == '__main__':
    unittest.main()
