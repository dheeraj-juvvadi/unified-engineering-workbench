// geometry.js - Geometry tab: loaders (STL / CSV / samples), three.js viewer, legend, node inspection.
import { get, post, ApiError } from './api.js';
import { $, el, clear, fmt, track, showError, setStatus } from './ui.js';
import { Viewer } from './viewer.js';
import { colormapNames, cssGradient } from './colormap.js';

const MAX_BYTES = 200 * 1024 * 1024;   // same limit as the server (API.md)
let viewer = null;
let csv = null;      // last CSV response (kept for node inspection)

export function init() {
  viewer = new Viewer($('#viewer'));
  viewer.onPick = showPick;

  // colour-map choices
  const sel = $('#cmap-select');
  for (const n of colormapNames) sel.append(el('option', { value: n }, n));
  sel.addEventListener('change', () => { viewer.recolor(sel.value); updateLegend(); });

  $('#stl-file').addEventListener('change', (e) => openFile(e, 'stl'));
  $('#csv-file').addEventListener('change', (e) => openFile(e, 'csv'));
  $('#sample-load').addEventListener('click', loadSample);
  $('#fit-btn').addEventListener('click', () => viewer.fit());
  $('#clear-btn').addEventListener('click', clearScene);
  $('#wire-toggle').addEventListener('change', (e) => viewer.setWireframe(e.target.checked));
  $('#show-mesh').addEventListener('change', (e) => { viewer.setMeshVisible(e.target.checked); });
  $('#show-points').addEventListener('change', (e) => {
    viewer.setPointsVisible(e.target.checked);
    $('#legend').hidden = !(csv && e.target.checked);
  });
  for (const id of ['#show-mesh', '#show-points']) $(id).addEventListener('change', () => viewer.fit());
  loadSampleList();
  window.__viewer = viewer;   // handy for manual debugging / automated tests
}

export function onShow() { if (viewer) viewer.resize(); }

// ------------------------------------------------------------ sample list
async function loadSampleList() {
  const sel = $('#sample-select');
  const data = await track('Load sample list', () => get('/api/samples'));
  clear(sel);
  if (!data || !data.files || data.files.length === 0) {
    sel.append(el('option', { value: '' }, '(no samples available)'));
    return;
  }
  // the server tags each file with group "good" or "bad" (deliberately broken demo files)
  const groups = { good: el('optgroup', { label: 'Sample files' }), bad: el('optgroup', { label: 'Broken files (error demos)' }) };
  for (const f of data.files) {
    (groups[f.group] || groups.good).append(el('option', { value: f.name, 'data-kind': f.kind }, `${f.name}  [${f.kind}]`));
  }
  for (const g of Object.values(groups)) if (g.children.length) sel.append(g);
}

async function loadSample() {
  const opt = $('#sample-select').selectedOptions[0];
  if (!opt || !opt.value) return;
  const name = opt.value, kind = opt.dataset.kind || (name.endsWith('.csv') ? 'csv' : 'stl');
  const btn = $('#sample-load');
  btn.disabled = true;
  try {
    await track(`Load sample ${name}`, async () => {
      // keep "/" in sub-folder names (bad/xxx.stl) but encode the other characters
      const path = '/api/samples/' + name.split('/').map(encodeURIComponent).join('/');
      const res = await fetch(path);
      if (!res.ok) {
        let d = null; try { d = await res.json(); } catch (e) { /* ignore */ }
        throw new ApiError((d && d.error) || `HTTP ${res.status}`, { code: d && d.code, hint: d && d.hint, line: d && d.line });
      }
      await upload(await res.text(), kind, name);
    });
  } finally { btn.disabled = false; }
}

// ---------------------------------------------------------- file loading
async function openFile(ev, kind) {
  const input = ev.target;
  const file = input.files[0];
  input.value = '';              // allows re-selecting the same file later
  if (!file) return;
  if (file.size > MAX_BYTES) {
    showError(new ApiError(`"${file.name}" is ${(file.size / 1048576).toFixed(0)} MB, which is over the 200 MB limit. Nothing was uploaded.`,
      { code: 'BAD_PARAM', hint: 'Choose a smaller file or decimate the mesh / result set first.' }), 'Open file');
    return;
  }
  await track(`Open ${file.name}`, async () => {
    const text = await file.text();      // read in the browser; parsing happens in C++
    await upload(text, kind, file.name);
  });
}

/** POST raw text to the parser, then display the answer. Errors propagate to track(). */
async function upload(text, kind, name) {
  $('#geo-error').hidden = true;
  try {
    if (kind === 'stl') showStl(await post('/api/geometry/stl', text), name);
    else showCsv(await post('/api/results/csv', text), name);
  } catch (err) {
    // also keep the message visible in the Dataset card (the toast disappears after a while)
    if (err instanceof ApiError) {
      const e = $('#geo-error');
      e.hidden = false;
      e.textContent = `${name}: ${err.message}` + (err.line > 0 && !/^\s*Line\s+\d+/i.test(err.message) ? ` (line ${err.line})` : '') + (err.hint ? ` — ${err.hint}` : '');
    }
    throw err;
  }
}

// ----------------------------------------------------------- display STL
function showStl(d, name) {
  viewer.showMesh(d.positions, d.normals);
  viewer.setWireframe($('#wire-toggle').checked);
  // show one dataset at a time by default (STL is usually in mm, results in m);
  // the checkboxes in the View card bring the other one back.
  $('#show-mesh').checked = true;
  $('#show-points').checked = false;
  viewer.setMeshVisible(true);
  viewer.setPointsVisible(false);
  $('#legend').hidden = true;
  viewer.fit();
  info([
    ['File', d.name || name], ['Type', 'STL surface mesh'],
    ['Triangles', d.triangle_count.toLocaleString()],
    ['BBox min [m]', vec(d.bbox_min)], ['BBox max [m]', vec(d.bbox_max)],
    ['Size [m]', vec(d.bbox_max.map((v, i) => v - d.bbox_min[i]))],
    ['Parse time', `${fmt(d.parse_ms, 3)} ms (C++)`],
  ], 'stl');
}

// ----------------------------------------------------------- display CSV
function showCsv(d, name) {
  csv = d;
  const cmap = $('#cmap-select').value;
  viewer.showPoints(d.nodes, d.t_min, d.t_max, d.bbox_min, d.bbox_max, cmap);
  $('#show-points').checked = true;
  $('#show-mesh').checked = false;
  viewer.setPointsVisible(true);
  viewer.setMeshVisible(false);
  viewer.fit();
  updateLegend();
  showPick(null);
  info([
    ['File', name], ['Type', 'Results point cloud'],
    ['Nodes', d.count.toLocaleString()],
    ['T range', `${fmt(d.t_min)} … ${fmt(d.t_max)} °C`],
    ['BBox min [m]', vec(d.bbox_min)], ['BBox max [m]', vec(d.bbox_max)],
    ['Parse time', `${fmt(d.parse_ms, 3)} ms (C++)`],
  ], 'csv');
}

const vec = (a) => a.map((v) => fmt(v, 4)).join(', ');

/** Dataset card: STL and CSV can both be loaded, keep one block per kind. */
const infoRows = { stl: null, csv: null };
function info(rows, kind) {
  infoRows[kind] = rows;
  const dl = $('#dataset-info');
  clear(dl);
  for (const k of ['stl', 'csv']) {
    if (!infoRows[k]) continue;
    dl.append(el('dt', { class: 'kv-head' }, k === 'stl' ? 'STL mesh' : 'Results CSV'));
    for (const [a, b] of infoRows[k]) dl.append(el('dt', {}, a), el('dd', {}, b));
  }
}

function clearScene() {
  viewer.clearMesh(); viewer.clearPoints();
  csv = null; infoRows.stl = infoRows.csv = null;
  clear($('#dataset-info')); $('#dataset-info').append(el('dt', {}, 'Status'), el('dd', {}, 'Nothing loaded.'));
  $('#legend').hidden = true;
  $('#geo-error').hidden = true;
  showPick(null);
  setStatus('Scene cleared.');
}

// --------------------------------------------------------------- legend
function updateLegend() {
  if (!csv) return;
  $('#legend').hidden = !$('#show-points').checked;
  $('#legend-bar').style.background = cssGradient($('#cmap-select').value);
  $('#leg-max').textContent = `${fmt(csv.t_max, 4)} °C`;
  $('#leg-mid').textContent = `${fmt((csv.t_min + csv.t_max) / 2, 4)} °C`;
  $('#leg-min').textContent = `${fmt(csv.t_min, 4)} °C`;
}

// ----------------------------------------------------------- inspection
function showPick(i) {
  const dl = $('#pick-info');
  clear(dl);
  viewer.highlight(i);
  if (i === null || !csv) {
    dl.append(el('dt', {}, 'Status'), el('dd', {}, csv ? 'Nothing selected (click on a node).' : 'Load a results CSV, then click a node.'));
    return;
  }
  const n = csv.nodes;
  dl.append(
    el('dt', {}, 'node_id'), el('dd', {}, String(n.id[i])),
    el('dt', {}, 'x [m]'), el('dd', {}, fmt(n.x[i], 6)),
    el('dt', {}, 'y [m]'), el('dd', {}, fmt(n.y[i], 6)),
    el('dt', {}, 'z [m]'), el('dd', {}, fmt(n.z[i], 6)),
    el('dt', {}, 'T [°C]'), el('dd', {}, fmt(n.t[i], 6)));
}
