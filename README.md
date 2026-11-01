# Unified Engineering Workbench

PASS Science Works R&D Internship 2026-27, Round 1. Candidate ID: `RA2311031010070`

A small desktop tool that puts five engineering tasks behind one window: a 3D viewer for CAD/ANSYS-style
data, a 1-D finite-element heat solver, a serial-versus-parallel benchmark, a simulated embedded-hardware
panel, and a one-neuron backpropagation check. The calculations are in C++17. The window is a local web page
that the program opens in your default browser.

The full write-up is in [`docs/REPORT.pdf`](docs/REPORT.pdf) (source: [`docs/REPORT.md`](docs/REPORT.md)).

## Quick start (Windows 10/11, x64)

1. Unzip the archive anywhere (for example into Downloads).
2. Double-click `Launch.exe`.
3. Your default browser opens the Workbench. Close the tab or press **Quit app** to stop the program.

No installer, no administrator rights, no internet connection and no other software are needed. `Launch.exe`
is a static executable that only uses Windows system DLLs. There is no console window.

**Windows SmartScreen.** The executable is not code-signed, so Windows may show "Windows protected your PC".
Click **More info**, then **Run anyway**. If the zip was downloaded, "Properties > Unblock" on the zip before
extracting also removes the warning.

Notes:
- The server listens on `127.0.0.1` only, on a free port chosen at start. Nothing is reachable from the network.
- Logs are written to `logs/workbench.log` next to `Launch.exe`.
- If no browser opens, read the address from the log and open it by hand. `Launch.exe --no-browser` starts the
  server without opening a browser, `--port N` picks a port.

## What is in the ZIP

```
Launch.exe            the application (Windows x64, static)
web/                  the user interface (plain HTML/JS, three.js r160 vendored); Launch.exe serves this folder
data/                 sample STL/CSV files, data/bad/ = deliberately broken files, PROVENANCE.md
src/
  core/               pure C++17: fea1d, batch + worker pool, sysinfo, stl_ascii, ansys_csv, embedded_mock, backprop
  server/             HTTP routes, JSON mapping, background job manager
  app/main.cpp        launcher: port, browser, logging, heartbeat watchdog
  frontend/           source of web/ (identical copy)
tests/                76 unit and integration tests (single executable wb_tests)
tools/                C++ programs that regenerate results/ and data/ (fea_report, bench_report, backprop_report, gen_samples)
results/              CSV, text summaries and SVG plots produced by the tools; results/windows/ = Windows VM test runs
docs/                 REPORT.pdf/.md/.html, API.md, FORMATS.md, EMBEDDED_DESIGN.md, VSCODE.md,
                      AI_USE_LOG.md, OWNERSHIP.md, screenshots/
third_party/          vendored cpp-httplib and nlohmann/json, licences, VERSIONS.md (pinned versions + SHA-256)
cmake/                MinGW-w64 cross toolchain file
scripts/              build_native.sh, package.sh
.vscode/              tasks.json, launch.json
CMakeLists.txt
vcs/workbench.bundle  full git history (git bundle); vcs/commit_log.txt = readable log
```

To inspect the history: `git clone vcs/workbench.bundle workbench-history`.

## Architecture

```
  Browser tab (default browser)                      Launch.exe  (one process, one C++17 binary)
 +------------------------------+   HTTP/JSON    +--------------------------------------------------+
 | web/  HTML + JS + three.js   | <------------> | src/app/main.cpp   launcher, log, watchdog       |
 |  Geometry | Thermal | Perf.  |  127.0.0.1:    |        |                                         |
 |  Hardware | Backprop         |  free port     |        v                                         |
 +------------------------------+                |  src/server   routes, JSON mapping,              |
        ^                                        |               job manager (progress / cancel)    |
        | ShellExecuteW opens the page           |        |                                         |
        +----------------------------------------|        v                                         |
                                                 |  src/core     fea1d, batch + worker pool,        |
                                                 |               stl_ascii, ansys_csv, sysinfo,     |
                                                 |               embedded_mock, backprop            |
                                                 |               (no HTTP, no JSON)                 |
                                                 +--------------------------------------------------+
```

- `src/core` has no knowledge of HTTP or JSON, so the tests and the `tools/` programs link it directly.
- Long jobs (the benchmark) run on a background thread. The page polls `GET /api/jobs/<id>`. A result is
  stored only when the job finishes; **Cancel** ends in state `cancelled` with no result, never a partial one.
- The UI sends a heartbeat every 5 s. If none arrives for 180 s the program exits (the limit is long because
  browsers throttle timers in background tabs). Requests with a foreign `Origin` or `Host` header are rejected.
- HTTP contract: [`docs/API.md`](docs/API.md).

## Using the tabs

**Geometry.** Load an ASCII STL (**Open STL**), a nodal temperature CSV (**Open results CSV**), or pick a file
under **Sample file**. The STL is drawn as a mesh (wireframe toggle), the CSV as a colour-coded point cloud with
a legend. Left-drag rotates, right-drag pans, wheel zooms, a click on a node shows its id, coordinates and
temperature. **Fit to view** and **Clear scene** reset the view. Files under `bad/` in the sample list are broken on
purpose; the error appears with the line number and a hint.

**Thermal Analysis.** Edit the rod parameters (N, L, A, k, q, T0, TL; defaults N=10, L=1 m, A=1e-4 m^2,
k=10 W/(m K), q=1e4 W/m^3, T0=100, TL=20 degC) and press **Solve**. The plot shows the FE solution against the exact one,
the table lists the nodal and midpoint errors. **Run convergence (10,20,40,80)** plots the midpoint error against the element size
on log-log axes and reports the fitted slope. Both results can be exported as CSV.

**Performance.** Set cases, mesh size, seed, repetitions, warm-up and the worker counts (1, 2, 4 or custom), then
**Start**. A progress bar updates while the job runs, **Cancel** stops it. The result table gives median time,
speedup and efficiency per worker count, a correctness check against the serial run, and a speedup plot. The
System card shows OS, CPU, cores, RAM and compiler. **Export CSV** saves the raw timings.

**Hardware.** Everything on this tab is **SIMULATED**; no serial, USB or JTAG port is opened. **Discover** lists an
MCU, an FPGA and a JTAG probe with their capabilities. Each device has Connect, Disconnect, Read telemetry and
Inject timeout. After an injected timeout the next read fails with a recoverable error; **Reconnect** clears it.
The event log shows each action. Design notes: [`docs/EMBEDDED_DESIGN.md`](docs/EMBEDDED_DESIGN.md).

**Backprop.** Shows the forward values, analytic gradients, central finite-difference gradients and their
difference, the parameters after one update, and the loss over further steps.

## Data formats

Short version (details, rejection rules and measured parse times in [`docs/FORMATS.md`](docs/FORMATS.md)):

- **ASCII STL.** Standard `solid / facet / outer loop / vertex / endloop / endfacet / endsolid`. Units are not
  part of the format; mm are assumed. Binary STL is rejected with the hint to export ASCII.
- **Nodal temperature CSV.** Header exactly `node_id,x_m,y_m,z_m,temperature_C`; coordinates in metres,
  temperature in degC.
- Limits: 200 MB per file, 2,000,000 triangles, 5,000,000 CSV rows. Both parsers are strict: a file is read
  completely or rejected with the line number (0 = whole file).
- The sample files in `data/` are **synthetic** (made by `tools/gen_samples.cpp`); see
  [`data/PROVENANCE.md`](data/PROVENANCE.md). The CSV is not output from a real ANSYS run.

## Building from source

Needs CMake 3.16 or newer and a C++17 compiler (MSVC 2019+, MinGW-w64, GCC, Clang). No other dependency:
everything else is vendored in `third_party/`.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release
```

The executable is `build/Launch` (Windows: `build/Release/Launch.exe` with MSVC, `build/Launch.exe` with MinGW);
a `web/` folder is copied next to it. On Linux/macOS, `scripts/build_native.sh` does the three steps above.

**Windows executable from Linux.** With MinGW-w64 (posix threads) installed:

```
cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-toolchain.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-win --target Launch
```

`CANDIDATE_ID=RA2311031010070 scripts/package.sh` does this, strips the binary and assembles
`dist/RA2311031010070_Workbench/{Launch.exe, web/, data/}`. The final ZIP (adding src/, docs/ and so on) is put
together from that folder and the repository.

## Regenerating the results

Each tool is a small C++ program built by the same CMake project (`cmake --build build`), run from the repository root:

| Program | Writes |
|---|---|
| `build/fea_report` | `results/fea_N*.csv`, `fea_midpoints_N*.csv`, `convergence.csv`, `fea_temperature.svg`, `fea_convergence.svg` |
| `build/bench_report` | `results/bench.csv`, `bench_summary.txt`, `bench_speedup.svg` |
| `build/backprop_report` | `results/backprop.csv`, `backprop_summary.txt`, `backprop_loss.svg` |
| `build/gen_samples data` | `data/*.stl`, `data/*.csv`, `data/bad/*` (deterministic, byte for byte) |

All tools take an optional output directory as first argument (defaults: `results`, and `data` for `gen_samples`);
`bench_report` also accepts `--cases N --mesh N --reps N`. Benchmark numbers depend on the machine; the other outputs are deterministic.

## Running from VS Code

One task, **Run packaged Workbench** (Ctrl+Shift+B), starts the app; F5 uses the C/C++ extension. See
[`docs/VSCODE.md`](docs/VSCODE.md).

## Tests

76 tests run as one executable through CTest: FEA 9, batch and benchmark 11, backprop 6, parsers 19,
embedded mock 12, server and job manager 19. They include negative tests (malformed files, bad parameters,
cancel, timeout and recovery). The suite also ran clean under AddressSanitizer/UBSanitizer and
ThreadSanitizer during development. Run with `ctest --test-dir build --output-on-failure` or start `build/wb_tests`.

## Known limitations

- The packaged executable is Windows x64 only. The code also builds and passes its tests on Linux with GCC 13;
  the Windows build was verified twice on a Windows 11 VM (launch-to-ready 56 to 87 ms;
  `results/windows/README.md`, screenshots in `docs/screenshots/windows/`).
- The executable is unsigned, so SmartScreen warns.
- STL: ASCII only, one solid per file, units assumed to be mm. CSV: fixed header and units.
- The Hardware tab is a simulation. No real MCU, FPGA or JTAG probe was used.
- The FEA is one-dimensional with constant coefficients and Dirichlet boundaries only.
- Benchmark speedup depends on memory bandwidth, core count and system load; on a 2-vCPU VM that shares one
  physical core, 2 workers gave 1.46x to 1.52x in two runs (`results/windows/README.md`).
- The browser must allow `localhost` pages and JavaScript modules (any current Edge, Chrome or Firefox).

## Effort log

| Part | Task | Actual effort |
|---|---|---|
| A | Architecture, launcher, one-click packaging | Built by AI agents, reviewed by me (counted in the total) |
| B | STL / ANSYS-CSV viewer and parsers | Built by AI agents, reviewed by me (counted in the total) |
| C | 1-D thermal FEA and verification | Built by AI agents, reviewed by me (counted in the total) |
| D | Serial vs parallel benchmark | Built by AI agents, reviewed by me (counted in the total) |
| E | Embedded hardware design and mock adapters | Built by AI agents, reviewed by me (counted in the total) |
| F | Backpropagation (code and typed derivation) | Built by AI agents, reviewed by me (counted in the total) |
| - | Testing, Windows verification, documentation, report | Built by AI agents, reviewed by me (counted in the total) |
| | **Total** | About 2 h of my own time on 2 Nov 2026: directing the agents, reviewing results, providing the Azure student subscription for the Windows test VM, and submission. Agent run time is not counted as my effort. |

## AI use and ownership

AI assistance is disclosed in [`docs/AI_USE_LOG.md`](docs/AI_USE_LOG.md). The ownership statement is in
[`docs/OWNERSHIP.md`](docs/OWNERSHIP.md).

## Licences

Own code: see `docs/OWNERSHIP.md`. Sample data: CC0 / MIT (`data/PROVENANCE.md`). Vendored libraries, all MIT
(texts in `third_party/LICENSES/`, versions and checksums in `third_party/VERSIONS.md`):
cpp-httplib v0.18.3, nlohmann/json 3.11.3, three.js r160 (including OrbitControls).
