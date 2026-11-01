// backprop.js - Backprop tab: forward values, analytic vs finite-difference gradients, loss curve.
import { post } from './api.js';
import { $, el, clear, fmt, track } from './ui.js';
import { drawPlot } from './plot.js';

const TOL = 1e-5;   // pass/fail limit for |analytic - FD|

export function init() {
  $('#bp-fields').append(el('div', { class: 'field' },
    el('label', { for: 'bp-steps' }, 'Further gradient-descent steps'),
    el('input', { id: 'bp-steps', type: 'text', inputmode: 'numeric', value: '100', autocomplete: 'off', 'aria-describedby': 'bp-err-steps' }),
    el('span', { class: 'field-error', id: 'bp-err-steps', role: 'alert' })));
  $('#bp-form').addEventListener('submit', (e) => { e.preventDefault(); run(); });
}

async function run() {
  const raw = $('#bp-steps').value.trim();
  const steps = Number(raw);
  const bad = raw === '' || !Number.isInteger(steps) || steps < 0 || steps > 10000;
  $('#bp-steps').setAttribute('aria-invalid', String(bad));
  $('#bp-err-steps').textContent = bad ? 'Enter a whole number from 0 to 10 000.' : '';
  if (bad) return;

  const btn = $('#bp-run');
  btn.disabled = true;
  try {
    const d = await track('Run backprop', () => post('/api/backprop/run', { steps }));
    if (d) show(d);
  } finally { btn.disabled = false; }
}

const kv = (rows) => el('dl', { class: 'kv' }, rows.map(([a, b]) => [el('dt', {}, a), el('dd', {}, b)]));

function show(d) {
  const out = $('#bp-out');
  clear(out);

  // 1. forward pass
  const f = d.forward;
  out.append(el('div', { class: 'card' }, el('h2', {}, 'Forward pass'),
    el('p', { class: 'hint' }, `Input x = ${d.x}, target y = ${d.y}, learning rate = ${d.lr}, FD step ε = ${fmt(d.eps, 3)}. Initial parameters: ` +
      Object.entries(d.p0).map(([k, v]) => `${k} = ${fmt(v, 5)}`).join(', ') + '.'),
    kv([['z = w x + b', fmt(f.z, 6)], ['h = tanh(z)', fmt(f.h, 6)], ['ŷ = v h + c', fmt(f.yhat, 6)],
        ['e = ŷ − y', fmt(f.e, 6)], ['J = ½ e²  (dimensionless)', fmt(f.J, 6)]])));

  // 2. gradient check
  const names = ['w', 'b', 'v', 'c'];
  let allPass = true;
  const rows = names.map((n) => {
    const a = d.grads['d' + n], g = d.grads_fd['d' + n];
    const err = Math.abs(a - g), pass = err <= TOL;
    if (!pass) allPass = false;
    return el('tr', {}, el('td', {}, `∂J/∂${n}`), el('td', {}, fmt(a, 8)), el('td', {}, fmt(g, 8)), el('td', {}, fmt(err, 3)),
      el('td', { class: pass ? 'pass' : 'fail' }, pass ? '✓ pass' : '✗ FAIL'));
  });
  out.append(el('div', { class: 'card' },
    el('div', { class: 'card-head' }, el('h2', {}, 'Gradient check: analytic vs finite difference'),
      el('span', { class: `badge ${allPass ? 'ok' : 'bad'}` }, `${allPass ? 'All pass' : 'Some FAIL'} (|error| ≤ 1e-5), max = ${fmt(d.max_fd_err, 3)}`)),
    el('table', { class: 'data' },
      el('thead', {}, el('tr', {}, ...['Gradient', 'Analytic', 'Finite difference', '|abs error|', 'Result'].map((t) => el('th', {}, t)))),
      el('tbody', {}, ...rows))));

  // 3. updated parameters
  out.append(el('div', { class: 'card' }, el('h2', {}, 'Parameters after one update'),
    el('table', { class: 'data' },
      el('thead', {}, el('tr', {}, ...['Parameter', 'Before', 'After'].map((t) => el('th', {}, t)))),
      el('tbody', {}, ...names.map((n) => el('tr', {}, el('td', {}, n), el('td', {}, fmt(d.p0[n], 7)), el('td', {}, fmt(d.p1[n], 7)))))),
    el('p', { class: 'hint' }, `Loss after the update: J₁ = ${fmt(d.J1, 6)} (before: ${fmt(f.J, 6)}).`)));

  // 4. loss curve: step 0 is the initial loss, loss[0] is after the first update
  const plotBox = el('div', { class: 'plot-box' });
  out.append(el('div', { class: 'card' }, el('h2', {}, 'Loss curve'), plotBox));
  const xs = [0, ...d.loss.map((_, i) => i + 1)];
  const ys = [f.J, ...d.loss];
  drawPlot(plotBox, {
    title: 'Training loss', xlabel: 'step (gradient-descent updates)', ylabel: 'J (dimensionless)',
    series: [{ name: 'J', x: xs, y: ys, color: '#1f5fa8', markers: xs.length <= 40, radius: 3 }],
    ariaLabel: 'Loss J against gradient-descent step',
  });
}
