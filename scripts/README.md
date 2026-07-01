# scripts/

Helper and diagnostic scripts. The everyday entry points stay at the repo
root (`setup.ps1`, `build.ps1`, `launch.vbs`, `publish-gui.ps1`,
`create_setup.ps1`).

| Script | Purpose |
|---|---|
| `push.ps1` | Commit/push helper |
| `update_workspace.ps1` | Convert the USE_AS_DEFAULT workspace preset to the shipped `assets/default_layout.bin` |
| `publish.example.ps1` | Template for publish configuration |

## diag/

Diagnostic launchers. All except the AppVerifier one are thin delegates into
the root `launch.ps1` (single source of truth — see its help text).

| Script | Purpose |
|---|---|
| `launch-crtdebug.bat` | Console run, prefers Debug build (CRT heap validation), output to `debug.log` |
| `launch-framehash.bat` | A/B render-path verification — GPU-signatures every composited frame to `framehash.csv` (slow) |
| `launch-spinediag.bat` | Enables the per-clip `[SPINE-BLEND-DIAG]` logger; read `logs/perf_log.txt` |
| `launch-validation.bat` | Vulkan validation layers, non-fatal; messages in `logs/perf_log.txt` (slow) |
| `launch-appverifier.bat` | Run under Application Verifier / PageHeap to catch heap corruption at the write site. Requires Administrator; self-contained (does NOT route through `launch.ps1`) |
| `setup_appverif.ps1` | Enable/disable AppVerifier + WER dump registry settings for `roundtable.exe` |
