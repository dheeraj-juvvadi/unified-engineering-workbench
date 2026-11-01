# Running the Workbench from VS Code

There is one action, **Run packaged Workbench**.

| How | Needs |
|---|---|
| Terminal > Run Build Task (Ctrl+Shift+B) | nothing (no extension) |
| Run and Debug > "Run packaged Workbench" (F5) | extension **ms-vscode.cpptools** (C/C++) |

**Windows 11**: both start the packaged `Launch.exe`. The task searches the workspace
(up to 2 levels deep) for `Launch.exe`, so it works when you open the unzipped
folder itself or this repository after `scripts/package.sh` (`dist/<ID>_Workbench/`).
The F5 configuration runs `${workspaceFolder}\Launch.exe`, i.e. it expects the unzipped
submission folder (where `Launch.exe` sits at the root) to be the opened folder.

**Linux/macOS**: the task builds natively the first time (`scripts/build_native.sh`:
cmake + tests) and then starts `build/Launch`. For F5, run that script once first.

The app opens your default browser by itself. Closing the browser tab (or the
"Quit" button) stops the program; logs are in `logs/workbench.log` next to `Launch.exe`.
