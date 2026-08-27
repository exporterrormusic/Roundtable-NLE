# Local Voice Generation

ROUNDTABLE supports two local voice engines through one persistent worker
service. Only one engine is kept in GPU memory at a time.

## Install

From the repository root:

```powershell
.\tools\install_voice_models.ps1
```

The script creates separate environments under `.voice-runtime/` because Fish
and OmniVoice require incompatible Transformers versions. That directory is
ignored by Git and the models are not included in normal application packages.

## Workflow

- The **TTS** item in AUDIO's left workflow rail opens generation controls
  beside the existing script and matched-line workspace. Select a script line
  to populate its character and dialogue.
- By default, ROUNDTABLE automatically selects confirmed (approved) Audio Sync
  matches for that character. It can combine clips from several imported
  tracks and takes the highest-confidence material until it reaches about 8
  seconds for OmniVoice or 20 seconds for Fish S2 Pro.
- The automatic plan never samples unmatched or merely tentative clips. If no
  approved clips exist in the current project, it can use the newest saved
  reference for that character.
- **Manual reference override** is a fallback for problem cases. It exposes all
  tracks from Audio Sync's Import tab, plus saved references, with waveform
  handles and exact start/end controls. Enter the exact transcript for a
  manually selected range.
- **Save Approved...** concatenates every confirmed clip for the selected
  character (with short gaps) into a 192 kbps MP3. The MP3 and a transcript
  metadata sidecar are stored in the application's **Voice References**
  library, so they are available in other projects.
- **Generate Draft** creates an unapproved temporary WAV. Use **Listen** to
  audition it; drafts do not enter Audio Sync or Project Bin.
- **Approve & Sync to Script** saves the WAV beside the imported reference
  source with a unique `CHARACTER-yyyyMMdd-HHmmss-zzz.wav` name, imports it
  into **Generated VO**, and matches its dialogue only against script lines for
  the selected character. If no line qualifies, it remains unmatched.
- **Approve & Import Only** saves and imports the same way without changing
  Audio Sync matches. Only approved results appear in the draggable generated
  clips list.
- **Unload Model / Free VRAM** stops the shared local worker immediately. The
  same shutdown runs automatically when ROUNDTABLE exits so Fish or OmniVoice
  cannot remain resident after a normal application close.

## Quality and performance

- OmniVoice runs FP16 at 24 kHz with 32 decoding steps. A clean 3–10 second
  reference is a useful target. It also supports fixed-duration generation.
- Fish S2 Pro runs the official BF16 weights at 44.1 kHz. Use a clean 10–30
  second reference and an exact transcript when possible.
- The Fish worker caps its KV cache at 4,096 tokens. This reduces working VRAM
  without quantizing weights or lowering output quality and still permits
  several minutes of speech in one request.
- On the development RTX 4090, Fish generated a 3.67-second smoke-test line in
  15.3 seconds after loading. Initial loading takes roughly 90 seconds. The
  persistent worker avoids paying that startup cost for every line.

## Licensing and release builds

OmniVoice is Apache-2.0. Fish S2 Pro uses the Fish Audio Research License and
must be treated as personal/research-only unless separately licensed. Neither
runtime nor its model weights are redistributed by the normal build.

For a public/commercial build, configure:

```powershell
cmake -S . -B build -DROUNDTABLE_ENABLE_FISH_S2_PERSONAL=OFF
```

This removes Fish from the UI while leaving OmniVoice available.
