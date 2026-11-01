# Workbench HTTP API (contract between `src/server` and `src/frontend`)

Server binds **127.0.0.1 only**, port chosen at launch. All bodies are JSON
(`Content-Type: application/json`) unless noted. Every error response has HTTP
status 4xx/5xx and body:

```json
{"error": "human readable message", "code": "PARSE_ERROR|BAD_PARAM|NOT_FOUND|CANCELLED|BUSY|INTERNAL",
 "line": 12, "hint": "what the user can do"}
```
(`line` only for parse errors; 0 = whole file. For parse errors `error` already starts with "Line N: ".)

Static files: `GET /` → `web/index.html`, `GET /<path>` → `web/<path>` (`.js`/`.mjs` are served as `text/javascript`).

Implementation notes (server behaviour beyond the tables below):
- Request bodies must be JSON objects; **unknown keys are rejected** with 400 `BAD_PARAM`
  (so a typo cannot be silently ignored). An empty body means "all defaults".
- Requests whose `Host` is not `127.0.0.1:<port>`/`localhost:<port>` or that carry a foreign
  `Origin` header get 403 (other websites cannot drive the local API).
- A POST must send `Content-Length` (browsers' `fetch` does; with curl use `-d ''`).
- Unmatched routes return the standard JSON error (404 `NOT_FOUND`, 405).

## Lifecycle
| Method | Path | Body | Response |
|---|---|---|---|
| GET | /api/health | – | `{"ok":true,"version":"1.0.0","launch_to_ready_ms":123.4,"simulated_hw":true}` |
| POST | /api/heartbeat | – | `{"ok":true}` (UI sends every 5 s; server exits after 180 s without one (hidden tabs throttle timers to ~1/min), only once at least one heartbeat was received, and not while a benchmark job is still running) |
| POST | /api/shutdown | – | `{"ok":true}` then server stops |
| GET | /api/sysinfo | – | `{"os","cpu_model","logical_cpus","physical_cores","ram_gb","compiler","build_flags","build_type"}` |

## Geometry (Task B)
`POST /api/geometry/stl` — body = raw STL text (`Content-Type: text/plain`), max 200 MB.
```json
{"name":"cube","triangle_count":12,"positions":[x,y,z,...9 per tri],"normals":[...3 per tri],
 "bbox_min":[0,0,0],"bbox_max":[1,1,1],"parse_ms":1.2}
```
`POST /api/results/csv` — body = raw CSV text.
```json
{"count":441,"nodes":{"id":[...],"x":[...],"y":[...],"z":[...],"t":[...]},
 "t_min":20.0,"t_max":188.2,"bbox_min":[..],"bbox_max":[..],"units":{"xyz":"m","t":"degC"},"parse_ms":0.8}
```
(Column arrays, not array-of-objects, to keep big payloads small.)

`GET /api/samples` → `{"files":[{"name":"cube.stl","kind":"stl","group":"good"},{"name":"bad/empty.csv","kind":"csv","group":"bad"}]}`
(`data/*.stl|csv` have `group:"good"`; `data/bad/*.stl|csv` are listed as `bad/<file>` with `group:"bad"` - they are meant to be rejected by the parsers.)
`GET /api/samples/<name>` → raw text (`text/plain`) of a listed file; the name must equal a listing entry (e.g. `bad/empty.csv`), anything else is 404.

## Thermal FEA (Task C)
`POST /api/fea/solve` (1 ≤ N ≤ 1000000; L, A, k > 0; others finite) body `{"N":10,"L":1,"A":1e-4,"k":10,"q":10000,"T0":100,"TL":20}` (all optional, defaults shown)
```json
{"params":{...},"h":0.1,"x":[...],"T":[...],"T_exact":[...],"x_mid":[...],"T_mid_fe":[...],"T_mid_exact":[...],
 "max_nodal_err":1e-13,"max_mid_err":1.25,"units":{"x":"m","T":"degC"}}
```
`POST /api/fea/convergence` body `{"Ns":[10,20,40,80], ...same optional params}` (Ns: 2..40 integers, at least two distinct, each 1..1000000; an `N` key is accepted but ignored)
```json
{"points":[{"N":10,"h":0.1,"max_mid_err":1.25,"max_nodal_err":1e-13},...],"slope":2.0}
```
CSV export: same endpoints with query `?format=csv` return `text/csv` with
`Content-Disposition: attachment; filename="fea_N10.csv"` (columns
`x_m,T_fe_C,T_exact_C,abs_err_C`) or `convergence.csv` (`N,h_m,max_mid_err_C,max_nodal_err_C`).

## Performance (Task D) — background job
`POST /api/bench/start` body `{"cases":3000,"mesh":5000,"seed":42,"workers":[1,2,4],"reps":5,"warmup":1}`
→ `{"job":"j3"}` or 409 `{"code":"BUSY"}` if a benchmark is already running.
Limits: 1 ≤ cases ≤ 100000, 2 ≤ mesh ≤ 100000, cases*mesh ≤ 2e7, 1 ≤ reps ≤ 20, workers each 1..64 (capped at logical CPUs server-side, the cap is reported).

`GET /api/jobs/<id>`
```json
{"id":"j3","kind":"bench","state":"running|done|cancelled|error","progress":0.42,"message":"workers=2 rep 3/5",
 "result": null | {BenchReport}, "error": null | "..."}
```
BenchReport:
```json
{"config":{"cases":..,"mesh":..,"seed":..,"workers":[..],"reps":5,"warmup":1},
 "serial_times_s":[..],"serial_median_s":1.23,
 "rows":[{"workers":1,"times_s":[..],"median_s":..,"speedup":..,"efficiency":..},...],
 "max_abs_diff":0.0,"verified":true,"sysinfo":{...},"capped_workers":[1,2,4]}
```
Extra report fields: `requested_workers` (what the client asked for); `config.workers` and `capped_workers` are the list actually run (each value ≤ logical CPUs, duplicates removed). `warmup` is 0..10, `seed` 0..2^53.
`result` is only non-null when `state=="done"` (results are swapped in atomically, never partial).
`POST /api/jobs/<id>/cancel` → `{"ok":true}`; job ends in `cancelled` with `result:null`.
`GET /api/jobs/<id>?format=csv` (done only, else 409) → `bench.csv` columns `workers,rep,time_s,median_s,speedup,efficiency`; the serial baseline rows come first with `workers` = `serial` (speedup = efficiency = 1).

## Hardware (Task E) — ALL SIMULATED
| Method | Path | Response |
|---|---|---|
| GET | /api/hw/discover | `{"simulated":true,"devices":[{"id","kind":"MCU|FPGA|JTAG","name","transport","port","state":"Disconnected|Connected|Fault","capabilities":{"k":"v"},"simulated":true}]}` |
| POST | /api/hw/<id>/connect | `{"ok","code","message","hint","recoverable","device":{...}}` |
| POST | /api/hw/<id>/disconnect | same shape |
| GET | /api/hw/<id>/telemetry | `{"ok":true,"code":"OK","telemetry":{"device_id","seq","t_s","values":{"vdd_V":3.3},"units":{"vdd_V":"V"},"simulated":true}}` or 504-style `{"ok":false,"code":"TIMEOUT","message","hint":"Reconnect the device","recoverable":true}` (HTTP 200 so UI shows it inline) |
| POST | /api/hw/<id>/inject-timeout | `{"ok":true,"message":"next telemetry read will time out"}` |
| GET | /api/hw/log | `{"lines":[...]}` |

Unknown device ids give HTTP 404 with the same body shape plus `error` (code `UNKNOWN_DEVICE`); all other outcomes (including NOT_CONNECTED, ALREADY_CONNECTED) are HTTP 200 with `ok` true/false.

## Backprop (Task F)
`POST /api/backprop/run` body `{"steps":100}` →
```json
{"x":1,"y":2,"lr":0.1,"eps":1e-6,"p0":{"w","b","v","c"},"forward":{"z","h","yhat","e","J"},
 "grads":{"dw","db","dv","dc"},"grads_fd":{...},"max_fd_err":1e-10,"p1":{...},"J1":0.6857,"loss":[...101 values]}
```
