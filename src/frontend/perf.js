// perf.js - Performance tab: start / poll / cancel a background benchmark, show results.
import { get, post, downloadFrom } from './api.js';
import { $, $$, el, clear, fmt, track, setStatus, showError } from './ui.js';
import { drawPlot } from './plot.js';

const POLL_MS = 300;
const TOL = 1e-8;           // serial vs parallel agreement limit [degC]

// name, label, default, min, max (integers)
const FIELDS = [
  ['cases',  'Cases (number of rods)',      3000, 1, 100000],
  ['mesh',   'Mesh (nodes per rod)',        5000, 2, 100000],
  ['seed',   'Random seed',                 42,   0, 2147483647],
  ['reps',   'Repetitions per setting',     5,    1, 20],
  ['warmup', 'Warm-up runs (not timed)',    1,    0, 10],
];

let job = null;        // {id, cancelling}
let sysinfoLoaded = false;

export function init() {
  const grid = $('#bench-fields');
  for (const [name, label, def] of FIELDS) {
    grid.append(el('div', { class: 'field' },
      el('label', { for: `bench-${name}` }, label),
      el('input', { id: `bench-${name}`, type: 'text', inputmode: 'numeric', value: String(def), autocomplete: 'off',
        'aria-describedby': `bench-err-${name}` }),
      el('span', { class: 'field-error', id: `bench-err-${name}`, role: 'alert' })));
  }
  $('#bench-form').addEventListener('submit', (e) => { e.preventDefault(); start(); });
  $('#bench-cancel').addEventListener('click', cancel);
  $('#bench-export').addEventListener('click', exportCsv);
}

export async function onShow() {
  if (sysinfoLoaded) return;
  const s = await track('Read system info', () => get('/api/sysinfo'));
  if (s) { showSysinfo(s); sysinfoLoaded = true; }
}

function showSysinfo(s) {
  const dl = $('#sysinfo');
  clear(dl);
  const rows = [
    ['OS', s.os], ['CPU', s.cpu_model], ['Logical cores', s.logical_cpus], ['Physical cores', s.physical_cores],
    ['RAM', s.ram_gb !== undefined ? `${fmt(s.ram_gb, 3)} GB` : '–'], ['Compiler', s.compiler],
    ['Build type', s.build_type], ['Compiler flags', s.build_flags],
  ];
  for (const [a, b] of rows) dl.append(el('dt', {}, a), el('dd', {}, b === undefined || b === '' ? '–' : String(b)));
}

// -------------------------------------------------------------- validation
function readConfig() {
  const cfg = {};
  let ok = true;
  for (const [name, , , lo, hi] of FIELDS) {
    const raw = $(`#bench-${name}`).value.trim();
    const v = raw === '' ? NaN : Number(raw);
    let msg = '';
    if (!Number.isInteger(v)) msg = 'Enter a whole number.';
    else if (v < lo || v > hi) msg = `Must be between ${lo} and ${hi}.`;
    $(`#bench-${name}`).setAttribute('aria-invalid', msg ? 'true' : 'false');
    $(`#bench-err-${name}`).textContent = msg;
    if (msg) ok = false; else cfg[name] = v;
  }
  if (ok && cfg.cases * cfg.mesh > 2e7) {
    $('#bench-err-mesh').textContent = 'cases × mesh must not exceed 20 000 000.';
    $('#bench-mesh').setAttribute('aria-invalid', 'true');
    ok = false;
  }

  // workers: ticked boxes + custom list
  const set = new Set($$('input[name="w"]:checked').map((c) => Number(c.value)));
  let wmsg = '';
  const custom = $('#bench-custom').value.trim();
  if (custom) {
    for (const tok of custom.split(/[\s,;]+/).filter(Boolean)) {
      const w = Number(tok);
      if (!Number.isInteger(w) || w < 1 || w > 64) { wmsg = `"${tok}" is not a worker count from 1 to 64.`; break; }
      set.add(w);
    }
  }
  if (!wmsg && set.size === 0) wmsg = 'Choose at least one worker count.';
  $('#err-workers').textContent = wmsg;
  if (wmsg) ok = false; else cfg.workers = Array.from(set).sort((a, b) => a - b);
  return ok ? cfg : null;
}

// ---------------------------------------------------------- run / poll
async function start() {
  if (job) return;
  const cfg = readConfig();
  if (!cfg) { setStatus('Fix the highlighted inputs first.'); return; }
  $('#bench-results').hidden = true;
  const r = await track('Start benchmark', () => post('/api/bench/start', cfg));
  if (!r) return;
  job = { id: r.job, cancelling: false };
  setRunning(true);
  progress(0, `Job ${r.job} queued…`);
  poll(0);
}

function setRunning(on) {
  $('#bench-start').disabled = on;
  $('#bench-cancel').disabled = !on;
}

function progress(frac, msg) {
  const pct = Math.round(Math.max(0, Math.min(1, frac)) * 100);
  $('#bench-fill').style.width = pct + '%';
  $('#bench-bar').setAttribute('aria-valuenow', String(pct));
  $('#bench-msg').textContent = `${pct} % — ${msg}`;
}

async function poll(failures) {
  if (!job) return;
  const id = job.id;
  let j;
  try {
    j = await get(`/api/jobs/${id}`);
  } catch (err) {
    if (failures >= 3) {            // give up after repeated failures
      showError(err, 'Benchmark status');
      job = null; setRunning(false); $('#bench-msg').textContent = 'Lost contact with the backend.';
      return;
    }
    setTimeout(() => poll(failures + 1), POLL_MS * 3);
    return;
  }
  if (!job || job.id !== id) return;
  if (j.state === 'running' || j.state === 'queued') {
    progress(j.progress || 0, j.message || 'running');
    setTimeout(() => poll(0), POLL_MS);       // setTimeout (not setInterval) so polls never overlap
    return;
  }
  // finished in some way
  const finished = job;
  job = null;
  setRunning(false);
  if (j.state === 'done' && j.result) {
    progress(1, 'done');
    setStatus(`Benchmark ${id} finished`);
    showResult(j.result, id);
  } else if (j.state === 'cancelled') {
    $('#bench-fill').style.width = '0%';
    $('#bench-msg').textContent = 'Cancelled — no results were written.';
    setStatus('Benchmark cancelled.');
  } else {
    $('#bench-msg').textContent = `Benchmark failed: ${j.error || 'unknown error'}`;
    showError(new Error(j.error || 'The benchmark job failed.'), 'Benchmark');
  }
  void finished;
}

async function cancel() {
  if (!job || job.cancelling) return;
  job.cancelling = true;
  $('#bench-cancel').disabled = true;
  $('#bench-msg').textContent = 'Cancelling…';
  await track('Cancel benchmark', () => post(`/api/jobs/${job.id}/cancel`));
  // the polling loop notices state === 'cancelled' and updates the UI
}

// ------------------------------------------------------------- results
let lastJobId = null;

function showResult(r, id) {
  lastJobId = id;
  $('#bench-results').hidden = false;
  if (r.sysinfo) showSysinfo(r.sysinfo);

  // verification badge
  const diff = r.max_abs_diff;
  const pass = r.verified !== false && Number.isFinite(diff) && diff <= TOL;
  const badge = $('#verify-badge');
  badge.className = `badge ${pass ? 'ok' : 'bad'}`;
  badge.textContent = `${pass ? '✓ Verified' : '✗ NOT verified'}: max |serial − parallel| = ${fmt(diff, 3)} °C ${pass ? '≤' : '>'} 1e-8 °C`;

  const cfg = r.config || {};
  let summary = `Cases ${cfg.cases}, mesh ${cfg.mesh}, seed ${cfg.seed}, ${cfg.reps} reps, ${cfg.warmup} warm-up. ` +
    `Serial baseline median: ${fmt(r.serial_median_s, 4)} s (reps: ${(r.serial_times_s || []).map((t) => fmt(t, 3)).join(', ')}). `;
  const used = r.capped_workers || [];
  if (cfg.workers && used.length && JSON.stringify(used) !== JSON.stringify(cfg.workers)) {
    summary += `Worker counts were capped to the logical CPU count: requested [${cfg.workers.join(', ')}], used [${used.join(', ')}].`;
  }
  $('#bench-summary').textContent = summary;

  const tb = $('#bench-table');
  clear(tb);
  tb.append(el('thead', {}, el('tr', {}, ...['Workers', 'Median [s]', 'Speedup', 'Efficiency', 'All rep times [s]'].map((t) => el('th', {}, t)))),
    el('tbody', {}, ...r.rows.map((w) => el('tr', {},
      el('td', {}, String(w.workers)), el('td', {}, fmt(w.median_s, 4)), el('td', {}, `${fmt(w.speedup, 3)}×`),
      el('td', {}, `${(w.efficiency * 100).toFixed(1)} %`),
      el('td', { class: 'left times' }, (w.times_s || []).map((t) => fmt(t, 3)).join('  ')))))); 

  // speedup chart: measured vs ideal (speedup = workers)
  const wk = r.rows.map((w) => w.workers), sp = r.rows.map((w) => w.speedup);
  const wmax = Math.max(...wk), wmin = Math.min(...wk);
  drawPlot($('#bench-plot'), {
    title: 'Speedup vs worker threads', xlabel: 'workers [threads]', ylabel: 'speedup [× vs serial]',
    series: [
      { name: 'Ideal (speedup = workers)', x: [wmin, wmax], y: [wmin, wmax], color: '#6b7280', dash: '7 5', width: 1.6 },
      { name: 'Measured', x: wk, y: sp, color: '#1f5fa8', markers: true },
    ],
    ariaLabel: 'Measured speedup against ideal linear speedup',
  });
}

async function exportCsv() {
  if (!lastJobId) return;
  await track('Export benchmark CSV', () => downloadFrom('GET', `/api/jobs/${lastJobId}?format=csv`, undefined, 'bench.csv'));
}
