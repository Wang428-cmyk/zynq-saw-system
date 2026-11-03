const defaults = {
  startMhz: 912,
  bandwidthMhz: 6,
  stepKhz: 60,
  loGuardKhz: 300,
  fineHalfBandwidthKhz: 100,
  fineStepKhz: 1,
  txUs: 10,
  guard1Us: 25,
  rxUs: 20,
  guard2Us: 5,
  coherentCount: 64,
  noncoherentCount: 16,
  txSampleRate: 61.44,
  rxSampleRate: 61.44,
  powerCalibrationDb: 0,
};

const savedConfigBindings = {
  start_freq_hz: ["startMhz", 1e-6],
  bandwidth_hz: ["bandwidthMhz", 1e-6],
  step_hz: ["stepKhz", 1e-3],
  tx_time_us: ["txUs", 1],
  guard1_time_us: ["guard1Us", 1],
  rx_time_us: ["rxUs", 1],
  guard2_time_us: ["guard2Us", 1],
  coherent_repetitions: ["coherentCount", 1],
  noncoherent_segments: ["noncoherentCount", 1],
  tx_sampling_freq_hz: ["txSampleRate", 1e-6],
  rx_sampling_freq_hz: ["rxSampleRate", 1e-6],
  fine_half_bandwidth_hz: ["fineHalfBandwidthKhz", 1e-3],
  fine_step_hz: ["fineStepKhz", 1e-3],
  coarse_lo_guard_hz: ["loGuardKhz", 1e-3],
};

const $ = (id) => document.getElementById(id);
const fieldIds = Object.keys(defaults);
let requestInFlight = false;
let latestScan = null;
let latestFineScans = [];
let latestGeneration = 0;
let latestSession = null;
let latestPollInFlight = false;
let latestBoardStatus = null;
let selectedFrequencyHz = null;
let chartGeometry = null;
let latestAuthoritativeCoarsePeaks = null;
let latestDetectedPeakCount = 0;
let latestStability = null;
let selectedStabilityTrackId = 1;
let lastStabilityState = "idle";
let stabilityPollInFlight = false;

function numberValue(id) {
  return Number($(id).value);
}

function median(values) {
  if (!values.length) return null;
  const sorted = [...values].sort((left, right) => left - right);
  const middle = Math.floor(sorted.length / 2);
  return sorted.length % 2
    ? sorted[middle]
    : (sorted[middle - 1] + sorted[middle]) / 2;
}

const MAX_DISPLAY_PEAKS = 4;
const PEAK_COLORS = ["#b23b3b", "#6f4ba8", "#0b7f78", "#d16a18"];

function estimateNoiseProfile(powers) {
  const valid = powers.filter(Number.isFinite).sort((left, right) => left - right);
  if (!valid.length) return { floor: null, sigma: 0 };

  // 去掉最高的约 25%，避免多个真实峰把底噪估计抬高。
  const quietCount = Math.max(3, Math.ceil(valid.length * 0.75));
  const quiet = valid.slice(0, Math.min(quietCount, valid.length));
  const floor = median(quiet);
  const mad = median(quiet.map((power) => Math.abs(power - floor))) || 0;
  return { floor, sigma: Math.max(0.25, 1.4826 * mad) };
}

function interpolatePeakOffset(powers, index) {
  if (
    index <= 0
    || index >= powers.length - 1
    || ![powers[index - 1], powers[index], powers[index + 1]].every(Number.isFinite)
  ) return 0;

  const left = powers[index - 1];
  const center = powers[index];
  const right = powers[index + 1];
  const denominator = left - (2 * center) + right;
  if (Math.abs(denominator) <= 1e-12) return 0;
  return Math.max(-1, Math.min(1, 0.5 * (left - right) / denominator));
}

function detectSawPeaks(scan, rawPowers, excludedPoints) {
  const stepHz = Math.max(1, Math.abs(Number(scan.step_hz)) || 1);
  const detectionPowers = rawPowers.map((power, index) => {
    if (excludedPoints[index] || !Number.isFinite(power)) return null;
    if (
      index > 0
      && index < rawPowers.length - 1
      && !excludedPoints[index - 1]
      && !excludedPoints[index + 1]
      && Number.isFinite(rawPowers[index - 1])
      && Number.isFinite(rawPowers[index + 1])
    ) return median([rawPowers[index - 1], power, rawPowers[index + 1]]);
    return power;
  });
  const { floor: noiseFloor, sigma: noiseSigma } = estimateNoiseProfile(detectionPowers);
  if (noiseFloor === null || detectionPowers.length < 3) {
    return { peaks: [], noiseFloor, threshold: null };
  }

  const threshold = noiseFloor + Math.max(8, 4 * noiseSigma);
  const prominenceRadius = Math.max(4, Math.min(24, Math.ceil(120e3 / stepHz)));
  const candidates = [];

  for (let index = 1; index < detectionPowers.length - 1; index += 1) {
    const power = rawPowers[index];
    const left = rawPowers[index - 1];
    const right = rawPowers[index + 1];
    const supportedPower = detectionPowers[index];
    if (![power, left, right, supportedPower].every(Number.isFinite)) continue;
    if (!(power >= left && power > right)) continue;

    const leftWindow = [];
    const rightWindow = [];
    const rawLeftWindow = [];
    const rawRightWindow = [];
    for (let distance = 1; distance <= prominenceRadius; distance += 1) {
      const leftIndex = index - distance;
      const rightIndex = index + distance;
      if (leftIndex >= 0 && Number.isFinite(detectionPowers[leftIndex])) {
        leftWindow.push(detectionPowers[leftIndex]);
        rawLeftWindow.push(rawPowers[leftIndex]);
      }
      if (rightIndex < detectionPowers.length && Number.isFinite(detectionPowers[rightIndex])) {
        rightWindow.push(detectionPowers[rightIndex]);
        rawRightWindow.push(rawPowers[rightIndex]);
      }
    }
    if (!leftWindow.length || !rightWindow.length) continue;

    const supportedProminenceDb = supportedPower
      - Math.max(Math.min(...leftWindow), Math.min(...rightWindow));
    const prominenceDb = power
      - Math.max(Math.min(...rawLeftWindow), Math.min(...rawRightWindow));
    const supportedPeak = supportedPower >= threshold && supportedProminenceDb >= 6;
    const narrowStrongPeak = power >= noiseFloor + 12 && prominenceDb >= 10;
    if (!supportedPeak && !narrowStrongPeak) continue;
    let neighborhoodScore = 0;
    for (let distance = -2; distance <= 2; distance += 1) {
      const sample = rawPowers[index + distance];
      if (Number.isFinite(sample)) neighborhoodScore += Math.max(0, sample - noiseFloor);
    }
    candidates.push({ index, powerDbm: power, prominenceDb, neighborhoodScore });
  }

  // 先保留最显著的峰，再用最小间距抑制同一宽峰上的重复局部极大值。
  const minimumSeparationPoints = Math.max(3, Math.ceil(120e3 / stepHz));
  const selected = [];
  candidates
    .sort((left, right) =>
      (right.neighborhoodScore - left.neighborhoodScore)
      || (right.prominenceDb - left.prominenceDb)
      || (right.powerDbm - left.powerDbm))
    .some((candidate) => {
      const overlaps = selected.some((peak) =>
        Math.abs(peak.index - candidate.index) < minimumSeparationPoints);
      if (!overlaps) selected.push(candidate);
      return selected.length >= MAX_DISPLAY_PEAKS;
    });

  const startFrequencyHz = Number(scan.start_frequency_hz);
  const peaks = selected
    .map((peak) => {
      const offset = interpolatePeakOffset(rawPowers, peak.index);
      return {
        ...peak,
        offset,
        frequencyHz: startFrequencyHz + (peak.index + offset) * stepHz,
      };
    })
    .sort((left, right) => left.frequencyHz - right.frequencyHz);

  return { peaks, noiseFloor, threshold };
}

function coarsePeaksFromFineScans(scan, rawPowers, excludedPoints, fineScans) {
  const stepHz = Math.max(1, Math.abs(Number(scan.step_hz)) || 1);
  const startFrequencyHz = Number(scan.start_frequency_hz);
  const prominenceRadius = Math.max(4, Math.min(24, Math.ceil(120e3 / stepHz)));

  return fineScans.map((fineResult, slot) => {
    const targetFrequencyHz = Number(fineResult.target_frequency_hz);
    const reportedIndex = Number(fineResult.coarse_index);
    const frequencyIndex = Math.round((targetFrequencyHz - startFrequencyHz) / stepHz);
    const index = Math.max(
      0,
      Math.min(
        rawPowers.length - 1,
        Number.isInteger(reportedIndex) ? reportedIndex : frequencyIndex,
      ),
    );
    const power = rawPowers[index];
    const leftWindow = [];
    const rightWindow = [];
    for (let distance = 1; distance <= prominenceRadius; distance += 1) {
      const leftIndex = index - distance;
      const rightIndex = index + distance;
      if (
        leftIndex >= 0
        && !excludedPoints[leftIndex]
        && Number.isFinite(rawPowers[leftIndex])
      ) leftWindow.push(rawPowers[leftIndex]);
      if (
        rightIndex < rawPowers.length
        && !excludedPoints[rightIndex]
        && Number.isFinite(rawPowers[rightIndex])
      ) rightWindow.push(rawPowers[rightIndex]);
    }
    const prominenceDb = (
      Number.isFinite(power)
      && leftWindow.length
      && rightWindow.length
    )
      ? power - Math.max(Math.min(...leftWindow), Math.min(...rightWindow))
      : 0;
    const offset = Math.max(
      -1,
      Math.min(1, (targetFrequencyHz - startFrequencyHz) / stepHz - index),
    );
    return {
      index,
      offset,
      frequencyHz: targetFrequencyHz,
      powerDbm: power,
      prominenceDb,
      peakId: Number(fineResult.peak_id ?? slot),
      source: "ps_fine_target",
    };
  }).sort((left, right) => left.frequencyHz - right.frequencyHz);
}

function renderPeakList(peaks) {
  $("detectedPeakCount").textContent = `${peaks.length} / ${MAX_DISPLAY_PEAKS}`;
  const list = $("peakList");
  list.replaceChildren();

  for (let slot = 0; slot < MAX_DISPLAY_PEAKS; slot += 1) {
    const peak = peaks[slot];
    const card = document.createElement("article");
    card.className = `peak-card${peak ? "" : " empty"}`;
    card.style.setProperty("--peak-color", PEAK_COLORS[slot]);

    const title = document.createElement("span");
    title.className = "peak-card-title";
    title.textContent = `峰 ${slot + 1}`;
    const frequency = document.createElement("strong");
    frequency.textContent = peak ? `${(peak.frequencyHz / 1e6).toFixed(6)} MHz` : "--";
    const details = document.createElement("span");
    details.className = "peak-card-details";
    details.textContent = peak
      ? (
        peak.source === "ps_fine_target"
          ? `${peak.powerDbm.toFixed(2)} dBm · PS精扫目标`
          : `${peak.powerDbm.toFixed(2)} dBm · 显著度 ${peak.prominenceDb.toFixed(1)} dB`
      )
      : "未检测到";
    card.append(title, frequency, details);
    list.append(card);
  }
}

function connectionPayload() {
  return {
    board_ip: $("boardIp").value.trim(),
    board_port: numberValue("boardPort"),
    timeout_ms: numberValue("timeoutMs"),
  };
}

function configPayload() {
  return {
    start_freq_hz: Math.round(numberValue("startMhz") * 1e6),
    bandwidth_hz: Math.round(numberValue("bandwidthMhz") * 1e6),
    step_hz: Math.round(numberValue("stepKhz") * 1e3),
    tx_time_us: Math.round(numberValue("txUs")),
    guard1_time_us: Math.round(numberValue("guard1Us")),
    rx_time_us: Math.round(numberValue("rxUs")),
    guard2_time_us: Math.round(numberValue("guard2Us")),
    coherent_repetitions: Math.round(numberValue("coherentCount")),
    noncoherent_segments: Math.round(numberValue("noncoherentCount")),
    tx_sampling_freq_hz: Math.round(numberValue("txSampleRate") * 1e6),
    rx_sampling_freq_hz: Math.round(numberValue("rxSampleRate") * 1e6),
    fine_enabled: $("enableFineScan").checked ? 1 : 0,
    fine_half_bandwidth_hz: Math.round(numberValue("fineHalfBandwidthKhz") * 1e3),
    fine_step_hz: Math.round(numberValue("fineStepKhz") * 1e3),
    fine_lo_offset_hz: 1_000_000,
    coarse_lo_blind_hz: Math.round(numberValue("loBlindKhz") * 1e3),
    coarse_lo_guard_hz: Math.round(numberValue("loGuardKhz") * 1e3),
  };
}

function uiSettingsPayload() {
  return {
    power_calibration_db: numberValue("powerCalibrationDb"),
    lo_elimination: $("loElimination").checked,
    lo_blind_khz: numberValue("loBlindKhz"),
  };
}

function applySavedSettings(data) {
  if (data.config) {
    Object.entries(savedConfigBindings).forEach(([field, [id, scale]]) => {
      const value = Number(data.config[field]);
      if (Number.isFinite(value)) $(id).value = value * scale;
    });
    $("enableFineScan").checked = Number(data.config.fine_enabled) === 1;
  }
  if (data.ui_settings) {
    const powerCalibration = Number(data.ui_settings.power_calibration_db);
    const loBlindKhz = Number(data.ui_settings.lo_blind_khz);
    if (Number.isFinite(powerCalibration)) $("powerCalibrationDb").value = powerCalibration;
    if (Number.isFinite(loBlindKhz) && loBlindKhz >= 0) $("loBlindKhz").value = loBlindKhz;
    $("loElimination").checked = Boolean(data.ui_settings.lo_elimination);
  }
}

function configMatchesStatus(status) {
  if (!status) return false;
  const requested = configPayload();
  return Object.entries(requested).every(
    ([field, value]) => Number(status[field]) === value,
  );
}

function calculateDerived() {
  const start = numberValue("startMhz");
  const bandwidth = numberValue("bandwidthMhz");
  const loGuardKhz = numberValue("loGuardKhz");
  const stepKhz = numberValue("stepKhz");
  const startHz = Math.round(start * 1e6);
  const bandwidthHz = Math.round(bandwidth * 1e6);
  const loGuardHz = Math.round(loGuardKhz * 1e3);
  const stepHz = Math.round(stepKhz * 1e3);
  const frameUs = numberValue("txUs") + numberValue("guard1Us") + numberValue("rxUs") + numberValue("guard2Us");
  const repetitions = numberValue("coherentCount") * numberValue("noncoherentCount");
  const points = stepHz > 0 ? Math.floor(bandwidthHz / stepHz) + 1 : 0;
  const actualStop = (startHz + Math.max(0, points - 1) * stepHz) / 1e6;
  const rxSamples = Math.ceil(numberValue("rxUs") * numberValue("rxSampleRate"));
  const pointMs = frameUs * repetitions / 1000;
  const scanSeconds = pointMs * points / 1000;
  const fineEnabled = $("enableFineScan").checked;
  const fineHalfKhz = numberValue("fineHalfBandwidthKhz");
  const fineStepKhz = numberValue("fineStepKhz");
  const finePoints = fineStepKhz > 0
    ? Math.floor((fineHalfKhz * 2) / fineStepKhz) + 1
    : 0;
  const fineChunks = finePoints > 0 ? Math.ceil(finePoints / 127) : 0;
  const finePeakSeconds = pointMs * finePoints / 1000;
  const currentPeakCount = Math.max(
    0,
    Math.min(MAX_DISPLAY_PEAKS, Number(latestDetectedPeakCount) || 0),
  );
  const fineScanSeconds = fineEnabled
    ? finePeakSeconds * currentPeakCount
    : 0;
  const totalScanSeconds = scanSeconds + fineScanSeconds;

  $("startFreq").textContent = `${start.toFixed(3)} MHz`;
  $("stopFreq").textContent = `${actualStop.toFixed(3)} MHz`;
  $("pointCount").textContent = String(points);
  $("frameTime").textContent = `${frameUs.toFixed(0)} us`;
  $("repetitions").textContent = `${repetitions.toLocaleString()} 次`;
  $("pointTime").textContent = `${pointMs.toFixed(3)} ms`;
  $("scanTime").textContent = `${scanSeconds.toFixed(3)} s`;
  $("rxCaptureSamples").textContent = `${rxSamples.toLocaleString()} 点`;
  $("finePointCount").textContent = fineEnabled
    ? `${finePoints} 点 / ${fineChunks} 段`
    : "关闭";
  $("fineScanTime").textContent = fineEnabled
    ? `${fineScanSeconds.toFixed(3)} s`
    : "关闭";
  $("totalScanTime").textContent =
    `${totalScanSeconds.toFixed(3)} s / ${currentPeakCount} 峰`;

  const errors = [];
  if (!Number.isFinite(start) || start < 70 || start > 4000) errors.push("最低频率范围为70至4000 MHz");
  if (!Number.isFinite(bandwidth) || bandwidth < 0.2) errors.push("扫描带宽必须不小于0.2 MHz");
  if (
    Number.isFinite(start)
    && Number.isFinite(bandwidth)
    && (start < 70 || start + bandwidth > 4000)
  ) errors.push("粗扫起止频率必须在70至4000 MHz之间");
  if (stepHz > 0 && bandwidthHz % stepHz !== 0) errors.push("扫描带宽必须能被扫描步进整除");
  if (points < 1 || points > 127) errors.push("频点数必须在1至127之间");
  if (!Number.isFinite(loGuardKhz) || loGuardKhz <= numberValue("loBlindKhz")) {
    errors.push("粗扫LO保护偏置必须大于LO盲区半宽");
  }
  if (Number.isFinite(start) && Number.isFinite(loGuardKhz) && start - loGuardKhz / 1000 < 70) {
    errors.push("粗扫LO频率不能低于70 MHz");
  }
  if (Number.isFinite(bandwidth) && Number.isFinite(loGuardKhz)
      && bandwidth * 1000 + loGuardKhz >= 30720) {
    errors.push("单边扫描最高NCO频偏必须小于30.72 MHz");
  }
  if (Number.isFinite(bandwidth) && Number.isFinite(loGuardKhz)
      && 2 * (bandwidth * 1000 + loGuardKhz + 200) > 56000) {
    errors.push("粗扫所需射频带宽超过AD9361的56 MHz上限");
  }
  if (!Number.isFinite(fineHalfKhz) || fineHalfKhz < 1) errors.push("精扫半带宽必须不小于1 kHz");
  if (!Number.isFinite(fineStepKhz) || fineStepKhz < 1) errors.push("精扫步进必须不小于1 kHz");
  if (fineEnabled && (finePoints < 1 || finePoints > 1024)) errors.push("单个峰精扫点数必须在1至1024之间");
  if (fineEnabled && fineHalfKhz * 2 < stepKhz) {
    errors.push("精扫半带宽至少应覆盖粗扫半个步进");
  }
  if (fineEnabled && fineHalfKhz + numberValue("loBlindKhz") >= 1000) {
    errors.push("精扫半带宽与LO盲区之和必须小于1 MHz");
  }
  if (
    fineEnabled
    && (start - fineHalfKhz / 1000 < 70
      || start + bandwidth + fineHalfKhz / 1000 > 4000)
  ) errors.push("启用精扫后，粗扫范围外还需预留精扫半带宽");
  if (fineEnabled && (1000 + fineHalfKhz + 200) * 2 > 56000) {
    errors.push("精扫所需接收带宽超过AD9361的56 MHz上限");
  }
  if (numberValue("txSampleRate") !== 61.44 || numberValue("rxSampleRate") !== 61.44) errors.push("当前PL只支持61.44 MSPS");
  if (rxSamples < 1 || rxSamples > 65536) errors.push("RX窗口最多采集65536点，61.44 MSPS下约为1.067 ms");
  const configPending = latestBoardStatus !== null &&
    !configMatchesStatus(latestBoardStatus);
  const validation = $("validationMessage");
  validation.textContent = errors.length
    ? errors[0]
    : (configPending
      ? "输入参数尚未在板卡生效，请先点击“保存配置并运行”"
      : "参数已与板卡生效配置一致");
  validation.className = `validation ${(errors.length || configPending) ? "error" : "ok"}`;
  $("applyButton").disabled = errors.length > 0 || requestInFlight;
  $("startButton").disabled = errors.length > 0 || configPending || requestInFlight;
  return errors.length === 0;
}

function addLog(action, message, isError = false) {
  const item = document.createElement("li");
  const time = document.createElement("time");
  time.textContent = new Date().toLocaleTimeString();
  const text = document.createElement("span");
  text.textContent = `${action}：${message}`;
  if (isError) text.className = "error";
  item.append(time, text);
  $("logList").prepend(item);
}

function updateStatus(status) {
  latestBoardStatus = status;
  const online = $("connectionBadge");
  online.className = "badge online";
  online.innerHTML = "<i></i>UDP 在线";
  const scan = $("scanBadge");
  const stageNames = {
    0: "已停止",
    1: "正在粗扫",
    2: `正在精扫 峰${Number(status.current_fine_peak) + 1} / 段${Number(status.current_fine_chunk) + 1}`,
    3: "粗扫与精扫完成",
    4: "扫描异常",
  };
  scan.textContent = stageNames[status.scan_stage]
    || (status.done_latched ? "扫描完成，结果可读" : (status.busy ? "正在扫描" : "已停止"));
  scan.className = `badge ${(status.busy || status.scan_stage === 1 || status.scan_stage === 2) ? "running" : "stopped"}`;
  $("currentIndex").textContent = String(status.current_freq_index);
  const appliedFrameUs = status.tx_time_us + status.guard1_time_us +
    status.rx_time_us + status.guard2_time_us;
  const appliedScanSeconds = appliedFrameUs * status.coherent_repetitions *
    status.noncoherent_segments * status.point_count / 1e6;
  $("revision").textContent = `${status.config_revision} / ${status.coherent_repetitions}×${status.noncoherent_segments} / ${appliedScanSeconds.toFixed(3)} s`;
  $("rfState").textContent = `${status.force_rx_on ? "RX ON" : "RX OFF"} / ${status.force_tx_on ? "TX ON" : "TX OFF"}`;
  $("currentRxSamples").textContent = `${status.rx_samples_last.toLocaleString()} / ${status.rx_capture_depth.toLocaleString()}`;
  if (status.scan_stage === 2) {
    $("currentFrequency").textContent = `精扫峰 ${Number(status.current_fine_peak) + 1}`;
  } else {
    const frequency = status.start_freq_hz + status.current_freq_index * status.step_hz;
    $("currentFrequency").textContent = `${(frequency / 1e6).toFixed(3)} MHz`;
  }
}

function setBusy(value) {
  requestInFlight = value;
  ["statusButton", "startButton", "stopButton", "resultsButton"].forEach((id) => $(id).disabled = value);
  calculateDerived();
}

async function callApi(command, extra = {}) {
  setBusy(true);
  try {
    const response = await fetch(`/api/${command}`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ ...connectionPayload(), ...extra }),
    });
    const result = await response.json();
    if (!response.ok || !result.ok) throw new Error(result.message || `HTTP ${response.status}`);
    if (result.status) updateStatus(result.status);
    addLog(command.toUpperCase(), result.message);
    return result;
  } catch (error) {
    $("connectionBadge").className = "badge neutral";
    $("connectionBadge").innerHTML = "<i></i>连接失败";
    addLog(command.toUpperCase(), error.message, true);
    throw error;
  } finally {
    setBusy(false);
  }
}

function renderScan(scan, authoritativeFineScans = latestAuthoritativeCoarsePeaks) {
  latestScan = scan;
  latestAuthoritativeCoarsePeaks = authoritativeFineScans;
  const calibrationOffset = numberValue("powerCalibrationDb");
  const calibrationDelta = calibrationOffset - scan.dbm_calibration_offset_db;
  const rawPointPowerDbm = (point) => Number.isFinite(point.power_dbm)
    ? point.power_dbm + calibrationDelta
    : null;
  const loEliminationEnabled = $("loElimination").checked;
  const blindHalfWidthHz = Math.max(0, numberValue("loBlindKhz") || 0) * 1e3;
  const fallbackLoFrequencyHz = Number(scan.start_frequency_hz);
  const loFrequencyHz = Number.isFinite(Number(scan.lo_frequency_hz))
    ? Number(scan.lo_frequency_hz)
    : (Number.isFinite(Number(latestBoardStatus?.start_freq_hz))
      ? Number(latestBoardStatus.start_freq_hz) - Number(latestBoardStatus.coarse_lo_guard_hz || 0)
      : fallbackLoFrequencyHz);
  // LO 是否显示与 LO 是否参与找峰相互独立：LO 盲区永远不参与有效峰识别。
  const isLoBlindPoint = (point) =>
    Math.abs(Number(point.frequency_hz) - loFrequencyHz) <= blindHalfWidthHz;
  const excludedPeakPoints = scan.points.map(isLoBlindPoint);
  const noiseReference = scan.points
    .filter((point) => !isLoBlindPoint(point))
    .map(rawPointPowerDbm)
    .filter(Number.isFinite);
  const fallbackReference = scan.points
    .map(rawPointPowerDbm)
    .filter(Number.isFinite);
  const floorReference = noiseReference.length ? noiseReference : fallbackReference;
  const noiseFloor = median(floorReference);
  const medianDeviation = noiseFloor === null
    ? null
    : median(floorReference.map((power) => Math.abs(power - noiseFloor)));
  const noiseDeviationLimit = medianDeviation === null
    ? 0
    : Math.max(0.5, 3 * medianDeviation);
  const filteredNoiseSamples = noiseFloor === null
    ? []
    : floorReference.filter((power) => Math.abs(power - noiseFloor) <= noiseDeviationLimit);
  const noiseSamples = filteredNoiseSamples.length ? filteredNoiseSamples : floorReference;
  const noiseSampleSeed = noiseSamples.length
    ? Math.abs(Math.round(loFrequencyHz / 1e3)) % noiseSamples.length
    : 0;
  let blindPointOrdinal = 0;
  const displayPowers = scan.points.map((point) => {
    const rawPower = rawPointPowerDbm(point);
    if (!loEliminationEnabled || !isLoBlindPoint(point) || !noiseSamples.length) return rawPower;
    const sampleIndex = (noiseSampleSeed + blindPointOrdinal * 17) % noiseSamples.length;
    blindPointOrdinal += 1;
    return noiseSamples[sampleIndex];
  });
  const displayPowerByFrequency = new Map(
    scan.points.map((point, index) => [Number(point.frequency_hz), displayPowers[index]]),
  );
  const pointPowerDbm = (point) =>
    displayPowerByFrequency.get(Number(point.frequency_hz)) ?? null;

  const rawPowers = scan.points.map(rawPointPowerDbm);
  const detection = detectSawPeaks(scan, rawPowers, excludedPeakPoints);
  const detectedPeaks = Array.isArray(authoritativeFineScans)
    ? coarsePeaksFromFineScans(
      scan,
      rawPowers,
      excludedPeakPoints,
      authoritativeFineScans,
    )
    : detection.peaks;
  const primaryPeak = detectedPeaks.reduce((best, peak) =>
    !best || peak.powerDbm > best.powerDbm ? peak : best, null);

  $("resonanceFrequency").textContent = primaryPeak
    ? `${(primaryPeak.frequencyHz / 1e6).toFixed(6)} MHz`
    : "--";
  $("peakIndex").textContent = primaryPeak === null
    ? "--"
    : `${primaryPeak.index} (${primaryPeak.offset >= 0 ? "+" : ""}${primaryPeak.offset.toFixed(3)})`;
  $("peakPower").textContent = primaryPeak === null ? "--" : `${primaryPeak.powerDbm.toFixed(2)} dBm`;
  $("peakDbfs").textContent = primaryPeak === null
    ? "--"
    : `${(primaryPeak.powerDbm - calibrationOffset).toFixed(2)} dBFS`;
  $("resultPointCount").textContent = String(scan.point_count);
  latestDetectedPeakCount = Math.min(MAX_DISPLAY_PEAKS, detectedPeaks.length);
  renderPeakList(detectedPeaks);
  calculateDerived();
  $("chartEmpty").hidden = true;

  const canvas = $("scanChart");
  const ratio = window.devicePixelRatio || 1;
  const width = Math.max(320, canvas.clientWidth);
  const height = 300;
  canvas.width = Math.round(width * ratio);
  canvas.height = Math.round(height * ratio);
  const context = canvas.getContext("2d");
  context.setTransform(ratio, 0, 0, ratio, 0, 0);
  context.clearRect(0, 0, width, height);

  const margin = { left: 54, right: 18, top: 24, bottom: 42 };
  const plotWidth = width - margin.left - margin.right;
  const plotHeight = height - margin.top - margin.bottom;
  const powers = scan.points.map(pointPowerDbm).filter(Number.isFinite);
  let minPower = powers.length ? Math.min(...powers) : -100;
  let maxPower = powers.length ? Math.max(...powers) : -90;
  if ((maxPower - minPower) < 10) {
    const middle = (maxPower + minPower) / 2;
    minPower = middle - 5;
    maxPower = middle + 5;
  }
  const axisMin = Math.floor((minPower - 1) / 5) * 5;
  const axisMax = Math.ceil((maxPower + 1) / 5) * 5;
  const axisRange = Math.max(5, axisMax - axisMin);
  context.strokeStyle = "#d7dde0";
  context.fillStyle = "#66747c";
  context.font = "11px Segoe UI, Microsoft YaHei, sans-serif";
  context.textAlign = "right";
  context.textBaseline = "middle";
  for (let tick = 0; tick <= 4; tick += 1) {
    const value = axisMin + axisRange * tick / 4;
    const chartY = margin.top + plotHeight * (1 - tick / 4);
    context.beginPath();
    context.moveTo(margin.left, chartY);
    context.lineTo(width - margin.right, chartY);
    context.stroke();
    context.fillText(`${value.toFixed(1)}`, margin.left - 8, chartY);
  }
  context.textAlign = "left";
  context.textBaseline = "bottom";
  context.fillText("dBm", margin.left, margin.top - 5);

  const firstFrequency = scan.points[0].frequency_hz;
  const lastFrequency = scan.points[scan.points.length - 1].frequency_hz;
  context.textBaseline = "top";
  context.textAlign = "left";
  context.fillText(`${(firstFrequency / 1e6).toFixed(3)} MHz`, margin.left, height - margin.bottom + 12);
  context.textAlign = "right";
  context.fillText(`${(lastFrequency / 1e6).toFixed(3)} MHz`, width - margin.right, height - margin.bottom + 12);

  const pointX = (index) => margin.left + (scan.points.length === 1 ? plotWidth / 2 : index * plotWidth / (scan.points.length - 1));
  chartGeometry = { width, height, margin, plotWidth, plotHeight };
  context.strokeStyle = "#176d9c";
  context.lineWidth = 2;
  context.beginPath();
  scan.points.forEach((point, index) => {
    const x = pointX(index);
    const power = pointPowerDbm(point);
    const y = margin.top + plotHeight * (1 - ((power === null ? axisMin : power) - axisMin) / axisRange);
    if (index === 0) context.moveTo(x, y);
    else context.lineTo(x, y);
  });
  context.stroke();

  if (loFrequencyHz >= firstFrequency && loFrequencyHz <= lastFrequency) {
    const loPosition = scan.points.length === 1
      ? 0
      : (loFrequencyHz - firstFrequency) / (lastFrequency - firstFrequency)
        * (scan.points.length - 1);
    const loX = pointX(loPosition);
    context.strokeStyle = "#7c8990";
    context.lineWidth = 1;
    context.setLineDash([3, 4]);
    context.beginPath();
    context.moveTo(loX, margin.top);
    context.lineTo(loX, margin.top + plotHeight);
    context.stroke();
    context.setLineDash([]);
    context.fillStyle = "#66747c";
    context.font = "600 10px Segoe UI, Microsoft YaHei, sans-serif";
    context.textAlign = "center";
    context.textBaseline = "top";
    context.fillText("LO", loX, margin.top + 4);
  }

  detectedPeaks.forEach((peak, peakNumber) => {
    const peakX = pointX(peak.index + peak.offset);
    const peakY = margin.top + plotHeight
      * (1 - (peak.powerDbm - axisMin) / axisRange);
    context.strokeStyle = PEAK_COLORS[peakNumber];
    context.lineWidth = 1;
    context.setLineDash([5, 4]);
    context.beginPath();
    context.moveTo(peakX, margin.top);
    context.lineTo(peakX, margin.top + plotHeight);
    context.stroke();
    context.setLineDash([]);

    context.fillStyle = PEAK_COLORS[peakNumber];
    context.strokeStyle = "#ffffff";
    context.lineWidth = 2;
    context.beginPath();
    context.arc(peakX, peakY, 5, 0, Math.PI * 2);
    context.fill();
    context.stroke();

    context.fillStyle = PEAK_COLORS[peakNumber];
    context.font = "700 11px Segoe UI, Microsoft YaHei, sans-serif";
    context.textAlign = "center";
    context.textBaseline = "bottom";
    context.fillText(String(peakNumber + 1), peakX, Math.max(margin.top + 14, peakY - 8));
  });

  if (selectedFrequencyHz !== null && scan.points.length) {
    let selectedIndex = 0;
    let closestDifference = Number.POSITIVE_INFINITY;
    scan.points.forEach((point, index) => {
      const difference = Math.abs(Number(point.frequency_hz) - selectedFrequencyHz);
      if (difference < closestDifference) {
        closestDifference = difference;
        selectedIndex = index;
      }
    });

    const selectedPoint = scan.points[selectedIndex];
    const selectedPower = pointPowerDbm(selectedPoint);
    const selectedX = pointX(selectedIndex);
    const selectedY = margin.top + plotHeight
      * (1 - ((selectedPower === null ? axisMin : selectedPower) - axisMin) / axisRange);
    selectedFrequencyHz = Number(selectedPoint.frequency_hz);

    context.strokeStyle = "#d4a72c";
    context.lineWidth = 1;
    context.setLineDash([3, 3]);
    context.beginPath();
    context.moveTo(selectedX, margin.top);
    context.lineTo(selectedX, margin.top + plotHeight);
    if (selectedPower !== null) {
      context.moveTo(margin.left, selectedY);
      context.lineTo(margin.left + plotWidth, selectedY);
    }
    context.stroke();
    context.setLineDash([]);

    if (selectedPower !== null) {
      context.fillStyle = "#d4a72c";
      context.strokeStyle = "#ffffff";
      context.lineWidth = 2;
      context.beginPath();
      context.arc(selectedX, selectedY, 5, 0, Math.PI * 2);
      context.fill();
      context.stroke();
    }

    $("selectedFrequency").textContent = `${(selectedFrequencyHz / 1e6).toFixed(6)} MHz`;
    $("selectedPower").textContent = selectedPower === null ? "--" : `${selectedPower.toFixed(2)} dBm`;
    $("chartReadout").hidden = false;
  } else {
    $("chartReadout").hidden = true;
  }
}

function calibratedFinePowers(scan) {
  const calibrationDelta = numberValue("powerCalibrationDb")
    - Number(scan.dbm_calibration_offset_db || 0);
  return scan.points.map((point) => Number.isFinite(point.power_dbm)
    ? point.power_dbm + calibrationDelta
    : null);
}

function calibratedSmoothedFinePowers(scan) {
  const calibrationDelta = numberValue("powerCalibrationDb")
    - Number(scan.dbm_calibration_offset_db || 0);
  return scan.points.map((point) => Number.isFinite(point.smoothed_power_dbm)
    ? point.smoothed_power_dbm + calibrationDelta
    : null);
}

function drawFineChart(canvas, scan, selectedIndex = null) {
  const powers = calibratedFinePowers(scan);
  const smoothedPowers = calibratedSmoothedFinePowers(scan);
  const validPowers = [...powers, ...smoothedPowers].filter(Number.isFinite);
  const ratio = window.devicePixelRatio || 1;
  const width = Math.max(320, canvas.clientWidth);
  const height = 250;
  canvas.width = Math.round(width * ratio);
  canvas.height = Math.round(height * ratio);
  const context = canvas.getContext("2d");
  context.setTransform(ratio, 0, 0, ratio, 0, 0);
  context.clearRect(0, 0, width, height);

  const margin = { left: 54, right: 18, top: 22, bottom: 40 };
  const plotWidth = width - margin.left - margin.right;
  const plotHeight = height - margin.top - margin.bottom;
  let minPower = validPowers.length ? Math.min(...validPowers) : -100;
  let maxPower = validPowers.length ? Math.max(...validPowers) : -90;
  if (maxPower - minPower < 10) {
    const middle = (maxPower + minPower) / 2;
    minPower = middle - 5;
    maxPower = middle + 5;
  }
  const axisMin = Math.floor((minPower - 1) / 5) * 5;
  const axisMax = Math.ceil((maxPower + 1) / 5) * 5;
  const axisRange = Math.max(5, axisMax - axisMin);
  const pointX = (index) => margin.left + (
    scan.points.length === 1
      ? plotWidth / 2
      : index * plotWidth / (scan.points.length - 1)
  );
  const pointY = (power) => margin.top + plotHeight
    * (1 - ((power === null ? axisMin : power) - axisMin) / axisRange);

  context.strokeStyle = "#d7dde0";
  context.fillStyle = "#66747c";
  context.font = "11px Segoe UI, Microsoft YaHei, sans-serif";
  context.textAlign = "right";
  context.textBaseline = "middle";
  for (let tick = 0; tick <= 4; tick += 1) {
    const value = axisMin + axisRange * tick / 4;
    const y = margin.top + plotHeight * (1 - tick / 4);
    context.beginPath();
    context.moveTo(margin.left, y);
    context.lineTo(width - margin.right, y);
    context.stroke();
    context.fillText(value.toFixed(1), margin.left - 8, y);
  }
  context.textAlign = "left";
  context.textBaseline = "bottom";
  context.fillText("dBm", margin.left, margin.top - 4);

  const firstFrequency = Number(scan.points[0].frequency_hz);
  const lastFrequency = Number(scan.points[scan.points.length - 1].frequency_hz);
  context.textBaseline = "top";
  context.fillText(`${(firstFrequency / 1e6).toFixed(6)} MHz`, margin.left, height - margin.bottom + 11);
  context.textAlign = "right";
  context.fillText(`${(lastFrequency / 1e6).toFixed(6)} MHz`, width - margin.right, height - margin.bottom + 11);

  context.strokeStyle = "#9aa8b0";
  context.lineWidth = 1;
  context.beginPath();
  scan.points.forEach((point, index) => {
    const x = pointX(index);
    const y = pointY(powers[index]);
    if (index === 0) context.moveTo(x, y);
    else context.lineTo(x, y);
  });
  context.stroke();

  if (smoothedPowers.some(Number.isFinite)) {
    context.strokeStyle = "#176d9c";
    context.lineWidth = 2.2;
    context.beginPath();
    let drawing = false;
    smoothedPowers.forEach((power, index) => {
      if (!Number.isFinite(power)) {
        drawing = false;
        return;
      }
      const x = pointX(index);
      const y = pointY(power);
      if (!drawing) context.moveTo(x, y);
      else context.lineTo(x, y);
      drawing = true;
    });
    context.stroke();
  }

  const peakIndex = Math.max(
    0,
    Math.min(scan.points.length - 1, Number(scan.peak_index) || 0),
  );
  const markerIndex = selectedIndex === null ? peakIndex : selectedIndex;
  const fittedPosition = Number(scan.fitted_peak_position);
  const defaultPosition = Number.isFinite(fittedPosition) ? fittedPosition : peakIndex;
  const markerPosition = selectedIndex === null ? defaultPosition : selectedIndex;
  const fittedPower = Number(scan.fitted_power_dbm) + (
    numberValue("powerCalibrationDb") - Number(scan.dbm_calibration_offset_db || 0)
  );
  const markerPower = selectedIndex === null && Number.isFinite(fittedPower)
    ? fittedPower
    : powers[markerIndex];
  const markerX = pointX(markerPosition);
  const markerY = pointY(markerPower);
  context.strokeStyle = selectedIndex === null ? "#b23b3b" : "#d4a72c";
  context.lineWidth = 1;
  context.setLineDash([4, 4]);
  context.beginPath();
  context.moveTo(markerX, margin.top);
  context.lineTo(markerX, margin.top + plotHeight);
  context.stroke();
  context.setLineDash([]);
  if (Number.isFinite(markerPower)) {
    context.fillStyle = selectedIndex === null ? "#b23b3b" : "#d4a72c";
    context.strokeStyle = "#ffffff";
    context.lineWidth = 2;
    context.beginPath();
    context.arc(markerX, markerY, 5, 0, Math.PI * 2);
    context.fill();
    context.stroke();
  }
  return { width, margin, plotWidth, markerIndex, markerPower };
}

function renderFineScans(fineScans = []) {
  latestFineScans = fineScans;
  const panel = $("fineResultPanel");
  const summary = $("fineResultSummary");
  const container = $("fineCharts");
  summary.replaceChildren();
  container.replaceChildren();
  panel.hidden = fineScans.length === 0;
  $("fineResultCount").textContent = `${fineScans.length} 个峰`;
  if (!fineScans.length) return;

  fineScans.forEach((fineResult, slot) => {
    const scan = fineResult.scan;
    const calibrationDelta = numberValue("powerCalibrationDb")
      - Number(scan.dbm_calibration_offset_db || 0);
    const peakPoint = scan.points[scan.peak_index] || scan.points[0];
    const peakFrequency = Number.isFinite(Number(scan.resonance_frequency_hz))
      ? Number(scan.resonance_frequency_hz)
      : Number(peakPoint?.frequency_hz);
    const peakPower = Number.isFinite(Number(scan.peak_power_dbm))
      ? Number(scan.peak_power_dbm) + calibrationDelta
      : (
        Number.isFinite(Number(peakPoint?.power_dbm))
          ? Number(peakPoint.power_dbm) + calibrationDelta
          : null
      );

    const summaryItem = document.createElement("article");
    summaryItem.className = "fine-summary-item";
    const summaryTitle = document.createElement("span");
    summaryTitle.className = "fine-summary-title";
    summaryTitle.textContent = `精扫峰 ${slot + 1}`;
    const summaryMetrics = document.createElement("div");
    summaryMetrics.className = "fine-summary-metrics";
    const summaryFrequency = document.createElement("div");
    const summaryFrequencyLabel = document.createElement("span");
    summaryFrequencyLabel.textContent = scan.fit_valid ? "拟合峰值频率" : "平滑峰值频率";
    const summaryFrequencyValue = document.createElement("strong");
    summaryFrequencyValue.textContent = Number.isFinite(peakFrequency)
      ? `${(peakFrequency / 1e6).toFixed(6)} MHz`
      : "--";
    const summaryPower = document.createElement("div");
    const summaryPowerLabel = document.createElement("span");
    summaryPowerLabel.textContent = "峰值幅度";
    const summaryPowerValue = document.createElement("strong");
    summaryPowerValue.textContent = peakPower === null
      ? "--"
      : `${peakPower.toFixed(2)} dBm`;
    summaryFrequency.append(summaryFrequencyLabel, summaryFrequencyValue);
    summaryPower.append(summaryPowerLabel, summaryPowerValue);
    summaryMetrics.append(summaryFrequency, summaryPower);
    summaryItem.append(summaryTitle, summaryMetrics);
    summary.append(summaryItem);

    const card = document.createElement("article");
    card.className = "fine-chart-card";
    const heading = document.createElement("div");
    heading.className = "fine-chart-heading";
    const titleBlock = document.createElement("div");
    const title = document.createElement("h3");
    title.textContent = `精扫峰 ${slot + 1}`;
    const subtitle = document.createElement("p");
    const psFrequency = Number(scan.ps_resonance_frequency_hz);
    const fitQuality = Number(scan.fit_r_squared);
    subtitle.textContent = `粗扫目标 ${(fineResult.target_frequency_hz / 1e6).toFixed(6)} MHz · RX LO ${(fineResult.lo_frequency_hz / 1e6).toFixed(6)} MHz · ${scan.point_count} 点${Number.isFinite(psFrequency) ? ` · PS原始 ${(psFrequency / 1e6).toFixed(6)} MHz` : ""}${Number.isFinite(fitQuality) ? ` · 拟合R² ${fitQuality.toFixed(3)}` : ""}`;
    titleBlock.append(title, subtitle);

    const metrics = document.createElement("div");
    metrics.className = "fine-chart-metrics";
    const frequencyMetric = document.createElement("div");
    const frequencyLabel = document.createElement("span");
    frequencyLabel.textContent = "拟合峰值频率";
    const frequencyValue = document.createElement("strong");
    const powerMetric = document.createElement("div");
    const powerLabel = document.createElement("span");
    powerLabel.textContent = "精扫峰值幅度";
    const powerValue = document.createElement("strong");
    frequencyMetric.append(frequencyLabel, frequencyValue);
    powerMetric.append(powerLabel, powerValue);
    metrics.append(frequencyMetric, powerMetric);
    heading.append(titleBlock, metrics);

    const canvas = document.createElement("canvas");
    canvas.setAttribute("aria-label", `精扫峰${slot + 1}频率与接收功率曲线`);
    canvas.style.cursor = "crosshair";
    card.append(heading, canvas);
    container.append(card);

    const showMarker = (selectedIndex = null) => {
      const geometry = drawFineChart(canvas, scan, selectedIndex);
      const index = geometry.markerIndex;
      const point = scan.points[index];
      const power = Number.isFinite(point.power_dbm)
        ? point.power_dbm + calibrationDelta
        : null;
      frequencyLabel.textContent = selectedIndex === null
        ? (scan.fit_valid ? "拟合峰值频率" : "平滑峰值频率")
        : "选中频率";
      powerLabel.textContent = selectedIndex === null ? "拟合峰值幅度" : "选中幅度";
      const displayedFrequency = selectedIndex === null
        ? Number(scan.resonance_frequency_hz)
        : Number(point.frequency_hz);
      const displayedPower = selectedIndex === null
        ? (
          Number.isFinite(scan.peak_power_dbm)
            ? scan.peak_power_dbm + calibrationDelta
            : null
        )
        : power;
      frequencyValue.textContent = `${(displayedFrequency / 1e6).toFixed(6)} MHz`;
      powerValue.textContent = displayedPower === null ? "--" : `${displayedPower.toFixed(2)} dBm`;
      canvas._fineGeometry = geometry;
    };
    canvas.addEventListener("pointerdown", (event) => {
      if (event.button !== 0 || !canvas._fineGeometry) return;
      const bounds = canvas.getBoundingClientRect();
      const x = (event.clientX - bounds.left)
        * canvas._fineGeometry.width / bounds.width;
      const { margin, plotWidth } = canvas._fineGeometry;
      if (x < margin.left || x > margin.left + plotWidth) return;
      const position = scan.points.length === 1
        ? 0
        : (x - margin.left) * (scan.points.length - 1) / plotWidth;
      showMarker(Math.max(0, Math.min(scan.points.length - 1, Math.round(position))));
    });
    showMarker();
  });
}

function formatStabilityNumber(value, digits = 2, unit = "") {
  const number = Number(value);
  return Number.isFinite(number) ? `${number.toFixed(digits)}${unit}` : "--";
}

function drawStabilityTrend(canvas, trend, fields, unit, color) {
  const series = Array.isArray(fields)
    ? fields
    : [{ field: fields, color, width: 1.8 }];
  const ratio = window.devicePixelRatio || 1;
  const width = Math.max(320, canvas.clientWidth);
  const height = 220;
  canvas.width = Math.round(width * ratio);
  canvas.height = Math.round(height * ratio);
  const context = canvas.getContext("2d");
  context.setTransform(ratio, 0, 0, ratio, 0, 0);
  context.clearRect(0, 0, width, height);
  const margin = { left: 62, right: 18, top: 18, bottom: 35 };
  const plotWidth = width - margin.left - margin.right;
  const plotHeight = height - margin.top - margin.bottom;
  const values = series.flatMap(({ field }) => trend
    .filter((point) => Number.isFinite(Number(point[field])))
    .map((point) => Number(point[field])));
  const maxRound = Math.max(1, ...trend.map((point) => Number(point.round) || 0));
  let minimum = values.length ? Math.min(...values) : -1;
  let maximum = values.length ? Math.max(...values) : 1;
  if (maximum - minimum < 1e-9) {
    const padding = series.some(({ field }) => field === "power_dbm") ? 0.5 : 1;
    minimum -= padding;
    maximum += padding;
  } else {
    const padding = (maximum - minimum) * 0.1;
    minimum -= padding;
    maximum += padding;
  }
  const range = maximum - minimum;
  const xFor = (round) => margin.left + (Math.max(1, round) - 1) * plotWidth / Math.max(1, maxRound - 1);
  const yFor = (value) => margin.top + plotHeight * (1 - (value - minimum) / range);

  context.strokeStyle = "#d7dde0";
  context.fillStyle = "#66747c";
  context.font = "11px Segoe UI, Microsoft YaHei, sans-serif";
  context.textBaseline = "middle";
  context.textAlign = "right";
  for (let tick = 0; tick <= 4; tick += 1) {
    const value = minimum + range * tick / 4;
    const y = yFor(value);
    context.beginPath();
    context.moveTo(margin.left, y);
    context.lineTo(width - margin.right, y);
    context.stroke();
    context.fillText(
      value.toFixed(series.some(({ field }) => field === "power_dbm") ? 2 : 1),
      margin.left - 7,
      y,
    );
  }
  context.textAlign = "left";
  context.textBaseline = "top";
  context.fillText(unit, margin.left, 2);
  context.fillText("1", margin.left, height - margin.bottom + 10);
  context.textAlign = "right";
  context.fillText(String(maxRound), width - margin.right, height - margin.bottom + 10);
  context.textAlign = "center";
  context.fillText("轮次", margin.left + plotWidth / 2, height - 16);

  series.forEach(({ field, color: seriesColor, width = 1.8 }) => {
    context.strokeStyle = seriesColor;
    context.lineWidth = width;
    context.beginPath();
    let drawing = false;
    trend.forEach((point) => {
      const value = Number(point[field]);
      if (!point.valid || !Number.isFinite(value)) {
        drawing = false;
        return;
      }
      const x = xFor(Number(point.round));
      const y = yFor(value);
      if (!drawing) context.moveTo(x, y);
      else context.lineTo(x, y);
      drawing = true;
    });
    context.stroke();
  });
  if (!values.length) {
    context.fillStyle = "#7b878e";
    context.textAlign = "center";
    context.textBaseline = "middle";
    context.fillText("等待有效精扫峰值", margin.left + plotWidth / 2, margin.top + plotHeight / 2);
  }
}

function renderStability(snapshot) {
  if (!snapshot) return;
  latestStability = snapshot;
  const active = Boolean(snapshot.active);
  const target = Number(snapshot.target_rounds) || 1000;
  const completed = Number(snapshot.completed_rounds) || 0;
  const progress = Math.max(0, Math.min(100, Number(snapshot.progress_percent) || 0));
  const stateNames = {
    idle: "未开始",
    running: "正在记录",
    stopped: "已停止",
    completed: "采集完成",
  };
  $("stabilityState").textContent = stateNames[snapshot.state] || snapshot.state;
  $("stabilityStart").disabled = active;
  $("stabilityStop").disabled = !active;
  $("stabilityTarget").disabled = active;
  $("stabilityProgressBar").style.width = `${progress}%`;
  $("stabilityProgressText").textContent = `${completed} / ${target} 轮`;
  $("stabilityMessage").textContent = snapshot.writer_error
    ? `写盘错误：${snapshot.writer_error}`
    : `${snapshot.message || ""}${snapshot.writer_pending ? `；待写任务 ${snapshot.writer_pending}` : ""}`;
  $("stabilityMessage").className = `stability-message${snapshot.writer_error ? " error" : ""}`;
  $("stabilityPath").textContent = snapshot.session_path || "--";

  const tracks = snapshot.tracks || [];
  if (tracks.length && !tracks.some((track) => Number(track.track_id) === selectedStabilityTrackId)) {
    selectedStabilityTrackId = Number(tracks[0].track_id);
  }
  const select = $("stabilityTrack");
  const previous = String(selectedStabilityTrackId);
  select.replaceChildren();
  tracks.forEach((track) => {
    const option = document.createElement("option");
    option.value = String(track.track_id);
    option.textContent = `峰值轨迹 ${track.track_id} · ${(Number(track.last_frequency_hz) / 1e6).toFixed(6)} MHz`;
    select.append(option);
  });
  if (!tracks.length) {
    const option = document.createElement("option");
    option.value = "1";
    option.textContent = "等待精扫峰值";
    select.append(option);
  }
  select.value = previous;

  const cards = $("stabilityTrackCards");
  cards.replaceChildren();
  tracks.forEach((track, index) => {
    const card = document.createElement("button");
    card.type = "button";
    card.className = `stability-track-card${Number(track.track_id) === selectedStabilityTrackId ? " selected" : ""}`;
    card.style.setProperty("--peak-color", PEAK_COLORS[index % PEAK_COLORS.length]);
    const title = document.createElement("span");
    title.textContent = `峰值轨迹 ${track.track_id}`;
    const frequency = document.createElement("strong");
    frequency.textContent = `${(Number(track.last_frequency_hz) / 1e6).toFixed(6)} MHz`;
    const medianFrequency = document.createElement("span");
    const medianFrequencyHz = track.last_median_frequency_hz === null
      || track.last_median_frequency_hz === undefined
      ? Number.NaN
      : Number(track.last_median_frequency_hz);
    medianFrequency.textContent = Number.isFinite(medianFrequencyHz)
      ? `最近5轮中值 ${(medianFrequencyHz / 1e6).toFixed(6)} MHz`
      : "最近5轮中值 --";
    const valid = document.createElement("span");
    valid.textContent = `有效 ${track.metrics.valid_count} / ${track.metrics.total_count}`;
    card.append(title, frequency, medianFrequency, valid);
    card.addEventListener("click", () => {
      selectedStabilityTrackId = Number(track.track_id);
      renderStability(latestStability);
    });
    cards.append(card);
  });

  const selected = tracks.find((track) => Number(track.track_id) === selectedStabilityTrackId);
  const metrics = selected?.metrics || {};
  $("stabilityValidRate").textContent = formatStabilityNumber(metrics.valid_rate_percent, 1, "%");
  const latestMedianFrequencyHz = metrics.latest_median_frequency_hz === null
    || metrics.latest_median_frequency_hz === undefined
    ? Number.NaN
    : Number(metrics.latest_median_frequency_hz);
  $("stabilityMedianFrequency").textContent = Number.isFinite(latestMedianFrequencyHz)
    ? `${(latestMedianFrequencyHz / 1e6).toFixed(6)} MHz`
    : "--";
  $("stabilityFrequencyMean").textContent = formatStabilityNumber(metrics.frequency_mean_hz, 1, " Hz");
  $("stabilityFrequencyStd").textContent = formatStabilityNumber(metrics.frequency_std_hz, 2, " Hz");
  $("stabilityFrequencyPp").textContent = formatStabilityNumber(metrics.frequency_pp_hz, 1, " Hz");
  $("stabilityDrift").textContent = formatStabilityNumber(metrics.drift_hz_per_min, 2, " Hz/min");
  $("stabilityMaxJump").textContent = formatStabilityNumber(metrics.max_jump_hz, 1, " Hz");
  $("stabilityPowerMean").textContent = formatStabilityNumber(metrics.power_mean_dbm, 2, " dBm");
  $("stabilityPowerStd").textContent = formatStabilityNumber(metrics.power_std_db, 3, " dB");
  const trend = selected?.trend || [];
  drawStabilityTrend($("stabilityFrequencyChart"), trend, [
    { field: "delta_hz", color: "#aeb8be", width: 1.2 },
    { field: "filtered_delta_hz", color: "#176d9c", width: 2.2 },
  ], "Hz");
  drawStabilityTrend($("stabilityPowerChart"), trend, "power_dbm", "dBm", "#b23b3b");

  if (lastStabilityState === "running" && snapshot.state === "completed") {
    addLog("STABILITY", `已完成${target}轮，统计文件已生成`);
  }
  lastStabilityState = snapshot.state;
}

async function stabilityApi(action, payload = {}) {
  const response = await fetch(`/api/stability/${action}`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(payload),
  });
  const result = await response.json();
  if (!response.ok || !result.ok) throw new Error(result.message || `HTTP ${response.status}`);
  renderStability(result.stability);
  return result.stability;
}

async function pollStability() {
  if (stabilityPollInFlight) return;
  stabilityPollInFlight = true;
  try {
    const response = await fetch("/api/stability", { cache: "no-store" });
    if (response.ok) renderStability(await response.json());
  } catch (_) {
  } finally {
    stabilityPollInFlight = false;
  }
}

$("scanChart").addEventListener("pointerdown", (event) => {
  if (event.button !== 0 || !latestScan || !chartGeometry || !latestScan.points.length) return;

  const canvas = $("scanChart");
  const bounds = canvas.getBoundingClientRect();
  const canvasX = (event.clientX - bounds.left) * chartGeometry.width / bounds.width;
  const { margin, plotWidth } = chartGeometry;
  if (canvasX < margin.left || canvasX > margin.left + plotWidth) return;

  const pointPosition = latestScan.points.length === 1
    ? 0
    : (canvasX - margin.left) * (latestScan.points.length - 1) / plotWidth;
  const selectedIndex = Math.max(
    0,
    Math.min(latestScan.points.length - 1, Math.round(pointPosition)),
  );
  selectedFrequencyHz = Number(latestScan.points[selectedIndex].frequency_hz);
  renderScan(latestScan);
});

async function pollLatestResult() {
  if (latestPollInFlight) return;
  latestPollInFlight = true;
  try {
    const response = await fetch("/api/latest", { cache: "no-store" });
    const latest = await response.json();
    if (latest.session !== latestSession) {
      latestSession = latest.session;
      latestGeneration = 0;
    }
    if (!latest.ok || !latest.result || latest.generation <= latestGeneration) return;
    latestGeneration = latest.generation;
    if (latest.result.runtime_status) updateStatus(latest.result.runtime_status);
    else if (latest.result.status) updateStatus(latest.result.status);
    const fineScans = latest.result.fine_scans || [];
    const fineScanEnabled = Number(latest.result.status?.fine_enabled) === 1;
    renderScan(latest.result.scan, fineScanEnabled ? fineScans : null);
    renderFineScans(fineScans);
    addLog("AUTO", `第${latestGeneration}轮结果已更新`);
    if (latest.warning) addLog("WARN", latest.warning);
  } catch (_) {
  } finally {
    latestPollInFlight = false;
  }
}

$("configForm").addEventListener("submit", async (event) => {
  event.preventDefault();
  if (!calculateDerived()) return;
  try {
    const result = await callApi("config", {
      config: configPayload(),
      ui_settings: uiSettingsPayload(),
    });
    if (result.settings_saved) {
      addLog("SAVE", "参数已保存，下次启动将自动读取");
    } else if (result.settings_warning) {
      addLog("SAVE", result.settings_warning, true);
    }
  } catch (_) {}
});
$("statusButton").addEventListener("click", async () => { try { await callApi("status"); } catch (_) {} });
$("startButton").addEventListener("click", async () => {
  if (!calculateDerived()) return;
  try {
    const current = await callApi("status");
    if (!configMatchesStatus(current.status)) {
      addLog("START", "输入参数尚未在板卡生效，请先下发配置", true);
      calculateDerived();
      return;
    }
    await callApi("start");
  } catch (_) {}
});
$("stopButton").addEventListener("click", async () => { try { await callApi("stop"); } catch (_) {} });
$("resultsButton").addEventListener("click", async () => {
  try {
    const result = await callApi("results");
    const fineScans = result.fine_scans || [];
    const fineScanEnabled = Number(result.status?.fine_enabled) === 1;
    renderScan(result.scan, fineScanEnabled ? fineScans : null);
    renderFineScans(fineScans);
  } catch (_) {}
});
$("stabilityStart").addEventListener("click", async () => {
  if (!$("enableFineScan").checked) {
    addLog("STABILITY", "必须先启用精扫，否则没有精扫峰值可记录", true);
    return;
  }
  const targetRounds = Math.round(numberValue("stabilityTarget"));
  if (!Number.isFinite(targetRounds) || targetRounds < 1 || targetRounds > 100000) {
    addLog("STABILITY", "目标轮数必须在1至100000之间", true);
    return;
  }
  try {
    const snapshot = await stabilityApi("start", {
      target_rounds: targetRounds,
      config: configPayload(),
      ui_settings: uiSettingsPayload(),
    });
    addLog("STABILITY", `已开始记录${targetRounds}轮，保存到 ${snapshot.session_path}`);
  } catch (error) {
    addLog("STABILITY", error.message, true);
  }
});
$("stabilityStop").addEventListener("click", async () => {
  try {
    await stabilityApi("stop");
    addLog("STABILITY", "已停止记录，统计报告正在后台写入");
  } catch (error) {
    addLog("STABILITY", error.message, true);
  }
});
$("stabilityTrack").addEventListener("change", () => {
  selectedStabilityTrackId = Number($("stabilityTrack").value) || 1;
  renderStability(latestStability);
});
$("restoreButton").addEventListener("click", () => {
  for (const [id, value] of Object.entries(defaults)) $(id).value = value;
  $("enableFineScan").checked = false;
  $("loElimination").checked = false;
  $("loBlindKhz").value = 180;
  calculateDerived();
  if (latestScan) renderScan(latestScan);
});
$("clearLog").addEventListener("click", () => $("logList").replaceChildren());
fieldIds.forEach((id) => $(id).addEventListener("input", () => {
  calculateDerived();
  if (id === "powerCalibrationDb" && latestScan) {
    renderScan(latestScan);
    renderFineScans(latestFineScans);
  }
}));
["loElimination", "loBlindKhz"].forEach((id) => $(id).addEventListener("input", () => {
  calculateDerived();
  if (latestScan) renderScan(latestScan);
}));
$("enableFineScan").addEventListener("input", calculateDerived);
window.addEventListener("resize", () => {
  if (latestScan) renderScan(latestScan);
  if (latestFineScans.length) renderFineScans(latestFineScans);
  if (latestStability) renderStability(latestStability);
});
window.setInterval(pollLatestResult, 100);
window.setInterval(pollStability, 500);

fetch("/api/defaults").then((response) => response.json()).then((data) => {
  $("boardIp").value = data.board_ip;
  $("boardPort").value = data.board_port;
  $("timeoutMs").value = data.timeout_ms;
  applySavedSettings(data);
  calculateDerived();
  if (data.settings_loaded) addLog("LOAD", "已自动读取上次成功下发的参数");
}).catch(() => {});
fetch("/api/stability", { cache: "no-store" })
  .then((response) => response.json())
  .then(renderStability)
  .catch(() => {});
$("peakPower").previousElementSibling.textContent = "主峰功率";
$("peakDbfs").previousElementSibling.textContent = "主峰数字电平";
$("resonanceFrequency").previousElementSibling.textContent = "主峰频率";
$("peakIndex").previousElementSibling.textContent = "主峰索引";
renderPeakList([]);
calculateDerived();
