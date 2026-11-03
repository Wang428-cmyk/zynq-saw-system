import threading
import struct
import unittest
import zlib
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest.mock import Mock, patch

import server


def make_result(point_count=101):
    return {
        "scan": {
            "points": [
                {
                    "index": index,
                    "frequency_hz": 912_000_000 + index * 60_000,
                    "power_dbm": -70.0 + index / 10,
                }
                for index in range(point_count)
            ]
        }
    }


def make_stability_result(generation, peaks):
    coarse_points = [
        {
            "index": index,
            "frequency_hz": 433_000_000 + index * 100_000,
            "power_dbfs": -80.0,
            "power_dbm": -80.0,
        }
        for index in range(40)
    ]
    fine_scans = []
    for coarse_index, frequency_hz, power_dbm in peaks:
        points = [
            {
                "index": index,
                "frequency_hz": frequency_hz - 1000 + index * 1000,
                "power_dbfs": power_dbm - 1 + index,
                "power_dbm": power_dbm - 1 + index,
                "energy": str(index + 1),
            }
            for index in range(3)
        ]
        fine_scans.append({
            "peak_id": len(fine_scans),
            "coarse_index": coarse_index,
            "target_frequency_hz": coarse_points[coarse_index]["frequency_hz"],
            "lo_frequency_hz": frequency_hz + 1_000_000,
            "scan": {
                "peak_index": 1,
                "peak_power_dbfs": power_dbm,
                "peak_power_dbm": power_dbm,
                "resonance_frequency_hz": frequency_hz,
                "dbm_calibration_offset_db": 0.0,
                "points": points,
            },
        })
    return {
        "status": {
            "result_generation": generation,
            "fine_half_bandwidth_hz": 100_000,
            "step_hz": 100_000,
        },
        "scan": {"points": coarse_points},
        "fine_scans": fine_scans,
    }


class FakeServer:
    timeout_ms = 10
    notify_port = 4002

    def __init__(self):
        self.monitoring = True
        self.results = []
        self.errors = []

    def monitoring_target(self):
        return ("192.0.2.10", 4001) if self.monitoring else None

    def is_monitoring(self):
        return self.monitoring

    def store_result(self, result, warning=None):
        self.results.append((result, warning))

    def store_error(self, message):
        self.errors.append(message)

    def board_generation_seen(self, generation):
        return False

    def set_monitoring(self, enabled, board_ip=None, board_port=None):
        self.monitoring = bool(enabled)


def make_stream_packet(command, sequence, payload, result=0):
    header = (
        server.MAGIC_RESPONSE
        + bytes((command,))
        + struct.pack("!HHH", sequence, result, len(payload))
    )
    body = header + payload
    return body + struct.pack("!I", zlib.crc32(body) & 0xFFFFFFFF)


def make_stream_data(
    generation, scan_type, peak_id, coarse_index, target_frequency,
    lo_frequency, start_frequency, step_hz, energies,
):
    metadata = struct.pack(
        "!12I",
        generation,
        3,
        scan_type,
        peak_id,
        coarse_index,
        target_frequency,
        lo_frequency,
        start_frequency,
        step_hz,
        len(energies),
        0,
        len(energies),
    )
    encoded_energies = b"".join(
        struct.pack("!2I", energy >> 32, energy & 0xFFFFFFFF)
        for energy in energies
    )
    return make_stream_packet(
        server.RESULT_DATA_COMMAND, generation, metadata + encoded_energies
    )


def make_stream_end(generation, packet_count, coarse_points, fine_peaks):
    status = {field: 0 for field in server.STATUS_FIELDS}
    status.update({
        "config_revision": 3,
        "point_count": coarse_points,
        "start_freq_hz": 435_900_000,
        "bandwidth_hz": 200_000,
        "step_hz": 100_000,
        "coherent_repetitions": 1,
        "noncoherent_segments": 1,
        "rx_samples_last": 1,
        "rx_capture_depth": 65_536,
        "scan_stage": 3,
        "fine_peak_count": fine_peaks,
        "result_generation": generation,
        "continuous_enabled": 1,
        "emmc_mounted": 1,
        "emmc_config_valid": 1,
        "stream_receiver_valid": 1,
    })
    payload = struct.pack(
        "!7I",
        generation,
        0,
        packet_count,
        coarse_points,
        fine_peaks,
        0,
        len(server.STATUS_FIELDS),
    )
    payload += struct.pack(
        f"!{len(server.STATUS_FIELDS)}I",
        *(status[field] for field in server.STATUS_FIELDS),
    )
    return make_stream_packet(
        server.RESULT_END_COMMAND, generation + 100, payload
    )


class ResultStreamTests(unittest.TestCase):
    def test_out_of_order_push_assembles_coarse_and_fine_without_start(self):
        fake_server = FakeServer()
        listener = object.__new__(server.SawNotificationListener)
        listener.server = fake_server
        listener.bundles = {}
        listener.last_packet_at = 0.0
        generation = 9
        source = ("192.0.2.10", 4001)
        coarse = make_stream_data(
            generation, 0, 0xFFFFFFFF, 0xFFFFFFFF, 0,
            436_000_000, 435_900_000, 100_000, [1, 4, 1],
        )
        fine = make_stream_data(
            generation, 1, 0, 1, 436_000_000,
            437_000_000, 435_999_000, 1_000, [2, 8],
        )
        end = make_stream_end(generation, 2, 3, 1)

        listener.accept_stream_packet(end, source)
        listener.accept_stream_packet(fine, source)
        self.assertFalse(fake_server.results)
        listener.accept_stream_packet(coarse, source)

        self.assertEqual(1, len(fake_server.results))
        result = fake_server.results[0][0]
        self.assertEqual(3, result["scan"]["point_count"])
        self.assertEqual(1, len(result["fine_scans"]))
        self.assertEqual(437_000_000, result["fine_scans"][0]["lo_frequency_hz"])
        self.assertFalse(fake_server.errors)


class StorageErrorProtocolTests(unittest.TestCase):
    def test_config_error_reports_fatfs_detail(self):
        status = {field: 0 for field in server.STATUS_FIELDS}
        status["emmc_mounted"] = 1
        status["emmc_last_error"] = 6
        payload = struct.pack(
            f"!{len(server.STATUS_FIELDS)}I",
            *(status[field] for field in server.STATUS_FIELDS),
        )
        packet = make_stream_packet(
            server.COMMANDS["config"], 21, payload, result=7
        )

        parsed = server.SawUdpClient.parse_response(
            packet, server.COMMANDS["config"], 21
        )

        self.assertFalse(parsed["ok"])
        self.assertIn("FR_INVALID_NAME", parsed["message"])


class ResultSessionTests(unittest.TestCase):
    def test_each_server_start_has_a_new_result_session(self):
        first = object.__new__(server.SawHttpServer)
        second = object.__new__(server.SawHttpServer)
        first.initialize_result_state()
        second.initialize_result_state()

        self.assertNotEqual(first.result_session, second.result_session)
        self.assertEqual(first.result_session, first.latest_snapshot()["session"])

    def test_second_http_server_cannot_reuse_the_same_port(self):
        first = server.SawHttpServer(("127.0.0.1", 0), server.RequestHandler)
        port = first.server_address[1]
        try:
            with self.assertRaises(OSError):
                second = server.SawHttpServer(
                    ("127.0.0.1", port), server.RequestHandler
                )
                second.server_close()
        finally:
            first.server_close()


class SavedSettingsTests(unittest.TestCase):
    @staticmethod
    def settings_body():
        return {
            "board_ip": "192.168.2.88",
            "board_port": 4010,
            "timeout_ms": 2200,
            "config": {
                "center_freq_hz": 436_000_000,
                "half_bandwidth_hz": 3_000_000,
                "step_hz": 5_000,
                "tx_time_us": 100,
                "guard1_time_us": 2,
                "rx_time_us": 20,
                "guard2_time_us": 5,
                "coherent_repetitions": 10,
                "noncoherent_segments": 6,
                "tx_sampling_freq_hz": 61_440_000,
                "rx_sampling_freq_hz": 61_440_000,
            },
            "ui_settings": {
                "power_calibration_db": -3.25,
                "lo_elimination": True,
                "lo_blind_khz": 200,
            },
        }

    def test_settings_round_trip(self):
        with TemporaryDirectory() as directory:
            path = Path(directory) / "last_settings.json"
            expected = server.normalize_saved_settings(self.settings_body())
            server.write_saved_settings(path, expected)

            self.assertEqual(expected, server.load_saved_settings(path))

    def test_server_updates_in_memory_settings_after_save(self):
        with TemporaryDirectory() as directory:
            instance = object.__new__(server.SawHttpServer)
            instance.initialize_settings_state(Path(directory) / "last_settings.json")

            instance.save_settings(self.settings_body())

            saved = instance.saved_settings_snapshot()
            self.assertEqual(433_000_000, saved["config"]["start_freq_hz"])
            self.assertEqual(6_000_000, saved["config"]["bandwidth_hz"])
            self.assertEqual(300_000, saved["config"]["coarse_lo_guard_hz"])
            self.assertEqual(100_000, saved["config"]["fine_half_bandwidth_hz"])
            self.assertTrue(saved["ui_settings"]["lo_elimination"])


class FineResultProtocolTests(unittest.TestCase):
    def test_parse_fine_result_packet(self):
        values = (
            7, 2, 1, 64, 915_800_000, 916_800_000,
            915_700_000, 1_000, 201, 127, 2,
        )
        payload = struct.pack("!11I", *values) + struct.pack(
            "!4I", 0x00000001, 0x00000002, 0x00000003, 0x00000004
        )
        header = (
            server.MAGIC_RESPONSE
            + bytes((server.COMMANDS["fine_results"],))
            + struct.pack("!HHH", 12, 0, len(payload))
        )
        packet_without_crc = header + payload
        packet = packet_without_crc + struct.pack(
            "!I", zlib.crc32(packet_without_crc) & 0xFFFFFFFF
        )

        parsed = server.SawUdpClient.parse_response(
            packet, server.COMMANDS["fine_results"], 12
        )

        self.assertTrue(parsed["ok"])
        self.assertEqual(201, parsed["total_points"])
        self.assertEqual(127, parsed["start_index"])
        self.assertEqual([0x0000000100000002, 0x0000000300000004],
                         parsed["energies"])

    def test_fetch_fine_result_merges_two_ps_chunks(self):
        client = server.SawUdpClient()
        calls = []

        def transact(_ip, _port, _timeout, command, **kwargs):
            self.assertEqual("fine_results", command)
            start = kwargs["result_start"]
            count = min(kwargs["result_count"], 201 - start)
            calls.append((start, count))
            return {
                "ok": True,
                "generation": 9,
                "fine_peak_count": 1,
                "peak_id": 0,
                "coarse_index": 64,
                "target_frequency_hz": 915_800_000,
                "lo_frequency_hz": 916_800_000,
                "start_frequency_hz": 915_700_000,
                "step_hz": 1_000,
                "total_points": 201,
                "start_index": start,
                "energies": [1_000_000] * count,
            }

        client.transact = Mock(side_effect=transact)
        status = {
            "fine_peak_count": 1,
            "result_generation": 9,
            "rx_samples_last": 1,
            "noncoherent_segments": 1,
            "coherent_repetitions": 1,
            "start_freq_hz": 912_000_000,
            "bandwidth_hz": 6_000_000,
            "step_hz": 50_000,
        }

        fine_scans = client.fetch_fine_results(
            "192.0.2.10", 4001, 100, status
        )

        self.assertEqual([(0, 127), (127, 74)], calls)
        self.assertEqual(1, len(fine_scans))
        self.assertEqual(201, fine_scans[0]["scan"]["point_count"])
        self.assertEqual(916_800_000, fine_scans[0]["scan"]["lo_frequency_hz"])


class FinePeakRefinementTests(unittest.TestCase):
    def test_sg_and_fifteen_point_fit_recover_sub_bin_peak(self):
        start_frequency = 433_800_000
        step_hz = 1_000
        true_position = 100.35
        points = []
        for index in range(201):
            noise = (0.10, -0.08, 0.04, -0.03)[index % 4]
            power_dbfs = -55.0 - 0.018 * (index - true_position) ** 2 + noise
            points.append({
                "index": index,
                "frequency_hz": start_frequency + index * step_hz,
                "power_dbfs": power_dbfs,
                "power_dbm": power_dbfs,
                "energy": "1",
            })
        scan = {
            "point_count": len(points),
            "step_hz": step_hz,
            "peak_index": 100,
            "peak_offset": 0.0,
            "peak_power_dbfs": points[100]["power_dbfs"],
            "peak_power_dbm": points[100]["power_dbm"],
            "resonance_frequency_hz": start_frequency + 100 * step_hz,
            "dbm_calibration_offset_db": 0.0,
            "points": points,
        }

        server.refine_fine_scan(scan)

        expected_frequency = start_frequency + true_position * step_hz
        self.assertTrue(scan["fit_valid"])
        self.assertEqual("sg11_quadratic15", scan["refinement_method"])
        self.assertLess(abs(scan["fitted_frequency_hz"] - expected_frequency), 100.0)
        self.assertEqual(start_frequency + 100 * step_hz,
                         scan["ps_resonance_frequency_hz"])
        self.assertTrue(all(
            "smoothed_power_dbfs" in point for point in scan["points"]
        ))


class StabilityManagerTests(unittest.TestCase):
    def test_five_value_median_skips_missing_rounds(self):
        with TemporaryDirectory() as directory:
            manager = server.StabilityManager(Path(directory))
            try:
                manager.start(6, settings={
                    "config": {
                        "fine_half_bandwidth_hz": 100_000,
                        "step_hz": 100_000,
                    },
                })
                base = 434_000_000
                manager.ingest(make_stability_result(1, [(10, base, -60.0)]))
                manager.ingest(make_stability_result(2, [(10, base + 10, -60.0)]))
                manager.ingest(make_stability_result(3, []))
                manager.ingest(make_stability_result(4, [(10, base + 20, -60.0)]))
                manager.ingest(make_stability_result(5, [(10, base + 30, -60.0)]))
                manager.ingest(make_stability_result(6, [(10, base + 1000, -60.0)]))

                track = manager.snapshot()["tracks"][0]
                self.assertEqual(
                    base + 20,
                    track["metrics"]["latest_median_frequency_hz"],
                )
            finally:
                manager.close()

    def test_tracks_peaks_by_frequency_and_writes_summary(self):
        with TemporaryDirectory() as directory:
            manager = server.StabilityManager(Path(directory))
            try:
                manager.start(3, settings={
                    "config": {
                        "fine_half_bandwidth_hz": 100_000,
                        "step_hz": 100_000,
                    },
                    "ui_settings": {"power_calibration_db": 2.0},
                })
                manager.ingest(make_stability_result(1, [
                    (10, 434_000_000, -60.0),
                    (30, 436_000_000, -55.0),
                ]))
                # Reverse packet order: track identities must still follow frequency.
                manager.ingest(make_stability_result(2, [
                    (30, 436_001_000, -54.5),
                    (10, 434_002_000, -59.5),
                ]))
                # The lower-frequency peak is absent in the last round.
                manager.ingest(make_stability_result(3, [
                    (30, 436_003_000, -54.0),
                ]))

                snapshot = manager.snapshot()
                self.assertFalse(snapshot["active"])
                self.assertEqual("completed", snapshot["state"])
                self.assertEqual(3, snapshot["completed_rounds"])
                self.assertEqual(2, len(snapshot["tracks"]))
                self.assertEqual([1, 2], [
                    track["track_id"] for track in snapshot["tracks"]
                ])
                low, high = snapshot["tracks"]
                self.assertEqual(2, low["metrics"]["valid_count"])
                self.assertEqual(3, high["metrics"]["valid_count"])
                self.assertEqual(2000.0, low["metrics"]["frequency_pp_hz"])
                self.assertEqual(3000.0, high["metrics"]["frequency_pp_hz"])
                self.assertEqual(
                    434_001_000.0,
                    low["metrics"]["latest_median_frequency_hz"],
                )
                self.assertEqual(
                    436_001_000.0,
                    high["metrics"]["latest_median_frequency_hz"],
                )
                self.assertEqual(
                    high["metrics"]["latest_median_frequency_hz"],
                    high["last_median_frequency_hz"],
                )
                # UI calibration is included in persisted amplitude values.
                self.assertEqual(-58.0, low["trend"][0]["power_dbm"])

                session_dir = Path(snapshot["session_path"])
                manager.close()
                rows = (session_dir / "summary.csv").read_text(
                    encoding="utf-8-sig"
                ).splitlines()
                self.assertEqual(1 + 6, len(rows))
                self.assertTrue((session_dir / "report.csv").is_file())
                self.assertTrue((session_dir / "config.json").is_file())
                self.assertTrue((session_dir / "session.json").is_file())
            finally:
                if manager.writer.is_alive():
                    manager.close()


if __name__ == "__main__":
    unittest.main()
