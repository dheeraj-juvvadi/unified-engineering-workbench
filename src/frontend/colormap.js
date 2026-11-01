// colormap.js - viridis / turbo style colour maps (piecewise-linear through control colours).

const MAPS = {
  viridis: ['#440154', '#472c7a', '#3b518b', '#2c718e', '#21908d', '#27ad81', '#5cc863', '#aadc32', '#fde725'],
  turbo:   ['#30123b', '#4145ab', '#4675ed', '#39a2fc', '#1bcfd4', '#24eca6', '#61fc6c', '#a4fc3b',
            '#e1dd37', '#f66b19', '#7a0402'],
};

const parsed = {};
for (const [name, list] of Object.entries(MAPS)) {
  parsed[name] = list.map((h) => [1, 3, 5].map((i) => parseInt(h.slice(i, i + 2), 16) / 255));
}

export const colormapNames = Object.keys(MAPS);

/** Map t in [0,1] to [r,g,b] in 0..1. */
export function colorAt(name, t) {
  const stops = parsed[name] || parsed.viridis;
  const u = Math.min(1, Math.max(0, Number.isFinite(t) ? t : 0)) * (stops.length - 1);
  const i = Math.min(stops.length - 2, Math.floor(u));
  const f = u - i;
  return [0, 1, 2].map((c) => stops[i][c] + (stops[i + 1][c] - stops[i][c]) * f);
}

/** CSS gradient string (bottom = low, top = high) for the legend bar. */
export function cssGradient(name) {
  const n = 12;
  const stops = [];
  for (let i = 0; i < n; i++) {
    const [r, g, b] = colorAt(name, i / (n - 1));
    stops.push(`rgb(${Math.round(r * 255)},${Math.round(g * 255)},${Math.round(b * 255)}) ${(i / (n - 1) * 100).toFixed(1)}%`);
  }
  return `linear-gradient(to top, ${stops.join(', ')})`;
}
