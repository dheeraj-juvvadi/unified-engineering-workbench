# AI-use log

The brief allows AI tools if their use is disclosed. This log records how they were
used, what they produced, how the output was checked, and the AI mistakes that were
caught. Nothing here is reconstructed after the fact: the items come from the actual
build sessions and test runs.

## 1. Tools and models

| Tool | Model | Used for |
|---|---|---|
| Claude Code (cloud session) | Claude Opus 5.5 (lead agent) | Project plan, core C++ headers (API contract), CMake files, test framework, `docs/API.md`, integration builds, Windows VM test, benchmark re-runs, this log |
| Claude Code subagents | Claude Sonnet (5 parallel agents) | (1) FEA / batch / sysinfo / backprop + tests + report tools; (2) STL/CSV parsers, simulated hardware adapters, sample data + tests; (3) HTTP server, job manager, launcher, VS Code config, packaging scripts + tests; (4) frontend (HTML/JS/three.js); (5) embedded design doc + diagram, handwritten-backprop study guide |
| Claude Code subagent | Claude Sonnet | README and report drafting from verified results |

No AI tool was used to fill in the handwritten appendix: that is written by hand by the
candidate (`docs/appendix_handwritten.pdf`). The guide in `docs/handwritten_backprop_guide.md`
was only a study aid, and its numbers were re-checked independently (see §4).

## 2. How the work was split

The lead agent first wrote the C++ headers in `src/core/*.h` and the HTTP contract
`docs/API.md`, so the parallel agents could not drift apart. Each agent got a written
brief (goal, files it owned, files it must not touch, exact reference values to test
against, and a requirement to report every bug it hit). Each agent compiled its own
files directly with g++ and also compiled them for Windows with MinGW-w64 before
reporting. The lead then did the integrated CMake build, ran all tests, and tested
the packaged `Launch.exe` on a real Windows 11 VM.

## 3. Representative prompts (abridged)

- *Numerics agent:* "Implement fea1d.cpp per fea1d.h. Assemble from element matrices
  (kA/h)[[1,-1],[-1,1]] and loads qAh/2·[1,1], impose Dirichlet BCs by elimination,
  Thomas algorithm. Tests must include max midpoint error = qh²/(8k): N=10 → 1.25,
  20 → 0.3125, 40 → 0.078125, 80 → 0.01953125 within 1e-9; slope ≈ 2 ± 0.01;
  serial vs parallel (1–4 workers) max diff ≤ 1e-8; backprop reference values to 1e-7."
- *Parser agent:* "Strict ASCII STL and `node_id,x_m,y_m,z_m,temperature_C` CSV parsers;
  every rejection throws ParseError with a 1-based line and an actionable hint; reject
  NaN/Inf; size tests with a 200k-triangle STL and a 500k-row CSV; the sample CSV must
  be labelled SYNTHETIC with provenance."
- *Server agent:* "Implement docs/API.md exactly; results are swapped in only on job
  completion; cancel never leaves partial results; bind 127.0.0.1 only; static
  MinGW build whose only imports are Windows system DLLs."
- *Frontend agent:* "Plain HTML/JS, no CDN; four views + backprop; parsing happens in
  C++ only; click-to-inspect a node with a raycaster; test every tab with Playwright
  against the real backend and report console errors."

## 4. Validation (how AI output was checked)

- **Reference values, not "looks right".** FEA midpoint errors were checked against the
  closed form qh²/(8k), and nodal values against the exact solution
  T(x) = 100 − 80x + 500x(1−x). The backprop numbers were checked against hand values
  and against central finite differences (max error 1.1e-10 ≤ 1e-5).
- **76 automated tests** (`build/wb_tests`, CTest): positive, negative (malformed or
  non-finite files, bad parameters), numerical, concurrency, and server end-to-end
  (HTTP calls to the real server, a cancelled job returns no result). They were also
  run under AddressSanitizer/UBSan and ThreadSanitizer while being developed.
- **Real Windows run.** The packaged zip was extracted in a standard user's Downloads
  folder on an Azure Windows 11 Pro VM, and `Launch.exe` was started through
  `explorer.exe` (the same ShellExecute path as a double-click). Screenshots, the app
  log, launch-to-ready time and a Windows benchmark were collected
  (`docs/screenshots/windows/`, `results/windows/`). An earlier test under Wine was
  treated only as a smoke test, not as proof.
- **Independent re-run of claims.** Benchmark claims in agent reports were re-measured
  by the lead on a quiet machine before being used in the report (see 5.4).

## 5. AI mistakes that were caught

1. **Wrong expected value in an AI-written test.** The numerics agent's first test
   expected T(0.5 m) = 310 °C. The correct value is 185 °C, because q/(2k) = 500, so
   100 − 40 + 500·0.25 = 185. The solver was right and the test arithmetic was wrong.
   It was caught because the test failed against the analytic solution, and the
   expectation was fixed, not the solver.
2. **Convergence slope fitted to round-off.** With q = 0 the exact solution is linear,
   so every FE error is ~1e-14. The first `convergence()` still fitted a "slope" of
   −1.46 through that noise. Fixed by ignoring errors below a relative floor, plus a
   test for that case.
3. **Over-loose limit in the lead's own API contract.** `docs/API.md` first allowed
   cases × mesh ≤ 2e8. The numerics agent pointed out that this means ~1.6 GB per
   result set, so the limit was cut to 2e7 and the client validation matched to it.
4. **Misleading benchmark numbers.** An agent's first benchmark showed 4 workers no
   faster than 2 (2.37× at both). Re-running on a quiet machine gave 3.6–4.2×: the
   plateau came from other compile jobs sharing the CPUs. A later quiet run showed
   *super-linear* efficiency (106 %). An allocator experiment
   (`results/bench_malloc_experiment.txt`, glibc `MALLOC_TRIM_THRESHOLD_`/`MALLOC_TOP_PAD_`)
   removed the effect, showing it came from heap trimming and page faults in the
   serial reference run, not from real super-linear speed-up. Only the re-measured
   numbers are reported.
5. **Heartbeat watchdog would kill the app.** The lead's design said "exit 30 s after
   the last browser heartbeat". The server agent flagged that browsers throttle
   timers in background tabs to about once a minute, so the app could close while
   its tab was simply hidden. The timeout became 180 s, and the UI sends a heartbeat
   as soon as the tab becomes visible again.
6. **Stale frontend in the build tree (lead's CMake bug).** The frontend was copied
   next to the binary in a `POST_BUILD` step, which only runs when `Launch` relinks.
   After a JavaScript-only change the app served the old UI. This was caught when
   the automated demo screenshots showed no change. Fixed with an always-run
   `copy_web` target.
7. **Inside-out mesh.** The first generated heat-sink STL had inverted normals
   (signed volume −13 500). The generator's own closed-mesh/volume self-check caught
   it, and the facet winding was reversed.
8. **Windows screenshots unusable on the first try.** On the first Windows run the
   Start menu (first-logon behaviour) covered every screenshot. The test script was
   changed to dismiss it and maximise the browser, then re-run.

## 6. What the candidate did

- Chose the task interpretation and the overall architecture, and directed the agents.
- Reviewed the outputs and the reported numbers.
- Wrote the handwritten backprop derivation (appendix) without AI.
- Is responsible for being able to explain and modify every function in the interview.

*(Candidate: add your own notes here: which parts you read most carefully, anything you
changed by hand, and your actual hours. Do not overstate.)*
