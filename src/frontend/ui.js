// ui.js - small shared UI helpers: DOM builder, number formatting, status bar, toasts.
import { ApiError } from './api.js';

export const $ = (sel, root = document) => root.querySelector(sel);
export const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));

/** Tiny DOM builder: el('div', {class:'x', onclick: fn}, 'text', childNode, ...) */
export function el(tag, attrs = {}, ...children) {
  const node = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs || {})) {
    if (v === null || v === undefined || v === false) continue;
    if (k === 'class') node.className = v;
    else if (k === 'text') node.textContent = v;
    else if (k.startsWith('on') && typeof v === 'function') node.addEventListener(k.slice(2), v);
    else node.setAttribute(k, v === true ? '' : v);
  }
  for (const c of children.flat(Infinity)) {
    if (c === null || c === undefined || c === false) continue;
    node.append(c.nodeType ? c : document.createTextNode(String(c)));
  }
  return node;
}

export function clear(node) { while (node.firstChild) node.removeChild(node.firstChild); }

/** Number formatting: sig significant digits, exponent form for very small/large values. */
export function fmt(v, sig = 5) {
  if (v === null || v === undefined || Number.isNaN(v)) return '–';
  if (!Number.isFinite(v)) return v > 0 ? '∞' : '-∞';
  if (v === 0) return '0';
  const a = Math.abs(v);
  if (a < 1e-3 || a >= 1e6) return v.toExponential(sig - 1).replace(/\.?0+e/, 'e').replace('e+', 'e');
  return String(Number(v.toPrecision(sig)));
}

// ---------------------------------------------------------------- status bar
let busyCount = 0;

export function setStatus(text, ms) {
  $('#status-text').textContent = text;
  $('#status-time').textContent = ms === undefined ? '' : `${ms < 10 ? ms.toFixed(1) : Math.round(ms)} ms`;
}

function updateBusy() {
  $('#status-busy').hidden = busyCount === 0;
}

/**
 * Run an async action with status-bar feedback. Errors become toasts (and are
 * re-thrown as null result so callers can simply check for undefined).
 * Returns the action's result, or undefined if it failed.
 */
export async function track(label, fn) {
  const t0 = performance.now();
  busyCount++;
  updateBusy();
  setStatus(`${label}…`);
  try {
    const result = await fn();
    setStatus(label, performance.now() - t0);
    return result;
  } catch (err) {
    setStatus(`${label} failed`, performance.now() - t0);
    showError(err, label);
    return undefined;
  } finally {
    busyCount--;
    updateBusy();
  }
}

// -------------------------------------------------------------------- toasts
/** Show an error toast with the server's message, line number and hint. */
export function showError(err, context = '') {
  const isApi = err instanceof ApiError;
  const message = err && err.message ? err.message : String(err);
  const box = el('div', { class: 'toast', role: 'alert' });
  const head = el('div', { class: 'toast-head' },
    el('strong', {}, context ? `${context} failed` : 'Error'),
    isApi && err.code ? el('span', { class: 'toast-code' }, err.code) : null,
    el('button', { class: 'toast-close', type: 'button', 'aria-label': 'Dismiss message',
      onclick: () => box.remove() }, '×'));
  box.append(head, el('div', { class: 'toast-msg' }, message));
  // server messages already begin with "Line N: " when a line applies; do not repeat it
  if (isApi && err.line > 0 && !/^\s*Line\s+\d+/i.test(message)) box.append(el('div', { class: 'toast-line' }, `Line ${err.line}`));
  if (isApi && err.hint) box.append(el('div', { class: 'toast-hint' }, el('b', {}, 'What to do: '), err.hint));
  const host = $('#toasts');
  host.prepend(box);
  while (host.children.length > 4) host.lastChild.remove();
  setTimeout(() => box.remove(), 15000);
  console.warn('[workbench]', context, message, isApi ? { code: err.code, line: err.line, hint: err.hint } : '');
}

export function showInfo(text) {
  const box = el('div', { class: 'toast info', role: 'status' }, text);
  $('#toasts').prepend(box);
  setTimeout(() => box.remove(), 5000);
}

/** Disable a button while an async handler runs (prevents double submits). */
export function guarded(button, handler) {
  button.addEventListener('click', async (ev) => {
    if (button.disabled) return;
    button.disabled = true;
    button.classList.add('is-busy');
    try { await handler(ev); } finally {
      button.disabled = false;
      button.classList.remove('is-busy');
    }
  });
}
