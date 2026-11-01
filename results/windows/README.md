# Windows 11 test runs (real VM)

Machine: Azure Standard_B2as_v2, Windows 11 Pro 10.0.26100, AMD EPYC 7763
(2 logical CPUs = 1 physical core with SMT), 8 GB RAM. The tests ran as a standard
(non-admin) user, `wbtester`, in the interactive desktop session.

Procedure (`run.log`, unattended PowerShell at logon):
1. Extract the packaged zip into `%USERPROFILE%\Downloads`.
2. Start `Launch.exe` through `explorer.exe`, the same ShellExecute path as a
   double-click. No terminal is opened.
3. Read the port from `logs\workbench.log`, then open each tab with `?demo=1`
   (the UI clicks its own main buttons) and take a screenshot
   (`docs/screenshots/windows/`).
4. Call the API directly: health, sysinfo, convergence, and a benchmark with
   workers 1 and 2, 5 reps and 1 warm-up (`bench_windows.json`).
5. `POST /api/shutdown`, then check that `Launch.exe` is no longer running.

| Run | launch-to-ready | serial median | 1 worker | 2 workers | max diff |
|---|---|---|---|---|---|
| run1 (`run1/`) | 86.5 ms | 0.2488 s | 0.2503 s (0.99×) | 0.1635 s (1.52×) | 0 |
| run2 (this folder) | 56.5 ms | 0.2431 s | 0.2501 s (0.97×) | 0.1670 s (1.46×) | 0 |

The 2-worker speed-up stays well below 2 because both vCPUs are SMT siblings on one
physical core. In run1 the Windows Start menu (first logon) covered the screenshots,
so those were discarded. Run2 dismissed it and maximised the browser. Run2 also shows
an Edge "Restore pages" prompt after the VM reboot.
