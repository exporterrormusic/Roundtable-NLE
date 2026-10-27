# CorridorKey in Roundtable

This folder is CorridorKey 2.0 by Corridor Digital, copied **unmodified**
from https://github.com/nikopueringer/CorridorKey2.0 at commit `485f5e9`
(2026-10-15) with `git archive`. It is distributed under the CorridorKey
Licence in `LICENSE` (attribution: CorridorKey, Corridor Digital).

Roundtable uses it for Ultra Key's **Render with CorridorKey** button:

- `tools/corridorkey/roundtable_corridorkey.py` — the worker the app runs
  (also under the CorridorKey Licence, as it builds on this code).
- `tools/corridorkey/requirements.txt` — the pinned Python packages.
- `src/core/ai/CorridorKey.*`, `src/ui/corridorkey/*` — planning, setup and
  the render queue in the app.

The model weights are not in this folder. The app's CorridorKey setup
downloads them (with Python and PyTorch) into `runtimes/corridorkey/`.

To update: replace this folder's contents with a newer `git archive` of the
upstream repo, then update `kCodeCommit`, `kSetupVersion` and the pins.
