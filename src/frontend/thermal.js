// thermal.js - Thermal Analysis tab: 1-D FE solve, error table, convergence study, CSV export.
import { post, downloadFrom } from './api.js';
import { $, el, clear, fmt, track, setStatus } from './ui.js';
import { drawPlot } from './plot.js';

// name, label, unit, default, rule ('N' = integer 1..1e6, 'pos' = positive finite, 'fin' = finite)
const FIELDS = [
  ['N',  'N — number of elements', '–',        10,    'N'],
  ['L',  'L — rod length',          'm',        1,     'pos'],
  ['A',  'A — cross-section',       'm²',       1e-4,  'pos'],
  ['k',  'k — conductivity',        'W/(m·K)',  10,    'pos'],
  ['q',  'q — heat source',         'W/m³',     10000, 'fin'],
  ['T0', 'T0 — temperature at x = 0', '°C',     100,   'fin'],
  ['TL', 'TL — temperature at x = L', '°C',     20,    'fin'],
];
const NS = [10, 20, 40, 80];
const MAX_DRAWN = 1500;   // never draw more than this many FE points

export function init() {
  const grid = $('#fea-fields');
  for (const [name, label, unit, def] of FIELDS) {
    grid.append(el('div', { class: 'field' },
      el('label', { for: `fea-${name}` }, label, unit !== '–' ? el('span', { class: 'unit' }, ` [${unit}]`) : null),
      el('input', { id: `fea-${name}`, type: 'text', inputmode: 'decimal', autocomplete: 'off', value: String(def),
        'aria-describedby': `fea-err-${name}` }),
      el('span', { class: 'field-error', id: `fea-err-${name}`, role: 'alert' })));
  }
  for (const [name] of FIELDS) $(`#fea-${name}`).addEventListener('input', () => validate());

  $('#fea-form').addEventListener('submit', (e) => { e.preventDefault(); solve(); });
  $('#fea-conv').addEventListener('click', convergence);
  $('#fea-export').addEventListener('click', () => exportCsv(false));
  $('#fea-conv-export').addEventListener('click', () => exportCsv(true));
}

/** Check every field; show inline messages; return the parameter object or null. */
function validate(showAll = true) {
  const p = {};
  let ok = true;
  for (const [name, label, , , rule] of FIELDS) {
    const input = $(`#fea-${name}`);
    const raw = input.value.trim();
    const v = raw === '' ? NaN : Number(raw);
    let msg = '';
    if (raw === '' || !Number.isFinite(v)) msg = 'Enter a finite number.';
    else if (rule === 'N' && (!Number.isInteger(v) || v < 1 || v > 1e6)) msg = 'N must be an integer from 1 to 1 000 000.';
    else if (rule === 'pos' && !(v > 0)) msg = 'Must be greater than zero.';
    input.setAttribute('aria-invalid', msg ? 'true' : 'false');
    $(`#fea-err-${name}`).textContent = msg;
    if (msg) ok = false; else p[name] = v;
  }
  return ok ? p : null;
}

// ---------------------------------------------------------------- solve
async function solve() {
  const p = validate();
  if (!p) { setStatus('Fix the highlighted inputs first.'); $('[aria-invalid="true"]', $('#fea-form'))?.focus(); return; }
  const btn = $('#fea-solve');
  btn.disabled = true;
  try {
    const d = await track(`Solve N=${p.N}`, () => post('/api/fea/solve', p));
    if (d) showSolution(d);
  } finally { btn.disabled = false; }
}

function showSolution(d) {
  const P = d.params;
  const exact = (x) => P.T0 + (P.TL - P.T0) * x / P.L + P.q / (2 * P.k) * x * (P.L - x);

  // smooth exact curve: sample the closed form (drawing only), 201 points
  const ex = [], ey = [];
  for (let i = 0; i <= 200; i++) { const x = P.L * i / 200; ex.push(x); ey.push(exact(x)); }

  // FE nodes (decimated if the mesh is huge so the SVG stays light)
  const stride = Math.max(1, Math.ceil(d.x.length / MAX_DRAWN));
  const fx = [], fy = [];
  for (let i = 0; i < d.x.length; i += stride) { fx.push(d.x[i]); fy.push(d.T[i]); }
  if (fx[fx.length - 1] !== d.x[d.x.length - 1]) { fx.push(d.x[d.x.length - 1]); fy.push(d.T[d.T.length - 1]); }
  const small = d.x.length <= 200;

  const series = [
    { name: 'Exact T(x)', x: ex, y: ey, color: '#c2410c', width: 2.2 },
    { name: `FE nodes (N=${P.N})`, x: fx, y: fy, color: '#1f5fa8', line: true, markers: small, width: 1.8 },
  ];
  if (small) series.push({ name: 'FE at midpoints', x: d.x_mid, y: d.T_mid_fe, color: '#4b5563', line: false, markers: true, hollow: true, radius: 3.5 });
  drawPlot($('#fea-plot'), {
    title: `T(x), N = ${P.N}, h = ${fmt(d.h, 4)} m`, xlabel: 'x [m]', ylabel: 'T [°C]',
    ariaLabel: 'Temperature along the rod, finite-element solution versus exact solution', series,
  });
  $('#fea-note').textContent = stride > 1 ? `Plot decimated: every ${stride}th node drawn (errors below use all ${d.x.length} nodes).` : '';

  // error table, with the analytic expectation for the midpoint error: q h^2 / (8 k)
  const theory = P.q * d.h * d.h / (8 * P.k);
  const tb = $('#fea-errors');
  clear(tb);
  tb.append(el('thead', {}, el('tr', {}, el('th', {}, 'Quantity'), el('th', {}, 'Value'), el('th', {}, 'Unit'))),
    el('tbody', {},
      row('Element size h', fmt(d.h, 5), 'm'),
      row('Max nodal error  max |T_FE − T_exact|', fmt(d.max_nodal_err, 4), '°C'),
      row('Max midpoint error  max |T_FE,mid − T_exact,mid|', fmt(d.max_mid_err, 4), '°C'),
      row('Expected midpoint error  q h² / (8 k)', fmt(theory, 4), '°C')));
}
const row = (a, b, c) => el('tr', {}, el('td', {}, a), el('td', {}, b), el('td', { class: 'left' }, c));

// ---------------------------------------------------------- convergence
async function convergence() {
  const p = validate();
  if (!p) { setStatus('Fix the highlighted inputs first.'); return; }
  const btn = $('#fea-conv');
  btn.disabled = true;
  try {
    const { N, ...rest } = p;              // N is replaced by the list of meshes
    const d = await track('Convergence run', () => post('/api/fea/convergence', { Ns: NS, ...rest }));
    if (d) showConvergence(d);
  } finally { btn.disabled = false; }
}

function showConvergence(d) {
  const pts = d.points.slice().sort((a, b) => b.h - a.h);   // coarse -> fine
  const h = pts.map((q) => q.h), e = pts.map((q) => q.max_mid_err);
  const valid = pts.filter((q) => q.max_mid_err > 0);
  const series = [{ name: 'Max midpoint error', x: h, y: e, color: '#1f5fa8', markers: true }];
  const notes = [];
  if (valid.length >= 2) {
    // reference line with slope 2, anchored below the finest point so it does not hide the data
    const last = valid[valid.length - 1];
    const hr = [valid[0].h, last.h];
    series.push({ name: 'Reference slope 2 (shifted down)', x: hr, y: hr.map((x) => 0.4 * last.max_mid_err * Math.pow(x / last.h, 2)),
      color: '#6b7280', dash: '7 5', width: 1.6, line: true });
    const mid = valid[Math.min(1, valid.length - 1)];
    notes.push({ x: mid.h, y: mid.max_mid_err, dx: -10, dy: -12, anchor: 'end', text: `fitted slope = ${fmt(d.slope, 3)}` });
  }
  drawPlot($('#conv-plot'), {
    title: 'Convergence of the midpoint error (log–log)', xlabel: 'element size h [m]',
    ylabel: 'max midpoint error [°C]', xlog: true, ylog: true, series, notes,
    ariaLabel: 'Log-log plot of maximum midpoint error against element size',
  });

  const tb = $('#conv-table');
  tb.hidden = false;
  clear(tb);
  const rows = pts.map((q, i) => {
    const prev = pts[i - 1];
    const order = prev && q.max_mid_err > 0 && prev.max_mid_err > 0
      ? Math.log(prev.max_mid_err / q.max_mid_err) / Math.log(prev.h / q.h) : null;
    return el('tr', {}, el('td', {}, String(q.N)), el('td', {}, fmt(q.h, 4)),
      el('td', {}, fmt(q.max_mid_err, 4)), el('td', {}, fmt(q.max_nodal_err, 3)), el('td', {}, order === null ? '–' : order.toFixed(3)));
  });
  tb.append(el('thead', {}, el('tr', {}, ...['N', 'h [m]', 'max midpoint error [°C]', 'max nodal error [°C]', 'observed order p'].map((t) => el('th', {}, t)))),
    el('tbody', {}, ...rows),
    el('tfoot', {}, el('tr', {}, el('td', { colspan: 5, class: 'left' }, `Least-squares slope of log(error) vs log(h): ${fmt(d.slope, 4)} (theory: 2).`))));
}

// --------------------------------------------------------------- export
async function exportCsv(conv) {
  const p = validate();
  if (!p) { setStatus('Fix the highlighted inputs first.'); return; }
  await track(conv ? 'Export convergence CSV' : 'Export solution CSV', async () => {
    if (conv) {
      const { N, ...rest } = p;
      await downloadFrom('POST', '/api/fea/convergence?format=csv', { Ns: NS, ...rest }, 'convergence.csv');
    } else {
      await downloadFrom('POST', '/api/fea/solve?format=csv', p, `fea_N${p.N}.csv`);
    }
  });
}
