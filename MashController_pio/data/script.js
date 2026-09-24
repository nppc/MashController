/* MENU */
const sideMenu = document.getElementById('sideMenu');
const menuBtn = document.getElementById('menuBtn');
const sideMenuBackdrop = document.getElementById('sideMenuBackdrop');

function closeSideMenu() {
  sideMenu.classList.remove('open');
  sideMenuBackdrop.classList.remove('open');
}

function openSideMenu() {
  sideMenu.classList.add('open');
  sideMenuBackdrop.classList.add('open');
}

menuBtn.onclick = (e) => {
  e.stopPropagation();
  sideMenu.classList.contains('open') ? closeSideMenu() : openSideMenu();
};

sideMenuBackdrop.onclick = closeSideMenu;

document.addEventListener('click', (e) => {
  if (
    sideMenu.classList.contains('open') &&
    !sideMenu.contains(e.target) &&
    e.target !== menuBtn
  ) {
    closeSideMenu();
  }
});

document.addEventListener('keydown', (e) => {
  if (e.key === 'Escape') {
    closeSideMenu();
    closeHelpModal();
  }
});

/* CONTROLLER STATUS AND PROCESS */
let controllerStatus = {
  running: false,
  profileName: '',
  step: 0,
  stepTemp: 0,
  currentTemp: 0,
  remaining: 0
};

async function updateStatus() {
  try {
    const res = await fetch('status');
    const st = await res.json();

    controllerStatus = st;

    updateProcessUI(st);

    document.getElementById('curTemp').textContent =
      Number(st.currentTemp).toFixed(1);

    document.getElementById('targetTemp').textContent =
      (st.running && !st.coolDownActive)
        ? Number(st.stepTemp).toFixed(1)
        : '--.-';
        
    if (st.running && st.profileName) {
      document.getElementById('activeProfileName').textContent =
        st.profileName;
    }

    updateMainScreenProfileDetails(st);

    renderMainSteps();

    // Hide the target line immediately when stopped or cooling down,
    // instead of waiting for the next update() poll
    if ((!st.running || st.coolDownActive) &&
        chart.data.datasets[1].data.length) {
      chart.data.datasets[1].data = [];
      chart.update('none');
    }
  } catch (e) {
    console.error('Status error', e);
  }
}

async function startSelectedProfile() {

  try {

    await fetch(
      `startProfile?profile=${currentProfileIndex}`,
      {
        method: 'POST'
      }
    );

    updateStatus();

  } catch (e) {
    console.error(e);
  }
}

async function stopProfile() {

  try {

    await fetch('stopProfile', {
      method: 'POST'
    });

    updateStatus();

  } catch (e) {
    console.error(e);
  }
}

function formatTime(sec) {
  sec = Math.max(0, Math.floor(sec || 0));
  const m = Math.floor(sec / 60).toString().padStart(2, '0');
  const s = (sec % 60).toString().padStart(2, '0');
  return `${m}:${s}`;
}

function updateProcessUI(st) {
  const badge = document.getElementById('stateBadge');
  const isPaused = st.paused ?? false;             
  const isRunning = st.running ?? false;           
  const inCoolDown = st.coolDownActive ?? false;   
  const waitingForUser = st.waitingForUser ?? false;
  const waitingForTemperature = st.waiting ?? false;
  const instruction = document.getElementById('pauseInstruction');
  const resumeButton = document.getElementById('btnResume');

  badge.classList.remove('running', 'paused', 'cooldown');
	if (inCoolDown) {
		badge.textContent = 'Cooling Down';
		badge.classList.add('cooldown');
	} else if (isRunning && isPaused) {
		badge.textContent = 'Paused';
		badge.classList.add('paused');
	} else if (isRunning) {
		badge.textContent = 'Running';
		badge.classList.add('running');
	} else {
		badge.textContent = 'Stopped';
	}

	const timerEl = document.getElementById('remainingTime');
	timerEl.textContent = formatTime(inCoolDown ? (st.coolDownRemaining ?? 0) : (st.remaining ?? 0));
	timerEl.classList.toggle('inactive', !isRunning && !inCoolDown);
  if (instruction) {
    const showPauseMessage = isRunning && (isPaused || waitingForUser);
    instruction.classList.toggle('hidden', !showPauseMessage);
    instruction.classList.toggle('manual-step', waitingForUser);
    instruction.classList.toggle('temperature-wait', isPaused && waitingForTemperature);
    instruction.classList.toggle('timer-paused', isPaused && !waitingForTemperature && !waitingForUser);

    if (waitingForUser) {
      instruction.textContent = 'Manual step. Press RESUME to continue.';
    } else if (isPaused && waitingForTemperature) {
      instruction.textContent = 'Heating paused. Press RESUME to continue.';
    } else if (isPaused) {
      instruction.textContent = 'Timer paused. Temperature control remains active.';
    }
  }
  resumeButton.textContent = 'RESUME';
  
	document.getElementById('btnStart').classList.toggle('hidden', isRunning || inCoolDown);
	document.getElementById('btnPause').classList.toggle('hidden', !isRunning || isPaused || inCoolDown);
	document.getElementById('btnResume').classList.toggle('hidden', !isRunning || !isPaused || inCoolDown);
  document.getElementById('btnSkip').classList.toggle('hidden', !isRunning || inCoolDown || waitingForUser);
	document.getElementById('btnStop').classList.toggle('hidden', !isRunning && !inCoolDown);
  
    document.querySelector('.profile-picker').classList.toggle('disabled', isRunning || inCoolDown);

	const editor = document.getElementById('profilesEditor');
	const lockMsg = document.getElementById('profilesLocked');

	if (editor) {
		editor.classList.toggle('editor-disabled', isRunning || inCoolDown);
	}

	if (lockMsg) {
		lockMsg.classList.toggle('hidden', !(isRunning || inCoolDown));
	}

	const heater = document.getElementById('heaterBadge');

	if (heater) {
		heater.textContent = st.heaterOn ? 'ON' : 'OFF';
		heater.classList.toggle('on', st.heaterOn);
		heater.classList.toggle('off', !st.heaterOn);
	}

	renderMixerStatus(st);
}

/* MIXER: mode toggle + manual override */

// Server is the source of truth for mode; default to auto until first status arrives.
let mixerMode = 'auto';

function renderMixerStatus(st) {
	// Keep local mode in sync with whatever the firmware reports.
	if (st.mixerMode) mixerMode = st.mixerMode;

	const badge = document.getElementById('mixerBadge');
	const autoLabel = document.getElementById('mixerAutoLabel');
	const manLabel = document.getElementById('mixerManLabel');

	if (!badge) return;

	autoLabel.classList.toggle('active', mixerMode === 'auto');
	manLabel.classList.toggle('active', mixerMode === 'manual');
	badge.classList.toggle('manual', mixerMode === 'manual');

	badge.classList.toggle('on', !!st.mixerOn);
	badge.classList.toggle('off', !st.mixerOn);

	if (mixerMode === 'manual') {
		badge.textContent = st.mixerOn ? 'ON' : 'OFF';
	} else {
		badge.textContent = Number(st.mixerRemaining || 0);
	}
}

async function toggleMixerMode() {
	mixerMode = (mixerMode === 'auto') ? 'manual' : 'auto';

	// Optimistic UI update, then confirm with the server.
	document.getElementById('mixerAutoLabel').classList.toggle('active', mixerMode === 'auto');
	document.getElementById('mixerManLabel').classList.toggle('active', mixerMode === 'manual');
	document.getElementById('mixerBadge').classList.toggle('manual', mixerMode === 'manual');

	try {
		await fetch(`mixerMode?mode=${mixerMode}`, { method: 'POST' });
		updateStatus();
	} catch (e) {
		console.error(e);
	}
}

async function onMixerBadgeClick() {
	if (mixerMode !== 'manual') return; // read-only in auto mode

	try {
		await fetch('mixerToggle', { method: 'POST' });
		updateStatus();
	} catch (e) {
		console.error(e);
	}
}

async function pauseProfile() {
  try {
    await fetch('pauseProfile', { method: 'POST' });
    updateStatus();
  } catch (e) {
    console.error(e);
  }
}

async function resumeProfile() {
  try {
    await fetch('resumeProfile', { method: 'POST' });
    updateStatus();
  } catch (e) {
    console.error(e);
  }
}

async function skipStep() {
  try {
    await fetch('skipStep', { method: 'POST' });
    updateStatus();
  } catch (e) {
    console.error(e);
  }
}

function openScreen(id) {
  document.querySelectorAll('.screen').forEach(s => s.classList.remove('active'));
  document.getElementById(id).classList.add('active');
  closeSideMenu();

  if (id === 'settings') {
    loadSettings();   // re-read from the device each time the screen opens
  }
}

/* YOUR ORIGINAL CHART CODE */
const ctx = document.getElementById('tempChart').getContext('2d');

const gradient = ctx.createLinearGradient(0, 0, 0, 300);
gradient.addColorStop(0, 'rgba(255, 107, 91, 0.35)');
gradient.addColorStop(1, 'rgba(255, 107, 91, 0)');

const chart = new Chart(ctx, {
  type: 'line',
  data: {
    datasets: [
      {
        label: 'Temperature',
        data: [],
        borderColor: '#ff6b5b',
        backgroundColor: gradient,
        borderWidth: 2.5,
        pointRadius: 0,
        tension: 0.35,
        fill: true
      },
      {
        label: 'Target',
        data: [],
        borderColor: '#3ecf8e',
        borderWidth: 1.5,
        borderDash: [6, 4],
        pointRadius: 0,
        tension: 0,
        fill: false
      }
    ]
  },
  options: {
    responsive: true,
    maintainAspectRatio: false,
    animation: { duration: 300, easing: 'linear' },
    interaction: { intersect: false, mode: 'index' },
    plugins: {
      legend: { display: false },
      tooltip: { enabled: false }
		},
    scales: {
      x: {
        type: 'linear',
        grid: { color: '#232a3b', drawTicks: false },
        ticks: {
          color: '#8a92a6',
          maxTicksLimit: 6,
          font: { size: 11 },
          callback: (val) => {
            const diffSec = (Date.now() / 1000) - val;
            const diffMin = Math.floor(diffSec / 60);
            if (diffMin <= 0) return 'now';
            return `-${diffMin}m`;
          }
        },
        border: { display: false }
      },
      y: {
        min: 10,
        max: 100,
        grid: { color: '#232a3b', drawTicks: false },
        ticks: { color: '#8a92a6', font: { size: 11 } },
        border: { display: false }
      }
	}
  }
});

/* YOUR ORIGINAL UPDATE() */
async function update() {
  try {
    const res = await fetch('data');
    const d = await res.json();

    const spacing = 5;
    const now = Date.now() / 1000;

    const points = d.temps.map((v, i) => ({
      x: now - (d.temps.length - 1 - i) * spacing,
      y: v
    }));

    // Show the target line only while a step is being controlled
    const showTarget = controllerStatus.running && !controllerStatus.coolDownActive;

    chart.data.datasets[0].data = points;
    chart.data.datasets[1].data = showTarget
      ? points.map(p => ({ x: p.x, y: d.target }))
      : [];

    const windowSize = spacing * (d.temps.length - 1);
    chart.options.scales.x.min = now - windowSize;
    chart.options.scales.x.max = now;

    chart.update('none');

    document.getElementById('curTemp').textContent =
      d.temps.length ? d.temps[d.temps.length - 1].toFixed(1) : '--';

    // targetTemp text is owned by updateStatus(); don't overwrite it here
    document.getElementById('statusDot').style.background = '#3ecf8e';

  } catch (e) {
    document.getElementById('statusDot').style.background = '#ff6b5b';
    console.error('Data load error', e);
  }
}

setInterval(update, 5000);
setInterval(updateStatus, 1000);
update();
updateStatus();

/* PROFILES CODE */
let profiles = [];
let currentProfileIndex = 0;

/* Load profiles from server */
async function loadProfiles() {
  try {
    const res = await fetch('profiles_data.json');
    const data = await res.json();
    profiles = data.profiles || [];

    const sel = document.getElementById('profileSelect');
    sel.innerHTML = '';

    profiles.forEach((p, i) => {
      const opt = document.createElement('option');
      opt.value = i;
      opt.textContent = p.name;
      sel.appendChild(opt);
    });

    sel.onchange = () => {
      currentProfileIndex = Number(sel.value);
      updateProfileNameField();
      updateMainScreenProfileName();
      updateMainScreenProfileDetails();
      renderSteps();
      updateProfilePickerLabels();
    };

    // Load first profile
    updateProfileNameField();
    updateMainScreenProfileName();
    updateMainScreenProfileDetails();
    renderSteps();
    updateProfilePickerLabels();

  } catch (e) {
    console.error('Profile load error', e);
  }
}

/* Update profile name field */
function updateProfileNameField() {
  const nameInput = document.getElementById('profileName');
  if (profiles[currentProfileIndex]) {
    nameInput.value = profiles[currentProfileIndex].name;
    document.getElementById('profileWaterMass').value =
      Number(profiles[currentProfileIndex].waterMassKg ?? 20).toFixed(1);
    document.getElementById('profileGrainMass').value =
      Number(profiles[currentProfileIndex].grainMassKg ?? 5).toFixed(1);
  }
}

/* Show profile name on main screen */
function updateMainScreenProfileName() {
  const el = document.getElementById('activeProfileName');
  if (profiles[currentProfileIndex]) {
    el.textContent = profiles[currentProfileIndex].name;
  } else {
    el.textContent = '--';
  }
}

function updateMainScreenProfileDetails(status) {
  const detailsEl = document.getElementById('activeProfileDetails');
  const profile = profiles[currentProfileIndex];
  if (!detailsEl) return;

  const waterMass = profile?.waterMassKg ?? status?.waterMassKg;
  const grainMass = profile?.grainMassKg ?? status?.grainMassKg;
  const w = waterMass !== undefined ? Number(waterMass).toFixed(1) : '--';
  const g = grainMass !== undefined ? Number(grainMass).toFixed(1) : '--';
  detailsEl.textContent = `Water: ${w} l | Grain: ${g} kg`;
}

/* Render steps */
function renderSteps() {
  const cont = document.getElementById('stepsContainer');
  cont.innerHTML = '';

  const profile = profiles[currentProfileIndex];
  if (!profile) return;

  profile.steps.forEach((step, idx) => {
    const div = document.createElement('div');
	div.className = 'profile-step-card';

	div.innerHTML = `
		<div class="profile-step-header">
			<span class="profile-step-number">
				Step ${idx + 1}
			</span>

			<button class="btn-trash"
				onclick="removeStep(${idx})">
				🗑
			</button>
		</div>

		<div class="profile-step-grid">

			<div>
				<div class="label-small">
					Temperature
				</div>

				<input
					type="number"
					class="input-small"
					value="${step.temp}"
					onchange="updateStep(${idx}, 'temp', this.value)">
			</div>

			<div>
				<div class="label-small">
					Time (min)
          <button
            type="button"
            class="help-link step-time-help"
            aria-label="Manual step help">
            <span aria-hidden="true">i</span>
            <span class="step-time-tooltip">
              Set time to 0 for a manual pause. The controller waits for the target temperature, then pauses until RESUME is pressed.
            </span>
          </button>
				</div>

				<input
					type="number"
					class="input-small"
          min="0"
					value="${step.time}"
					onchange="updateStep(${idx}, 'time', this.value)">
			</div>

		</div>
	`;


    cont.appendChild(div);
  });
  
  renderMainSteps();
}


function renderMainSteps() {
  const cont = document.getElementById('mainStepsContainer');
  cont.innerHTML = '';

  const profile = profiles[currentProfileIndex];
  if (!profile) return;

  profile.steps.forEach((step, idx) => {

    const active =
      controllerStatus.running &&
      controllerStatus.step === idx;

    const completed =
      controllerStatus.running &&
      controllerStatus.step > idx;

    const div = document.createElement('div');

    div.className =
      'main-step' +
      (active ? ' active' : '') +
      (completed ? ' completed' : '');

    div.innerHTML = `
      <div class="step-badge">${idx + 1}</div>

      <div class="step-info">
        <div class="step-values">
          <span class="step-temp">${step.temp}°C</span>
          <span class="step-time">${step.time === 0 ? 'Manual' : `${step.time} min`}</span>
        </div>
      </div>
    `;

    cont.appendChild(div);
  });
}

/* Update profile name */
function updateProfileName(name) {
  profiles[currentProfileIndex].name = name;
  updateMainScreenProfileName();
  updateProfilePickerLabels();
}

function updateProfileMass(field, value) {
  profiles[currentProfileIndex][field] = Math.max(0, Number(value));
  updateMainScreenProfileDetails();
}

/* Update step */
function updateStep(index, field, value) {
  profiles[currentProfileIndex].steps[index][field] = Number(value);
}

/* Add step */
function addStep() {
  const p = profiles[currentProfileIndex];
  if (p.steps.length >= 6) return showToast("Maximum 6 steps", "error");

  p.steps.push({ temp: 65, time: 10 });
  renderSteps();
}

/* Remove step */
function removeStep(index) {
  const p = profiles[currentProfileIndex];
  if (p.steps.length <= 1) return showToast("Minimum 1 step", "error");

  p.steps.splice(index, 1);
  renderSteps();
}

/* Save profiles to server */
async function saveProfiles() {
  try {
    const keepIndex = currentProfileIndex;   // сохраняем текущий профиль

    await fetch("saveProfiles", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ profiles })
    });

    showToast("Profiles are saved");

    // Загружаем профили заново
    await loadProfiles();

    // Восстанавливаем выбранный профиль
    currentProfileIndex = keepIndex;

    const sel = document.getElementById('profileSelect');
    sel.value = keepIndex;

    updateProfileNameField();
    updateMainScreenProfileName();
    updateMainScreenProfileDetails();
    renderSteps();

  } catch (e) {
    showToast("Ошибка сохранения", "error");
    console.error(e);
  }
}

function addProfile() {
    if (profiles.length >= 15) {
        showToast("Maximum 15 profiles allowed");
        return;
    }

    const profileName = `New Profile ${profiles.length + 1}`;

    profiles.push({
        name: profileName,
      waterMassKg: 20,
      grainMassKg: 5,
        steps: [{ temp: 52, time: 20 }]
    });

    currentProfileIndex = profiles.length - 1;

    const sel = document.getElementById('profileSelect');

    const opt = document.createElement('option');
    opt.value = currentProfileIndex;
    opt.textContent = profileName;

    sel.appendChild(opt);
    sel.value = currentProfileIndex;

    updateProfileNameField();
    updateMainScreenProfileName();
    updateProfilePickerLabels();
    renderSteps();
    updateProfileButtons();

    showToast(`Profile added (${profiles.length}/15)`);
}

function updateProfileButtons() {
    const addBtn = document.getElementById('addProfileBtn');

    if (addBtn) {
        addBtn.disabled = profiles.length >= 15;
    }
}

async function deleteProfile() {
  if (!profiles[currentProfileIndex]) return;

	const profile = profiles[currentProfileIndex];

	if (!await showConfirm(
		`Are you sure you want to permanently delete profile "${profile.name}"?`,
		"Delete Profile"
	)) return;

  // Удаляем профиль
  profiles.splice(currentProfileIndex, 1);

  // Если профилей не осталось — создаём пустой
  if (profiles.length === 0) {
    profiles.push({
      name: "New Profile",
      waterMassKg: 20,
      grainMassKg: 5,
      steps: [
        { temp: 52, time: 20 },
        { temp: 63, time: 40 }
      ]
    });
    currentProfileIndex = 0;
  } else {
    // Если удалили последний — смещаем индекс назад
    if (currentProfileIndex >= profiles.length) {
      currentProfileIndex = profiles.length - 1;
    }
  }

  // Перерисовываем список профилей
  const sel = document.getElementById('profileSelect');
  sel.innerHTML = '';

  profiles.forEach((p, i) => {
    const opt = document.createElement('option');
    opt.value = i;
    opt.textContent = p.name;
    sel.appendChild(opt);
  });

  sel.value = currentProfileIndex;

  updateProfileNameField();
  updateMainScreenProfileName();
  updateMainScreenProfileDetails();
  updateProfilePickerLabels();
  renderSteps();
}

function openProfilePicker(overlayId = 'profilePickerOverlay') {
  if (controllerStatus.running || controllerStatus.coolDownActive) return;

  const picker = document.getElementById(overlayId);
  if (!picker.classList.contains('hidden')) {
    picker.classList.add('hidden');
    return;
  }

  const list = picker.querySelector('[id$="PickerList"]');
  list.innerHTML = '';

  profiles.forEach((p, i) => {
    const div = document.createElement('div');
    div.className = 'profile-option' + (i === currentProfileIndex ? ' selected' : '');
    div.textContent = p.name;
    div.onclick = () => selectProfile(i);
    list.appendChild(div);
  });

  picker.classList.remove('hidden');
}

function closeProfilePicker(e) {
  if (e.target === e.currentTarget) {
    e.currentTarget.classList.add('hidden');
  }
}

function selectProfile(index) {
  currentProfileIndex = index;
  updateProfileNameField();
  updateMainScreenProfileName();
  updateMainScreenProfileDetails();
  renderSteps();

  const sel = document.getElementById('profileSelect');
  if (sel) sel.value = index;

  updateProfilePickerLabels();
  document.querySelectorAll('.profile-dropdown').forEach(picker => {
    picker.classList.add('hidden');
  });
}

function updateProfilePickerLabels() {
  const name = profiles[currentProfileIndex]?.name || '--';
  const editorName = document.getElementById('editorProfileName');
  if (editorName) editorName.textContent = name;
}

document.addEventListener('click', (e) => {
  const pickerContainers = document.querySelectorAll('.profile-picker-container');
  const pickers = document.querySelectorAll('.profile-dropdown');

  pickerContainers.forEach((container, index) => {
    if (pickers[index] && !container.contains(e.target)) {
      pickers[index].classList.add('hidden');
    }
  });
});


let settings = {};

async function loadSettings() {
    try {
        const res = await fetch('settings');
        settings = await res.json();

        document.getElementById('setHeaterPowerW').value =
          settings.heaterPowerW ?? 2000;

        document.getElementById('setHeaterPowerEffW').value =
          Math.round(settings.heaterPowerEffW ?? 1900);

        document.getElementById('setHeaterTauSec').value =
          Math.round(settings.heaterTauSec ?? 90);

        document.getElementById('setHeaterStoreGain').value =
          Number(settings.heaterStoreGain ?? 1.2).toFixed(2);

        document.getElementById('setHeaterLossWPerC').value =
          Number(settings.heaterLossWPerC ?? 5).toFixed(2);

        document.getElementById('setHeaterAmbientC').value =
          settings.heaterAmbientC ?? 20;

        document.getElementById('setHeaterDeadband').value =
          Number(settings.heaterDeadband ?? 0.1).toFixed(1);

        document.getElementById('setHeaterMinSwitchSec').value =
          settings.heaterMinSwitchSec ?? 10;

        document.getElementById('setMixerOnSec').value =
            settings.mixerOnSec ?? 5;

        document.getElementById('setMixerRestSec').value =
            settings.mixerRestSec ?? 15;

        document.getElementById('setWifiSsid').value =
            settings.wifiSSID ?? '';

		document.getElementById('setCoolDownSec').value =
			settings.coolDownSec ?? 180;

		renderSensorInfo(settings);

    } catch (e) {
        console.error('Settings load error', e);
        showToast('Failed to load settings', 'error');
    }
}

/* DS18B20 sensor info (read-only) */
function renderSensorInfo(s) {
    const addrEl = document.getElementById('sensorAddress');
    const tempEl = document.getElementById('sensorTempAtRead');
    const statusEl = document.getElementById('sensorStatusBadge');

    if (!addrEl || !tempEl || !statusEl) return;

    addrEl.textContent = s.sensorFound ? s.sensorAddress : 'Not found';
    tempEl.textContent = s.sensorFound
        ? `${Number(s.sensorTempAtRead).toFixed(1)} °C`
        : '-- °C';

    if (s.sensorDebugFake) {
        statusEl.textContent = 'DEBUG';
        statusEl.classList.remove('status-error');
        statusEl.classList.add('status-ok');
    } else if (s.sensorFound && s.sensorOk) {
        statusEl.textContent = 'OK';
        statusEl.classList.remove('status-error');
        statusEl.classList.add('status-ok');
    } else {
        statusEl.textContent = s.sensorFound ? 'Error' : 'Not Found';
        statusEl.classList.remove('status-ok');
        statusEl.classList.add('status-error');
    }
}

async function rediscoverSensor() {
    const btn = document.getElementById('btnRediscoverSensor');
    if (btn) {
        btn.disabled = true;
        btn.textContent = 'Searching...';
    }

    try {
        const res = await fetch('rediscoverSensor', { method: 'POST' });

        if (res.status === 409) {
            showToast('Sensor read in progress, try again shortly', 'error');
            return;
        }

        const data = await res.json();

        if (data.sensorFound) {
            showToast(`Sensor found: ${data.sensorAddress}`);
        } else {
            showToast('No sensor found', 'error');
        }

        await loadSettings();

    } catch (e) {
        console.error(e);
        showToast('Rediscover failed', 'error');
    } finally {
        if (btn) {
            btn.disabled = false;
            btn.textContent = 'Rediscover Sensor';
        }
    }
}

async function saveSettings() {
    try {

        const payload = {
            heaterPowerW: parseFloat(
              document.getElementById('setHeaterPowerW').value
            ),

            heaterPowerEffW: parseFloat(
              document.getElementById('setHeaterPowerEffW').value
            ),

            heaterTauSec: parseFloat(
              document.getElementById('setHeaterTauSec').value
            ),

            heaterStoreGain: parseFloat(
              document.getElementById('setHeaterStoreGain').value
            ),

            heaterLossWPerC: parseFloat(
              document.getElementById('setHeaterLossWPerC').value
            ),

            heaterAmbientC: parseFloat(
              document.getElementById('setHeaterAmbientC').value
            ),

            heaterDeadband: parseFloat(
              document.getElementById('setHeaterDeadband').value
            ),

            heaterMinSwitchSec: parseInt(
              document.getElementById('setHeaterMinSwitchSec').value
            ),

            mixerOnSec: parseInt(
                document.getElementById('setMixerOnSec').value
            ),

            mixerRestSec: parseInt(
                document.getElementById('setMixerRestSec').value
            ),

			coolDownSec: parseInt(
				document.getElementById('setCoolDownSec').value
			),

            wifiSSID: document.getElementById('setWifiSsid').value,

            wifiPass: document.getElementById('setWifiPass').value
        };

        const response = await fetch('saveSettings', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify(payload)
        });

        if (!response.ok) {
          throw new Error(`Settings save failed (${response.status})`);
        }

        showToast('Settings saved');

    } catch (e) {
        console.error(e);
        showToast('Settings save failed', 'error');
    }
}


function showToast(message, type = 'success') {
  const container = document.getElementById('toastContainer');
  const toast = document.createElement('div');
  toast.className = 'toast' + (type === 'error' ? ' error' : '');
  toast.textContent = message;
  container.appendChild(toast);

  requestAnimationFrame(() => toast.classList.add('show'));

  setTimeout(() => {
    toast.classList.remove('show');
    setTimeout(() => toast.remove(), 250);
  }, 2500);
}

function showConfirm(message, title = "Confirm Action") {
    return new Promise(resolve => {

        const overlay = document.getElementById('confirmOverlay');
        const msg = document.getElementById('confirmMessage');
        const ttl = document.getElementById('confirmTitle');
        const ok = document.getElementById('confirmOk');
        const cancel = document.getElementById('confirmCancel');

        ttl.textContent = title;
        msg.textContent = message;

        overlay.classList.remove('hidden');

        const cleanup = () => {
            overlay.classList.add('hidden');

            ok.removeEventListener('click', okHandler);
            cancel.removeEventListener('click', cancelHandler);
        };

        const okHandler = () => {
            cleanup();
            resolve(true);
        };

        const cancelHandler = () => {
            cleanup();
            resolve(false);
        };

        ok.addEventListener('click', okHandler);
        cancel.addEventListener('click', cancelHandler);
    });
}

function showHelpModal(title, message, imageSrc = '') {
  const overlay = document.getElementById('helpOverlay');
  const titleEl = document.getElementById('helpTitle');
  const messageEl = document.getElementById('helpMessage');
  const imageWrap = document.getElementById('helpImageWrap');
  const imageEl = document.getElementById('helpImage');

  titleEl.textContent = title;
  messageEl.textContent = message;

  const hintEl = document.getElementById('helpImageHint');
  resetHelpZoom();

  if (imageSrc) {
    imageEl.src = imageSrc;
    imageEl.alt = `${title} illustration`;
    imageWrap.classList.remove('hidden');
    hintEl.textContent = window.matchMedia('(hover: none)').matches
      ? 'Pinch or double-tap to zoom'
      : 'Scroll or double-click to zoom';
    hintEl.classList.remove('hidden');
  } else {
    imageWrap.classList.add('hidden');
    hintEl.classList.add('hidden');
  }

  overlay.classList.remove('hidden');
  overlay.querySelector('.help-modal').scrollTop = 0;
}

function closeHelpModal() {
  document.getElementById('helpOverlay').classList.add('hidden');
  resetHelpZoom();
}

/* Pinch / double-tap / wheel zoom for the help image */
const HELP_ZOOM_MAX = 4;
const helpZoom = { s: 1, x: 0, y: 0 };

function applyHelpZoom() {
  const wrap = document.getElementById('helpImageWrap');
  const img = document.getElementById('helpImage');
  const w = wrap.clientWidth;
  const h = wrap.clientHeight;

  helpZoom.s = Math.min(HELP_ZOOM_MAX, Math.max(1, helpZoom.s));
  helpZoom.x = Math.min(0, Math.max(w * (1 - helpZoom.s), helpZoom.x));
  helpZoom.y = Math.min(0, Math.max(h * (1 - helpZoom.s), helpZoom.y));

  img.style.transform = helpZoom.s === 1
    ? ''
    : `translate(${helpZoom.x}px, ${helpZoom.y}px) scale(${helpZoom.s})`;
  wrap.classList.toggle('zoomed', helpZoom.s > 1);
}

function resetHelpZoom() {
  helpZoom.s = 1;
  helpZoom.x = 0;
  helpZoom.y = 0;
  applyHelpZoom();
}

/* Zoom to newS keeping the point (cx, cy) - in wrap coordinates - fixed */
function helpZoomAt(cx, cy, newS) {
  newS = Math.min(HELP_ZOOM_MAX, Math.max(1, newS));
  const k = newS / helpZoom.s;
  helpZoom.x = cx - (cx - helpZoom.x) * k;
  helpZoom.y = cy - (cy - helpZoom.y) * k;
  helpZoom.s = newS;
  applyHelpZoom();
}

(function initHelpImageZoom() {
  const wrap = document.getElementById('helpImageWrap');
  if (!wrap) return;

  const pos = (p) => {
    const r = wrap.getBoundingClientRect();
    return { x: p.clientX - r.left, y: p.clientY - r.top };
  };

  const toggleZoomAt = (p) => {
    if (helpZoom.s > 1) resetHelpZoom();
    else helpZoomAt(p.x, p.y, 2.5);
  };

  let pinch = null;   // active two-finger gesture
  let pan = null;     // active one-finger gesture
  let lastTap = 0;

  wrap.addEventListener('touchstart', (e) => {
    if (e.touches.length === 2) {
      const a = pos(e.touches[0]);
      const b = pos(e.touches[1]);
      pinch = {
        dist: Math.hypot(a.x - b.x, a.y - b.y) || 1,
        s: helpZoom.s, x: helpZoom.x, y: helpZoom.y,
        mx: (a.x + b.x) / 2, my: (a.y + b.y) / 2
      };
      pan = null;
    } else if (e.touches.length === 1) {
      const p = pos(e.touches[0]);
      pinch = null;
      pan = { x: p.x, y: p.y, total: 0 };
    }
  }, { passive: true });

  wrap.addEventListener('touchmove', (e) => {
    if (pinch && e.touches.length === 2) {
      const a = pos(e.touches[0]);
      const b = pos(e.touches[1]);
      const dist = Math.hypot(a.x - b.x, a.y - b.y);
      const mx = (a.x + b.x) / 2;
      const my = (a.y + b.y) / 2;
      const s = Math.min(HELP_ZOOM_MAX, Math.max(1, pinch.s * dist / pinch.dist));

      // keep the image point that started under the fingers under them
      helpZoom.s = s;
      helpZoom.x = mx - (pinch.mx - pinch.x) * (s / pinch.s);
      helpZoom.y = my - (pinch.my - pinch.y) * (s / pinch.s);
      applyHelpZoom();
      e.preventDefault();
    } else if (pan && e.touches.length === 1) {
      const p = pos(e.touches[0]);
      const dx = p.x - pan.x;
      const dy = p.y - pan.y;
      pan.total += Math.abs(dx) + Math.abs(dy);
      pan.x = p.x;
      pan.y = p.y;
      if (helpZoom.s > 1) {
        helpZoom.x += dx;
        helpZoom.y += dy;
        applyHelpZoom();
        e.preventDefault();
      }
    }
  }, { passive: false });

  wrap.addEventListener('touchend', (e) => {
    if (e.touches.length === 0) {
      if (helpZoom.s < 1.05) resetHelpZoom();

      // double-tap: a short touch with almost no movement, twice in a row
      if (pan && pan.total < 10) {
        const now = Date.now();
        if (now - lastTap < 300) {
          toggleZoomAt(pan);
          lastTap = 0;
          if (e.cancelable) e.preventDefault();   // suppress the synthetic dblclick
        } else {
          lastTap = now;
        }
      }
      pinch = null;
      pan = null;
    } else if (e.touches.length === 1) {
      // one finger left after a pinch: continue as a pan, never as a tap
      const p = pos(e.touches[0]);
      pinch = null;
      pan = { x: p.x, y: p.y, total: 999 };
    }
  }, { passive: false });

  wrap.addEventListener('touchcancel', () => { pinch = null; pan = null; });

  /* Desktop: wheel / trackpad-pinch zoom, drag to pan, double-click toggle */
  wrap.addEventListener('wheel', (e) => {
    e.preventDefault();
    const p = pos(e);
    helpZoomAt(p.x, p.y, helpZoom.s * Math.exp(-e.deltaY * 0.002));
  }, { passive: false });

  let drag = null;
  wrap.addEventListener('mousedown', (e) => {
    if (helpZoom.s > 1) {
      drag = { x: e.clientX, y: e.clientY };
      e.preventDefault();
    }
  });
  window.addEventListener('mousemove', (e) => {
    if (!drag) return;
    helpZoom.x += e.clientX - drag.x;
    helpZoom.y += e.clientY - drag.y;
    drag = { x: e.clientX, y: e.clientY };
    applyHelpZoom();
  });
  window.addEventListener('mouseup', () => { drag = null; });
  wrap.addEventListener('dblclick', (e) => toggleZoomAt(pos(e)));

  window.addEventListener('orientationchange', resetHelpZoom);
})();

/* Load profiles and Settings on startup */
loadProfiles();
loadSettings();