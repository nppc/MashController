let calibrationPoll = null;
let lastCalibrationStatus = null;
let lastAnalysedCount = -1;
let lastAnalysis = null;
let calibrationLog = { runId: null, t: [], T: [] };

const WATER_J_PER_L_C = 4186;
// Heating slope is measured over the last minutes before switch-off, where the
// stored energy has levelled off and the water rises in a straight line.
const HEATING_WINDOW_SEC = 480;
const TAU_MIN_SEC = 5;
const TAU_MAX_SEC = 1200;
const PULSE_TIMES_SEC = [5, 10, 20, 30, 60, 120, 300];

const PHASE_TEXT = {
  0: 'Ready',
  2: 'Coasting: waiting for the peak',
  3: 'Complete',
  4: 'Heating timeout',
  5: 'Stopped: temperature sensor failure',
  6: 'Stopped',
  7: 'Stopped: log full'
};

function calibrationFormatTime(seconds) {
  seconds = Number(seconds);
  if (!Number.isFinite(seconds) || seconds < 0) return '--:--';
  seconds = Math.round(seconds);
  const h = Math.floor(seconds / 3600);
  const m = Math.floor(seconds % 3600 / 60).toString().padStart(2, '0');
  const s = (seconds % 60).toString().padStart(2, '0');
  return h ? `${h}:${m}:${s}` : `${m}:${s}`;
}

function formatNumber(value, decimals, unit = '') {
  return Number.isFinite(value) ? `${value.toFixed(decimals)}${unit}` : '--';
}

function average(values) {
  return values.length ? values.reduce((sum, v) => sum + v, 0) / values.length : NaN;
}

function calibrationSamples(status) {
  const times = status.t || [];
  const temps = status.T || [];
  const samples = [];
  for (let i = 0; i < times.length; i++) {
    const t = Number(times[i]);
    const T = Number(temps[i]);
    if (Number.isFinite(t) && Number.isFinite(T)) samples.push({ t, T });
  }
  return samples;
}

function linearRegression(points) {
  if (points.length < 2) return null;
  const meanT = average(points.map(p => p.t));
  const meanTemp = average(points.map(p => p.T));
  let sxx = 0;
  let sxy = 0;
  for (const p of points) {
    sxx += (p.t - meanT) * (p.t - meanT);
    sxy += (p.t - meanT) * (p.T - meanTemp);
  }
  return sxx > 0 ? { slope: sxy / sxx, meanT, meanTemp } : null;
}

function solve3(m, v) {
  const a = m.map((row, i) => [...row, v[i]]);
  for (let col = 0; col < 3; col++) {
    let pivot = col;
    for (let row = col + 1; row < 3; row++) {
      if (Math.abs(a[row][col]) > Math.abs(a[pivot][col])) pivot = row;
    }
    if (Math.abs(a[pivot][col]) < 1e-12) return null;
    [a[col], a[pivot]] = [a[pivot], a[col]];
    for (let row = 0; row < 3; row++) {
      if (row === col) continue;
      const factor = a[row][col] / a[col][col];
      for (let k = col; k < 4; k++) a[row][k] -= factor * a[col][k];
    }
  }
  return [a[0][3] / a[0][0], a[1][3] / a[1][1], a[2][3] / a[2][2]];
}

// Coast after switch-off: T(x) = A + B * (1 - exp(-x / tau)) - r * x
//   A   temperature at switch-off
//   B   rise still to come from stored energy (stored J / water capacity)
//   r   cooling rate from heat loss (°C/s)
// For each tau the other three are linear, so tau is searched and A, B, r solved.
function fitCoast(points) {
  if (points.length < 12) return null;
  let best = null;
  const profile = [];

  for (let tau = TAU_MIN_SEC; tau <= TAU_MAX_SEC; tau += tau < 200 ? 1 : 5) {
    const m = [[0, 0, 0], [0, 0, 0], [0, 0, 0]];
    const v = [0, 0, 0];
    const basis = points.map(p => [1, 1 - Math.exp(-p.x / tau), -p.x]);
    points.forEach((p, i) => {
      const f = basis[i];
      for (let r = 0; r < 3; r++) {
        v[r] += f[r] * p.T;
        for (let c = 0; c < 3; c++) m[r][c] += f[r] * f[c];
      }
    });
    const coeff = solve3(m, v);
    if (!coeff) continue;
    let sse = 0;
    points.forEach((p, i) => {
      const f = basis[i];
      const d = coeff[0] * f[0] + coeff[1] * f[1] + coeff[2] * f[2] - p.T;
      sse += d * d;
    });
    profile.push({ tau, sse });
    if (!best || sse < best.sse) best = { tau, A: coeff[0], B: coeff[1], r: coeff[2], sse };
  }
  if (!best) return null;

  // Approximate 95 % range of tau: where the error is not significantly worse.
  const n = points.length;
  const limit = best.sse * (1 + 4 / Math.max(1, n - 4));
  const inRange = profile.filter(p => p.sse <= limit).map(p => p.tau);
  best.tauLo = Math.min(...inRange);
  best.tauHi = Math.max(...inRange);
  best.rms = Math.sqrt(best.sse / n);
  best.n = n;
  best.coastSec = points[points.length - 1].x;
  return best;
}

// Rise after switch-off for a given stored energy; same formula as HeaterModel::predictPeak.
function predictedRise(storedJ, capacity, lossW, tau) {
  const rise = storedJ / capacity;
  if (!(lossW > 0)) return { rise, tPeak: 5 * tau };
  const lossRate = lossW / capacity;
  const initialRate = storedJ / (tau * capacity);
  if (initialRate <= lossRate) return { rise: 0, tPeak: 0 };
  const tPeak = tau * Math.log(initialRate / lossRate);
  return { rise: rise * (1 - lossRate / initialRate) - lossRate * tPeak, tPeak };
}

function analyseCalibration(status) {
  const samples = calibrationSamples(status);
  const off = Number(status.heatEndSec);
  if (!Number.isFinite(off)) return null;

  const capacity = Number(status.waterLiters) * WATER_J_PER_L_C;
  const ambient = Number(status.ambientC);
  const target = Number(status.targetC);
  const rated = Number(status.heaterPowerW);

  const coast = samples.filter(p => p.t >= off).map(p => ({ x: p.t - off, T: p.T }));
  const fit = fitCoast(coast);
  if (!fit) return null;
  const tau = fit.tau;
  const warnings = [];
  const errors = [];

  // Heat loss, scaled to W per °C above room temperature.
  const coastMean = average(coast.map(p => p.T));
  const lossW = fit.r * capacity;
  const lossWPerC = coastMean - ambient > 5 ? lossW / (coastMean - ambient) : NaN;
  if (!Number.isFinite(lossWPerC)) errors.push('Water was not clearly above room temperature; heat loss cannot be scaled.');
  if (!(fit.r > 0)) errors.push('The water did not cool after the peak. Check the sensor position and the mixer.');

  // Effective power from the straight part of the heating curve.
  const heatWindow = samples.filter(p => p.t < off && p.t >= off - HEATING_WINDOW_SEC);
  const reg = heatWindow.length >= 8 ? linearRegression(heatWindow) : null;
  let powerEff = NaN;
  if (reg) {
    const levelled = 1 - Math.exp(-reg.meanT / tau);
    const heatingLoss = Number.isFinite(lossWPerC) ? lossWPerC * (reg.meanTemp - ambient) : lossW;
    powerEff = (capacity * reg.slope + heatingLoss) / levelled;
    if (levelled < 0.9) warnings.push('Heating was short compared with the time constant; power estimate is less certain.');
  } else {
    errors.push('Not enough heating data before switch-off.');
  }

  // Overshoot size compared with effective power x tau. Above 1 when sensor lag
  // and mixing add to the heat stored in the element; the controller scales by it.
  const powerFromCoast = fit.B * capacity / (tau * (1 - Math.exp(-off / tau)));
  const storeGain = powerFromCoast / powerEff;

  if (tau <= TAU_MIN_SEC || tau >= TAU_MAX_SEC || tau > fit.coastSec / 2) {
    errors.push('Time constant is outside the measurable range.');
  } else if ((fit.tauHi - fit.tauLo) / tau > 0.5) {
    warnings.push(`Time constant is poorly defined (${fit.tauLo}-${fit.tauHi} s).`);
  }
  if (off < 5 * tau) warnings.push('Heating lasted less than 5 time constants. Start with colder water.');
  if (fit.rms > 0.15) warnings.push(`Curve fits the readings loosely (RMS ${fit.rms.toFixed(2)} °C).`);
  if (!(storeGain > 0)) {
    errors.push('No overshoot was measured after switch-off.');
  } else if (storeGain < 0.8 || storeGain > 3.5) {
    warnings.push(`Overshoot is ${storeGain.toFixed(2)}x what the heating rate predicts. ` +
      'Check the sensor position and mixer; a second, slower heat store may be present.');
  }
  if (rated > 0 && Number.isFinite(powerEff)) {
    if (powerEff > rated * 1.05) warnings.push('Effective power is above the rated power. Check the water volume and the Heater Power setting.');
    if (powerEff < rated * 0.6) warnings.push('Effective power is below 60 % of rated power. Check the element, voltage and lid.');
  }
  const coastEnd = coast[coast.length - 1].x;
  const peakCoast = Number(status.peakCoastSec);
  if (Number.isFinite(peakCoast) && coastEnd - peakCoast < Number(status.peakSettleSec || 300) - 30) {
    warnings.push('The coast ended before the peak had clearly passed.');
  }

  const lossAtTarget = Number.isFinite(lossWPerC) ? lossWPerC * (target - ambient) : lossW;
  const fullHeat = predictedRise(storeGain * powerEff * tau, capacity, lossAtTarget, tau);
  const pulses = PULSE_TIMES_SEC.map(seconds => {
    const stored = storeGain * powerEff * tau * (1 - Math.exp(-seconds / tau));
    return {
      seconds,
      total: powerEff * seconds / capacity,
      after: predictedRise(stored, capacity, lossAtTarget, tau)
    };
  });

  return {
    fit, tau, off, capacity, ambient, target, rated, powerEff, powerFromCoast, storeGain,
    lossW, lossWPerC, lossAtTarget, reg, fullHeat, pulses, warnings, errors,
    measuredRise: Number(status.peakTemp) - fit.A,
    peakCoast,
    usable: errors.length === 0 && powerEff > 0 && storeGain > 0
  };
}

function calibrationChartSvg(samples, status, analysis, tMin, tMax, label) {
  const W = 600, H = 220, L = 44, R = 10, T = 10, B = 26;
  const target = Number(status.targetC);
  const off = Number(status.heatEndSec);
  const shown = samples.filter(p => p.t >= tMin && p.t <= tMax);
  let yMin = Math.min(...shown.map(p => p.T));
  let yMax = Math.max(...shown.map(p => p.T));
  if (Number.isFinite(target)) {
    yMin = Math.min(yMin, target);
    yMax = Math.max(yMax, target);
  }
  const span = Math.max(yMax - yMin, 0.5);
  yMin -= span * 0.05;
  yMax += span * 0.05;
  const x = t => L + (W - L - R) * (t - tMin) / (tMax - tMin);
  const y = v => T + (H - T - B) * (1 - (v - yMin) / (yMax - yMin));
  const path = pts => pts.map((p, i) => `${i ? 'L' : 'M'}${x(p.t).toFixed(1)},${y(p.T).toFixed(1)}`).join('');

  let svg = `<svg viewBox='0 0 ${W} ${H}' class='cal-chart' role='img' aria-label='${label}'>`;
  svg += `<line class='axis' x1='${L}' y1='${H - B}' x2='${W - R}' y2='${H - B}'/>`;
  svg += `<line class='axis' x1='${L}' y1='${T}' x2='${L}' y2='${H - B}'/>`;
  svg += `<text x='${L - 4}' y='${y(yMax) + 4}' text-anchor='end'>${yMax.toFixed(1)}</text>`;
  svg += `<text x='${L - 4}' y='${y(yMin)}' text-anchor='end'>${yMin.toFixed(1)}</text>`;
  svg += `<text x='${L}' y='${H - 8}'>${calibrationFormatTime(tMin)}</text>`;
  svg += `<text x='${(L + W - R) / 2}' y='${H - 8}' text-anchor='middle'>${label}</text>`;
  svg += `<text x='${W - R}' y='${H - 8}' text-anchor='end'>${calibrationFormatTime(tMax)}</text>`;
  if (Number.isFinite(target)) {
    svg += `<line class='target' x1='${L}' y1='${y(target)}' x2='${W - R}' y2='${y(target)}'/>`;
  }
  if (Number.isFinite(off) && off >= tMin && off <= tMax) {
    svg += `<line class='off' x1='${x(off)}' y1='${T}' x2='${x(off)}' y2='${H - B}'/>`;
    svg += `<text x='${x(off) + 4}' y='${T + 12}'>heater off</text>`;
  }
  svg += `<path class='data' d='${path(shown)}'/>`;
  if (analysis) {
    const f = analysis.fit;
    const fitted = [];
    for (let i = 0; i <= 80; i++) {
      const xs = f.coastSec * i / 80;
      fitted.push({ t: off + xs, T: f.A + f.B * (1 - Math.exp(-xs / f.tau)) - f.r * xs });
    }
    svg += `<path class='fit' d='${path(fitted)}'/>`;
  }
  return svg + '</svg>';
}

function renderCalibrationChart(samples, status, analysis) {
  const chart = document.getElementById('calibrationChart');
  if (samples.length < 2) {
    chart.innerHTML = '';
    return;
  }
  const tEnd = Math.max(samples[samples.length - 1].t, 60);
  let html = calibrationChartSvg(samples, status, analysis, 0, tEnd, 'Whole run');
  const off = Number(status.heatEndSec);
  if (Number.isFinite(off) && tEnd - off >= 60) {
    // Zoom on switch-off: the overshoot is too small to see on the full chart.
    html += calibrationChartSvg(samples, status, analysis, Math.max(0, off - 300), tEnd, 'Switch-off and coast');
  }
  chart.innerHTML = html;
}

function renderCalibrationTable(samples, status) {
  const off = Number(status.heatEndSec);
  document.getElementById('calibrationSampleCount').textContent = samples.length;
  document.getElementById('calibrationSamples').innerHTML = samples.length
    ? samples.map(p => {
        const phase = Number.isFinite(off) && p.t >= off ? 'Coasting' : 'Heating';
        return `<tr><td>${calibrationFormatTime(p.t)}</td><td>${phase}</td><td>${p.T.toFixed(2)} °C</td></tr>`;
      }).join('')
    : '<tr><td colspan="3">No readings yet</td></tr>';
}

function renderLiveMetrics(status, samples, analysis) {
  const off = Number(status.heatEndSec);
  const now = samples.length ? samples[samples.length - 1].t : 0;
  const recent = samples.filter(p => p.t >= now - 300 && (!Number.isFinite(off) || p.t < off));
  const reg = recent.length >= 5 ? linearRegression(recent) : null;
  const rate = reg ? reg.slope * 60 : NaN;
  const target = Number(status.targetC);
  const current = Number(status.currentTemp);

  let items;
  if (!Number.isFinite(off)) {
    const remaining = rate > 0 ? (target - current) / rate * 60 : NaN;
    items = [
      ['Heating rate', formatNumber(rate, 2, ' °C/min')],
      ['Time to target', calibrationFormatTime(remaining)],
      ['Water power', formatNumber(reg ? reg.slope * status.waterLiters * WATER_J_PER_L_C : NaN, 0, ' W')],
      ['Target', formatNumber(target, 1, ' °C')]
    ];
  } else {
    const peak = Number(status.peakTemp);
    const peakCoast = Number(status.peakCoastSec);
    const coastNow = Number(status.elapsedSec) - off;
    items = [
      ['Peak so far', formatNumber(peak, 2, ' °C')],
      ['Rise since off', formatNumber(peak - target, 2, ' °C')],
      ['Since peak', calibrationFormatTime(coastNow - peakCoast)],
      ['Preliminary τ', analysis ? `${analysis.tau} s` : '--']
    ];
  }
  document.getElementById('calibrationLiveMetrics').innerHTML = items
    .map(([label, value]) => `<div class="live-metric"><span>${label}</span><strong>${value}</strong></div>`)
    .join('');
}

function renderCalibrationResult(analysis) {
  const result = document.getElementById('calibrationResult');
  result.classList.remove('hidden');
  if (!analysis) {
    result.innerHTML = 'Not enough data to calculate values. Check the sensor and run the test again.';
    return;
  }
  const a = analysis;
  const notes = [
    ...a.errors.map(text => `<li class="calibration-error">${text}</li>`),
    ...a.warnings.map(text => `<li>${text}</li>`)
  ].join('');
  const pulseRows = a.pulses.map(p =>
    `<tr><td>${p.seconds} s</td><td>${p.total.toFixed(2)} °C</td><td>${p.after.rise.toFixed(2)} °C</td></tr>`).join('');

  result.innerHTML = `
    <div class="result-heading">Calculated from logged values</div>
    <div class="result-values">
      Time constant τ: <strong>${a.tau} s</strong> <small>(range ${a.fit.tauLo}-${a.fit.tauHi} s)</small><br>
      Effective power: <strong>${formatNumber(a.powerEff, 0, ' W')}</strong>
        <small>${a.rated > 0 ? `(${(a.powerEff / a.rated * 100).toFixed(0)} % of rated ${a.rated.toFixed(0)} W)` : ''}</small><br>
      Store gain: <strong>${formatNumber(a.storeGain, 2)}</strong><br>
      Heat loss: <strong>${formatNumber(a.lossWPerC, 2, ' W/°C')}</strong>
        <small>(${formatNumber(a.lossAtTarget, 0, ' W')} at ${a.target.toFixed(0)} °C in a ${a.ambient.toFixed(0)} °C room)</small>
    </div>
    <div class="result-grid">
      <div><span>Heated for</span><strong>${calibrationFormatTime(a.off)}</strong></div>
      <div><span>Measured rise after off</span><strong>${formatNumber(a.measuredRise, 2, ' °C')}</strong></div>
      <div><span>Model rise after off</span><strong>${formatNumber(a.fullHeat.rise, 2, ' °C')}</strong></div>
      <div><span>Time to peak</span><strong>${calibrationFormatTime(a.peakCoast)}</strong></div>
      <div><span>Fit RMS</span><strong>${a.fit.rms.toFixed(3)} °C</strong></div>
    </div>
    <table class="calibration-table">
      <thead><tr><th>Heater pulse</th><th>Total gain</th><th>Rise after off</th></tr></thead>
      <tbody>${pulseRows}</tbody>
    </table>
    <small>Predicted at ${a.target.toFixed(0)} °C with ${(a.capacity / WATER_J_PER_L_C).toFixed(1)} L.
      "Rise after off" is how far the temperature keeps climbing after the heater switches off.</small>
    ${notes ? `<ul class="calibration-notes">${notes}</ul>` : ''}
    ${a.usable ? '<button class="btn green result-apply" id="applyCalibration">Use these values</button>' : ''}`;

  const applyButton = document.getElementById('applyCalibration');
  if (applyButton) applyButton.addEventListener('click', () => applyCalibrationValues(a));
}

function renderCalibrationStatus(status) {
  lastCalibrationStatus = status;
  const samples = calibrationSamples(status);
  const phase = Number(status.phase);

  if (samples.length !== lastAnalysedCount) {
    lastAnalysedCount = samples.length;
    lastAnalysis = analyseCalibration(status);
  }

  document.getElementById('calibrationTemp').textContent = formatNumber(Number(status.currentTemp), 1, ' °C');
  document.getElementById('calibrationElapsed').textContent = calibrationFormatTime(status.elapsedSec);
  const heater = document.getElementById('calibrationHeater');
  heater.textContent = status.heaterOn ? 'ON' : 'OFF';
  heater.classList.toggle('on', !!status.heaterOn);
  heater.classList.toggle('off', !status.heaterOn);

  document.getElementById('calibrationPhase').textContent = phase === 1
    ? `Heating to ${Number(status.targetC).toFixed(1)} °C`
    : PHASE_TEXT[phase] || 'Ready';
  document.getElementById('calibrationStart').classList.toggle('hidden', !!status.active);
  document.getElementById('calibrationStop').classList.toggle('hidden', !status.active);

  renderLiveMetrics(status, samples, lastAnalysis);
  renderCalibrationChart(samples, status, lastAnalysis);
  renderCalibrationTable(samples, status);

  if (status.completed) {
    renderCalibrationResult(lastAnalysis);
  }
}

function renderMixerBadge(status) {
  const mixer = document.getElementById('calibrationMixer');
  if (!mixer) return;
  mixer.textContent = status.mixerOn ? 'ON' : 'OFF';
  mixer.classList.toggle('on', !!status.mixerOn);
  mixer.classList.toggle('off', !status.mixerOn);
}

async function applyCalibrationValues(analysis) {
  const result = document.getElementById('calibrationResult');
  try {
    const response = await fetch('saveSettings', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        heaterTauSec: analysis.tau,
        heaterPowerEffW: Math.round(analysis.powerEff),
        heaterStoreGain: Number(analysis.storeGain.toFixed(3)),
        heaterLossWPerC: Number(analysis.lossWPerC.toFixed(3)),
        heaterAmbientC: analysis.ambient
      })
    });
    result.insertAdjacentHTML('beforeend', response.ok
      ? '<div class="calibration-saved">Saved to Settings.</div>'
      : '<div class="calibration-error">Could not save values.</div>');
  } catch (error) {
    result.insertAdjacentHTML('beforeend', '<div class="calibration-error">Could not save values.</div>');
  }
}

// The device sends the log in chunks, starting at ?since=. Keep a local copy
// and only ask for samples not seen yet; a new runId starts a fresh copy.
async function fetchCalibrationStatus() {
  for (let request = 0; request < 20; request++) {
    const response = await fetch(`calibrationStatus?since=${calibrationLog.t.length}`);
    const status = await response.json();
    if (status.runId !== calibrationLog.runId || status.samplesFrom !== calibrationLog.t.length) {
      const fresh = status.runId !== calibrationLog.runId || status.samplesFrom !== 0;
      calibrationLog = { runId: status.runId, t: [], T: [] };
      if (fresh && status.samplesFrom !== 0) continue;
    }
    calibrationLog.t.push(...status.t);
    calibrationLog.T.push(...status.T);
    if (calibrationLog.t.length >= status.sampleCount) {
      return { ...status, t: calibrationLog.t, T: calibrationLog.T };
    }
  }
  throw new Error('Calibration log did not load');
}

async function pollCalibration() {
  try {
    const [status, mixerResponse] = await Promise.all([
      fetchCalibrationStatus(),
      fetch('status')
    ]);
    renderCalibrationStatus(status);
    if (mixerResponse.ok) {
      renderMixerBadge(await mixerResponse.json());
    }
    if (lastCalibrationStatus && !lastCalibrationStatus.active && calibrationPoll) {
      clearInterval(calibrationPoll);
      calibrationPoll = null;
    }
  } catch (error) {
    document.getElementById('calibrationPhase').textContent = 'Connection error';
  }
}

function readCalibrationInput(id, min, max, message) {
  const input = document.getElementById(id);
  const value = Number(input.value);
  if (!(value >= min && value <= max)) {
    document.getElementById('calibrationPhase').textContent = message;
    input.focus();
    return NaN;
  }
  return value;
}

document.getElementById('calibrationStart').addEventListener('click', async () => {
  const liters = readCalibrationInput('calibrationWaterLiters', 0.1, 100, 'Enter 0.1-100 litres');
  const target = readCalibrationInput('calibrationTargetC', 40, 80, 'Enter a target of 40-80 °C');
  const ambient = readCalibrationInput('calibrationAmbientC', -10, 45, 'Enter a room temperature of -10-45 °C');
  if (![liters, target, ambient].every(Number.isFinite)) return;

  const query = new URLSearchParams({ liters, target, ambient });
  const response = await fetch(`calibrationStart?${query}`, { method: 'POST' });
  if (!response.ok) {
    document.getElementById('calibrationPhase').textContent = await response.text();
    return;
  }
  document.getElementById('calibrationResult').classList.add('hidden');
  lastAnalysedCount = -1;
  lastAnalysis = null;
  await pollCalibration();
  if (!calibrationPoll) calibrationPoll = setInterval(pollCalibration, 2000);
});

document.getElementById('calibrationStop').addEventListener('click', async () => {
  await fetch('calibrationStop', { method: 'POST' });
  await pollCalibration();
});

renderCalibrationTable([], {});
pollCalibration().then(() => {
  if (lastCalibrationStatus?.active && !calibrationPoll) {
    calibrationPoll = setInterval(pollCalibration, 2000);
  }
});
