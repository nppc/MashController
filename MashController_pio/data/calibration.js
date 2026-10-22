let calibrationPoll = null;
let lastCalibrationStatus = null;
let lastAnalysedCount = -1;
let lastAnalysis = null;
// The Room temperature field starts blank; fill it once from the device's
// current ambientC the first time we hear from it, then leave it alone.
let ambientDefaultSet = false;
let calibrationLog = { runId: null, t: [], T: [] };

const WATER_J_PER_L_C = 4186;
// Heating slope is measured over the last minutes before switch-off, where the
// stored energy has levelled off and the water rises in a straight line.
const HEATING_WINDOW_SEC = 480;
const TAU_MIN_SEC = 5;
const TAU_MAX_SEC = 1200;

const PHASE_TEXT = {
  0: 'Ready',
  1: 'Step 1: Heating to 50 °C',
  2: 'Step 1: Coasting (detecting peak at 50 °C)',
  3: 'Step 2: Heating to 70 °C',
  4: 'Step 2: Coasting (detecting peak at 70 °C)',
  5: 'Complete',
  6: 'Heating timeout',
  7: 'Stopped: temperature sensor failure',
  8: 'Stopped',
  9: 'Stopped: log full'
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
function fitCoast(points) {
  if (points.length < 8) return null;
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

  // Approximate 95 % range of tau: where error is not significantly worse.
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

// Rise after switch-off for a given stored energy; matches HeaterModel::predictPeak.
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
  const off1 = Number(status.heatEnd1Sec);
  if (!Number.isFinite(off1)) return null;

  const capacity = Number(status.waterLiters) * WATER_J_PER_L_C;
  const ambient = Number(status.ambientC);
  const rated = Number(status.heaterPowerW);
  const t1 = Number(status.target1C || 50.0);
  const t2 = Number(status.target2C || 70.0);
  const step2Start = Number(status.step2StartSec);
  const off2 = Number(status.heatEnd2Sec);

  const warnings = [];
  const errors = [];

  // Coast 1 slice: samples between off1 and step2Start (or current end if step 2 not started)
  const coast1 = samples
    .filter(p => p.t >= off1 && (!Number.isFinite(step2Start) || p.t < step2Start))
    .map(p => ({ x: p.t - off1, T: p.T }));

  const fit1 = fitCoast(coast1);
  if (!fit1) return null;

  const tau1 = fit1.tau;
  const lossW1 = fit1.r * capacity;
  const coast1Mean = average(coast1.map(p => p.T));

  // Effective power from Step 1 heating
  const heat1Window = samples.filter(p => p.t < off1 && p.t >= off1 - HEATING_WINDOW_SEC);
  const reg1 = heat1Window.length >= 8 ? linearRegression(heat1Window) : null;
  let powerEff1 = NaN;
  if (reg1) {
    const levelled1 = 1 - Math.exp(-reg1.meanT / tau1);
    powerEff1 = (capacity * reg1.slope + Math.max(0, lossW1)) / Math.max(0.1, levelled1);
    if (levelled1 < 0.9) warnings.push('Step 1 heating was short compared with time constant; power estimate has higher uncertainty.');
  } else {
    warnings.push('Not enough heating data before Step 1 switch-off.');
  }

  // Overshoot ratio 1
  const powerFromCoast1 = fit1.B * capacity / (tau1 * (1 - Math.exp(-off1 / tau1)));
  const storeGain1 = (powerEff1 > 0) ? powerFromCoast1 / powerEff1 : 1.0;

  const step1 = {
    target: t1,
    off: off1,
    fit: fit1,
    tau: tau1,
    lossW: lossW1,
    coastMean: coast1Mean,
    storeGain: storeGain1,
    peakTemp: Number(status.peakTemp1),
    peakCoastSec: Number(status.peakCoast1Sec),
    measuredRise: Number(status.peakTemp1) - fit1.A
  };

  // Check Step 2
  let fit2 = null;
  let step2 = null;
  let powerEff = powerEff1;

  if (Number.isFinite(off2)) {
    const coast2 = samples
      .filter(p => p.t >= off2)
      .map(p => ({ x: p.t - off2, T: p.T }));

    fit2 = fitCoast(coast2);
    if (fit2) {
      const tau2 = fit2.tau;
      const lossW2 = fit2.r * capacity;
      const coast2Mean = average(coast2.map(p => p.T));
      const step2HeatDuration = off2 - step2Start;

      // Heating rate in Step 2
      const heat2Window = samples.filter(p => p.t >= step2Start && p.t < off2);
      const reg2 = heat2Window.length >= 8 ? linearRegression(heat2Window) : null;
      let powerEff2 = NaN;
      if (reg2) {
        powerEff2 = capacity * reg2.slope + Math.max(0, (lossW1 + lossW2) / 2);
      }
      if (Number.isFinite(powerEff2) && powerEff2 > 0 && Number.isFinite(powerEff1) && powerEff1 > 0) {
        powerEff = (powerEff1 + powerEff2) / 2;
      } else if (Number.isFinite(powerEff2) && powerEff2 > 0) {
        powerEff = powerEff2;
      }

      const charge2 = 1 - Math.exp(-step2HeatDuration / tau2);
      const powerFromCoast2 = fit2.B * capacity / (tau2 * Math.max(0.1, charge2));
      const storeGain2 = (powerEff > 0) ? powerFromCoast2 / powerEff : 1.0;

      step2 = {
        target: t2,
        off: off2,
        duration: step2HeatDuration,
        fit: fit2,
        tau: tau2,
        lossW: lossW2,
        coastMean: coast2Mean,
        storeGain: storeGain2,
        peakTemp: Number(status.peakTemp2),
        peakCoastSec: Number(status.peakCoast2Sec),
        measuredRise: Number(status.peakTemp2) - fit2.A
      };
    }
  }

  // Calculate power-law heat loss exponent alpha if both steps are available
  let alpha = 1.35;
  if (step1 && step2 && step1.lossW > 1.0 && step2.lossW > 1.0) {
    const dt1 = Math.max(step1.target - ambient, 5.0);
    const dt2 = Math.max(step2.target - ambient, dt1 + 5.0);
    if (step2.lossW > step1.lossW) {
      alpha = Math.log(step2.lossW / step1.lossW) / Math.log(dt2 / dt1);
      alpha = Math.max(1.0, Math.min(2.0, alpha));
    }
  }

  // Model validation checks
  if (tau1 <= TAU_MIN_SEC || tau1 >= TAU_MAX_SEC) {
    errors.push('Anchor 1 time constant is outside measurable range.');
  }
  if (step2 && (step2.tau <= TAU_MIN_SEC || step2.tau >= TAU_MAX_SEC)) {
    errors.push('Anchor 2 time constant is outside measurable range.');
  }
  if (!(fit1.r > 0)) {
    errors.push('Water did not cool after the first peak. Check sensor position and mixer.');
  }
  if (fit2 && !(fit2.r > 0)) {
    errors.push('Water did not cool after the second peak. Check sensor position and mixer.');
  }
  if (rated > 0 && Number.isFinite(powerEff)) {
    if (powerEff > rated * 1.05) warnings.push('Effective power is higher than rated power. Verify water volume and Heater Power setting.');
    if (powerEff < rated * 0.6) warnings.push('Effective power is below 60% of rated power. Check heating element, line voltage, and kettle lid.');
  }

  // Predictions across standard brewing rests
  const rests = [
    { name: 'Acid / Glucanase', temp: 45 },
    { name: 'Low Rest (Anchor 1)', temp: 50 },
    { name: 'Protein Rest', temp: 55 },
    { name: 'Beta-Amylase', temp: 65 },
    { name: 'High Rest (Anchor 2)', temp: 70 },
    { name: 'Alpha-Amylase', temp: 72 },
    { name: 'Mash-Out', temp: 78 }
  ].map(r => {
    const T = r.temp;
    let tau = tau1;
    let gain = storeGain1;
    let loss = lossW1;

    if (step2) {
      const frac = (T - t1) / (t2 - t1);
      tau = Math.round(tau1 + (step2.tau - tau1) * frac);
      gain = storeGain1 + (step2.storeGain - storeGain1) * frac;
      const dt = Math.max(T - ambient, 1.0);
      const dt1 = Math.max(t1 - ambient, 1.0);
      loss = step1.lossW * Math.pow(dt / dt1, alpha);
    } else {
      const dt = Math.max(T - ambient, 1.0);
      const dt1 = Math.max(t1 - ambient, 1.0);
      loss = step1.lossW * (dt / dt1);
    }

    const fullHeat = predictedRise(gain * powerEff * tau, capacity, loss, tau);
    const pulse30 = predictedRise(gain * powerEff * tau * (1 - Math.exp(-30 / tau)), capacity, loss, tau);

    return {
      name: r.name,
      temp: T,
      tau,
      lossW: loss,
      gain,
      fullRise: fullHeat.rise,
      tPeak: fullHeat.tPeak,
      pulse30Rise: pulse30.rise
    };
  });

  return {
    capacity,
    ambient,
    rated,
    powerEff,
    step1,
    step2,
    hasDual: !!step2,
    alpha,
    rests,
    warnings,
    errors,
    usable: errors.length === 0 && powerEff > 0 && (!step2 || step2.storeGain > 0)
  };
}

function calibrationChartSvg(samples, status, analysis, tMin, tMax, label) {
  const W = 600, H = 220, L = 44, R = 10, T = 10, B = 26;
  const t1 = Number(status.target1C || 50.0);
  const t2 = Number(status.target2C || 70.0);
  const off1 = Number(status.heatEnd1Sec);
  const off2 = Number(status.heatEnd2Sec);
  const shown = samples.filter(p => p.t >= tMin && p.t <= tMax);
  if (!shown.length) return '';
  let yMin = Math.min(...shown.map(p => p.T));
  let yMax = Math.max(...shown.map(p => p.T));
  if (Number.isFinite(t1) && t1 >= yMin - 5 && t1 <= yMax + 5) {
    yMin = Math.min(yMin, t1);
    yMax = Math.max(yMax, t1);
  }
  if (Number.isFinite(t2) && t2 >= yMin - 5 && t2 <= yMax + 5) {
    yMin = Math.min(yMin, t2);
    yMax = Math.max(yMax, t2);
  }
  const span = Math.max(yMax - yMin, 0.5);
  yMin -= span * 0.05;
  yMax += span * 0.05;
  const x = t => L + (W - L - R) * (t - tMin) / Math.max(tMax - tMin, 1);
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

  if (Number.isFinite(t1) && t1 >= yMin && t1 <= yMax) {
    svg += `<line class='target' x1='${L}' y1='${y(t1)}' x2='${W - R}' y2='${y(t1)}'/>`;
    svg += `<text x='${W - R - 4}' y='${y(t1) - 4}' text-anchor='end'>50°C</text>`;
  }
  if (Number.isFinite(t2) && t2 >= yMin && t2 <= yMax) {
    svg += `<line class='target' x1='${L}' y1='${y(t2)}' x2='${W - R}' y2='${y(t2)}'/>`;
    svg += `<text x='${W - R - 4}' y='${y(t2) - 4}' text-anchor='end'>70°C</text>`;
  }

  if (Number.isFinite(off1) && off1 >= tMin && off1 <= tMax) {
    svg += `<line class='off' x1='${x(off1)}' y1='${T}' x2='${x(off1)}' y2='${H - B}'/>`;
    svg += `<text x='${x(off1) + 4}' y='${T + 12}'>off 1 (50°C)</text>`;
  }
  if (Number.isFinite(off2) && off2 >= tMin && off2 <= tMax) {
    svg += `<line class='off' x1='${x(off2)}' y1='${T}' x2='${x(off2)}' y2='${H - B}'/>`;
    svg += `<text x='${x(off2) + 4}' y='${T + 12}'>off 2 (70°C)</text>`;
  }

  svg += `<path class='data' d='${path(shown)}'/>`;

  if (analysis) {
    if (analysis.step1?.fit && Number.isFinite(off1)) {
      const f1 = analysis.step1.fit;
      const fitted1 = [];
      for (let i = 0; i <= 60; i++) {
        const xs = f1.coastSec * i / 60;
        const pt = { t: off1 + xs, T: f1.A + f1.B * (1 - Math.exp(-xs / f1.tau)) - f1.r * xs };
        if (pt.t >= tMin && pt.t <= tMax) fitted1.push(pt);
      }
      if (fitted1.length > 1) svg += `<path class='fit' d='${path(fitted1)}'/>`;
    }
    if (analysis.step2?.fit && Number.isFinite(off2)) {
      const f2 = analysis.step2.fit;
      const fitted2 = [];
      for (let i = 0; i <= 60; i++) {
        const xs = f2.coastSec * i / 60;
        const pt = { t: off2 + xs, T: f2.A + f2.B * (1 - Math.exp(-xs / f2.tau)) - f2.r * xs };
        if (pt.t >= tMin && pt.t <= tMax) fitted2.push(pt);
      }
      if (fitted2.length > 1) svg += `<path class='fit' d='${path(fitted2)}'/>`;
    }
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
  let html = calibrationChartSvg(samples, status, analysis, 0, tEnd, 'Complete Run');

  const off1 = Number(status.heatEnd1Sec);
  const step2Start = Number(status.step2StartSec);
  if (Number.isFinite(off1)) {
    const endCoast1 = Number.isFinite(step2Start) ? step2Start : tEnd;
    if (endCoast1 - off1 >= 30) {
      html += calibrationChartSvg(samples, status, analysis, Math.max(0, off1 - 180), endCoast1 + 20, 'Step 1: 50 °C Peak & Coast');
    }
  }

  const off2 = Number(status.heatEnd2Sec);
  if (Number.isFinite(off2) && tEnd - off2 >= 30) {
    html += calibrationChartSvg(samples, status, analysis, Math.max(0, off2 - 180), tEnd, 'Step 2: 70 °C Peak & Coast');
  }

  chart.innerHTML = html;
}

function renderCalibrationTable(samples, status) {
  const off1 = Number(status.heatEnd1Sec);
  const step2Start = Number(status.step2StartSec);
  const off2 = Number(status.heatEnd2Sec);

  document.getElementById('calibrationSampleCount').textContent = samples.length;
  document.getElementById('calibrationSamples').innerHTML = samples.length
    ? samples.map(p => {
        let phase = 'Heat to 50°C';
        if (Number.isFinite(off2) && p.t >= off2) phase = 'Coast at 70°C';
        else if (Number.isFinite(step2Start) && p.t >= step2Start) phase = 'Heat to 70°C';
        else if (Number.isFinite(off1) && p.t >= off1) phase = 'Coast at 50°C';
        return `<tr><td>${calibrationFormatTime(p.t)}</td><td>${phase}</td><td>${p.T.toFixed(2)} °C</td></tr>`;
      }).join('')
    : '<tr><td colspan="3">No readings yet</td></tr>';
}

function renderLiveMetrics(status, samples, analysis) {
  const phase = Number(status.phase);
  const current = Number(status.currentTemp);
  const now = samples.length ? samples[samples.length - 1].t : 0;
  const off1 = Number(status.heatEnd1Sec);
  const step2Start = Number(status.step2StartSec);
  const off2 = Number(status.heatEnd2Sec);

  let items = [];

  if (phase === 1) { // Heating to 50 °C
    const recent = samples.filter(p => p.t >= now - 300);
    const reg = recent.length >= 5 ? linearRegression(recent) : null;
    const rate = reg ? reg.slope * 60 : NaN;
    const remaining = rate > 0 ? (50.0 - current) / rate * 60 : NaN;
    items = [
      ['Target', '50.0 °C (Step 1)'],
      ['Heating Rate', formatNumber(rate, 2, ' °C/min')],
      ['Time to 50 °C', calibrationFormatTime(remaining)],
      ['Water Power', formatNumber(reg ? reg.slope * status.waterLiters * WATER_J_PER_L_C : NaN, 0, ' W')]
    ];
  } else if (phase === 2) { // Coasting at 50 °C
    const peak = Number(status.peakTemp1);
    const peakKnown = Number.isFinite(peak);
    const coastNow = now - off1;
    const peakCoast = Number(status.peakCoast1Sec);
    items = [
      ['50 °C Peak so far', peakKnown ? formatNumber(peak, 2, ' °C') : 'Waiting for peak'],
      ['Rise since off', peakKnown ? formatNumber(peak - 50.0, 2, ' °C') : '--'],
      ['Since peak', (peakKnown && Number.isFinite(peakCoast)) ? calibrationFormatTime(coastNow - peakCoast) : '--'],
      ['Preliminary τ₁', analysis?.step1 ? `${analysis.step1.tau} s` : '--']
    ];
  } else if (phase === 3) { // Heating to 70 °C
    const recent = samples.filter(p => p.t >= now - 300 && p.t >= step2Start);
    const reg = recent.length >= 5 ? linearRegression(recent) : null;
    const rate = reg ? reg.slope * 60 : NaN;
    const remaining = rate > 0 ? (70.0 - current) / rate * 60 : NaN;
    items = [
      ['Target', '70.0 °C (Step 2)'],
      ['Heating Rate', formatNumber(rate, 2, ' °C/min')],
      ['Time to 70 °C', calibrationFormatTime(remaining)],
      ['Water Power', formatNumber(reg ? reg.slope * status.waterLiters * WATER_J_PER_L_C : NaN, 0, ' W')]
    ];
  } else if (phase === 4) { // Coasting at 70 °C
    const peak = Number(status.peakTemp2);
    const peakKnown = Number.isFinite(peak);
    const coastNow = now - off2;
    const peakCoast = Number(status.peakCoast2Sec);
    items = [
      ['70 °C Peak so far', peakKnown ? formatNumber(peak, 2, ' °C') : 'Waiting for peak'],
      ['Rise since off', peakKnown ? formatNumber(peak - 70.0, 2, ' °C') : '--'],
      ['Since peak', (peakKnown && Number.isFinite(peakCoast)) ? calibrationFormatTime(coastNow - peakCoast) : '--'],
      ['Preliminary τ₂', analysis?.step2 ? `${analysis.step2.tau} s` : '--']
    ];
  } else if (phase === 5) { // Complete
    items = [
      ['Status', 'Calibration Complete'],
      ['Effective Power', formatNumber(analysis?.powerEff, 0, ' W')],
      ['τ (50 °C / 70 °C)', analysis ? `${analysis.step1?.tau ?? '--'}s / ${analysis.step2?.tau ?? '--'}s` : '--'],
      ['Loss (50 °C / 70 °C)', analysis ? `${formatNumber(analysis.step1?.lossW, 0)}W / ${formatNumber(analysis.step2?.lossW, 0)}W` : '--']
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
  const s1 = a.step1;
  const s2 = a.step2;

  const notes = [
    ...a.errors.map(text => `<li class="calibration-error">${text}</li>`),
    ...a.warnings.map(text => `<li>${text}</li>`)
  ].join('');

  let tableRows = '';
  if (a.rests) {
    tableRows = a.rests.map(r => `
      <tr>
        <td><strong>${r.name}</strong> (${r.temp}°C)</td>
        <td>${r.tau} s</td>
        <td>${r.lossW.toFixed(0)} W</td>
        <td>+${r.fullRise.toFixed(2)} °C</td>
        <td>+${r.pulse30Rise.toFixed(2)} °C</td>
      </tr>
    `).join('');
  }

  result.innerHTML = `
    <div class="result-heading">Dual-Anchor Calibration Results</div>

    <div class="result-values">
      Effective Heating Power: <strong>${formatNumber(a.powerEff, 0, ' W')}</strong>
      <small>${a.rated > 0 ? `(${(a.powerEff / a.rated * 100).toFixed(0)} % of rated ${a.rated.toFixed(0)} W)` : ''}</small><br>
      Heat Loss Non-Linearity (α): <strong>${a.alpha.toFixed(2)}</strong>
      <small>(loss scales as ΔT<sup>${a.alpha.toFixed(2)}</sup>)</small>
    </div>

    <div style="margin: 12px 0; overflow-x: auto;">
      <table class="calibration-table">
        <thead>
          <tr>
            <th>Parameter</th>
            <th>Anchor 1 (50 °C)</th>
            <th>Anchor 2 (70 °C)</th>
            <th>Dynamic Shift</th>
          </tr>
        </thead>
        <tbody>
          <tr>
            <td>Time Constant (τ)</td>
            <td><strong>${s1 ? s1.tau : '--'} s</strong></td>
            <td><strong>${s2 ? s2.tau : '--'} s</strong></td>
            <td>${s1 && s2 ? `${(s2.tau - s1.tau > 0 ? '+' : '')}${s2.tau - s1.tau} s (${((s2.tau - s1.tau)/s1.tau*100).toFixed(0)}%)` : '--'}</td>
          </tr>
          <tr>
            <td>Heat Loss Rate</td>
            <td><strong>${s1 ? formatNumber(s1.lossW, 1, ' W') : '--'}</strong></td>
            <td><strong>${s2 ? formatNumber(s2.lossW, 1, ' W') : '--'}</strong></td>
            <td>${s1 && s2 ? `+${(s2.lossW - s1.lossW).toFixed(1)} W` : '--'}</td>
          </tr>
          <tr>
            <td>Store Gain Factor</td>
            <td><strong>${s1 ? s1.storeGain.toFixed(2) : '--'}</strong></td>
            <td><strong>${s2 ? s2.storeGain.toFixed(2) : '--'}</strong></td>
            <td>${s1 && s2 ? `${(s2.storeGain - s1.storeGain > 0 ? '+' : '')}${(s2.storeGain - s1.storeGain).toFixed(2)}` : '--'}</td>
          </tr>
          <tr>
            <td>Measured Overshoot</td>
            <td><strong>${s1 ? formatNumber(s1.measuredRise, 2, ' °C') : '--'}</strong></td>
            <td><strong>${s2 ? formatNumber(s2.measuredRise, 2, ' °C') : '--'}</strong></td>
            <td>Peak after heater off</td>
          </tr>
        </tbody>
      </table>
    </div>

    <div class="result-heading" style="margin-top:16px;">Predicted Behaviour Across Mash Schedule</div>
    <div style="overflow-x: auto;">
      <table class="calibration-table">
        <thead>
          <tr>
            <th>Rest</th>
            <th>Dynamic τ</th>
            <th>Heat Loss</th>
            <th>Full Ramp Overshoot</th>
            <th>30s Pulse Rise</th>
          </tr>
        </thead>
        <tbody>
          ${tableRows}
        </tbody>
      </table>
    </div>
    <small style="display:block; margin-top:6px; color:var(--text-dim);">
      Predicted with ${(a.capacity / WATER_J_PER_L_C).toFixed(1)} L water at ${a.ambient.toFixed(1)} °C room temperature.
    </small>

    ${notes ? `<ul class="calibration-notes">${notes}</ul>` : ''}
    ${a.usable ? '<button class="btn green result-apply" id="applyCalibration">Use these values</button>' : ''}
  `;

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

  const litersInput = document.getElementById('calibrationWaterLiters');
  const ambientInput = document.getElementById('calibrationAmbientC');
  [litersInput, ambientInput].forEach(input => {
    if (input) input.disabled = !!status.active;
  });

  if (status.active) {
    const deviceLiters = Number(status.waterLiters);
    const deviceAmbient = Number(status.ambientC);
    if (litersInput && Number.isFinite(deviceLiters)) litersInput.value = deviceLiters;
    if (ambientInput && Number.isFinite(deviceAmbient)) ambientInput.value = deviceAmbient.toFixed(1);
  } else if (!ambientDefaultSet) {
    ambientDefaultSet = true;
    const deviceAmbient = Number(status.ambientC);
    if (ambientInput && !ambientInput.value && Number.isFinite(deviceAmbient)) {
      ambientInput.value = deviceAmbient.toFixed(1);
    }
  }

  document.getElementById('calibrationTemp').textContent = formatNumber(Number(status.currentTemp), 1, ' °C');
  document.getElementById('calibrationElapsed').textContent = calibrationFormatTime(status.elapsedSec);
  const heater = document.getElementById('calibrationHeater');
  heater.textContent = status.heaterOn ? 'ON' : 'OFF';
  heater.classList.toggle('on', !!status.heaterOn);
  heater.classList.toggle('off', !status.heaterOn);

  document.getElementById('calibrationPhase').textContent = PHASE_TEXT[phase] || 'Ready';
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

function renderMashLock(status) {
  const startBtn = document.getElementById('calibrationStart');
  if (!startBtn) return;
  const mashBusy = !!(status.running || status.coolDownActive);
  startBtn.disabled = mashBusy;
  startBtn.title = mashBusy ? 'A mash is running. Stop it before calibrating.' : '';
}

async function applyCalibrationValues(analysis) {
  const result = document.getElementById('calibrationResult');
  const a = analysis;
  const s1 = a.step1;
  const s2 = a.step2 || a.step1;

  try {
    const response = await fetch('saveSettings', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        heaterT1: 50.0,
        heaterTau1: Math.round(s1.tau),
        heaterLossW1: Number(s1.lossW.toFixed(1)),
        heaterStoreGain1: Number(s1.storeGain.toFixed(3)),

        heaterT2: 70.0,
        heaterTau2: Math.round(s2.tau),
        heaterLossW2: Number(s2.lossW.toFixed(1)),
        heaterStoreGain2: Number(s2.storeGain.toFixed(3)),

        heaterPowerEffW: Math.round(a.powerEff),
        heaterAmbientC: a.ambient,

        // Legacy fallbacks
        heaterTauSec: Math.round(s1.tau),
        heaterStoreGain: Number(s1.storeGain.toFixed(3)),
        heaterLossWPerC: Number((s1.lossW / Math.max(1, 50.0 - a.ambient)).toFixed(2))
      })
    });
    result.insertAdjacentHTML('beforeend', response.ok
      ? '<div class="calibration-saved">✓ Saved dual-anchor calibration to settings!</div>'
      : '<div class="calibration-error">Could not save values.</div>');
  } catch (error) {
    result.insertAdjacentHTML('beforeend', '<div class="calibration-error">Could not save values.</div>');
  }
}

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

async function pollMashStatus() {
  try {
    const response = await fetch('status');
    if (!response.ok) return;
    const status = await response.json();
    renderMixerBadge(status);
    renderMashLock(status);
  } catch (error) {
  }
}

async function pollCalibration() {
  try {
    const status = await fetchCalibrationStatus();
    renderCalibrationStatus(status);
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
  const ambient = readCalibrationInput('calibrationAmbientC', -10, 45, 'Enter a room temperature of -10-45 °C');
  if (![liters, ambient].every(Number.isFinite)) return;

  const query = new URLSearchParams({ liters, ambient });
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
pollMashStatus();
setInterval(pollMashStatus, 3000);
pollCalibration().then(() => {
  if (lastCalibrationStatus?.active && !calibrationPoll) {
    calibrationPoll = setInterval(pollCalibration, 2000);
  }
});
