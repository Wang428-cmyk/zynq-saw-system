#!/usr/bin/env python3
import argparse
import csv
import json
import math
import mimetypes
import queue
import socket
import struct
import threading
import time
import uuid
import webbrowser
import zlib
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse


APP_ROOT = Path(__file__).resolve().parent
WEB_ROOT = APP_ROOT / "web"
SETTINGS_PATH = APP_ROOT / "last_settings.json"
STABILITY_ROOT = APP_ROOT / "StabilityData"
ADC_FULL_SCALE_CODE = 2048.0
# Input power corresponding to a 0 dBFS detector result. The RX path now uses
# fixed 20 dB MGC, but the completed RF board still needs a one-point bench
# calibration to include connector, matching-network and analog-path losses.
DBM_CALIBRATION_OFFSET_DB = 0.0
RX_MANUAL_GAIN_DB = 20
RESULT_POINTS_PER_PACKET = 127
MAX_REASONABLE_POWER_DBFS = 12.0
NOTIFICATION_RETRY_ATTEMPTS = 3
NOTIFICATION_RETRY_DELAY_SECONDS = 0.05
WATCHDOG_INTERVAL_SECONDS = 1.0
FINE_SMOOTHING_VERSION = 1
FINE_SG11_COEFFICIENTS = (-36, 9, 44, 69, 84, 89, 84, 69, 44, 9, -36)
FINE_SG11_DIVISOR = 429.0
FINE_FIT_HALF_WINDOW = 7
MAGIC_REQUEST = b"\xAA\x55\x02"
MAGIC_RESPONSE = b"\x7F\x4E\x02"
COMMANDS = {
    "config": 0xE0,
    "start": 0xE1,
    "stop": 0xE2,
    "status": 0xE3,
    "results": 0xE4,
    "fine_results": 0xE6,
    "subscribe": 0xE7,
}
RESULT_DATA_COMMAND = 0xE8
RESULT_END_COMMAND = 0xE9
CONFIG_FIELDS = (
    "start_freq_hz",
    "bandwidth_hz",
    "step_hz",
    "tx_time_us",
    "guard1_time_us",
    "rx_time_us",
    "guard2_time_us",
    "coherent_repetitions",
    "noncoherent_segments",
    "tx_sampling_freq_hz",
    "rx_sampling_freq_hz",
    "fine_enabled",
    "fine_half_bandwidth_hz",
    "fine_step_hz",
    "fine_lo_offset_hz",
    "coarse_lo_blind_hz",
    "coarse_lo_guard_hz",
)
OPTIONAL_CONFIG_DEFAULTS = {
    "fine_enabled": 0,
    "fine_half_bandwidth_hz": 100_000,
    "fine_step_hz": 1_000,
    "fine_lo_offset_hz": 1_000_000,
    "coarse_lo_blind_hz": 180_000,
    "coarse_lo_guard_hz": 300_000,
}
UI_SETTING_FIELDS = (
    "power_calibration_db",
    "lo_elimination",
    "lo_blind_khz",
)
STATUS_FIELDS = (
    "pl_status",
    "pl_debug",
    "current_freq_index",
    "config_revision",
    "point_count",
    *CONFIG_FIELDS,
    "rx_samples_last",
    "rx_capture_depth",
    "scan_stage",
    "fine_peak_count",
    "current_fine_peak",
    "current_fine_chunk",
    "result_generation",
    "continuous_enabled",
    "emmc_mounted",
    "emmc_config_valid",
    "emmc_save_generation",
    "stream_receiver_valid",
    "stream_packets_sent",
    "stream_send_errors",
    "emmc_last_error",
)
RESULT_MESSAGES = {
    0: "操作成功",
    1: "协议参数错误",
    2: "配置超出允许范围",
    3: "当前PL时钟仅支持61.44 MSPS",
    4: "AD9361配置失败",
    5: "等待PL停止超时",
    6: "扫描尚未完成或结果正在更新",
}
RESULT_MESSAGES.update({
    0: "操作成功",
    1: "协议参数错误",
    2: "配置超出允许范围",
    3: "当前PL时钟仅支持61.44 MSPS",
    4: "AD9361配置失败",
    5: "等待PL停止超时",
    6: "扫描尚未完成或结果正在更新",
    7: "eMMC参数保存或读取失败",
})
FATFS_RESULT_MESSAGES = {
    0: "FR_OK",
    1: "FR_DISK_ERR",
    2: "FR_INT_ERR",
    3: "FR_NOT_READY",
    4: "FR_NO_FILE",
    5: "FR_NO_PATH",
    6: "FR_INVALID_NAME",
    7: "FR_DENIED",
    8: "FR_EXIST",
    9: "FR_INVALID_OBJECT",
    10: "FR_WRITE_PROTECTED",
    11: "FR_INVALID_DRIVE",
    12: "FR_NOT_ENABLED",
    13: "FR_NO_FILESYSTEM",
    14: "FR_MKFS_ABORTED",
    15: "FR_TIMEOUT",
    16: "FR_LOCKED",
    17: "FR_NOT_ENOUGH_CORE",
    18: "FR_TOO_MANY_OPEN_FILES",
    19: "FR_INVALID_PARAMETER",
    0xFFFFFFFF: "文件写后校验失败",
}


def normalize_saved_settings(body):
    config = body.get("config")
    ui_settings = body.get("ui_settings", {})
    if not isinstance(config, dict) or not isinstance(ui_settings, dict):
        raise ValueError("保存参数格式错误")

    board_ip = str(body["board_ip"])
    board_port = int(body["board_port"])
    timeout_ms = int(body["timeout_ms"])
    socket.inet_aton(board_ip)
    if not (1 <= board_port <= 65535):
        raise ValueError("UDP端口无效")
    if timeout_ms < 1:
        raise ValueError("超时时间无效")

    config = dict(config)
    if "start_freq_hz" not in config and {
        "center_freq_hz", "half_bandwidth_hz"
    }.issubset(config):
        center_hz = int(config["center_freq_hz"])
        half_bandwidth_hz = int(config["half_bandwidth_hz"])
        config["start_freq_hz"] = center_hz - half_bandwidth_hz
        config["bandwidth_hz"] = half_bandwidth_hz * 2
    if "coarse_lo_guard_hz" not in config:
        blind_hz = int(config.get("coarse_lo_blind_hz", 180_000))
        step_hz = int(config.get("step_hz", 20_000))
        config["coarse_lo_guard_hz"] = max(300_000, blind_hz + step_hz)

    normalized_config = {}
    for field in CONFIG_FIELDS:
        if field in config:
            normalized_config[field] = int(config[field])
        elif field in OPTIONAL_CONFIG_DEFAULTS:
            normalized_config[field] = OPTIONAL_CONFIG_DEFAULTS[field]
        else:
            raise KeyError(field)
    power_calibration_db = float(ui_settings.get("power_calibration_db", 0))
    lo_blind_khz = float(ui_settings.get("lo_blind_khz", 180))
    if not math.isfinite(power_calibration_db):
        raise ValueError("功率校准值无效")
    if not math.isfinite(lo_blind_khz) or lo_blind_khz < 0:
        raise ValueError("LO盲区半宽无效")

    return {
        "version": 2,
        "board_ip": board_ip,
        "board_port": board_port,
        "timeout_ms": timeout_ms,
        "config": normalized_config,
        "ui_settings": {
            "power_calibration_db": power_calibration_db,
            "lo_elimination": bool(ui_settings.get("lo_elimination", False)),
            "lo_blind_khz": lo_blind_khz,
        },
    }


def load_saved_settings(path=SETTINGS_PATH):
    try:
        loaded = json.loads(Path(path).read_text(encoding="utf-8"))
        return normalize_saved_settings(loaded)
    except (FileNotFoundError, json.JSONDecodeError, KeyError, TypeError, ValueError, OSError):
        return None


def write_saved_settings(path, settings):
    path = Path(path)
    temporary_path = path.with_suffix(path.suffix + ".tmp")
    temporary_path.write_text(
        json.dumps(settings, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    temporary_path.replace(path)


class SawUdpClient:
    _lock = threading.Lock()
    _sequence = 0

    @classmethod
    def next_sequence(cls):
        with cls._lock:
            cls._sequence = (cls._sequence + 1) & 0xFFFF
            return cls._sequence

    @staticmethod
    def build_packet(command, sequence, payload=b""):
        header = MAGIC_REQUEST + bytes((command,)) + struct.pack("!HH", sequence, len(payload))
        body = header + payload
        return body + struct.pack("!I", zlib.crc32(body) & 0xFFFFFFFF)

    @staticmethod
    def parse_response(data, expected_command, expected_sequence):
        if len(data) < 14 or data[:3] != MAGIC_RESPONSE:
            raise ValueError("无效的PS应答帧")
        command = data[3]
        sequence, result, payload_length = struct.unpack("!HHH", data[4:10])
        if command != expected_command or sequence != expected_sequence:
            raise ValueError("PS应答序号或命令不匹配")
        if len(data) != 10 + payload_length + 4:
            raise ValueError("PS应答长度错误")
        expected_crc = struct.unpack("!I", data[-4:])[0]
        actual_crc = zlib.crc32(data[:-4]) & 0xFFFFFFFF
        if expected_crc != actual_crc:
            raise ValueError("PS应答CRC错误")
        if command == COMMANDS["fine_results"]:
            if payload_length < 44 or ((payload_length - 44) % 8) != 0:
                raise ValueError("PS精扫结果长度错误")
            (
                generation,
                fine_peak_count,
                peak_id,
                coarse_index,
                target_frequency_hz,
                lo_frequency_hz,
                start_frequency_hz,
                step_hz,
                total_points,
                start_index,
                returned_points,
            ) = struct.unpack("!11I", data[10:54])
            if payload_length != 44 + returned_points * 8:
                raise ValueError("PS精扫结果点数不匹配")
            energies = []
            for offset in range(54, 54 + returned_points * 8, 8):
                high, low = struct.unpack("!2I", data[offset:offset + 8])
                energies.append((high << 32) | low)
            return {
                "ok": result == 0,
                "result_code": result,
                "message": RESULT_MESSAGES.get(result, f"未知错误 {result}"),
                "generation": generation,
                "fine_peak_count": fine_peak_count,
                "peak_id": peak_id,
                "coarse_index": coarse_index,
                "target_frequency_hz": target_frequency_hz,
                "lo_frequency_hz": lo_frequency_hz,
                "start_frequency_hz": start_frequency_hz,
                "step_hz": step_hz,
                "total_points": total_points,
                "start_index": start_index,
                "energies": energies,
            }
        if command == COMMANDS["results"]:
            if payload_length < 12 or ((payload_length - 12) % 8) != 0:
                raise ValueError("PS扫描结果长度错误")
            total_points, start_index, returned_points = struct.unpack(
                "!3I", data[10:22]
            )
            if payload_length != 12 + returned_points * 8:
                raise ValueError("PS扫描结果点数不匹配")
            energies = []
            for offset in range(22, 22 + returned_points * 8, 8):
                high, low = struct.unpack("!2I", data[offset:offset + 8])
                energies.append((high << 32) | low)
            return {
                "ok": result == 0,
                "result_code": result,
                "message": RESULT_MESSAGES.get(result, f"未知错误 {result}"),
                "total_points": total_points,
                "start_index": start_index,
                "energies": energies,
            }
        if payload_length != len(STATUS_FIELDS) * 4:
            raise ValueError("PS状态字段数量错误")
        values = struct.unpack(f"!{len(STATUS_FIELDS)}I", data[10:-4])
        status = dict(zip(STATUS_FIELDS, values))
        status["busy"] = bool(status["pl_status"] & 0x02)
        status["done_latched"] = bool(status["pl_status"] & 0x04)
        status["force_rx_on"] = bool(status["pl_status"] & 0x08)
        status["force_tx_on"] = bool(status["pl_status"] & 0x10)
        message = RESULT_MESSAGES.get(result, f"未知错误 {result}")
        if result == 7:
            fatfs_error = int(status.get("emmc_last_error", 0xFFFFFFFF))
            message = (
                f"{message}：mounted={status.get('emmc_mounted', 0)}，"
                f"valid={status.get('emmc_config_valid', 0)}，"
                f"{FATFS_RESULT_MESSAGES.get(fatfs_error, f'FatFs错误{fatfs_error}')}"
            )
        return {
            "ok": result == 0,
            "result_code": result,
            "message": message,
            "status": status,
        }

    def transact(self, board_ip, board_port, timeout_ms, command_name,
                 config=None, result_start=0, result_count=10,
                 notify_port=0, peak_id=0, persist_receiver=False):
        command = COMMANDS[command_name]
        sequence = self.next_sequence()
        payload = b""
        if command_name == "config":
            values = []
            for field in CONFIG_FIELDS:
                value = int(config[field])
                if value < 0 or value > 0xFFFFFFFF:
                    raise ValueError(f"{field}超出uint32范围")
                values.append(value)
            payload = struct.pack(f"!{len(values)}I", *values)
        elif command_name == "results":
            payload = struct.pack("!2I", int(result_start), int(result_count))
        elif command_name == "fine_results":
            payload = struct.pack(
                "!3I", int(peak_id), int(result_start), int(result_count)
            )
        elif command_name == "start" and notify_port:
            payload = struct.pack("!I", int(notify_port))
        elif command_name == "subscribe":
            if not notify_port:
                raise ValueError("subscribe requires a UDP receive port")
            payload = struct.pack(
                "!2I", int(notify_port), int(bool(persist_receiver))
            )
        packet = self.build_packet(command, sequence, payload)

        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
            udp.settimeout(max(0.1, timeout_ms / 1000.0))
            udp.sendto(packet, (board_ip, board_port))
            response, source = udp.recvfrom(2048)
        parsed = self.parse_response(response, command, sequence)
        parsed["source"] = f"{source[0]}:{source[1]}"
        parsed["sequence"] = sequence
        return parsed

    def fetch_results(self, board_ip, board_port, timeout_ms):
        initial = self.transact(board_ip, board_port, timeout_ms, "status")
        status = initial["status"]
        energies = []
        total_points = None
        start_index = 0
        points_per_packet = RESULT_POINTS_PER_PACKET
        while total_points is None or start_index < total_points:
            chunk = self.transact(
                board_ip,
                board_port,
                timeout_ms,
                "results",
                result_start=start_index,
                result_count=(
                    points_per_packet
                    if total_points is None
                    else min(points_per_packet, total_points - start_index)
                ),
            )
            # Remain compatible with an older board ELF whose result packet
            # limit was ten points. The new ELF returns all 101 points at once.
            if (not chunk["ok"] and points_per_packet > 10 and
                    chunk.get("result_code") == 1):
                points_per_packet = 10
                continue
            if not chunk["ok"]:
                raise ValueError(chunk["message"])
            if total_points is None:
                total_points = chunk["total_points"]
            if chunk["total_points"] != total_points or not chunk["energies"]:
                raise ValueError("PS扫描结果在读取过程中发生变化")
            energies.extend(chunk["energies"])
            start_index += len(chunk["energies"])

        final_status = self.transact(
            board_ip, board_port, timeout_ms, "status"
        )["status"]
        result = self.build_scan_result(final_status, energies)
        result["fine_scans"] = self.fetch_fine_results(
            board_ip,
            board_port,
            timeout_ms,
            final_status,
        )
        result["message"] = (
            f"读取到{len(energies)}个粗扫点及"
            f"{len(result['fine_scans'])}条精扫曲线"
        )
        return result

    def fetch_fine_results(self, board_ip, board_port, timeout_ms, status):
        fine_scans = []
        fine_peak_count = int(status.get("fine_peak_count", 0))
        generation = int(status.get("result_generation", 0))
        for peak_id in range(fine_peak_count):
            energies = []
            metadata = None
            start_index = 0
            while metadata is None or start_index < metadata["total_points"]:
                remaining = (
                    RESULT_POINTS_PER_PACKET
                    if metadata is None
                    else metadata["total_points"] - start_index
                )
                chunk = self.transact(
                    board_ip,
                    board_port,
                    timeout_ms,
                    "fine_results",
                    peak_id=peak_id,
                    result_start=start_index,
                    result_count=min(RESULT_POINTS_PER_PACKET, remaining),
                )
                if not chunk["ok"]:
                    raise ValueError(chunk["message"])
                if (
                    chunk["generation"] != generation
                    or chunk["fine_peak_count"] != fine_peak_count
                    or chunk["peak_id"] != peak_id
                    or chunk["start_index"] != start_index
                    or not chunk["energies"]
                ):
                    raise ValueError("PS精扫结果在读取过程中发生变化")
                current_metadata = {
                    field: chunk[field]
                    for field in (
                        "coarse_index",
                        "target_frequency_hz",
                        "lo_frequency_hz",
                        "start_frequency_hz",
                        "step_hz",
                        "total_points",
                    )
                }
                if metadata is not None and current_metadata != metadata:
                    raise ValueError("PS精扫结果元数据不一致")
                metadata = current_metadata
                energies.extend(chunk["energies"])
                start_index += len(chunk["energies"])

            if len(energies) != metadata["total_points"]:
                raise ValueError("PS精扫结果点数不完整")
            fine_result = self.build_scan_result(
                status,
                energies,
                start_frequency_hz=metadata["start_frequency_hz"],
                step_hz=metadata["step_hz"],
                center_frequency_hz=metadata["target_frequency_hz"],
                lo_frequency_hz=metadata["lo_frequency_hz"],
            )
            fine_scans.append({
                "peak_id": peak_id,
                "coarse_index": metadata["coarse_index"],
                "target_frequency_hz": metadata["target_frequency_hz"],
                "lo_frequency_hz": metadata["lo_frequency_hz"],
                "scan": fine_result["scan"],
            })
        return fine_scans

    @staticmethod
    def build_scan_result(
        status,
        energies,
        *,
        start_frequency_hz=None,
        step_hz=None,
        center_frequency_hz=None,
        lo_frequency_hz=None,
    ):
        point_count = len(energies)
        if point_count == 0:
            raise ValueError("扫描结果为空")
        start_frequency = (
            status["start_freq_hz"]
            if start_frequency_hz is None
            else int(start_frequency_hz)
        )
        step_frequency = (
            status["step_hz"] if step_hz is None else int(step_hz)
        )
        center_frequency = (
            status["start_freq_hz"] + status["bandwidth_hz"] // 2
            if center_frequency_hz is None
            else int(center_frequency_hz)
        )
        lo_frequency = (
            center_frequency
            if lo_frequency_hz is None
            else int(lo_frequency_hz)
        )
        peak_index = max(range(point_count), key=energies.__getitem__)
        peak_energy = energies[peak_index]
        detector_normalization_divisor = max(
            1,
            int(status.get("rx_samples_last", 1)) ** 2
            * int(status.get("noncoherent_segments", 1))
            * int(status.get("coherent_repetitions", 1)),
        )
        dbfs_normalization_divisor = (
            detector_normalization_divisor * ADC_FULL_SCALE_CODE ** 2
        )
        power_dbfs = [
            10.0 * math.log10(value / dbfs_normalization_divisor)
            if value else None
            for value in energies
        ]
        invalid_power_indexes = [
            index for index, value in enumerate(power_dbfs)
            if value is not None and value > MAX_REASONABLE_POWER_DBFS
        ]
        if invalid_power_indexes:
            raise ValueError(
                "扫描能量超过数字满量程合理范围，结果可能存在BRAM读延迟错位"
            )
        power_dbm = [
            value + DBM_CALIBRATION_OFFSET_DB if value is not None else None
            for value in power_dbfs
        ]
        peak_offset = 0.0
        if (
            0 < peak_index < point_count - 1
            and all(power_dbfs[index] is not None for index in
                    (peak_index - 1, peak_index, peak_index + 1))
        ):
            left = power_dbfs[peak_index - 1]
            center = power_dbfs[peak_index]
            right = power_dbfs[peak_index + 1]
            denominator = left - (2.0 * center) + right
            if abs(denominator) > 1e-12:
                peak_offset = max(-1.0, min(1.0, 0.5 * (left - right) / denominator))
        resonance_frequency = start_frequency + (peak_index + peak_offset) * step_frequency
        points = [
            {
                "index": index,
                "frequency_hz": start_frequency + index * step_frequency,
                "energy": str(energy),
                "power_dbfs": power_dbfs[index],
                "power_dbm": power_dbm[index],
            }
            for index, energy in enumerate(energies)
        ]
        return {
            "ok": True,
            "result_code": 0,
            "message": f"读取到{point_count}个扫描频点",
            "status": status,
            "scan": {
                "point_count": point_count,
                "center_frequency_hz": center_frequency,
                "lo_frequency_hz": lo_frequency,
                "start_frequency_hz": start_frequency,
                "stop_frequency_hz": start_frequency + (point_count - 1) * step_frequency,
                "step_hz": step_frequency,
                "peak_index": peak_index,
                "peak_offset": peak_offset,
                "peak_energy": str(peak_energy),
                "peak_power_dbfs": power_dbfs[peak_index],
                "peak_power_dbm": power_dbm[peak_index],
                "dbm_calibration_offset_db": DBM_CALIBRATION_OFFSET_DB,
                "power_normalization_divisor": detector_normalization_divisor,
                "adc_full_scale_code": ADC_FULL_SCALE_CODE,
                "rx_manual_gain_db": RX_MANUAL_GAIN_DB,
                "detector_type": "complex_ddc_coherent_integrator",
                "resonance_frequency_hz": resonance_frequency,
                "points": points,
            },
        }


def _solve_three_by_three(matrix, vector):
    augmented = [
        [float(matrix[row][column]) for column in range(3)] + [float(vector[row])]
        for row in range(3)
    ]
    for pivot in range(3):
        best = max(range(pivot, 3), key=lambda row: abs(augmented[row][pivot]))
        if abs(augmented[best][pivot]) < 1e-12:
            return None
        augmented[pivot], augmented[best] = augmented[best], augmented[pivot]
        divisor = augmented[pivot][pivot]
        augmented[pivot] = [value / divisor for value in augmented[pivot]]
        for row in range(3):
            if row == pivot:
                continue
            factor = augmented[row][pivot]
            augmented[row] = [
                augmented[row][column] - factor * augmented[pivot][column]
                for column in range(4)
            ]
    return tuple(augmented[row][3] for row in range(3))


def refine_fine_scan(scan):
    """Add upper-computer SG smoothing and a robust local quadratic peak."""
    if int(scan.get("refinement_version", 0)) >= FINE_SMOOTHING_VERSION:
        return scan
    points = scan.get("points") or []
    raw = [point.get("power_dbfs") for point in points]
    if len(points) < 2 or not all(value is None or math.isfinite(float(value)) for value in raw):
        return scan

    scan["ps_peak_index"] = scan.get("peak_index")
    scan["ps_peak_offset"] = scan.get("peak_offset")
    scan["ps_resonance_frequency_hz"] = scan.get("resonance_frequency_hz")
    scan["ps_peak_power_dbfs"] = scan.get("peak_power_dbfs")
    scan["ps_peak_power_dbm"] = scan.get("peak_power_dbm")
    valid_indexes = [index for index, value in enumerate(raw) if value is not None]
    if len(valid_indexes) < 7:
        scan.update({
            "refinement_version": FINE_SMOOTHING_VERSION,
            "refinement_method": "fallback_ps_insufficient_points",
            "fit_valid": False,
            "fit_r_squared": None,
        })
        return scan

    smoothed = list(raw)
    half_sg = len(FINE_SG11_COEFFICIENTS) // 2
    for index in range(half_sg, len(raw) - half_sg):
        window = raw[index - half_sg:index + half_sg + 1]
        if all(value is not None for value in window):
            smoothed[index] = sum(
                coefficient * float(value)
                for coefficient, value in zip(FINE_SG11_COEFFICIENTS, window)
            ) / FINE_SG11_DIVISOR

    calibration_offset = float(scan.get("dbm_calibration_offset_db", 0.0))
    for index, point in enumerate(points):
        value = smoothed[index]
        point["smoothed_power_dbfs"] = value
        point["smoothed_power_dbm"] = (
            value + calibration_offset if value is not None else None
        )

    smooth_peak_index = max(
        (index for index, value in enumerate(smoothed) if value is not None),
        key=lambda index: smoothed[index],
    )
    start = max(0, smooth_peak_index - FINE_FIT_HALF_WINDOW)
    stop = min(len(points), smooth_peak_index + FINE_FIT_HALF_WINDOW + 1)
    samples = [
        (index - smooth_peak_index, float(smoothed[index]))
        for index in range(start, stop) if smoothed[index] is not None
    ]
    sums = {
        power: sum(float(x) ** power for x, _ in samples)
        for power in range(5)
    }
    y0 = sum(y for _, y in samples)
    y1 = sum(x * y for x, y in samples)
    y2 = sum(x * x * y for x, y in samples)
    solved = _solve_three_by_three(
        (
            (sums[4], sums[3], sums[2]),
            (sums[3], sums[2], sums[1]),
            (sums[2], sums[1], sums[0]),
        ),
        (y2, y1, y0),
    )
    fit_valid = False
    r_squared = None
    fitted_position = float(smooth_peak_index)
    fitted_power_dbfs = float(smoothed[smooth_peak_index])
    if solved is not None:
        quadratic, linear, constant = solved
        predictions = [quadratic * x * x + linear * x + constant for x, _ in samples]
        mean_y = sum(y for _, y in samples) / len(samples)
        total_error = sum((y - mean_y) ** 2 for _, y in samples)
        residual_error = sum(
            (y - predicted) ** 2
            for (_, y), predicted in zip(samples, predictions)
        )
        r_squared = 1.0 - residual_error / total_error if total_error > 1e-12 else None
        vertex = -linear / (2.0 * quadratic) if abs(quadratic) > 1e-12 else None
        fit_valid = (
            quadratic < 0.0
            and vertex is not None
            and start - smooth_peak_index <= vertex <= stop - 1 - smooth_peak_index
            and (r_squared is None or r_squared >= 0.50)
        )
        if fit_valid:
            fitted_position = smooth_peak_index + vertex
            fitted_power_dbfs = quadratic * vertex * vertex + linear * vertex + constant

    first_frequency = float(points[0]["frequency_hz"])
    if len(points) > 1:
        frequency_step = float(points[1]["frequency_hz"]) - first_frequency
    else:
        frequency_step = float(scan.get("step_hz", 0.0))
    fitted_frequency = first_frequency + fitted_position * frequency_step
    nearest_index = max(0, min(len(points) - 1, int(round(fitted_position))))
    scan.update({
        "refinement_version": FINE_SMOOTHING_VERSION,
        "refinement_method": (
            "sg11_quadratic15" if fit_valid else "sg11_peak_fallback"
        ),
        "fit_valid": fit_valid,
        "fit_r_squared": r_squared,
        "fit_window_start": start,
        "fit_window_stop": stop - 1,
        "smoothed_peak_index": smooth_peak_index,
        "fitted_peak_position": fitted_position,
        "fitted_frequency_hz": fitted_frequency,
        "fitted_power_dbfs": fitted_power_dbfs,
        "fitted_power_dbm": fitted_power_dbfs + calibration_offset,
        "peak_index": nearest_index,
        "peak_offset": fitted_position - nearest_index,
        "resonance_frequency_hz": fitted_frequency,
        "peak_power_dbfs": fitted_power_dbfs,
        "peak_power_dbm": fitted_power_dbfs + calibration_offset,
    })
    return scan


def refine_fine_results(result):
    for fine_result in result.get("fine_scans") or []:
        scan = fine_result.get("scan")
        if isinstance(scan, dict):
            refine_fine_scan(scan)
    return result


CLIENT = SawUdpClient()


class SawNotificationListener(threading.Thread):
    def __init__(self, server):
        super().__init__(name="saw-result-stream-listener", daemon=True)
        self.server = server
        self.running = threading.Event()
        self.running.set()
        self.udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        if hasattr(socket, "SO_EXCLUSIVEADDRUSE"):
            self.udp.setsockopt(
                socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1
            )
        self.udp.bind(("0.0.0.0", server.notify_port))
        self.udp.settimeout(0.5)
        self.bundles = {}
        self.last_packet_at = time.monotonic()
        self.last_watchdog_check = 0.0
        self.last_subscribe_at = 0.0

    def close(self):
        self.running.clear()
        self.udp.close()

    def subscribe(self, board_ip, board_port, persist=False):
        response = CLIENT.transact(
            board_ip,
            board_port,
            self.server.timeout_ms,
            "subscribe",
            notify_port=self.server.notify_port,
            persist_receiver=persist,
        )
        if not response["ok"]:
            raise ValueError(response["message"])
        self.last_subscribe_at = time.monotonic()
        return response

    @staticmethod
    def parse_stream_packet(data):
        if len(data) < 14 or data[:3] != MAGIC_RESPONSE:
            raise ValueError("无效的PS结果推送帧")
        command = data[3]
        if command not in (RESULT_DATA_COMMAND, RESULT_END_COMMAND):
            return None
        sequence, result, payload_length = struct.unpack("!HHH", data[4:10])
        if len(data) != 10 + payload_length + 4:
            raise ValueError("PS结果推送帧长度错误")
        expected_crc = struct.unpack("!I", data[-4:])[0]
        if expected_crc != (zlib.crc32(data[:-4]) & 0xFFFFFFFF):
            raise ValueError("PS结果推送帧CRC错误")

        if command == RESULT_DATA_COMMAND:
            if result != 0 or payload_length < 48 or (payload_length - 48) % 8:
                raise ValueError("PS结果数据分片格式错误")
            values = struct.unpack("!12I", data[10:58])
            (
                generation,
                config_revision,
                scan_type,
                peak_id,
                coarse_index,
                target_frequency_hz,
                lo_frequency_hz,
                start_frequency_hz,
                step_hz,
                total_points,
                start_index,
                returned_points,
            ) = values
            if payload_length != 48 + returned_points * 8:
                raise ValueError("PS结果数据分片点数错误")
            energies = [
                (high << 32) | low
                for high, low in (
                    struct.unpack("!2I", data[offset:offset + 8])
                    for offset in range(58, 58 + returned_points * 8, 8)
                )
            ]
            return {
                "kind": "data",
                "sequence": sequence,
                "generation": generation,
                "config_revision": config_revision,
                "scan_type": scan_type,
                "peak_id": peak_id,
                "coarse_index": coarse_index,
                "target_frequency_hz": target_frequency_hz,
                "lo_frequency_hz": lo_frequency_hz,
                "start_frequency_hz": start_frequency_hz,
                "step_hz": step_hz,
                "total_points": total_points,
                "start_index": start_index,
                "energies": energies,
            }

        if payload_length < 28:
            raise ValueError("PS结果结束帧过短")
        (
            generation,
            result_code,
            packet_count,
            coarse_point_count,
            fine_peak_count,
            elapsed_us,
            status_word_count,
        ) = struct.unpack("!7I", data[10:38])
        if status_word_count != len(STATUS_FIELDS):
            raise ValueError("PS结果结束帧状态字段数不匹配")
        if payload_length != 28 + status_word_count * 4:
            raise ValueError("PS结果结束帧长度错误")
        status_values = struct.unpack(
            f"!{status_word_count}I", data[38:-4]
        )
        status = dict(zip(STATUS_FIELDS, status_values))
        status["busy"] = bool(status["pl_status"] & 0x02)
        status["done_latched"] = bool(status["pl_status"] & 0x04)
        status["force_rx_on"] = bool(status["pl_status"] & 0x08)
        status["force_tx_on"] = bool(status["pl_status"] & 0x10)
        return {
            "kind": "end",
            "sequence": sequence,
            "generation": generation,
            "result_code": result_code or result,
            "packet_count": packet_count,
            "coarse_point_count": coarse_point_count,
            "fine_peak_count": fine_peak_count,
            "elapsed_us": elapsed_us,
            "status": status,
        }

    @staticmethod
    def assemble_scan(chunks):
        if not chunks:
            return None
        chunks = sorted(chunks, key=lambda item: item["start_index"])
        expected_index = 0
        metadata_keys = (
            "config_revision",
            "scan_type",
            "peak_id",
            "coarse_index",
            "target_frequency_hz",
            "lo_frequency_hz",
            "start_frequency_hz",
            "step_hz",
            "total_points",
        )
        metadata = {key: chunks[0][key] for key in metadata_keys}
        energies = []
        for chunk in chunks:
            if any(chunk[key] != metadata[key] for key in metadata_keys):
                raise ValueError("PS结果分片元数据不一致")
            if chunk["start_index"] != expected_index:
                return None
            energies.extend(chunk["energies"])
            expected_index += len(chunk["energies"])
        if expected_index != metadata["total_points"]:
            return None
        return metadata, energies

    def try_finalize(self, key):
        bundle = self.bundles.get(key)
        if not bundle or bundle.get("end") is None:
            return
        end = bundle["end"]
        if end["result_code"] != 0:
            self.server.set_monitoring(False)
            self.server.store_error(
                f"板卡扫描任务失败，错误码 {end['result_code']}"
            )
            self.bundles.pop(key, None)
            return
        if len(bundle["chunks"]) < end["packet_count"]:
            return
        if len(bundle["chunks"]) != end["packet_count"]:
            raise ValueError("PS推送结果分片数量错误")

        coarse_chunks = [
            chunk for chunk in bundle["chunks"].values()
            if chunk["scan_type"] == 0
        ]
        coarse = self.assemble_scan(coarse_chunks)
        if coarse is None:
            return
        coarse_meta, coarse_energies = coarse
        if len(coarse_energies) != end["coarse_point_count"]:
            raise ValueError("PS推送粗扫点数错误")

        status = end["status"]
        result = CLIENT.build_scan_result(
            status,
            coarse_energies,
            start_frequency_hz=coarse_meta["start_frequency_hz"],
            step_hz=coarse_meta["step_hz"],
            center_frequency_hz=(
                status["start_freq_hz"] + status["bandwidth_hz"] // 2
            ),
            lo_frequency_hz=coarse_meta["lo_frequency_hz"],
        )
        fine_scans = []
        for peak_id in range(end["fine_peak_count"]):
            fine_chunks = [
                chunk for chunk in bundle["chunks"].values()
                if chunk["scan_type"] == 1 and chunk["peak_id"] == peak_id
            ]
            fine = self.assemble_scan(fine_chunks)
            if fine is None:
                return
            fine_meta, fine_energies = fine
            fine_result = CLIENT.build_scan_result(
                status,
                fine_energies,
                start_frequency_hz=fine_meta["start_frequency_hz"],
                step_hz=fine_meta["step_hz"],
                center_frequency_hz=fine_meta["target_frequency_hz"],
                lo_frequency_hz=fine_meta["lo_frequency_hz"],
            )
            fine_scans.append({
                "peak_id": peak_id,
                "coarse_index": fine_meta["coarse_index"],
                "target_frequency_hz": fine_meta["target_frequency_hz"],
                "lo_frequency_hz": fine_meta["lo_frequency_hz"],
                "scan": fine_result["scan"],
            })
        if len(fine_scans) != end["fine_peak_count"]:
            return

        result["fine_scans"] = fine_scans
        result["message"] = (
            f"PS主动推送：{len(coarse_energies)}个粗扫点，"
            f"{len(fine_scans)}条精扫曲线"
        )
        result["stream_generation"] = end["generation"]
        if not self.server.board_generation_seen(end["generation"]):
            self.server.store_result(result)
        self.bundles.pop(key, None)

    def accept_stream_packet(self, data, source):
        packet = self.parse_stream_packet(data)
        if packet is None:
            return
        target = self.server.monitoring_target()
        if target is not None and source[0] != target[0]:
            return
        self.last_packet_at = time.monotonic()
        key = (source[0], packet["generation"])
        bundle = self.bundles.setdefault(
            key, {"created": time.monotonic(), "chunks": {}, "end": None}
        )
        if packet["kind"] == "data":
            chunk_key = (
                packet["scan_type"],
                packet["peak_id"],
                packet["start_index"],
            )
            bundle["chunks"][chunk_key] = packet
        else:
            bundle["end"] = packet
        self.try_finalize(key)

    def watchdog_tick(self):
        now = time.monotonic()
        if now - self.last_watchdog_check < WATCHDOG_INTERVAL_SECONDS:
            return
        self.last_watchdog_check = now
        for key, bundle in list(self.bundles.items()):
            if now - bundle["created"] > 30.0:
                self.bundles.pop(key, None)

        target = self.server.monitoring_target()
        if target is None:
            return
        board_ip, board_port = target
        try:
            status_result = CLIENT.transact(
                board_ip, board_port, self.server.timeout_ms, "status"
            )
            status = status_result["status"]
            if status.get("scan_stage") == 4:
                self.server.set_monitoring(False)
                self.server.store_error("板卡扫描任务进入错误状态，自动扫描已停止")
                return
            if (
                now - self.last_packet_at > 10.0
                and now - self.last_subscribe_at > 10.0
            ):
                self.subscribe(board_ip, board_port, persist=False)
        except (socket.timeout, OSError, KeyError, TypeError, ValueError) as exc:
            self.server.store_error(f"板卡状态/结果订阅检查失败：{exc}")

    def run(self):
        while self.running.is_set():
            try:
                data, source = self.udp.recvfrom(2048)
                self.accept_stream_packet(data, source)
            except socket.timeout:
                self.watchdog_tick()
            except OSError:
                if self.running.is_set():
                    self.server.store_error("扫描结果监听失败")
            except (KeyError, TypeError, ValueError) as exc:
                self.server.store_error(str(exc))


class StabilityManager:
    """Track fine-scan peaks and persist stability data without blocking UI updates."""

    SUMMARY_HEADER = (
        "round", "timestamp", "board_generation", "track_id", "valid",
        "coarse_index", "coarse_target_hz", "coarse_frequency_hz",
        "coarse_power_dbfs", "coarse_power_dbm", "fine_peak_index",
        "ps_frequency_hz", "ps_power_dbfs", "ps_power_dbm",
        "fitted_frequency_hz", "filtered_frequency_hz",
        "fitted_power_dbfs", "fitted_power_dbm", "fit_valid",
        "fit_r_squared", "fine_target_hz", "rx_lo_hz",
    )

    REPORT_HEADER = (
        "track_id", "valid_count", "total_count", "valid_rate_percent",
        "frequency_mean_hz", "frequency_std_hz", "frequency_pp_hz",
        "frequency_std_ppm", "drift_hz_per_min", "max_jump_hz",
        "power_mean_dbm", "power_std_db", "power_pp_db",
    )

    def __init__(self, root=STABILITY_ROOT):
        self.root = Path(root)
        self.lock = threading.Lock()
        self.write_queue = queue.Queue()
        self.writer_error = None
        self.active = False
        self.state = "idle"
        self.message = "尚未开始稳定度采集"
        self.session_id = None
        self.session_dir = None
        self.target_rounds = 1000
        self.completed_rounds = 0
        self.started_at = None
        self.finished_at = None
        self.settings = {}
        self.tracks = []
        self.writer = threading.Thread(
            target=self._writer_loop,
            name="stability-csv-writer",
            daemon=True,
        )
        self.writer.start()

    @staticmethod
    def _timestamp():
        return datetime.now().astimezone().isoformat(timespec="milliseconds")

    @staticmethod
    def _safe_float(value):
        try:
            number = float(value)
            return number if math.isfinite(number) else None
        except (TypeError, ValueError):
            return None

    @staticmethod
    def _mean(values):
        return sum(values) / len(values) if values else None

    @staticmethod
    def _median(values):
        if not values:
            return None
        ordered = sorted(values)
        middle = len(ordered) // 2
        if len(ordered) % 2:
            return ordered[middle]
        return (ordered[middle - 1] + ordered[middle]) / 2.0

    @classmethod
    def _std(cls, values):
        if not values:
            return None
        mean = cls._mean(values)
        return math.sqrt(sum((value - mean) ** 2 for value in values) / len(values))

    @classmethod
    def _metrics(cls, records):
        valid = [record for record in records if record["valid"]]
        frequencies = [record["frequency_hz"] for record in valid]
        median_frequencies = [
            record.get("filtered_frequency_hz") for record in valid
            if record.get("filtered_frequency_hz") is not None
        ]
        powers = [record["power_dbm"] for record in valid if record["power_dbm"] is not None]
        frequency_mean = cls._mean(frequencies)
        frequency_std = cls._std(frequencies)
        power_mean = cls._mean(powers)
        power_std = cls._std(powers)
        drift = None
        if len(valid) >= 2:
            origin = valid[0]["elapsed_seconds"]
            times_minutes = [(record["elapsed_seconds"] - origin) / 60.0 for record in valid]
            time_mean = cls._mean(times_minutes)
            denominator = sum((value - time_mean) ** 2 for value in times_minutes)
            if denominator > 0:
                drift = sum(
                    (time_value - time_mean) * (frequency - frequency_mean)
                    for time_value, frequency in zip(times_minutes, frequencies)
                ) / denominator
        jumps = [
            abs(frequencies[index] - frequencies[index - 1])
            for index in range(1, len(frequencies))
        ]
        return {
            "valid_count": len(valid),
            "total_count": len(records),
            "valid_rate_percent": (100.0 * len(valid) / len(records)) if records else 0.0,
            "frequency_mean_hz": frequency_mean,
            "latest_median_frequency_hz": (
                median_frequencies[-1] if median_frequencies else None
            ),
            "frequency_std_hz": frequency_std,
            "frequency_pp_hz": (max(frequencies) - min(frequencies)) if frequencies else None,
            "frequency_std_ppm": (
                frequency_std / frequency_mean * 1e6
                if frequency_std is not None and frequency_mean
                else None
            ),
            "drift_hz_per_min": drift,
            "max_jump_hz": max(jumps) if jumps else None,
            "power_mean_dbm": power_mean,
            "power_std_db": power_std,
            "power_pp_db": (max(powers) - min(powers)) if powers else None,
        }

    def _writer_loop(self):
        while True:
            task = self.write_queue.get()
            try:
                if task is None:
                    return
                operation, path, payload = task
                path = Path(path)
                path.parent.mkdir(parents=True, exist_ok=True)
                if operation == "json":
                    path.write_text(
                        json.dumps(payload, ensure_ascii=False, indent=2),
                        encoding="utf-8",
                    )
                elif operation == "csv":
                    header, row = payload
                    new_file = not path.exists()
                    with path.open("a", newline="", encoding="utf-8-sig") as handle:
                        writer = csv.writer(handle)
                        if new_file:
                            writer.writerow(header)
                        writer.writerow(row)
                elif operation == "csv_all":
                    header, rows = payload
                    with path.open("w", newline="", encoding="utf-8-sig") as handle:
                        writer = csv.writer(handle)
                        writer.writerow(header)
                        writer.writerows(rows)
            except Exception as exc:  # Keep acquisition alive on disk errors.
                with self.lock:
                    self.writer_error = str(exc)
            finally:
                self.write_queue.task_done()

    def _queue_json(self, path, payload):
        self.write_queue.put(("json", path, payload))

    def _queue_csv(self, path, header, row):
        self.write_queue.put(("csv", path, (header, row)))

    def start(self, target_rounds=1000, settings=None):
        target_rounds = int(target_rounds)
        if not 1 <= target_rounds <= 100000:
            raise ValueError("稳定度采集轮数必须在1至100000之间")
        if self.active:
            self.stop("启动了新的稳定度任务")
        now = datetime.now()
        session_id = f"{now:%Y%m%d_%H%M%S}_{uuid.uuid4().hex[:6]}"
        session_dir = self.root / session_id
        session_dir.mkdir(parents=True, exist_ok=False)
        with self.lock:
            self.active = True
            self.state = "running"
            self.message = "正在采集精扫峰值稳定度"
            self.session_id = session_id
            self.session_dir = session_dir
            self.target_rounds = target_rounds
            self.completed_rounds = 0
            self.started_at = self._timestamp()
            self.started_monotonic = time.monotonic()
            self.finished_at = None
            self.settings = json.loads(json.dumps(settings or {}))
            self.tracks = []
            self.writer_error = None
            metadata = {
                "session_id": session_id,
                "target_rounds": target_rounds,
                "started_at": self.started_at,
                "settings": self.settings,
                "notes": "每轮按精扫峰值频率跟踪，最多4路；缺失峰保留无效记录。",
            }
        self._queue_json(session_dir / "config.json", metadata)
        return self.snapshot()

    def stop(self, reason="用户停止"):
        with self.lock:
            if not self.active:
                return self._snapshot_locked()
            self.active = False
            self.state = "stopped"
            self.message = reason
            self.finished_at = self._timestamp()
            session_dir = self.session_dir
            report_rows = self._report_rows_locked()
            metadata = self._snapshot_locked(include_trends=False)
        self.write_queue.put((
            "csv_all", session_dir / "report.csv", (self.REPORT_HEADER, report_rows)
        ))
        self._queue_json(session_dir / "session.json", metadata)
        return self.snapshot()

    def close(self):
        if self.active:
            self.stop("上位机关闭")
        self.write_queue.join()
        self.write_queue.put(None)
        self.write_queue.join()
        self.writer.join(timeout=2.0)

    def _calibration_delta_locked(self, scan):
        ui_settings = self.settings.get("ui_settings", {})
        requested = self._safe_float(ui_settings.get("power_calibration_db"))
        recorded = self._safe_float(scan.get("dbm_calibration_offset_db")) or 0.0
        return (requested if requested is not None else recorded) - recorded

    def _candidate(self, fine_result, coarse_scan):
        scan = fine_result.get("scan", {})
        points = scan.get("points") or []
        if not points:
            return None
        fine_index = max(0, min(len(points) - 1, int(scan.get("peak_index", 0))))
        fine_point = points[fine_index]
        frequency = self._safe_float(scan.get("resonance_frequency_hz"))
        if frequency is None:
            frequency = self._safe_float(fine_point.get("frequency_hz"))
        if frequency is None:
            return None
        coarse_index = int(fine_result.get("coarse_index", -1))
        coarse_points = coarse_scan.get("points") or []
        coarse_point = (
            coarse_points[coarse_index]
            if 0 <= coarse_index < len(coarse_points)
            else {}
        )
        fine_power_dbm = self._safe_float(scan.get("peak_power_dbm"))
        if fine_power_dbm is None:
            fine_power_dbm = self._safe_float(fine_point.get("power_dbm"))
        coarse_power_dbm = self._safe_float(coarse_point.get("power_dbm"))
        delta = self._calibration_delta_locked(scan)
        return {
            "fine_result": fine_result,
            "frequency_hz": frequency,
            "power_dbfs": self._safe_float(scan.get("peak_power_dbfs")),
            "power_dbm": fine_power_dbm + delta if fine_power_dbm is not None else None,
            "ps_frequency_hz": self._safe_float(scan.get("ps_resonance_frequency_hz")),
            "ps_power_dbfs": self._safe_float(scan.get("ps_peak_power_dbfs")),
            "ps_power_dbm": (
                self._safe_float(scan.get("ps_peak_power_dbm")) + delta
                if self._safe_float(scan.get("ps_peak_power_dbm")) is not None else None
            ),
            "fit_valid": bool(scan.get("fit_valid", False)),
            "fit_r_squared": self._safe_float(scan.get("fit_r_squared")),
            "fine_peak_index": fine_index,
            "coarse_index": coarse_index,
            "coarse_target_hz": self._safe_float(fine_result.get("target_frequency_hz")),
            "coarse_frequency_hz": self._safe_float(coarse_point.get("frequency_hz")),
            "coarse_power_dbfs": self._safe_float(coarse_point.get("power_dbfs")),
            "coarse_power_dbm": coarse_power_dbm + delta if coarse_power_dbm is not None else None,
            "fine_target_hz": self._safe_float(fine_result.get("target_frequency_hz")),
            "rx_lo_hz": self._safe_float(fine_result.get("lo_frequency_hz")),
        }

    def _matching_gate_locked(self, result):
        status = result.get("status", {})
        configured = self.settings.get("config", {})
        fine_half = int(status.get(
            "fine_half_bandwidth_hz", configured.get("fine_half_bandwidth_hz", 100000)
        ))
        coarse_step = int(status.get("step_hz", configured.get("step_hz", 1)))
        return max(1, fine_half, 3 * abs(coarse_step))

    def _make_track_locked(self, candidate, track_id=None):
        return {
            "track_id": int(track_id if track_id is not None else len(self.tracks) + 1),
            "reference_frequency_hz": candidate["frequency_hz"],
            "last_frequency_hz": candidate["frequency_hz"],
            "records": [],
        }

    def ingest(self, result):
        with self.lock:
            if not self.active:
                return
            fine_scans = result.get("fine_scans") or []
            coarse_scan = result.get("scan") or {}
            candidates = [
                candidate for candidate in (
                    self._candidate(fine_result, coarse_scan)
                    for fine_result in fine_scans[:4]
                ) if candidate is not None
            ]
            candidates.sort(key=lambda item: item["frequency_hz"])
            if not self.tracks:
                self.tracks = [
                    self._make_track_locked(candidate, track_id=index + 1)
                    for index, candidate in enumerate(candidates[:4])
                ]

            gate = self._matching_gate_locked(result)
            assignments = {}
            unmatched_tracks = set(range(len(self.tracks)))
            unmatched_candidates = set(range(len(candidates)))
            distances = sorted(
                (
                    abs(track["last_frequency_hz"] - candidate["frequency_hz"]),
                    track_index,
                    candidate_index,
                )
                for track_index, track in enumerate(self.tracks)
                for candidate_index, candidate in enumerate(candidates)
            )
            for distance, track_index, candidate_index in distances:
                if distance > gate:
                    break
                if track_index in unmatched_tracks and candidate_index in unmatched_candidates:
                    assignments[track_index] = candidate_index
                    unmatched_tracks.remove(track_index)
                    unmatched_candidates.remove(candidate_index)

            # Fill unused slots if a new peak appears after the first recorded round.
            for candidate_index in sorted(unmatched_candidates):
                if len(self.tracks) >= 4:
                    break
                track = self._make_track_locked(candidates[candidate_index])
                for old_round in range(1, self.completed_rounds + 1):
                    track["records"].append({
                        "round": old_round,
                        "valid": False,
                        "elapsed_seconds": 0.0,
                        "frequency_hz": None,
                        "filtered_frequency_hz": None,
                        "power_dbm": None,
                    })
                self.tracks.append(track)
                assignments[len(self.tracks) - 1] = candidate_index

            round_number = self.completed_rounds + 1
            timestamp = self._timestamp()
            elapsed = time.monotonic() - self.started_monotonic
            status = result.get("status", {})
            board_generation = int(status.get("result_generation", round_number))
            save_curves = round_number == 1 or round_number % 100 == 0 or round_number == self.target_rounds
            queued_rows = []
            queued_curves = []
            for track_index, track in enumerate(self.tracks):
                candidate_index = assignments.get(track_index)
                candidate = candidates[candidate_index] if candidate_index is not None else None
                valid = candidate is not None
                previous_frequency = track["last_frequency_hz"]
                if valid:
                    track["last_frequency_hz"] = candidate["frequency_hz"]
                # Match the PS tracker: the five-sample median window contains
                # accepted frequency samples, not simply the last five rounds.
                # Invalid/missing rounds therefore do not shorten the window.
                recent_frequencies = []
                for item in reversed(track["records"]):
                    if item["valid"] and item["frequency_hz"] is not None:
                        recent_frequencies.append(item["frequency_hz"])
                        if len(recent_frequencies) >= 4:
                            break
                if valid:
                    recent_frequencies.append(candidate["frequency_hz"])
                median_frequency = self._median(recent_frequencies)
                record = {
                    "round": round_number,
                    "valid": valid,
                    "elapsed_seconds": elapsed,
                    "timestamp": timestamp,
                    "frequency_hz": candidate["frequency_hz"] if valid else None,
                    "filtered_frequency_hz": median_frequency if valid else None,
                    "power_dbm": candidate["power_dbm"] if valid else None,
                }
                track["records"].append(record)
                values = candidate or {}
                queued_rows.append((
                    round_number, timestamp, board_generation, track["track_id"], int(valid),
                    values.get("coarse_index"), values.get("coarse_target_hz"),
                    values.get("coarse_frequency_hz"), values.get("coarse_power_dbfs"),
                    values.get("coarse_power_dbm"), values.get("fine_peak_index"),
                    values.get("ps_frequency_hz"), values.get("ps_power_dbfs"),
                    values.get("ps_power_dbm"), values.get("frequency_hz"),
                    median_frequency, values.get("power_dbfs"),
                    values.get("power_dbm"), int(values.get("fit_valid", False)),
                    values.get("fit_r_squared"), values.get("fine_target_hz"),
                    values.get("rx_lo_hz"),
                ))
                anomalous = valid and abs(candidate["frequency_hz"] - previous_frequency) > gate / 2
                if valid and (save_curves or anomalous):
                    queued_curves.append((track["track_id"], candidate["fine_result"]))

            self.completed_rounds = round_number
            session_dir = self.session_dir
            complete = round_number >= self.target_rounds
            if complete:
                self.active = False
                self.state = "completed"
                self.message = f"已完成{self.target_rounds}轮稳定度采集"
                self.finished_at = timestamp
                report_rows = self._report_rows_locked()
                metadata = self._snapshot_locked(include_trends=False)
            else:
                report_rows = None
                metadata = None

        for row in queued_rows:
            self._queue_csv(session_dir / "summary.csv", self.SUMMARY_HEADER, row)
        for track_id, fine_result in queued_curves:
            scan = fine_result.get("scan", {})
            rows = [
                (
                    point.get("index"), point.get("frequency_hz"), point.get("power_dbfs"),
                    point.get("power_dbm"), point.get("smoothed_power_dbfs"),
                    point.get("smoothed_power_dbm"), point.get("energy"),
                )
                for point in scan.get("points", [])
            ]
            path = session_dir / "curves" / f"round_{round_number:06d}_track_{track_id}.csv"
            self.write_queue.put((
                "csv_all", path,
                (("index", "frequency_hz", "power_dbfs", "power_dbm",
                  "smoothed_power_dbfs", "smoothed_power_dbm", "energy"), rows),
            ))
        if complete:
            self.write_queue.put((
                "csv_all", session_dir / "report.csv", (self.REPORT_HEADER, report_rows)
            ))
            self._queue_json(session_dir / "session.json", metadata)

    def _report_rows_locked(self):
        rows = []
        for track in self.tracks:
            metrics = self._metrics(track["records"])
            rows.append((track["track_id"], *(metrics[field] for field in self.REPORT_HEADER[1:])))
        return rows

    def _snapshot_locked(self, include_trends=True):
        tracks = []
        for track in self.tracks:
            metrics = self._metrics(track["records"])
            item = {
                "track_id": track["track_id"],
                "reference_frequency_hz": track["reference_frequency_hz"],
                "last_frequency_hz": track["last_frequency_hz"],
                "last_median_frequency_hz": metrics.get(
                    "latest_median_frequency_hz"
                ),
                "metrics": metrics,
            }
            if include_trends:
                valid_records = [record for record in track["records"] if record["valid"]]
                reference = valid_records[0]["frequency_hz"] if valid_records else None
                filtered_reference = (
                    valid_records[0]["filtered_frequency_hz"] if valid_records else None
                )
                item["trend"] = [
                    {
                        "round": record["round"],
                        "valid": record["valid"],
                        "frequency_hz": record["frequency_hz"],
                        "filtered_frequency_hz": record.get("filtered_frequency_hz"),
                        "delta_hz": (
                            record["frequency_hz"] - reference
                            if record["valid"] and reference is not None else None
                        ),
                        "filtered_delta_hz": (
                            record["filtered_frequency_hz"] - filtered_reference
                            if record["valid"]
                            and record.get("filtered_frequency_hz") is not None
                            and filtered_reference is not None else None
                        ),
                        "power_dbm": record["power_dbm"],
                    }
                    for record in track["records"]
                ]
            tracks.append(item)
        return {
            "active": self.active,
            "state": self.state,
            "message": self.message,
            "session_id": self.session_id,
            "session_path": str(self.session_dir) if self.session_dir else None,
            "target_rounds": self.target_rounds,
            "completed_rounds": self.completed_rounds,
            "progress_percent": (
                min(100.0, 100.0 * self.completed_rounds / self.target_rounds)
                if self.target_rounds else 0.0
            ),
            "started_at": self.started_at,
            "finished_at": self.finished_at,
            "writer_pending": self.write_queue.qsize(),
            "writer_error": self.writer_error,
            "tracks": tracks,
        }

    def snapshot(self, include_trends=True):
        with self.lock:
            return self._snapshot_locked(include_trends=include_trends)


class RequestHandler(BaseHTTPRequestHandler):
    server_version = "SAWControl/1.0"

    def log_message(self, fmt, *args):
        print(f"[{self.log_date_time_string()}] {fmt % args}")

    def send_json(self, status_code, payload):
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status_code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = urlparse(self.path).path
        if path == "/api/defaults":
            defaults = {
                "board_ip": self.server.board_ip,
                "board_port": self.server.board_port,
                "timeout_ms": self.server.timeout_ms,
            }
            saved_settings = self.server.saved_settings_snapshot()
            if saved_settings:
                defaults.update({
                    "board_ip": saved_settings["board_ip"],
                    "board_port": saved_settings["board_port"],
                    "timeout_ms": saved_settings["timeout_ms"],
                    "config": saved_settings["config"],
                    "ui_settings": saved_settings["ui_settings"],
                    "settings_loaded": True,
                })
            else:
                defaults["settings_loaded"] = False
            self.send_json(200, defaults)
            return
        if path == "/api/latest":
            self.send_json(200, self.server.latest_snapshot())
            return
        if path == "/api/stability":
            self.send_json(200, self.server.stability_snapshot())
            return
        if path == "/":
            path = "/index.html"
        requested = (WEB_ROOT / path.lstrip("/")).resolve()
        if WEB_ROOT not in requested.parents or not requested.is_file():
            self.send_error(404)
            return
        content = requested.read_bytes()
        content_type = mimetypes.guess_type(str(requested))[0] or "application/octet-stream"
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(content)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(content)

    def do_POST(self):
        path = urlparse(self.path).path
        command_name = path[5:] if path.startswith("/api/") else ""
        if command_name in ("stability/start", "stability/stop"):
            try:
                length = int(self.headers.get("Content-Length", "0"))
                if length > 65536:
                    raise ValueError("请求体过大")
                body = json.loads(self.rfile.read(length) or b"{}")
                if command_name == "stability/start":
                    snapshot = self.server.stability_manager.start(
                        body.get("target_rounds", 1000),
                        settings={
                            "config": body.get("config", {}),
                            "ui_settings": body.get("ui_settings", {}),
                        },
                    )
                else:
                    snapshot = self.server.stability_manager.stop("用户停止")
                self.send_json(200, {"ok": True, "stability": snapshot})
            except (TypeError, ValueError, OSError) as exc:
                self.send_json(400, {"ok": False, "message": str(exc)})
            return
        if command_name not in COMMANDS:
            self.send_json(404, {"ok": False, "message": "未知API"})
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if length > 65536:
                raise ValueError("请求体过大")
            body = json.loads(self.rfile.read(length) or b"{}")
            board_ip = str(body.get("board_ip", self.server.board_ip))
            board_port = int(body.get("board_port", self.server.board_port))
            timeout_ms = int(body.get("timeout_ms", self.server.timeout_ms))
            socket.inet_aton(board_ip)
            if not (1 <= board_port <= 65535):
                raise ValueError("UDP端口无效")
            if command_name == "stop":
                self.server.set_monitoring(False)
            if command_name in ("config", "start"):
                self.server.set_monitoring(True, board_ip, board_port)
                self.server.notification_listener.subscribe(
                    board_ip,
                    board_port,
                    persist=(command_name == "start"),
                )
            if command_name == "results":
                result = CLIENT.fetch_results(board_ip, board_port, timeout_ms)
            else:
                result = CLIENT.transact(
                    board_ip,
                    board_port,
                    timeout_ms,
                    command_name,
                    body.get("config"),
                    notify_port=0,
                )
            if command_name == "start" and result["ok"]:
                self.server.set_monitoring(True, board_ip, board_port)
            if command_name == "config" and result["ok"]:
                self.server.set_monitoring(True, board_ip, board_port)
                self.server.stop_stability_for_config_change()
                try:
                    self.server.save_settings(body)
                    result["settings_saved"] = True
                except (KeyError, TypeError, ValueError, OSError) as exc:
                    result["settings_saved"] = False
                    result["settings_warning"] = f"板卡配置成功，但参数保存失败：{exc}"
            if command_name == "results":
                self.server.store_result(result)
            self.send_json(200, result)
        except socket.timeout:
            self.send_json(504, {"ok": False, "message": "等待板卡UDP应答超时"})
        except (KeyError, TypeError, ValueError, OSError) as exc:
            self.send_json(400, {"ok": False, "message": str(exc)})


class SawHttpServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = False

    def server_bind(self):
        if hasattr(socket, "SO_EXCLUSIVEADDRUSE"):
            self.socket.setsockopt(
                socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1
            )
        super().server_bind()

    def initialize_result_state(self):
        self.result_lock = threading.Lock()
        self.result_session = uuid.uuid4().hex
        self.latest_generation = 0
        self.latest_result = None
        self.latest_error = None
        self.latest_warning = None
        self.latest_board_result_generation = None
        self.monitoring_enabled = False
        self.monitor_board_ip = None
        self.monitor_board_port = None

    def initialize_settings_state(self, settings_path=SETTINGS_PATH):
        self.settings_lock = threading.Lock()
        self.settings_path = Path(settings_path)
        self.saved_settings = load_saved_settings(self.settings_path)

    def initialize_stability_state(self, root=STABILITY_ROOT):
        self.stability_manager = StabilityManager(root)

    def stability_snapshot(self, include_trends=True):
        manager = getattr(self, "stability_manager", None)
        if manager is None:
            return {
                "active": False,
                "state": "idle",
                "message": "稳定度记录器尚未初始化",
                "target_rounds": 1000,
                "completed_rounds": 0,
                "progress_percent": 0.0,
                "tracks": [],
            }
        return manager.snapshot(include_trends=include_trends)

    def stop_stability_for_config_change(self):
        manager = getattr(self, "stability_manager", None)
        if manager is not None and manager.active:
            manager.stop("扫描配置已改变，当前稳定度任务已结束")

    def saved_settings_snapshot(self):
        with self.settings_lock:
            if self.saved_settings is None:
                return None
            return json.loads(json.dumps(self.saved_settings))

    def save_settings(self, body):
        settings = normalize_saved_settings(body)
        with self.settings_lock:
            write_saved_settings(self.settings_path, settings)
            self.saved_settings = settings

    def store_result(self, result, warning=None):
        refine_fine_results(result)
        with self.result_lock:
            self.latest_generation += 1
            self.latest_result = result
            self.latest_error = None
            self.latest_warning = warning
            status = result.get("status", {})
            if "result_generation" in status:
                self.latest_board_result_generation = int(
                    status["result_generation"]
                )
        manager = getattr(self, "stability_manager", None)
        if manager is not None:
            manager.ingest(result)

    def board_generation_seen(self, generation):
        with self.result_lock:
            return self.latest_board_result_generation == int(generation)

    def store_error(self, message):
        with self.result_lock:
            self.latest_error = message

    def latest_snapshot(self):
        with self.result_lock:
            snapshot = {
                "ok": self.latest_error is None,
                "session": self.result_session,
                "generation": self.latest_generation,
                "message": self.latest_error,
                "warning": self.latest_warning,
                "result": self.latest_result,
            }
        snapshot["stability"] = self.stability_snapshot(include_trends=False)
        return snapshot

    def set_monitoring(self, enabled, board_ip=None, board_port=None):
        with self.result_lock:
            self.monitoring_enabled = bool(enabled)
            if self.monitoring_enabled:
                self.monitor_board_ip = str(board_ip or self.board_ip)
                self.monitor_board_port = int(board_port or self.board_port)
            else:
                self.monitor_board_ip = None
                self.monitor_board_port = None

    def is_monitoring(self):
        with self.result_lock:
            return self.monitoring_enabled

    def monitoring_target(self):
        with self.result_lock:
            if not self.monitoring_enabled:
                return None
            return self.monitor_board_ip, self.monitor_board_port


def main():
    parser = argparse.ArgumentParser(description="SAW browser-to-UDP control bridge")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--board-ip", default="192.168.2.10")
    parser.add_argument("--board-port", type=int, default=4001)
    parser.add_argument("--timeout-ms", type=int, default=1500)
    parser.add_argument("--notify-port", type=int, default=4002)
    parser.add_argument("--open", action="store_true")
    args = parser.parse_args()

    try:
        server = SawHttpServer((args.host, args.port), RequestHandler)
    except OSError as exc:
        print(
            f"上位机启动失败：HTTP端口 {args.port} 已被占用，"
            "请先关闭已有的上位机窗口。"
        )
        print(f"系统错误：{exc}")
        return 1
    server.board_ip = args.board_ip
    server.board_port = args.board_port
    server.timeout_ms = args.timeout_ms
    server.notify_port = args.notify_port
    server.initialize_result_state()
    server.initialize_settings_state()
    server.initialize_stability_state()
    notification_listener = SawNotificationListener(server)
    server.notification_listener = notification_listener
    notification_listener.start()
    saved_settings = server.saved_settings_snapshot()
    monitor_ip = (
        saved_settings["board_ip"] if saved_settings else server.board_ip
    )
    monitor_port = (
        saved_settings["board_port"] if saved_settings else server.board_port
    )
    server.set_monitoring(True, monitor_ip, monitor_port)
    try:
        notification_listener.subscribe(
            monitor_ip, monitor_port, persist=False
        )
    except (socket.timeout, OSError, KeyError, TypeError, ValueError) as exc:
        print(f"初始结果订阅失败，后台将自动重试：{exc}")
    url = f"http://{args.host}:{args.port}"
    print(f"SAW控制页面: {url}")
    print(f"默认板卡: {args.board_ip}:{args.board_port}")
    if args.open:
        threading.Timer(0.5, lambda: webbrowser.open(url)).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        notification_listener.close()
        server.stability_manager.close()
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
