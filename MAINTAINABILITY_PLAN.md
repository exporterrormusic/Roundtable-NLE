# ROUNDTABLE NLE — Maintainability & Modularization Plan

*Authored 2026-06-19 against v0.29. Scope: a multi-month roadmap to make this*
*codebase easy to put down and pick back up months later (and easy to hand a*
*helper a single self-contained task). Spine: **deeper modularization + bug fixes.***

---

## 0. How to use this document

This is the **forward roadmap**. It is deliberately separate from:

- `docs/architecture.md` — *what the system is* (living reference; keep current).
- `fable_cleanup.txt` — *the Phase-4 feature-parity tracker* (Premiere features).
  Phases 0–3 (the structural cleanup) are **done**; this plan picks up the
  remaining *maintainability* axis, which fable_cleanup never covered.
- `docs/archive/**` — *frozen history*; do not mine it for current truth.

Work top-to-bottom: **Phase 1 de-risks, Phase 2 modularizes, Phase 3 polishes.**
The ordering is not cosmetic — Phase 2's refactors are unsafe without Phase 1's
test nets (see the ground rules). Each item has a **Definition of Done (DoD)** so
"future you" can tell, months later, whether it was actually finished.

> **Audience reminder:** this is for *you* (primary owner, returning after gaps)
> plus the occasional helper — not a public open-source onboarding funnel. That
> means: optimize for *footgun removal* and *a single source of truth*, not for
> exhaustive contributor docs. Cross-platform is an explicit **non-goal** (§6).

---

## 1. Honest state of the codebase

This is a **healthy, well-architected codebase** — better than most. The cleanup
audit (phases 0–3) genuinely paid off:

**What's already good (do not re-litigate):**
- **Clean layering** — `core ← gpu ← ui / export`, verified zero violations: no
  Qt/UI includes in `core` or `gpu`, no upward or circular includes, no
  `friend class` leaks across the four former god-objects.
- **Naming discipline** — the `<PrimaryName><Topic>.cpp` split convention is
  followed in 100% of files; zero orphans (guarded by `tools/check_orphans.ps1`).
- **Core tests** — 859 cases (589 in the per-PR `core` label), real assertions,
  no permanently-disabled tests, weekly ASan.
- **Crash/lifetime discipline** — clean crash logs across ~45 sessions; heap-safe
  VEH stack capture; documented (and *legitimate*) SEH band-aids.

**The five real hand-off gaps (this plan's targets):**

| # | Gap | Bite |
|---|-----|------|
| G1 | **Verification debt** — a cluster of export features that are code-complete + test-green but **never run on a real GPU with real media**. | "Done" may not mean done; the next session trusts it blindly. |
| G2 | **Riskiest code is least tested** — `VideoFrameMapping` (tick→frame) has *zero* tests; decode/GPU/compositor are nightly-only, unguarded per-PR. | The most dangerous code to refactor has no safety net. |
| G3 | **A few dense, high-cognitive-load areas** survive — the drag-drop state machine and a handful of 1,100–1,700-line TUs. | These are where a returning maintainer loses the most time. |
| G4 | **Tribal knowledge** — SEH/`/EHa`, `CommandStack` push discipline, the 5-cache lifecycle, unity-build footguns. Documented in headers, but scattered. | One wrong `push()` silently breaks all undo. |
| G5 | **Footguns + doc sprawl** — undocumented CMake-version trap & FFmpeg step; pre-commit hook not auto-installed; `[OPEN-PERF]` instrumentation left in; stale README; planning split across 19 files. | First day back is spent re-learning, not building. |

---

## 2. Ground rules (carry these into every phase)

These are inherited from the repo's own hard-won history. Violating them is how
regressions get in.

1. **Tests before refactor.** Never refactor `VideoFrameMapping`, the
   tick→frame mapping, smart-render, or the caches without tests landing *first*.
2. **One tick→frame mapping** (`core/decode/VideoFrameMapping::mapTickToSourceFrame`)
   and **one CPU convert** (`core/decode/ConvertDecodedFrame`). Never fork them.
   Export reuses the preview compositor with `forceExact` + cache bypass.
3. **Build the Release `roundtable` target to verify** — not just the libs.
   `ctest -L core` is the per-change gate.
4. **Archive `exe` + `pdb` per release** — heap-corruption postmortems need the
   matching symbols (`roundtable.exe+0xOFFSET` → release `.pdb`).
5. **Windows-only is deliberate** (§6). Don't add cross-platform abstraction
   "just in case" — it's pure carrying cost here.

---

## 3. Workstream A — Bug fixes & verification debt  *(half the spine)*

### A1 — Verify the export feature cluster  ★ highest priority
A cluster of features shipped 2026-06-13→17 are **code-complete, test-green, but
never GPU-verified on real media**. Until verified, treat them as *suspect*.

| ID | Feature | Where | Owed verification |
|----|---------|-------|-------------------|
| A1.1 | **16-bit float export passthrough** | `src/export/Rgba16fPack.*`, `gpu/CompositeService16f.cpp`, `shaders/p010_to_rgba16f.comp`, `shaders/yuva444p12_to_rgba16f.comp`, `RenderQueue.cpp` (~546) | Export a 10-bit HEVC + a ProRes-4444 master → ffprobe output is `yuv422p10le`/`444p10le`; **no** R/B swap; **no** brightness/saturation shift; visibly less banding than 8-bit. Confirm `[16F-PASS]` log fires, and that adding any fx/transition/caption silently reverts to 8-bit. Confirm an 8-bit H.264 export is **byte-identical** to before. |
| A1.2 | **Alpha export** | `composite.comp` `preserveAlpha` push-const; `Compositor::setPreserveAlpha` ← `CompositeService::m_exportAlpha` ← `setExportAlpha` ← Export Panel checkbox | Export a multi-layer comp over transparency → ProRes 4444; confirm real alpha (`pix_fmt yuva444p10le`) in Resolve/ffprobe. Confirm a PNG sequence is transparent. Confirm viewport/playback unchanged (default `preserveAlpha=0`). |
| A1.3 | **Per-stream color management** | `decode/ColorConversion.cpp`, `ConvertDecodedFrame`, GPU-convert bail-to-CPU | Smoke real SD/BT.601, full-range JPEG/MJPEG, BT.2020 sources. Confirm **no** brightness/saturation flicker as the cache churns (the 601/709 mix bug). Check open-time log: "GPU-eligible" (debug) vs "CPU sws path" (warn). |
| A1.4 | **Multi audio stream per file** | `AudioClip` ordinal (v27), `AudioFile::open(path,ordinal)`, ordinal-keyed caches | Open a multicam / OBS multi-track / camera scratch+lav file; switch the Properties "Audio Stream" combo; confirm the correct stream decodes and the waveform re-keys. |
| A1.5 | **ProRes/DNxHR profile selector** | Export Panel "Profile" combo, `EncoderConfig.proresProfile/dnxhrProfile` | Picking ProRes 4444 / DNxHR HQX\|444 is the only path to a 10-bit target — verify alongside A1.1. |

**DoD (A1):** a one-page `docs/VERIFICATION_LOG.md` with a dated ✅/❌ per row,
the ffprobe output pasted in, and any defects filed as new A3 items. Do this on a
**real RTX machine** (your dev box qualifies). This single page retires G1.

### A2 — The rotation / sideways-export bug  ★ scariest single defect
`VideoStreamInfo` has **no rotation field** (confirmed: zero matches for
`rotation`/`displayMatrix` in `src/core/decode`). The 16F passthrough (A1.1)
*bypasses the compositor*, so a portrait/rotated 10-bit clip may export sideways.

- **Fix:** read the display-matrix rotation in `VideoDecoderInit`, store it on
  `VideoStreamInfo`, and (v1) **reject passthrough** for any non-zero rotation
  (fall back to the 8-bit compositor path, which is the known-good behavior).
  A full rotated-passthrough path is a later optimization, not a v1 need.
- **DoD:** a rotated 10-bit phone clip exports upright; a rotation regression
  test exists for the eligibility predicate (`core/timeline/PassthroughEligibility`).

### A3 — Smaller open bugs / correctness notes
- **Captions narrow-layout glitch** — table is "broken until widened"
  (`CaptionsPanel.cpp:92`). Layout fix; low risk.
- **HDR (BT.2020 PQ/HLG) not tone-mapped** — renders dark/desaturated
  (`fable_cleanup.txt` 4.1c). Correct matrix, no tone-map. **Deferred** — rides on
  a future 16F compositor; flagged at runtime via `ColorConversion::isHdr`. Leave
  deferred unless you hit it often.
- **VFR ±half-frame frame-pick jitter** (`fable_cleanup.txt` 4.3b). Re-confirmed
  *not* A/V desync. **Deferred** — only fix with a concrete objectionable repro,
  and only tests-first (it touches the one unified mapping — see ground rule 1/2).

### A4 — Owed stability smokes (per release)
- One export per codec: NVENC H.264 / H.265 / AV1 + ProRes + DNxHR.
- One character-conversion run (CONVERT page; exercises `MediaFileEncoderBase`).
- The GPU-teardown-UAF and H264-clip-start fixes are live and crash-free across
  ~45 sessions, but never *formally* smoked via their repros — fold a "stop
  playback mid-clip, close project" pass into the release checklist.

---

## 4. Workstream B — Deeper modularization  *(the headline)*

> **Reality check first:** modularization here is already strong. The remaining
> wins are **surgical, not sweeping**. Resist the urge to "decompose for its own
> sake" — every split below is justified by *cognitive load when you return*, and
> each is gated on Workstream C tests where it touches behavior.

### B1 — Extract the timeline drag-drop into an explicit state object  ★ top B item
This is the densest, highest-load area in the UI: a 4-phase implicit state machine
spread across files, with state smuggled through structs.

- **Files:** `TimelinePanelMouseDrag.cpp` (131), `TimelinePanelMouseDragMove.cpp`
  (1,111), `TimelinePanelMouseDragRelease.cpp` (551), plus
  `DropControllerMediaDrop.cpp` (**1,700** — the single largest TU in the repo),
  and the `DragClipState` / `SnapInfo` structs in `TimelinePanel.h`.
- **Problem:** press→move→release→finalize is a state machine, but the "state"
  lives in panel members + opaque structs threaded across 4–5 TUs. Fixing a
  "clips snap wrong" bug means reading ~3,000 lines to reconstruct the flow.
- **Proposal:** introduce an explicit `DragSession` object that *owns* the drag
  lifecycle (begin/update/commit/cancel) and holds the state currently scattered
  across panel members. The panel TUs become thin event adapters that forward to
  `DragSession`. Split `DropControllerMediaDrop.cpp` by drop *source* (timeline
  re-order vs. project-bin import vs. external file vs. puppet/character URI).
- **DoD:** the drag state lives in one named type; each drop source is its own
  ≤400-line TU; a returning maintainer can read `DragSession`'s header and
  understand the whole flow. **Gate:** add UI/interaction tests or a manual
  drag-snap smoke checklist first (this is behavior-touching).

### B2 — Split the oversized omnibus TUs
Five TUs dominate the line counts. Some are legitimately big (dispatch must handle
all clip types); others are functional grab-bags that split cleanly.

| TU | Lines | Verdict |
|----|-------|---------|
| `DropControllerMediaDrop.cpp` | 1,700 | **Split** by drop source (folded into B1). |
| `core/timeline/EditOperations.cpp` | 1,427 | **Split** by operation family (ripple/roll, trim/slip-slide, insert/overwrite, lift/extract). Pure-core → easy to test → low risk. |
| `core/spine/ShotPreset.cpp` | 1,430 | **Review** — likely serialization + apply + query; split if those are independent. |
| `gpu/CompositeServiceFrame.cpp` | 1,137 | **Keep** — it's the per-tick dispatch hot path; the per-type builders are already extracted. Document the entry-point flow instead (G4). |
| `gpu/CompositeServiceLayerBuild.cpp` | 1,111 | **Keep / light trim** — already has Video/Spine/Nested siblings. Only split if a new clip type lands. |

- **DoD:** `EditOperations` and `ShotPreset` each drop below ~600 lines per TU
  with no behavior change; `core` suite green before and after (the EditOperations
  split is *exactly* the kind of change `test_timeline_edit.cpp`'s 68 cases protect).

### B3 — TimelineWorkspace wiring density  *(optional, lower value)*
The `*Wiring*.cpp` files (ClipSelection 372, Panels 367, Track 201, Viewport 128)
are Qt `connect()` boilerplate. They're *already* split by receiver and are not a
correctness risk — only a "where does this signal get wired?" hunt.

- **Option (only if it keeps annoying you):** push each wiring block into the
  *receiver* panel (`DropController::wireDropSignals` pattern). Trades central
  overview for locality. **Recommendation: leave as-is**; spend the time on B1/B2.

### B4 — Header weight  *(de-prioritized)*
The audit flagged `CompositeService.h` (891), `TimelinePanel.h` (716),
`MediaPool.h` (702), `FrameCache.h` (458) — but on inspection these are **mostly
documentation + data-struct layouts + trivial getters**, not algorithmic inline
bloat (`FrameCache::ensurePixels` is *intentionally* inline on the hot path).

- **Recommendation:** no action beyond keeping the docs accurate. Do **not**
  shuffle these to .cpp — you'd lose docs/inlining for no real compile win.

### B5 — NvdecDecoder: finish or delete  ★ decision required
`src/gpu/cuda/NvdecDecoder.cpp` is a **silent stub** — `open()` (76),
`decodeNext()` (124), `seek()` (136) are all `TODO`/`return false`. It compiles,
never errors, and always falls back to the working FFmpeg-CUDA hwaccel path.

- **Recommendation: DELETE it.** FFmpeg hwaccel already provides NVDEC decode;
  the direct-CUVID path was an unfinished optimization with no measured need. A
  silent always-false stub is a *trap* for a returning maintainer ("is this
  used?"). If a future perf need appears, resurrect from git history.
- **Alternative (only with a measured win):** finish the CUVID parser/decoder
  end-to-end. Higher cost, niche payoff.
- **DoD:** the file (and its dead references) are gone; the FFmpeg-hwaccel path is
  the documented single NVDEC route.

---

## 5. Workstream C — Test coverage  *(the enabler for Workstream B)*

This is **not** a separate goal — it is the safety net that makes Workstream B
safe. Land these tests *before* the refactor they protect.

### C1 — `VideoFrameMapping` tests  ★ must precede any mapping change
The tick→frame mapping is the historically buggy heart of playback ("each was a
bug at some point") and has **zero** tests. It's pure, header-resident math —
trivially unit-testable, no GPU/mock needed.

- **Cases:** various fps (24/25/30/29.97/60), speed scaling, rounding vs.
  truncation at frame boundaries, frame clamping at clip head/tail, the
  fps-priority path. Add to `tests/core`.
- **DoD:** the mapping is pinned by tests in the `core` label; a regression here
  fails CI on every PR. This retires the worst of G2.

### C2 — `ConvertDecodedFrame` + decode-pipeline tests
The CPU color-convert fallback (YUV→BGRA, packed-alpha unpack, tier downsample)
is untested. Synthesize input frames (no real media needed for the math).

- **DoD:** core-label tests for each convert path; cross-checks A1.3's behavior.

### C3 — Pull GPU-independent logic into the `core` label
Today the entire compositor/decode chain is `gpu`-labelled (nightly-only), so it's
**unguarded on every PR**. Identify the GPU-*independent* logic (layer planning,
eligibility predicates, blend-mode math, cache key derivation) and test it in
`core`. The genuinely device-bound parts stay nightly.

- **DoD:** the per-PR `core` gate covers the *decision* logic of compositing even
  when the *rendering* still needs a GPU.

### C4 — CI hardening
- Flip `nightly-tests.yml`'s export/ui steps from `continue-on-error` to
  **required** once observed green on the headless runner.
- Keep `gpu` per-release/local (genuinely needs a device).
- **DoD:** a red export/ui test breaks the nightly build instead of being ignored.

---

## 6. Workstream D — Footgun removal & doc consolidation  *(for future-you)*

Small, cheap, high-leverage. These are what make "first day back" smooth.

### D1 — Document & guard the CMake version trap
Conda's cmake 3.29.2 on PATH poisons `build/`; the project needs bundled 3.31.6.
`setup.ps1`/`build.ps1`/CI already prepend the bundled cmake, but it's
**undocumented**. → Add a TROUBLESHOOTING entry (D6) + an explicit error in
`build.ps1` if a sub-3.31 cmake is detected on PATH.

### D2 — Make FFmpeg non-silent
FFmpeg is a *manual* download; a fresh build succeeds but video import fails at
runtime (warning only). → Either auto-fetch it in `setup.ps1` (preferred, matches
cmake/Qt/glslc handling) **or** hard-fail configure with download instructions.

### D3 — Auto-install the pre-commit hook
`.githooks/pre-commit` (orphan check + version bump) isn't installed on clone.
→ Add `git config core.hooksPath .githooks` to `setup.ps1`.

### D4 — Remove `[OPEN-PERF]` instrumentation
The warn-level phase timers (e.g. `TimelineWorkspaceIntegration.cpp:51`) were left
in to find a slow path. They're noise now. → Remove, or demote to `debug`.

### D5 — Fix README/doc drift
`README.md` still describes `src/core/media/` — it was split into
`decode/cache/playback/audio/convert/analysis`. → Sync README + `architecture.md`
to the real tree (architecture.md is mostly current; README's structure block is
stale).

### D6 — Consolidate planning into one source of truth
Planning currently lives across **19 files** (`fable_cleanup.txt` + 17 in
`docs/archive/` + scattered notes). A returning maintainer can't tell what's live.

**Target structure:**
- `docs/architecture.md` — *what it is* (keep; fix drift per D5).
- `docs/ROADMAP.md` — *what's next* (merge `fable_cleanup.txt`'s open Phase-4
  items + this plan's open workstreams into one prioritized list; this file
  becomes the live tracker).
- `docs/GOTCHAS.md` — *tribal knowledge / footguns* (NEW; retires G4 — see below).
- `docs/archive/` — frozen; add a 2-line `README` saying "historical, not current."
- This `MAINTAINABILITY_PLAN.md` collapses into `ROADMAP.md` once its items are
  scheduled (don't keep two forward docs long-term).
- *(Note: the agent `MEMORY.md` is machine memory, not human docs — a helper
  shouldn't rely on it.)*

**`docs/GOTCHAS.md` should capture (one paragraph each):**
- `/EHa` SEH band-aids — *legit* in `FrameProducer.cpp` (GPU crash survival) and
  `ExportPanel.cpp` (hook-DLL/TDR faults); don't remove. The AudioMixer one was
  correctly deleted.
- `CommandStack` discipline — `execute()` for un-applied mutations vs.
  `pushWithoutExecute()` for already-applied (live drag). Swapping them
  double-applies or no-ops; this silently breaks undo.
- The 5-cache lifecycle — `FrameCache` / `DiskFrameCache` / GPU texture cache /
  `SegmentRenderCache` / prefetch pool, coordinated by `core/cache/CachePolicy`.
- Unity build footguns — `roundtable_ui` is a unity build; a malformed header
  comment cascades into an unrelated TU's errors. Use `-DCMAKE_UNITY_BUILD=OFF`
  for incremental dev; `SKIP_UNITY_BUILD_INCLUSION` lists carve-outs.
- The `<PrimaryName><Topic>.cpp` split convention + the `check_orphans` guardrail.
- `kEnableSmartRenderPassthrough=false` (hard-disabled, `RenderQueue.cpp:387`) and
  why (SPS/PPS bitstream work); the analyzer stays live + tested.

---

## 7. Suggested sequencing (months, as time permits)

```
PHASE 1 — DE-RISK (do first; unblocks everything)
  A1  Verify the export cluster on real GPU/media  → VERIFICATION_LOG.md
  A2  Rotation/sideways fix + reject-passthrough guard
  C1  VideoFrameMapping tests   ─┐ (these are the safety net
  C2  ConvertDecodedFrame tests ─┘  for Phase 2; land them now)
  D1–D5  Footgun removal (cheap, parallelizable, do anytime)

PHASE 2 — MODULARIZE (only on top of Phase 1's nets)
  B1  DragSession extraction + DropControllerMediaDrop split
  B2  EditOperations + ShotPreset splits
  B5  NvdecDecoder delete decision
  C3  Pull GPU-independent logic into the core label

PHASE 3 — POLISH
  A3  Captions layout; revisit deferred HDR/VFR only on real repro
  B3/B4  Wiring/header (optional — likely skip)
  C4  CI: make nightly export/ui required
  D6  Doc consolidation → ROADMAP.md + GOTCHAS.md; archive frozen
```

**Dependency rules:** C1/C2 **before** any B item that touches mapping/edit logic.
A1 **before** trusting any export feature. Everything in D is independent — knock
it out in spare slots.

---

## 8. Explicit non-goals (don't spend time here)

- **Cross-platform (Linux/macOS).** Windows/MSVC/Qt/WASAPI/NVENC lock-in is *by
  design* for the VTuber/streamer audience. Porting is a rewrite, not a refactor.
  Don't add portability abstraction speculatively.
- **Public open-source onboarding polish** (CONTRIBUTING funnel, issue templates).
  Audience is you + occasional helper; `GOTCHAS.md` + `ROADMAP.md` suffice.
- **Header-to-cpp shuffles** (B4) — no real win.
- **Full 16-bit float compositor** — only the targeted export passthrough is in
  scope; the full float compositor is a separate large project (`fable_cleanup` 4.2).
- **Re-modularizing what's already clean** — the layering, naming, friend-narrowing,
  and most splits are done. Touch only B1/B2/B5.

---

## 9. Done already (2026-06-19)
- **ProjectPanel UI redesign reverted** to committed v0.29. The in-progress
  inspector-split redesign is stashed (`git stash list` → "ProjectPanel UI
  redesign…"), recoverable via `git stash pop` or droppable via `git stash drop`.
  `_MOCKUP/*.html` left in place as the design reference.

---

## Appendix — Key file index for a returning maintainer

| Subsystem | Start here | Notes |
|-----------|------------|-------|
| Tick→frame mapping | `core/decode/VideoFrameMapping` | The one mapping; test before touching (C1). |
| CPU convert | `core/decode/ConvertDecodedFrame` | The one convert; untested (C2). |
| Compositor dispatch | `gpu/CompositeService.h` → `CompositeServiceFrame.cpp` | Per-tick hot path; per-type builders are siblings. |
| Edit ops | `core/timeline/EditOperations.cpp` | 1,427 lines; split candidate (B2). |
| Drag-drop | `TimelinePanelMouseDrag*.cpp` + `DropControllerMediaDrop.cpp` | Densest UI area; B1 target. |
| Undo/redo | `core/command/CommandStack.h` | Read the header's push-discipline note before editing. |
| Caches | `core/cache/CachePolicy.h` | Coordinates the 5 caches. |
| Export | `export/RenderQueue.cpp` | Smart-render disabled @ 387; 16F/alpha paths (A1). |
| HW decode | FFmpeg-CUDA hwaccel; `gpu/cuda/NvdecDecoder.cpp` is a dead stub (B5). |
| Crash handling | `core/CrashHandler.cpp` | VEH heap-safe capture; symbolize vs release `.pdb`. |
```
