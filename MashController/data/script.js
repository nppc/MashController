/* MENU */
document.getElementById('menuBtn').onclick = () => {
  document.getElementById('sideMenu').classList.toggle('open');
};

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
    const res = await fetch('/status');
    const st = await res.json();

    controllerStatus = st;

    updateProcessUI(st);
	
    document.getElementById('curTemp').textContent =
      Number(st.currentTemp).toFixed(1);

    document.getElementById('targetTemp').textContent =
      Number(st.stepTemp).toFixed(1);

    if (st.running && st.profileName)
      document.getElementById('activeProfileName').textContent =
        st.profileName;

	renderMainSteps();
  } catch (e) {
    console.error('Status error', e);
  }
}

async function startSelectedProfile() {

  try {

    await fetch(
      `/startProfile?profile=${currentProfileIndex}`,
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

    await fetch('/stopProfile', {
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
  const paused = !!st.paused;

  badge.classList.remove('running', 'paused');
  if (st.running && paused) {
    badge.textContent = 'Paused';
    badge.classList.add('paused');
  } else if (st.running) {
    badge.textContent = 'Running';
    badge.classList.add('running');
  } else {
    badge.textContent = 'Stopped';
  }

  const timerEl = document.getElementById('remainingTime');
  timerEl.textContent = formatTime(st.remaining);
  timerEl.classList.toggle('inactive', !st.running);
  
  document.getElementById('btnStart').classList.toggle('hidden', st.running);
  document.getElementById('btnPause').classList.toggle('hidden', !st.running || paused);
  document.getElementById('btnResume').classList.toggle('hidden', !st.running || !paused);
  document.getElementById('btnSkip').classList.toggle('hidden', !st.running);
  document.getElementById('btnStop').classList.toggle('hidden', !st.running);
  
  document.querySelector('.profile-picker').classList.toggle('disabled', st.running);
}

async function pauseProfile() {
  try {
    await fetch('/pauseProfile', { method: 'POST' });
    updateStatus();
  } catch (e) {
    console.error(e);
  }
}

async function resumeProfile() {
  try {
    await fetch('/resumeProfile', { method: 'POST' });
    updateStatus();
  } catch (e) {
    console.error(e);
  }
}

async function skipStep() {
  try {
    await fetch('/skipStep', { method: 'POST' });
    updateStatus();
  } catch (e) {
    console.error(e);
  }
}

function openScreen(id) {
  document.querySelectorAll('.screen').forEach(s => s.classList.remove('active'));
  document.getElementById(id).classList.add('active');
  document.getElementById('sideMenu').classList.remove('open');
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

    chart.data.datasets[0].data = points;
    chart.data.datasets[1].data = points.map(p => ({ x: p.x, y: d.target }));

    const windowSize = spacing * (d.temps.length - 1);
    chart.options.scales.x.min = now - windowSize;
    chart.options.scales.x.max = now;

    chart.update('none');

    document.getElementById('curTemp').textContent =
      d.temps.length ? d.temps[d.temps.length - 1].toFixed(1) : '--';

    document.getElementById('targetTemp').textContent = d.target.toFixed(1);
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
      renderSteps();
    };

    // Load first profile
    updateProfileNameField();
    updateMainScreenProfileName();
    renderSteps();

  } catch (e) {
    console.error('Profile load error', e);
  }
}

/* Update profile name field */
function updateProfileNameField() {
  const nameInput = document.getElementById('profileName');
  if (profiles[currentProfileIndex]) {
    nameInput.value = profiles[currentProfileIndex].name;
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

/* Render steps */
function renderSteps() {
  const cont = document.getElementById('stepsContainer');
  cont.innerHTML = '';

  const profile = profiles[currentProfileIndex];
  if (!profile) return;

  profile.steps.forEach((step, idx) => {
    const div = document.createElement('div');
    div.className = 'stat-card';
    div.style.marginTop = '12px';

    div.innerHTML = `
      <div class="label">Шаг ${idx + 1}</div>

      <div style="display:flex; gap:16px; align-items:flex-end; margin-top:12px;">

        <div style="flex:1;">
          <div class="label-small">Температура</div>
          <input type="number" class="input-small"
            value="${step.temp}"
            onchange="updateStep(${idx}, 'temp', this.value)">
        </div>

        <div style="flex:1;">
          <div class="label-small">Время (мин)</div>
          <input type="number" class="input-small"
            value="${step.time}"
            onchange="updateStep(${idx}, 'time', this.value)">
        </div>

        <button class="btn-trash" onclick="removeStep(${idx})">🗑</button>
      </div>
    `;

    cont.appendChild(div);
  });
  
  renderMainSteps();
}


function renderMainSteps() {

  const cont =
    document.getElementById('mainStepsContainer');

  cont.innerHTML = '';

  const profile = profiles[currentProfileIndex];

  if (!profile) return;

  profile.steps.forEach((step, idx) => {

    const active =
      controllerStatus.running &&
      controllerStatus.step === idx;

    const div = document.createElement('div');

    div.style.padding = '8px';
    div.style.marginTop = '6px';
    div.style.borderRadius = '6px';

    div.style.background =
      active ? '#3ecf8e33' : '#232a3b';

    div.innerHTML =
      `Step ${idx + 1}: ${step.temp}°C / ${step.time} min`;

    cont.appendChild(div);
  });
}

/* Update profile name */
function updateProfileName(name) {
  profiles[currentProfileIndex].name = name;
  updateMainScreenProfileName();
}

/* Update step */
function updateStep(index, field, value) {
  profiles[currentProfileIndex].steps[index][field] = Number(value);
}

/* Add step */
function addStep() {
  const p = profiles[currentProfileIndex];
  if (p.steps.length >= 6) return alert("Максимум 6 шагов");

  p.steps.push({ temp: 65, time: 10 });
  renderSteps();
}

/* Remove step */
function removeStep(index) {
  const p = profiles[currentProfileIndex];
  if (p.steps.length <= 1) return alert("Минимум 1 шаг");

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

    alert("Профили сохранены");

    // Загружаем профили заново
    await loadProfiles();

    // Восстанавливаем выбранный профиль
    currentProfileIndex = keepIndex;

    const sel = document.getElementById('profileSelect');
    sel.value = keepIndex;

    updateProfileNameField();
    updateMainScreenProfileName();
    renderSteps();

  } catch (e) {
    alert("Ошибка сохранения");
    console.error(e);
  }
}

function addProfile() {
  profiles.push({
    name: "New Profile",
    steps: [
      { temp: 52, time: 20 }
    ]
  });

  currentProfileIndex = profiles.length - 1;

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
  renderSteps();
}

function deleteProfile() {
  if (!profiles[currentProfileIndex]) return;

  if (!confirm("Удалить профиль?")) return;

  // Удаляем профиль
  profiles.splice(currentProfileIndex, 1);

  // Если профилей не осталось — создаём пустой
  if (profiles.length === 0) {
    profiles.push({
      name: "Новый профиль",
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
  renderSteps();
}

function openProfilePicker() {
  if (controllerStatus.running) return;

  const list = document.getElementById('profilePickerList');
  list.innerHTML = '';

  profiles.forEach((p, i) => {
    const div = document.createElement('div');
    div.className = 'profile-option' + (i === currentProfileIndex ? ' selected' : '');
    div.textContent = p.name;
    div.onclick = () => selectProfile(i);
    list.appendChild(div);
  });

  document.getElementById('profilePickerOverlay').classList.add('open');
}

function closeProfilePicker(e) {
  // only close if clicking the dark overlay itself, not the sheet content
  if (e.target.id === 'profilePickerOverlay') {
    document.getElementById('profilePickerOverlay').classList.remove('open');
  }
}

function selectProfile(index) {
  currentProfileIndex = index;
  updateMainScreenProfileName();

  const sel = document.getElementById('profileSelect');
  if (sel) sel.value = index;

  document.getElementById('profilePickerOverlay').classList.remove('open');
}

/* Load profiles on startup */
loadProfiles();
