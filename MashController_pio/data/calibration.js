let calibrationPoll = null;
let lastCalibrationStatus = null;
let lastLiveFitSampleCount = -1;
let lastLiveFit = null;

function calibrationFormatTime(seconds) {
  seconds = Number(seconds);
  if (!Number.isFinite(seconds) || seconds < 0) return '--:--';
  const minutes = Math.floor(seconds / 60).toString().padStart(2, '0');
  const remainder = Math.floor(seconds % 60).toString().padStart(2, '0');
  return `${minutes}:${remainder}`;
}

function renderCalibrationTable(samples) {
  document.getElementById('calibrationSamples').innerHTML = samples.length
    ? samples.map(sample => {
        const sampleTime = Number(sample.timeSec);
        const heatEnd = Number(lastCalibrationStatus?.heatEndSec);
        const phase = lastCalibrationStatus?.active && lastCalibrationStatus.phase === 1
          ? 'Heating'
          : Number.isFinite(heatEnd) && heatEnd > 0 && sampleTime >= heatEnd
            ? 'Cooling'
            : 'Heating';
        return `<tr><td>${calibrationFormatTime(sample.timeSec)}</td><td>${phase}</td><td>${Number(sample.temp).toFixed(1)} °C</td></tr>`;
      }).join('')
    : '<tr><td colspan="3">No readings yet</td></tr>';
}

function calibrationFit(samples) {
  if (samples.length < 8) return null;
  const power = Number(lastCalibrationStatus.heaterPowerW);
  if (!(power > 0)) return null;

  const initial = Number(samples[0].temp);
  const waterCapacity = Number(lastCalibrationStatus.waterLiters) * 4186;
  const heatEndSec = Number(lastCalibrationStatus.heatEndSec);
  const finalSec = Number(samples[samples.length - 1].timeSec);
  if (!(heatEndSec > 0) || !(finalSec > heatEndSec)) return null;
  let best = null;

  for (let k = 1; k <= 150; k += 2) {
    for (let elementCapacity = 250; elementCapacity <= 30000; elementCapacity += 250) {
      let waterTemp = initial;
      let elementTemp = initial;
      let sampleIndex = 0;
      let error = 0;

      for (let second = 0; second <= finalSec; second++) {
        if (sampleIndex < samples.length && second === samples[sampleIndex].timeSec) {
          const delta = waterTemp - Number(samples[sampleIndex].temp);
          error += delta * delta;
          sampleIndex++;
        }
        const heater = second < heatEndSec ? 1 : 0;
        const transfer = k * (elementTemp - waterTemp);
        elementTemp += (power * heater - transfer) / elementCapacity;
        waterTemp += transfer / waterCapacity;
      }

      if (!best || error < best.error) best = { k, elementCapacity, error };
    }
  }
  return best;
}

function calibrationMetrics(samples) {
  const byTime = new Map(samples.map(sample => [sample.timeSec, Number(sample.temp)]));
  const start = byTime.get(0);
  const heatEndSec = Number(lastCalibrationStatus.heatEndSec);
  const beforeHeatEnd = samples.filter(sample => sample.timeSec <= heatEndSec);
  const atHeatEnd = byTime.get(heatEndSec) ??
    (beforeHeatEnd.length ? beforeHeatEnd[beforeHeatEnd.length - 1].temp : NaN);
  const atEnd = Number(samples[samples.length - 1]?.temp);
  if (![start, atHeatEnd, atEnd].every(Number.isFinite)) return null;

  const waterCapacity = Number(lastCalibrationStatus.waterLiters) * 4186;
  const waterEnergy = waterCapacity * (atHeatEnd - start);
  return {
    start,
    atHeatEnd,
    atEnd,
    waterCapacity,
    waterEnergy,
    averageWaterPower: waterEnergy / heatEndSec,
    heatEndSec,
    peakTemp: Number(lastCalibrationStatus.peakTemp),
    cooldownDrop: Number(lastCalibrationStatus.peakTemp) - atEnd
  };
}

function renderLiveMetrics(status, samples) {
  const metricsEl = document.getElementById('calibrationLiveMetrics');
  const validSamples = samples.filter(sample => Number.isFinite(Number(sample.timeSec)) &&
    Number.isFinite(Number(sample.temp)));
  let recentRate = NaN;
  let averageRate = NaN;
  let waterPower = NaN;

  if (validSamples.length >= 2) {
    const previous = validSamples[validSamples.length - 2];
    const latest = validSamples[validSamples.length - 1];
    const seconds = Number(latest.timeSec) - Number(previous.timeSec);
    if (seconds > 0) recentRate = (Number(latest.temp) - Number(previous.temp)) * 60 / seconds;
  }

  const heatEndSec = Number(status.heatEndSec);
  const heatingSamples = Number.isFinite(heatEndSec) && heatEndSec > 0
    ? validSamples.filter(sample => Number(sample.timeSec) <= heatEndSec)
    : validSamples;

  if (heatingSamples.length >= 2) {
    const first = heatingSamples[0];
    const latest = heatingSamples[heatingSamples.length - 1];
    const seconds = Number(latest.timeSec) - Number(first.timeSec);
    if (seconds > 0) {
      averageRate = (Number(latest.temp) - Number(first.temp)) * 60 / seconds;
      const capacity = Number(status.waterLiters) * 4186;
      waterPower = capacity * (Number(latest.temp) - Number(first.temp)) / seconds;
    }
  }

  if (validSamples.length !== lastLiveFitSampleCount) {
    lastLiveFitSampleCount = validSamples.length;
    lastLiveFit = calibrationFit(validSamples);
  }

  const formatRate = value => Number.isFinite(value) ? `${value.toFixed(2)} °C/min` : '--';
  const formatPower = Number.isFinite(waterPower) ? `${waterPower.toFixed(1)} W` : '--';
  const fitText = lastLiveFit
    ? `k ${lastLiveFit.k.toFixed(1)} W/°C, C_e ${lastLiveFit.elementCapacity.toFixed(0)} J/°C`
    : '--';

  metricsEl.innerHTML = `
    <div class="live-metric"><span>Recent rate</span><strong>${formatRate(recentRate)}</strong></div>
    <div class="live-metric"><span>Average heating rate</span><strong>${formatRate(averageRate)}</strong></div>
    <div class="live-metric"><span>Water power</span><strong>${formatPower}</strong></div>
    <div class="live-metric"><span>Model fit</span><strong>${fitText}</strong></div>`;
}

function renderCalibrationStatus(status) {
  lastCalibrationStatus = status;
  const elapsed = Number(status.elapsedSec || 0);
  document.getElementById('calibrationTemp').textContent =
    `${Number(status.currentTemp).toFixed(1)} °C`;
  document.getElementById('calibrationElapsed').textContent = calibrationFormatTime(elapsed);
  const heater = document.getElementById('calibrationHeater');
  heater.textContent = status.heaterOn ? 'ON' : 'OFF';
  heater.classList.toggle('on', status.heaterOn);
  heater.classList.toggle('off', !status.heaterOn);
  renderLiveMetrics(status, status.samples || []);
  renderCalibrationTable(status.samples || []);

  if (status.samples?.length >= Number(status.maxSamples || 121)) {
    document.getElementById('calibrationPhase').textContent = 'Log full; test still running';
  }

  const completed = Boolean(status.completed);
  document.getElementById('calibrationPhase').textContent = status.active
    ? (status.phase === 1 ? 'Heating to 50 °C' : 'Cooling until 2 °C drop')
    : (completed ? 'Complete'
      : status.phase === 4 ? 'Safety timeout'
      : 'Ready');
  document.getElementById('calibrationStart').classList.toggle('hidden', status.active);
  document.getElementById('calibrationStop').classList.toggle('hidden', !status.active);

  if (completed) {
    const fit = calibrationFit(status.samples);
    const metrics = calibrationMetrics(status.samples);
    const result = document.getElementById('calibrationResult');
    result.classList.remove('hidden');
    result.innerHTML = fit && metrics
      ? `<div class="result-heading">Calculated from logged values</div>
         <div class="result-grid">
           <div><span>T0</span><strong>${metrics.start.toFixed(1)} °C</strong></div>
           <div><span>Heat end</span><strong>${metrics.atHeatEnd.toFixed(1)} °C</strong></div>
           <div><span>Final</span><strong>${metrics.atEnd.toFixed(1)} °C</strong></div>
           <div><span>Heat duration</span><strong>${calibrationFormatTime(metrics.heatEndSec)}</strong></div>
           <div><span>Cooldown drop</span><strong>${metrics.cooldownDrop.toFixed(1)} °C</strong></div>
           <div><span>Cw</span><strong>${metrics.waterCapacity.toFixed(0)} J/°C</strong></div>
           <div><span>Water energy</span><strong>${metrics.waterEnergy.toFixed(0)} J</strong></div>
           <div><span>Average water power</span><strong>${metrics.averageWaterPower.toFixed(1)} W</strong></div>
         </div>
         <div class="result-values">k: <strong>${fit.k.toFixed(1)} W/°C</strong><br>C_e: <strong>${fit.elementCapacity.toFixed(0)} J/°C</strong></div>
         <small>Curve-fit error: ${fit.error.toFixed(3)}</small>
         <button class="btn green result-apply" id="applyCalibration">Use these values</button>`
      : 'Not enough data to calculate values. Check the sensor and run the test again.';
    const applyButton = document.getElementById('applyCalibration');
    if (applyButton) {
      applyButton.addEventListener('click', () => applyCalibrationValues(fit));
    }
  }
}

async function applyCalibrationValues(fit) {
  const response = await fetch('saveSettings', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({
      heaterTransferCoeff: fit.k,
      heaterThermalMass: fit.elementCapacity
    })
  });
  const result = document.getElementById('calibrationResult');
  if (response.ok) {
    result.insertAdjacentHTML('beforeend', '<div class="calibration-saved">Saved to Settings.</div>');
  } else {
    result.insertAdjacentHTML('beforeend', '<div class="calibration-error">Could not save values.</div>');
  }
}

async function pollCalibration() {
  try {
    const response = await fetch('calibrationStatus');
    renderCalibrationStatus(await response.json());
    if (lastCalibrationStatus && !lastCalibrationStatus.active && calibrationPoll) {
      clearInterval(calibrationPoll);
      calibrationPoll = null;
    }
  } catch (error) {
    document.getElementById('calibrationPhase').textContent = 'Connection error';
  }
}

document.getElementById('calibrationStart').addEventListener('click', async () => {
  const litersInput = document.getElementById('calibrationWaterLiters');
  const liters = Number(litersInput.value);
  if (!(liters >= 0.1 && liters <= 100)) {
    document.getElementById('calibrationPhase').textContent = 'Enter 0.1-100 litres';
    litersInput.focus();
    return;
  }
  const response = await fetch(`calibrationStart?liters=${encodeURIComponent(liters)}`, { method: 'POST' });
  if (!response.ok) {
    document.getElementById('calibrationPhase').textContent = await response.text();
    return;
  }
  document.getElementById('calibrationResult').classList.add('hidden');
  lastLiveFitSampleCount = -1;
  lastLiveFit = null;
  await pollCalibration();
  calibrationPoll = setInterval(pollCalibration, 1000);
});

document.getElementById('calibrationStop').addEventListener('click', async () => {
  await fetch('calibrationStop', { method: 'POST' });
  await pollCalibration();
});

renderCalibrationTable([]);
pollCalibration();