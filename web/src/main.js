/*
 * main.js — Live tuner client for the Station Books footfall counter.
 *
 * Talks to tuner/tuner.ino through the dev server's serial bridge
 * (see ../serial-bridge.js) over ws://<this host>/serial. Everything on screen
 * is derived from the JSON lines the device emits; every slider writes a
 * `set <key> <ms>` command straight back to it.
 */

const COLOURS = {
  outer: '#4cc2ff',
  inner: '#ffcc4c',
  entry: '#3ddc84',
  exit: '#7aa2ff',
  noise: '#8a8f98',
  simul: '#ff8a5b',
  grid: '#22262d',
  axis: '#3a4048',
  text: '#c9d1d9',
};

const RESULT_LABEL = { entry: 'ENTRY', exit: 'EXIT', noise: 'discarded', simul: 'simultaneous' };
const MAX_DETECTIONS = 300;
const MAX_LOG_LINES = 200;
const SCOPE_HEIGHT = 220;
const DELTA_HEIGHT = 130;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

const state = {
  // Device clock: `ms` in every message is millis() since the board booted.
  // We track the newest device time we've seen alongside the host time we saw
  // it, and interpolate between status messages so the trace scrolls smoothly.
  devMs: 0,
  devMsHostTime: 0,
  seenDevice: false,

  levels: { o: 0, i: 0 },
  edges: { o: [], i: [] },   // [{ms, v}] rising/falling edges, oldest first
  detections: [],            // [{ms, r, dt, f}] newest last
  counters: { entry: 0, exit: 0, noise: 0, simul: 0 },
  status: { st: 'idle', af: 0, ao: 'o', deb: 0, warm: 0, up: 0 },

  cfg: { window: 400, debounce: 1000, simultaneous: 10, warmup: 5000 },
  cfgKnown: false,

  span: 8000,
  frozen: false,
  frozenAt: 0,
};

const $ = (id) => document.getElementById(id);

// Device "now", interpolated from the last message received.
function deviceNow() {
  if (!state.seenDevice) return 0;
  if (state.frozen) return state.frozenAt;
  return state.devMs + (performance.now() - state.devMsHostTime);
}

function noteDeviceTime(ms) {
  // A large step backwards means the board rebooted (reset button, reflash):
  // start the traces over rather than drawing across the discontinuity.
  if (state.seenDevice && ms + 1000 < state.devMs) resetTraces();
  state.devMs = ms;
  state.devMsHostTime = performance.now();
  state.seenDevice = true;
}

function resetTraces() {
  state.edges.o = [];
  state.edges.i = [];
  state.detections = [];
  state.devMs = 0;
  // Re-anchor the host clock too, or deviceNow() would interpolate from the
  // pre-reboot timestamp and the trace would jump before the next message.
  state.devMsHostTime = performance.now();
}

// ---------------------------------------------------------------------------
// WebSocket to the serial bridge
// ---------------------------------------------------------------------------

let ws = null;

function connect() {
  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  ws = new WebSocket(`${proto}//${location.host}/serial`);

  ws.onopen = () => send('get');
  ws.onclose = () => {
    setBridge('off', 'dev server lost — retrying');
    setTimeout(connect, 1500);
  };
  ws.onerror = () => ws.close();
  ws.onmessage = (ev) => {
    for (const line of ev.data.split('\n')) {
      const text = line.trim();
      if (!text) continue;
      let msg;
      try {
        msg = JSON.parse(text);
      } catch {
        // Boot-loader chatter and stray Serial.println output land here.
        addLog(text, 'raw');
        continue;
      }
      handle(msg);
    }
  };
}

function send(cmd) {
  if (ws && ws.readyState === WebSocket.OPEN) ws.send(cmd);
}

function handle(msg) {
  switch (msg.t) {
    case 'bridge':
      if (msg.state === 'open') setBridge('on', msg.port || 'serial');
      else if (msg.state === 'searching') setBridge('off', msg.detail);
      else setBridge('off', msg.detail || msg.state);
      if (msg.detail) addLog(`[bridge] ${msg.detail}`, 'raw');
      break;

    case 'hello':
      resetTraces();
      addLog(`device connected — OUTER=GPIO${msg.outerPin} INNER=GPIO${msg.innerPin}`, 'info');
      break;

    case 'cfg':
      state.cfg = { ...state.cfg, ...msg.cfg };
      state.cfgKnown = true;
      syncSliders();
      renderSnippet();
      break;

    case 'e': {  // edge
      noteDeviceTime(msg.ms);
      pushEdge(msg.s, msg.ms, msg.v);
      state.levels[msg.s] = msg.v;
      break;
    }

    case 'd': {  // detection
      noteDeviceTime(msg.ms);
      state.detections.push({ ms: msg.ms, r: msg.r, dt: msg.dt, f: msg.f });
      if (state.detections.length > MAX_DETECTIONS) state.detections.shift();
      const dir = msg.f === 'o' ? 'OUTER→INNER' : 'INNER→OUTER';
      addLog(`${RESULT_LABEL[msg.r] || msg.r} · ${dir} · delta ${msg.dt} ms`, msg.r);
      renderDeltas();
      break;
    }

    case 's': {  // status, 10 Hz
      noteDeviceTime(msg.ms);
      // Levels here are authoritative: if an edge line was lost they re-sync
      // the trace, so a dropped byte can't leave a channel stuck HIGH.
      for (const ch of ['o', 'i']) {
        if (state.levels[ch] !== msg[ch]) {
          pushEdge(ch, msg.ms, msg[ch]);
          state.levels[ch] = msg[ch];
        }
      }
      state.status = { st: msg.st, af: msg.af, ao: msg.ao, deb: msg.deb, warm: msg.warm, up: msg.up };
      state.counters = msg.n;
      renderStatus();
      break;
    }

    case 'log':
      addLog(msg.m, 'info');
      break;
  }
}

function pushEdge(ch, ms, v) {
  const arr = state.edges[ch];
  if (arr.length && arr[arr.length - 1].ms > ms) arr.length = 0;  // reboot guard
  arr.push({ ms, v });
  // Keep a little more than the widest span, plus one edge before the cutoff
  // so we always know the level at the left-hand edge of the canvas.
  const cutoff = ms - 65000;
  let drop = 0;
  while (drop + 1 < arr.length && arr[drop + 1].ms < cutoff) drop++;
  if (drop) arr.splice(0, drop);
}

// ---------------------------------------------------------------------------
// Header / status rendering
// ---------------------------------------------------------------------------

function setBridge(cls, text) {
  const el = $('bridge');
  el.className = `pill ${cls === 'on' ? 'pill-on' : 'pill-off'}`;
  el.textContent = cls === 'on' ? `● ${text}` : `○ ${text || 'disconnected'}`;
}

function renderStatus() {
  const s = state.status;
  $('outerVal').textContent = state.levels.o ? 'HIGH' : 'LOW';
  $('innerVal').textContent = state.levels.i ? 'HIGH' : 'LOW';
  $('lampOuter').classList.toggle('on', !!state.levels.o);
  $('lampInner').classList.toggle('on', !!state.levels.i);

  let label, cls;
  if (s.warm > 0) {
    label = `WARM-UP ${(s.warm / 1000).toFixed(1)} s`;
    cls = 'warm';
  } else if (s.deb > 0) {
    label = `DEBOUNCE ${s.deb} ms`;
    cls = 'debounce';
  } else if (s.st === 'armed') {
    label = `ARMED ${s.ao === 'o' ? 'OUTER' : 'INNER'} · ${s.af} ms`;
    cls = 'armed';
  } else {
    label = 'IDLE';
    cls = 'idle';
  }
  $('stateVal').textContent = label;
  $('lampState').className = `lamp wide ${cls}`;

  $('uptime').textContent = state.seenDevice ? `device up ${formatUptime(s.up)}` : '';

  $('nEntry').textContent = state.counters.entry ?? 0;
  $('nExit').textContent = state.counters.exit ?? 0;
  $('nNoise').textContent = state.counters.noise ?? 0;
  $('nSimul').textContent = state.counters.simul ?? 0;

  const noise = state.counters.noise ?? 0;
  const counted = (state.counters.entry ?? 0) + (state.counters.exit ?? 0);
  const hint = $('noiseHint');
  if (noise === 0 && counted === 0) {
    hint.textContent = '';
  } else if (noise > counted) {
    hint.textContent =
      'More discards than counted events. If those were real walk-throughs, ' +
      'the detection window is too narrow — widen it and try again.';
    hint.className = 'hint warn';
  } else {
    hint.textContent = 'Walk through and check each pass lands in the right column.';
    hint.className = 'hint';
  }
}

function formatUptime(sec) {
  if (sec < 60) return `${sec}s`;
  const m = Math.floor(sec / 60);
  if (m < 60) return `${m}m ${sec % 60}s`;
  return `${Math.floor(m / 60)}h ${m % 60}m`;
}

// ---------------------------------------------------------------------------
// Scope
// ---------------------------------------------------------------------------

const scope = $('scope');
const sctx = scope.getContext('2d');

// Size the backing store for the display's pixel ratio while keeping the CSS
// box at `cssHeight`. The height must be pinned in CSS: assigning canvas.height
// writes through to the height attribute, so reading it back would compound the
// dpr scaling and the canvas would grow on every frame.
function fitCanvas(canvas, cssHeight) {
  const dpr = window.devicePixelRatio || 1;
  const w = canvas.clientWidth;
  canvas.style.height = `${cssHeight}px`;
  if (canvas.width !== Math.round(w * dpr) || canvas.height !== Math.round(cssHeight * dpr)) {
    canvas.width = Math.round(w * dpr);
    canvas.height = Math.round(cssHeight * dpr);
  }
  const ctx = canvas.getContext('2d');
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  return { w, h: cssHeight };
}

// Level of channel `ch` at device time `t`, from the edge history.
function levelAt(ch, t) {
  const arr = state.edges[ch];
  let v = 0;
  for (const e of arr) {
    if (e.ms > t) break;
    v = e.v;
  }
  return v;
}

function drawScope() {
  const { w, h } = fitCanvas(scope, SCOPE_HEIGHT);
  const now = deviceNow();
  const t0 = now - state.span;
  const x = (t) => ((t - t0) / state.span) * w;

  sctx.clearRect(0, 0, w, h);

  // Time grid, one line per second (or per 5 s on the wider spans).
  const step = state.span <= 8000 ? 1000 : state.span <= 20000 ? 2000 : 10000;
  sctx.strokeStyle = COLOURS.grid;
  sctx.fillStyle = COLOURS.axis;
  sctx.lineWidth = 1;
  sctx.font = '10px ui-monospace, SFMono-Regular, Menlo, monospace';
  for (let t = Math.ceil(t0 / step) * step; t < now; t += step) {
    const px = Math.round(x(t)) + 0.5;
    sctx.beginPath();
    sctx.moveTo(px, 0);
    sctx.lineTo(px, h - 14);
    sctx.stroke();
    sctx.fillText(`-${((now - t) / 1000).toFixed(0)}s`, px + 3, h - 3);
  }

  const lanes = [
    { ch: 'o', label: 'OUTER', top: 18, height: 62, colour: COLOURS.outer },
    { ch: 'i', label: 'INNER', top: 104, height: 62, colour: COLOURS.inner },
  ];

  for (const lane of lanes) {
    const yHigh = lane.top;
    const yLow = lane.top + lane.height;

    sctx.strokeStyle = COLOURS.grid;
    sctx.beginPath();
    sctx.moveTo(0, yLow + 0.5);
    sctx.lineTo(w, yLow + 0.5);
    sctx.stroke();

    sctx.fillStyle = lane.colour;
    sctx.font = '11px ui-monospace, SFMono-Regular, Menlo, monospace';
    sctx.fillText(lane.label, 4, yHigh - 5);

    // Step trace: constant level between edges.
    sctx.strokeStyle = lane.colour;
    sctx.lineWidth = 2;
    sctx.beginPath();
    let v = levelAt(lane.ch, t0);
    let px = 0;
    let py = v ? yHigh : yLow;
    sctx.moveTo(px, py);
    for (const e of state.edges[lane.ch]) {
      if (e.ms <= t0) continue;
      if (e.ms > now) break;
      const ex = x(e.ms);
      sctx.lineTo(ex, py);
      py = e.v ? yHigh : yLow;
      sctx.lineTo(ex, py);
      v = e.v;
    }
    sctx.lineTo(w, py);
    sctx.stroke();
  }

  // Detection markers, with a bracket spanning the measured delta.
  sctx.font = '10px ui-monospace, SFMono-Regular, Menlo, monospace';
  for (const d of state.detections) {
    if (d.ms < t0) continue;
    const xEnd = x(d.ms);
    const xStart = x(d.ms - d.dt);
    const colour = COLOURS[d.r] || COLOURS.noise;

    sctx.strokeStyle = colour;
    sctx.lineWidth = 1;
    sctx.setLineDash(d.r === 'entry' || d.r === 'exit' ? [] : [3, 3]);
    sctx.beginPath();
    sctx.moveTo(xEnd, 12);
    sctx.lineTo(xEnd, h - 16);
    sctx.stroke();

    if (d.dt > 0) {
      sctx.beginPath();
      sctx.moveTo(xStart, 90);
      sctx.lineTo(xEnd, 90);
      sctx.stroke();
    }
    sctx.setLineDash([]);

    sctx.fillStyle = colour;
    const text = d.r === 'entry' || d.r === 'exit'
      ? `${RESULT_LABEL[d.r]} ${d.dt}ms`
      : `${RESULT_LABEL[d.r]}`;
    sctx.fillText(text, Math.min(xEnd + 4, w - sctx.measureText(text).width - 2), 10);
  }
}

// ---------------------------------------------------------------------------
// Delta plot + suggestion
// ---------------------------------------------------------------------------

const deltas = $('deltas');
const dctx = deltas.getContext('2d');
let suggested = null;

function countedDeltas() {
  return state.detections.filter((d) => d.r === 'entry' || d.r === 'exit');
}

function renderDeltas() {
  const { w, h } = fitCanvas(deltas, DELTA_HEIGHT);
  dctx.clearRect(0, 0, w, h);

  const list = countedDeltas();
  const stats = $('deltaStats');
  const btn = $('applySuggested');

  if (!list.length) {
    stats.textContent = 'no counted events yet';
    btn.disabled = true;
    suggested = null;
    dctx.fillStyle = COLOURS.noise;
    dctx.font = '12px system-ui, sans-serif';
    dctx.fillText('Walk through the doorway — each counted pass plots its delta here.', 12, h / 2);
    return;
  }

  const values = list.map((d) => d.dt).sort((a, b) => a - b);
  const min = values[0];
  const max = values[values.length - 1];
  const median = values[Math.floor(values.length / 2)];
  suggested = Math.ceil((max * 1.5) / 10) * 10;   // README rule, rounded to 10 ms

  stats.textContent =
    `${values.length} events · min ${min} ms · median ${median} ms · max ${max} ms · suggested window ${suggested} ms`;
  btn.disabled = suggested === state.cfg.window;
  btn.textContent = `Apply suggested window (${suggested} ms)`;

  const scaleMax = Math.max(max * 1.25, state.cfg.window * 1.2, 100);
  const pad = 30;
  const x = (ms) => pad + (ms / scaleMax) * (w - pad * 2);
  const baseY = h - 30;

  // Axis
  dctx.strokeStyle = COLOURS.axis;
  dctx.lineWidth = 1;
  dctx.beginPath();
  dctx.moveTo(pad, baseY + 0.5);
  dctx.lineTo(w - pad, baseY + 0.5);
  dctx.stroke();

  dctx.fillStyle = COLOURS.noise;
  dctx.font = '10px ui-monospace, SFMono-Regular, Menlo, monospace';
  const tick = scaleMax > 1200 ? 250 : scaleMax > 500 ? 100 : 50;
  for (let ms = 0; ms <= scaleMax; ms += tick) {
    const px = Math.round(x(ms)) + 0.5;
    dctx.beginPath();
    dctx.moveTo(px, baseY);
    dctx.lineTo(px, baseY + 4);
    dctx.stroke();
    dctx.fillText(`${ms}`, px - 8, baseY + 16);
  }

  // Current window and the suggestion.
  const line = (ms, colour, label, dash) => {
    if (ms > scaleMax) return;
    dctx.strokeStyle = colour;
    dctx.setLineDash(dash);
    dctx.beginPath();
    dctx.moveTo(Math.round(x(ms)) + 0.5, 8);
    dctx.lineTo(Math.round(x(ms)) + 0.5, baseY);
    dctx.stroke();
    dctx.setLineDash([]);
    dctx.fillStyle = colour;
    dctx.fillText(label, x(ms) + 4, 16);
  };
  line(state.cfg.window, COLOURS.simul, `window ${state.cfg.window}`, []);
  line(suggested, COLOURS.entry, `suggested ${suggested}`, [4, 3]);

  // One dot per counted event, newest at the top.
  const rows = Math.min(list.length, 12);
  list.slice(-rows).forEach((d, idx) => {
    const y = baseY - 10 - idx * ((baseY - 30) / Math.max(rows, 1));
    dctx.fillStyle = d.r === 'entry' ? COLOURS.entry : COLOURS.exit;
    dctx.beginPath();
    dctx.arc(x(d.dt), y, 4, 0, Math.PI * 2);
    dctx.fill();
  });
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

const SLIDERS = [
  { id: 'window', key: 'window' },
  { id: 'debounce', key: 'debounce' },
  { id: 'simultaneous', key: 'simultaneous' },
  { id: 'warmup', key: 'warmup' },
];

let suppressSliderEvents = false;

// Dragging a slider fires `input` on every pixel. The device only reads its
// serial buffer once per loop, so coalesce to ~15 writes/second and always send
// the final resting value.
const SLIDER_WRITE_MS = 65;
const pendingWrites = new Map();
let writeTimer = null;

function queueSet(key, value) {
  pendingWrites.set(key, value);
  if (writeTimer) return;
  writeTimer = setTimeout(() => {
    writeTimer = null;
    for (const [k, v] of pendingWrites) send(`set ${k} ${v}`);
    pendingWrites.clear();
  }, SLIDER_WRITE_MS);
}

function syncSliders() {
  suppressSliderEvents = true;
  for (const { id, key } of SLIDERS) {
    const el = $(id);
    if (document.activeElement !== el) el.value = state.cfg[key];
    $(`${id}Out`).textContent = `${state.cfg[key]} ms`;
  }
  suppressSliderEvents = false;
  renderDeltas();
}

for (const { id, key } of SLIDERS) {
  const el = $(id);
  el.addEventListener('input', () => {
    $(`${id}Out`).textContent = `${el.value} ms`;
    if (suppressSliderEvents) return;
    state.cfg[key] = Number(el.value);
    queueSet(key, el.value);
    renderSnippet();
    renderDeltas();
  });
}

$('widen').addEventListener('click', () => {
  send('set window 1500');
  addLog('window widened to 1500 ms for sampling — narrow it once you have deltas', 'info');
});

$('applySuggested').addEventListener('click', () => {
  if (suggested) send(`set window ${suggested}`);
});

$('reset').addEventListener('click', () => {
  send('reset');
  state.detections = [];
  renderDeltas();
});

$('span').addEventListener('change', (e) => { state.span = Number(e.target.value); });

$('freeze').addEventListener('click', () => {
  state.frozen = !state.frozen;
  state.frozenAt = deviceNow();
  $('freeze').textContent = state.frozen ? 'Resume' : 'Freeze';
  $('freeze').classList.toggle('active', state.frozen);
});

function renderSnippet() {
  $('snippet').textContent =
    `#define DETECTION_WINDOW_MS  ${String(state.cfg.window).padStart(5)}` +
    `  // max gap between sensor triggers\n` +
    `#define DEBOUNCE_MS          ${String(state.cfg.debounce).padStart(5)}` +
    `  // ignore window after a valid event\n` +
    `#define SIMULTANEOUS_MS      ${String(state.cfg.simultaneous).padStart(5)}` +
    `  // triggers closer than this = discard`;
}

$('copy').addEventListener('click', async () => {
  const btn = $('copy');
  try {
    await navigator.clipboard.writeText($('snippet').textContent);
    btn.textContent = 'Copied';
  } catch {
    btn.textContent = 'Select and copy';
  }
  setTimeout(() => { btn.textContent = 'Copy'; }, 1500);
});

// ---------------------------------------------------------------------------
// Log
// ---------------------------------------------------------------------------

function addLog(text, kind) {
  const log = $('log');
  const atBottom = log.scrollTop + log.clientHeight >= log.scrollHeight - 20;
  const row = document.createElement('div');
  row.className = `log-row log-${kind || 'info'}`;
  const t = new Date();
  row.textContent = `${t.toTimeString().slice(0, 8)}  ${text}`;
  log.appendChild(row);
  while (log.children.length > MAX_LOG_LINES) log.removeChild(log.firstChild);
  if (atBottom) log.scrollTop = log.scrollHeight;
}

$('clearLog').addEventListener('click', () => { $('log').innerHTML = ''; });

// ---------------------------------------------------------------------------
// Go
// ---------------------------------------------------------------------------

function frame() {
  drawScope();
  requestAnimationFrame(frame);
}

window.addEventListener('resize', () => { renderDeltas(); });

renderSnippet();
renderDeltas();
connect();
requestAnimationFrame(frame);
