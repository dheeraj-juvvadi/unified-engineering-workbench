// hardware.js - Hardware tab: simulated MCU / FPGA / JTAG devices (nothing real is touched).
import { get, post, ApiError } from './api.js';
import { $, el, clear, fmt, track, showError } from './ui.js';

const devices = new Map();   // id -> latest device object from the server
let logTimer = null;

export function init() {
  $('#hw-discover').addEventListener('click', discover);
  $('#hw-log-refresh').addEventListener('click', refreshLog);
}

export function onShow(first) {
  if (first) discover(); else refreshLog(true);
  clearInterval(logTimer);
  logTimer = setInterval(() => refreshLog(true), 3000);   // keep the log fresh while this tab is open
}
export function onHide() { clearInterval(logTimer); logTimer = null; }

// -------------------------------------------------------------- discover
async function discover() {
  const btn = $('#hw-discover');
  btn.disabled = true;
  try {
    const d = await track('Discover devices', () => get('/api/hw/discover'));
    if (!d) return;
    const host = $('#hw-cards');
    clear(host);
    devices.clear();
    for (const dev of d.devices) { devices.set(dev.id, dev); host.append(buildCard(dev)); }
    if (d.devices.length === 0) host.append(el('p', { class: 'muted' }, 'No devices found.'));
    await refreshLog(true);
  } finally { btn.disabled = false; }
}

// ------------------------------------------------------------------ card
const cardEl = (id) => document.getElementById(`dev-${id}`);

function buildCard(dev) {
  const id = dev.id;
  const card = el('article', { class: 'card dev', id: `dev-${id}`, 'aria-label': `${dev.kind} device ${dev.name}` },
    el('div', { class: 'dev-head' },
      el('span', { class: 'dev-kind' }, dev.kind),
      el('h2', {}, dev.name),
      el('span', { class: 'badge off', 'data-role': 'state' })),
    el('dl', { class: 'kv', 'data-role': 'meta' }),
    el('ul', { class: 'caps', 'data-role': 'caps' }),
    dev.kind === 'JTAG' ? el('p', { class: 'dev-note' }, 'Debug interface — not a compute board.') : null,
    el('div', { class: 'btn-row dev-actions' },
      el('button', { type: 'button', class: 'btn', 'data-act': 'connect', onclick: () => act(id, 'connect') }, 'Connect'),
      el('button', { type: 'button', class: 'btn', 'data-act': 'disconnect', onclick: () => act(id, 'disconnect') }, 'Disconnect'),
      el('button', { type: 'button', class: 'btn', 'data-act': 'telemetry', onclick: () => readTelemetry(id) }, 'Read telemetry'),
      el('button', { type: 'button', class: 'btn btn-quiet', 'data-act': 'inject', onclick: () => inject(id) }, 'Inject timeout')),
    el('div', { class: 'telemetry', 'data-role': 'out', 'aria-live': 'polite' }));
  paint(card, dev);
  return card;
}

/** Refresh the static part of a card from a device object. */
function paint(card, dev) {
  const state = $('[data-role="state"]', card);
  state.textContent = dev.state;
  state.className = `badge ${dev.state === 'Connected' ? 'ok' : dev.state === 'Fault' ? 'bad' : 'off'}`;

  const meta = $('[data-role="meta"]', card);
  clear(meta);
  meta.append(el('dt', {}, 'Kind'), el('dd', {}, dev.kind),
    el('dt', {}, 'Port'), el('dd', {}, dev.port || '–'),
    el('dt', {}, 'Transport'), el('dd', {}, dev.transport || '–'));

  const caps = $('[data-role="caps"]', card);
  clear(caps);
  for (const [k, v] of Object.entries(dev.capabilities || {})) caps.append(el('li', {}, `${k}: ${v}`));

  $('[data-act="connect"]', card).disabled = dev.state === 'Connected';
  $('[data-act="disconnect"]', card).disabled = dev.state === 'Disconnected';
}

function out(id) { const c = cardEl(id); return c ? $('[data-role="out"]', c) : null; }

function message(id, text, kind = 'info') {
  const o = out(id); if (!o) return;
  clear(o);
  o.append(el('p', { class: kind === 'info' ? 'hint' : 'inline-error' }, text));
}

// --------------------------------------------------------------- actions
/** connect / disconnect. Returns the parsed reply (or undefined on failure). */
async function act(id, what) {
  const r = await track(`${what} ${id}`, async () => {
    try { return await post(`/api/hw/${encodeURIComponent(id)}/${what}`); }
    catch (err) {
      // hardware errors may come back as 4xx with {ok:false,message,hint}: show inline instead of just a toast
      if (err instanceof ApiError && err.body && err.body.ok === false) return err.body;
      throw err;
    }
  });
  if (!r) return undefined;
  if (r.device) { devices.set(id, r.device); paint(cardEl(id), r.device); }
  if (r.ok === false) warn(id, r);
  else message(id, r.message || `${what} OK`);
  await refreshLog(true);
  return r;
}

function warn(id, r, withReconnect = false) {
  const o = out(id); if (!o) return;
  clear(o);
  const box = el('div', { class: 'warning-box', role: 'alert' },
    el('b', {}, `${r.code || 'ERROR'}${r.recoverable ? ' (recoverable)' : ''}`),
    el('span', {}, r.message || 'The device reported a problem.'),
    r.hint ? el('div', {}, el('b', { style: 'display:inline' }, 'Hint: '), r.hint) : null);
  if (withReconnect || r.recoverable) {
    box.append(el('div', { class: 'btn-row' },
      el('button', { type: 'button', class: 'btn btn-primary', onclick: () => reconnect(id) }, 'Reconnect')));
  }
  o.append(box);
}

async function readTelemetry(id) {
  const r = await track(`Read telemetry ${id}`, async () => {
    try { return await get(`/api/hw/${encodeURIComponent(id)}/telemetry`); }
    catch (err) {
      if (err instanceof ApiError && err.body && err.body.ok === false) return err.body;
      throw err;
    }
  });
  if (!r) return;
  if (r.ok && r.telemetry) {
    const t = r.telemetry, o = out(id);
    clear(o);
    const rows = Object.entries(t.values || {}).map(([k, v]) =>
      el('tr', {}, el('td', {}, k), el('td', {}, fmt(v, 5)), el('td', { class: 'left' }, (t.units && t.units[k]) || '')));
    o.append(
      el('table', { class: 'data' },
        el('thead', {}, el('tr', {}, el('th', {}, 'Channel'), el('th', {}, 'Value'), el('th', {}, 'Unit'))),
        el('tbody', {}, ...rows)),
      el('p', { class: 'hint' }, `sample #${t.seq} at t = ${fmt(t.t_s, 4)} s (simulated)`));
  } else {
    warn(id, r, r.code === 'TIMEOUT');   // timeouts always offer a Reconnect button
  }
  await refreshLog(true);
}

async function inject(id) {
  const r = await track(`Inject timeout ${id}`, () => post(`/api/hw/${encodeURIComponent(id)}/inject-timeout`));
  if (r) message(id, r.message || 'Timeout injected: the next telemetry read will fail.');
  await refreshLog(true);
}

/** Recover from a fault: disconnect, then connect again. */
async function reconnect(id) {
  await act(id, 'disconnect');      // may report an error if the device is in Fault state: carry on and connect anyway
  const b = await act(id, 'connect');
  if (b && b.ok !== false) message(id, 'Reconnected. You can read telemetry again.');
}

// -------------------------------------------------------------------- log
async function refreshLog(quiet = false) {
  try {
    const d = await get('/api/hw/log');
    const pre = $('#hw-log');
    const atBottom = pre.scrollTop + pre.clientHeight >= pre.scrollHeight - 8;
    pre.textContent = d.lines && d.lines.length ? d.lines.join('\n') : '(empty)';
    if (atBottom) pre.scrollTop = pre.scrollHeight;
  } catch (err) {
    if (!quiet) showError(err, 'Read hardware log');
  }
}
