// app.js - entry point: tabs, backend connection pill, heartbeat, quit button.
import { get, post } from './api.js';
import { $, $$, setStatus, showError, track } from './ui.js';
import * as geometry from './geometry.js';
import * as thermal from './thermal.js';
import * as perf from './perf.js';
import * as hardware from './hardware.js';
import * as backprop from './backprop.js';

// shared state other modules can read (filled from /api/health)
export const shared = { health: null };

const panels = { geometry, thermal, performance: perf, hardware, backprop };
const visited = new Set();

// ------------------------------------------------------------------ tabs
function selectTab(name, focus = false) {
  for (const tab of $$('[role="tab"]')) {
    const on = tab.id === `tab-${name}`;
    tab.setAttribute('aria-selected', String(on));
    tab.tabIndex = on ? 0 : -1;
    if (on && focus) tab.focus();
  }
  for (const p of $$('[role="tabpanel"]')) p.hidden = p.id !== `panel-${name}`;
  try { history.replaceState(null, '', `#${name}`); } catch (e) { /* file:// etc. */ }
  const mod = panels[name];
  if (mod.onShow) mod.onShow(!visited.has(name));
  visited.add(name);
  for (const [n, m] of Object.entries(panels)) if (n !== name && m.onHide) m.onHide();
}

function setupTabs() {
  const tabs = $$('[role="tab"]');
  tabs.forEach((tab, i) => {
    tab.addEventListener('click', () => selectTab(tab.id.slice(4)));
    // keyboard: arrow keys / Home / End move between tabs (WAI-ARIA tabs pattern)
    tab.addEventListener('keydown', (e) => {
      let j = null;
      if (e.key === 'ArrowRight') j = (i + 1) % tabs.length;
      else if (e.key === 'ArrowLeft') j = (i - 1 + tabs.length) % tabs.length;
      else if (e.key === 'Home') j = 0;
      else if (e.key === 'End') j = tabs.length - 1;
      if (j !== null) { e.preventDefault(); selectTab(tabs[j].id.slice(4), true); }
    });
  });
}

// ------------------------------------------------- connection pill / health
let connected = null;
function setConnected(ok, text) {
  if (connected === ok && !text) return;
  connected = ok;
  const pill = $('#conn-pill');
  pill.className = `pill ${ok ? 'pill-on' : 'pill-off'}`;
  $('#conn-text').textContent = ok ? (text || 'Connected') : 'Disconnected';
}

async function checkHealth() {
  try {
    const h = await get('/api/health');
    shared.health = h;
    setConnected(true, `Connected · v${h.version}`);
    const ms = $('#launch-ms');
    if (ms && h.launch_to_ready_ms !== undefined) ms.textContent = `${Number(h.launch_to_ready_ms).toFixed(1)} ms`;
  } catch (e) {
    setConnected(false);
  }
}

let timers = [];
function startTimers() {
  // heartbeat keeps the server alive (it exits ~180 s after the last one; generous because
  // browsers throttle timers in background tabs to about once per minute)
  const beat = async () => { try { await post('/api/heartbeat'); } catch (e) { setConnected(false); } };
  beat();
  timers.push(setInterval(beat, 5000));
  document.addEventListener('visibilitychange', () => { if (!document.hidden) beat(); });
  timers.push(setInterval(checkHealth, 5000));
}

// -------------------------------------------------------------------- quit
function setupQuit() {
  $('#quit-btn').addEventListener('click', async () => {
    await track('Shutting down', async () => {
      await post('/api/shutdown');
      timers.forEach(clearInterval);
      timers = [];
      setConnected(false);
      $('#closed-overlay').hidden = false;
      setStatus('Backend stopped. You can close this tab.');
    });
  });
}

// -------------------------------------------------------------------- boot
async function boot() {
  setupTabs();
  setupQuit();
  for (const m of Object.values(panels)) m.init({ shared });
  await checkHealth();
  startTimers();
  const start = (location.hash || '#geometry').slice(1);
  selectTab(panels[start] ? start : 'geometry');
  if (new URLSearchParams(location.search).has('demo')) runDemo(start);
}

// ---------------------------------------------------------------- demo mode
// "?demo=1#<tab>" clicks the main buttons of that tab, so a screenshot
// walkthrough can be recorded on a machine with nobody at the keyboard.
const DEMO_STEPS = {
  geometry: [['#sample-select', 'heatsink.stl'], '#sample-load'],
  thermal: ['#fea-solve', '#fea-conv'],
  performance: ['#bench-start'],
  hardware: ['#hw-discover', '[data-act="connect"]', '[data-act="telemetry"]'],
  backprop: ['#bp-run'],
};
async function runDemo(tab) {
  const wait = (ms) => new Promise((r) => setTimeout(r, ms));
  for (const step of DEMO_STEPS[tab] || []) {
    await wait(1500);
    if (Array.isArray(step)) {          // [select, value]: choose a sample file
      const sel = document.querySelector(step[0]);
      const opt = sel && [...sel.options].find((o) => o.value.endsWith(step[1]) || o.text.includes(step[1]));
      if (opt) { sel.value = opt.value; sel.dispatchEvent(new Event('change')); }
      continue;
    }
    const btn = document.querySelector(step);
    if (btn && !btn.disabled) btn.click();
  }
}

window.addEventListener('error', (e) => showError(new Error(e.message), 'Unexpected script error'));
window.addEventListener('unhandledrejection', (e) => showError(e.reason || new Error('Unhandled promise rejection'), 'Unexpected error'));
boot();
