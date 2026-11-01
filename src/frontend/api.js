// api.js - thin fetch wrapper for the Workbench backend (see docs/API.md).
// Every non-2xx answer becomes an ApiError carrying the server's error/code/line/hint.

export class ApiError extends Error {
  constructor(message, { code = 'INTERNAL', line = 0, hint = '', status = 0, body = null } = {}) {
    super(message);
    this.name = 'ApiError';
    this.code = code;
    this.line = line;
    this.hint = hint;
    this.status = status;
    this.body = body; // parsed JSON body (if any); hardware replies use it
  }
}

/**
 * Call the backend.
 *  - body undefined       -> no body
 *  - body is a string     -> sent raw as text/plain (STL / CSV upload)
 *  - body is an object    -> sent as JSON
 * opts.as: 'json' (default) | 'blob' (for CSV downloads)
 */
export async function api(method, path, body, opts = {}) {
  const init = { method, headers: {} };
  if (typeof body === 'string') {
    init.headers['Content-Type'] = 'text/plain';
    init.body = body;
  } else if (body !== undefined) {
    init.headers['Content-Type'] = 'application/json';
    init.body = JSON.stringify(body);
  }

  let res;
  try {
    res = await fetch(path, init);
  } catch (e) {
    // fetch only rejects on network failure (server down, connection reset ...)
    throw new ApiError('Cannot reach the Workbench backend.', {
      code: 'NETWORK',
      hint: 'Check that the Workbench program is still running, then reload this page.',
    });
  }

  if (!res.ok) {
    let data = null;
    try { data = await res.json(); } catch (e) { /* not JSON */ }
    throw new ApiError(
      (data && (data.error || data.message)) || `HTTP ${res.status} ${res.statusText}`,
      {
        code: (data && data.code) || 'INTERNAL',
        line: (data && data.line) || 0,
        hint: (data && data.hint) || '',
        status: res.status,
        body: data,
      }
    );
  }

  if (opts.as === 'blob') {
    // filename comes from Content-Disposition when the server sends one
    const cd = res.headers.get('Content-Disposition') || '';
    const m = /filename="?([^";]+)"?/i.exec(cd);
    return { blob: await res.blob(), filename: m ? m[1] : null };
  }
  try {
    return await res.json();
  } catch (e) {
    throw new ApiError('The server sent a reply that is not valid JSON.', { status: res.status });
  }
}

export const get = (path, opts) => api('GET', path, undefined, opts);
export const post = (path, body, opts) => api('POST', path, body, opts);

/** Fetch a CSV (or any file) from the backend and hand it to the browser as a download. */
export async function downloadFrom(method, path, body, fallbackName) {
  const { blob, filename } = await api(method, path, body, { as: 'blob' });
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = filename || fallbackName;
  document.body.appendChild(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 2000);
  return filename || fallbackName;
}
