// plot.js - small hand-written SVG line/scatter plot helper (no libraries).
// Supports linear and log axes, ticks, gridlines, axis labels with units, legend,
// markers, dashed lines and text annotations.
import { clear } from './ui.js';

const NS = 'http://www.w3.org/2000/svg';
const W = 760, H = 430;
const M = { l: 78, r: 24, t: 52, b: 58 };

function svg(tag, attrs = {}, text) {
  const n = document.createElementNS(NS, tag);
  for (const [k, v] of Object.entries(attrs)) n.setAttribute(k, v);
  if (text !== undefined) n.textContent = text;
  return n;
}

// ------------------------------------------------------------- tick helpers
/** "Nice" linear ticks covering [lo, hi]. Returns {ticks, lo, hi} with lo/hi rounded outward. */
export function niceTicks(lo, hi, target = 6) {
  if (!(hi > lo)) { const d = Math.abs(lo) * 0.1 || 1; lo -= d; hi += d; }
  const raw = (hi - lo) / target;
  const mag = Math.pow(10, Math.floor(Math.log10(raw)));
  const f = raw / mag;
  const step = (f < 1.5 ? 1 : f < 3 ? 2 : f < 7 ? 5 : 10) * mag;
  const nlo = Math.floor(lo / step) * step;
  const nhi = Math.ceil(hi / step) * step;
  const ticks = [];
  for (let v = nlo; v <= nhi + step * 1e-9; v += step) ticks.push(Math.abs(v) < step * 1e-9 ? 0 : v);
  return { ticks, lo: nlo, hi: nhi, step };
}

const SUP = { '-': '⁻', 0: '⁰', 1: '¹', 2: '²', 3: '³', 4: '⁴', 5: '⁵', 6: '⁶', 7: '⁷', 8: '⁸', 9: '⁹' };
const sup = (n) => String(n).split('').map((c) => SUP[c] || c).join('');

/** Ticks for a log axis: powers of ten, plus 2 and 5 when the span is short. */
function logTicks(lo, hi) {
  const e0 = Math.floor(Math.log10(lo)), e1 = Math.ceil(Math.log10(hi));
  const mults = (e1 - e0) <= 3 ? [1, 2, 5] : [1];
  const ticks = [];
  for (let e = e0; e <= e1; e++) for (const m of mults) {
    const v = m * Math.pow(10, e);
    if (v <= Math.pow(10, e1) * (1 + 1e-9)) ticks.push({ v, label: m === 1 ? `10${sup(e)}` : `${m}×10${sup(e)}` });
  }
  return { ticks, lo: Math.pow(10, e0), hi: Math.pow(10, e1) };
}

function linLabel(v, step) {
  if (v === 0) return '0';
  const a = Math.abs(v);
  if (a >= 1e5 || a < 1e-3) return v.toExponential(1).replace('e+', 'e');
  const decimals = Math.max(0, Math.min(8, -Math.floor(Math.log10(step)) ));
  return v.toFixed(decimals);
}

// ------------------------------------------------------------------- the plot
/**
 * drawPlot(container, {
 *   title, xlabel, ylabel, xlog, ylog, ariaLabel,
 *   series: [{name, x[], y[], color, line:true, markers:true, dash:'6 4', width:2, hollow:false, radius:3.5}],
 *   notes:  [{x, y, text, dx, dy, anchor}]   // annotations in data coordinates
 * })
 */
export function drawPlot(container, o) {
  clear(container);
  const series = o.series.map((s) => {
    // on log axes drop non-positive values (cannot be shown)
    const x = [], y = [];
    for (let i = 0; i < s.x.length; i++) {
      if (!Number.isFinite(s.x[i]) || !Number.isFinite(s.y[i])) continue;
      if (o.xlog && s.x[i] <= 0) continue;
      if (o.ylog && s.y[i] <= 0) continue;
      x.push(s.x[i]); y.push(s.y[i]);
    }
    return { ...s, x, y };
  });
  const all = series.flatMap((s) => s.x.map((xv, i) => [xv, s.y[i]]));
  if (all.length === 0) {
    container.append(Object.assign(document.createElement('p'), {
      className: 'muted', textContent: 'Nothing to plot (no positive finite values for the chosen axes).' }));
    return;
  }
  let xmin = Math.min(...all.map((p) => p[0])), xmax = Math.max(...all.map((p) => p[0]));
  let ymin = Math.min(...all.map((p) => p[1])), ymax = Math.max(...all.map((p) => p[1]));
  if (o.yminZero) ymin = Math.min(ymin, 0);

  // axis scales
  const ax = o.xlog ? logTicks(xmin, xmax) : niceTicks(xmin, xmax, 7);
  const ay = o.ylog ? logTicks(ymin, ymax) : niceTicks(ymin, ymax, 6);
  const xt = o.xlog ? ax.ticks : ax.ticks.map((v) => ({ v, label: linLabel(v, ax.step) }));
  const yt = o.ylog ? ay.ticks : ay.ticks.map((v) => ({ v, label: linLabel(v, ay.step) }));
  const pw = W - M.l - M.r, ph = H - M.t - M.b;
  const sx = (v) => M.l + (o.xlog ? (Math.log10(v) - Math.log10(ax.lo)) / (Math.log10(ax.hi) - Math.log10(ax.lo))
                                  : (v - ax.lo) / (ax.hi - ax.lo)) * pw;
  const sy = (v) => M.t + ph - (o.ylog ? (Math.log10(v) - Math.log10(ay.lo)) / (Math.log10(ay.hi) - Math.log10(ay.lo))
                                       : (v - ay.lo) / (ay.hi - ay.lo)) * ph;

  const root = svg('svg', { viewBox: `0 0 ${W} ${H}`, class: 'plot', role: 'img',
    'aria-label': o.ariaLabel || o.title || 'plot', preserveAspectRatio: 'xMidYMid meet' });
  if (o.title) root.append(svg('title', {}, o.title));
  root.append(svg('rect', { x: 0, y: 0, width: W, height: H, class: 'plot-bg' }));

  // gridlines + tick labels
  const grid = svg('g', { class: 'plot-grid' });
  for (const t of xt) {
    const X = sx(t.v);
    grid.append(svg('line', { x1: X, x2: X, y1: M.t, y2: M.t + ph }));
    root.append(svg('text', { x: X, y: M.t + ph + 18, class: 'plot-tick', 'text-anchor': 'middle' }, t.label));
  }
  for (const t of yt) {
    const Y = sy(t.v);
    grid.append(svg('line', { x1: M.l, x2: M.l + pw, y1: Y, y2: Y }));
    root.append(svg('text', { x: M.l - 8, y: Y + 4, class: 'plot-tick', 'text-anchor': 'end' }, t.label));
  }
  root.append(grid);
  root.append(svg('rect', { x: M.l, y: M.t, width: pw, height: ph, class: 'plot-frame' }));

  // axis labels (with units supplied by caller)
  root.append(svg('text', { x: M.l + pw / 2, y: H - 12, class: 'plot-label', 'text-anchor': 'middle' }, o.xlabel || ''));
  root.append(svg('text', { x: 18, y: M.t + ph / 2, class: 'plot-label', 'text-anchor': 'middle',
    transform: `rotate(-90 18 ${M.t + ph / 2})` }, o.ylabel || ''));
  if (o.title) root.append(svg('text', { x: M.l, y: 18, class: 'plot-title' }, o.title));

  // clip so lines never leave the frame
  const clipId = 'clip' + Math.random().toString(36).slice(2, 8);
  const defs = svg('defs');
  defs.append(svg('clipPath', { id: clipId }, undefined));
  defs.firstChild.append(svg('rect', { x: M.l, y: M.t, width: pw, height: ph }));
  root.append(defs);

  const data = svg('g', { 'clip-path': `url(#${clipId})` });
  for (const s of series) {
    const color = s.color || '#1f5fa8';
    if (s.line !== false && s.x.length > 1) {
      const d = s.x.map((xv, i) => `${i ? 'L' : 'M'}${sx(xv).toFixed(2)},${sy(s.y[i]).toFixed(2)}`).join('');
      data.append(svg('path', { d, fill: 'none', stroke: color, 'stroke-width': s.width || 2,
        'stroke-dasharray': s.dash || '', 'stroke-linejoin': 'round' }));
    }
    if (s.markers) {
      const r = s.radius || 3.5;
      s.x.forEach((xv, i) => {
        const c = svg('circle', { cx: sx(xv), cy: sy(s.y[i]), r, stroke: color, 'stroke-width': 1.6,
          fill: s.hollow ? '#fff' : color });
        c.append(svg('title', {}, `${s.name}: x=${Number(xv.toPrecision(6))}, y=${Number(s.y[i].toPrecision(6))}`));
        data.append(c);
      });
    }
  }
  root.append(data);

  // annotations (data coordinates)
  for (const n of o.notes || []) {
    if ((o.xlog && n.x <= 0) || (o.ylog && n.y <= 0)) continue;
    root.append(svg('text', { x: sx(n.x) + (n.dx || 0), y: sy(n.y) + (n.dy || 0), class: 'plot-note',
      'text-anchor': n.anchor || 'start' }, n.text));
  }

  // legend (top margin, one row)
  const legend = svg('g', { class: 'plot-legend' });
  let lx = M.l, ly = 38;
  for (const s of series) {
    const color = s.color || '#1f5fa8';
    if (s.line !== false) legend.append(svg('line', { x1: lx, x2: lx + 26, y1: ly - 4, y2: ly - 4, stroke: color,
      'stroke-width': s.width || 2, 'stroke-dasharray': s.dash || '' }));
    if (s.markers) legend.append(svg('circle', { cx: lx + 13, cy: ly - 4, r: 3.5, stroke: color, 'stroke-width': 1.6,
      fill: s.hollow ? '#fff' : color }));
    legend.append(svg('text', { x: lx + 32, y: ly }, s.name));
    lx += 32 + s.name.length * 6.6 + 22;
  }
  root.append(legend);
  container.append(root);
}
