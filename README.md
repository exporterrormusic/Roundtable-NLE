# ROUNDTABLE NLE

[![Latest Release](https://img.shields.io/github/v/release/exporterrormusic/Roundtable-NLE?label=latest&color=blue)](https://github.com/exporterrormusic/Roundtable-NLE/releases/latest)

**GPU-accelerated non-linear video editor — built for Spine animation workflows.**

ROUNDTABLE is a full-featured NLE written in C++20 with Vulkan 1.3 compute shaders and Qt 6. It combines professional-grade editing with native Spine animation compositing, AI-powered script-to-audio synchronization, real-time audio mixing, and hardware-accelerated export — purpose-built for creators working with 2D character animation and talking-head content.

## 📺 Tutorial Video

<a href="https://youtu.be/ENj9zA39scc">
  <img src="https://img.youtube.com/vi/ENj9zA39scc/maxresdefault.jpg" alt="Watch the ROUNDTABLE tutorial" width="640">
</a>

▶️ **[Click here to watch the tutorial](https://youtu.be/ENj9zA39scc)**

---

## Disclaimer

**ROUNDTABLE NLE is a fan-made project and is not affiliated with, endorsed by, or supported by Shift Up Corporation.** All *Goddess of Victory: NIKKE* characters, names, images, and related assets are the exclusive property of Shift Up Corporation. NIKKE is a registered trademark of Shift Up. This project does not claim any ownership over Shift Up's intellectual property. Character assets available through the Character Browser are provided by the community and are used under fair use for creative, non-commercial purposes.

---

## Support

If you'd like to support development, you can leave a tip at:
👉 **https://www.paypal.com/ncp/payment/7THEH3LWCTRZU**

All contributions are greatly appreciated!

---

## Download

The easiest way to get ROUNDTABLE is to download a pre-built installer from the [Releases page](https://github.com/exporterrormusic/Roundtable-NLE/releases). The installer is a portable Windows build — no dependencies, no package managers, no build steps. Just run the installer and launch.

```powershell
# After installation, launch from the Start Menu or desktop shortcut
```

---

## Building from Source

If you want to build the latest development version yourself (requires a dev environment):

### Prerequisites
- Visual Studio 2022 (C++ Desktop workload)
- Git
- Python 3.x

> No Vulkan SDK, CUDA Toolkit, or package manager required. All C++ dependencies are fetched by CMake. Qt 6.8.3 is installed locally by the setup script.

### One-Time Setup → Build → Run
```powershell
.\setup.bat      # Downloads Qt + shader compiler, configures CMake
.\build.bat      # Builds Release (takes a few minutes on first run)
.\launch.bat     # Launches ROUNDTABLE (no console window)
```

### Manual Build
```powershell
cmake -B build -S . -G "Visual Studio 17 2022" -A x64 ^
  -DCMAKE_PREFIX_PATH="third_party/qt/6.8.3/msvc2022_64"
cmake --build build --config Release --parallel
.\launch.bat
```

### Run Tests
```powershell
cd build
ctest --output-on-failure
```

### Portable / Move to Another Machine
1. Copy the entire folder (delete `build/` to save space)
2. Install VS2022, Git, Python 3
3. `setup.bat` → `build.bat` → `launch.bat`

---

## Key Features

### 🎬 Editing
- Multi-track timeline with drag-and-drop, ripple/insert editing, and snapping
- Clip types: Video, Audio, Spine Animation, Title, Graphic (multi-layer), Image, Image Sequence, Adjustment Layer, Color Matte
- Full undo/redo via command pattern with compound operations
- Customizable keyboard shortcuts, JKL shuttle with pitch-preserved audio
- Program Monitor with transform overlay, safe areas, and zoom controls
- Source Monitor, Properties Panel, History Panel
- Revamped New Sequence dialog with improved presets and workflow

### 🦴 Spine Animation (Native)
- Integrated spine-cpp runtime for real-time Spine character rendering — no pre-rendering needed
- Shot Composer for multi-character scene layout with reusable presets
- Character Browser with network-based asset discovery, hide/rename entries, and context menu management
- Animation caching and prerendering pipeline for smooth playback

### 🎨 Effects & Color
- **Video effects:** Color Correct, Blur, Sharpen, Glow, Chroma Key (Ultra Key — overhauled with improved spill suppression and edge refinement), Transform 2D, Vignette, LUT, Letterbox, Lumetri Color (replaced legacy Color Grading), Color Matte
- **Audio effects:** Fill Left with Right, Fill Right with Left (applied during playback and export)
- **Transitions:** 35+ types — dissolves, wipes, pushes, slides, zooms, iris patterns, and more
- Essential Graphics panel for multi-layer graphic clip editing
- Keyframe editor with bezier curve interpolation

### 🔊 Audio
- Real-time mixer via PortAudio (WASAPI exclusive-mode, <5 ms latency)
- Per-track volume, pan, solo/mute with VU metering
- SoundTouch time-stretching for pitch-preserved speed changes
- Audio-driven A/V sync — the audio callback is the master clock
- Waveform caching with compact binary disk format
- AudioSync: context menus, auto-match improvements, script session management

### 🤖 AI Script Sync
- GPU-accelerated speech-to-text via whisper.cpp (models from Tiny to Large V3)
- Script parser supporting plain text, HTML/Google Docs, and JSON formats
- Fuzzy script-to-audio matching with sequential bias and timecode awareness
- Raw content persistence for reliable multi-session workflows
- Automatically aligns transcribed dialogue to your script — ideal for talk shows, interviews, and voiceover work

### 📦 Export
- H.264, H.265/HEVC, AV1, ProRes, DNxHR, Image Sequence
- NVENC hardware acceleration
- Smart render / packet passthrough when source matches export settings
- Audio mixdown with full effects processing

### 💾 Project Management
- `.rtp` project files with custom binary serialization
- Auto-save: timestamped copies in "Roundtable Auto-Save" folder (max 20, configurable)
- Crash recovery on next launch
- Relink Media dialog for moved/missing assets
- Recent files, project bin with thumbnails

### ⚡ GPU Pipeline
- Vulkan 1.3 compute shaders for compositing, effects, and transitions
- Optional CUDA for NVDEC hardware decoding / NVENC hardware encoding
- Disk frame cache (binary format) for decoded frame reuse across sessions
- Staging ring buffer for efficient CPU ↔ GPU transfers

---

## Tech Stack

| Component | Technology |
|-----------|-----------|
| Language | C++20 (MSVC 2022) |
| Graphics API | Vulkan 1.3 (via volk) |
| GPU Compute | CUDA 12.x (optional) |
| UI Framework | Qt 6.8.3 |
| Spine Runtime | spine-cpp 4.1 / 4.2 |
| Video | FFmpeg (SW decode + NVDEC) |
| Audio I/O | PortAudio (WASAPI), libsndfile, SoundTouch |
| Speech-to-Text | whisper.cpp |
| Math | GLM |
| Logging | spdlog |
| Configuration | toml11 |
| Testing | Google Test |
| Build System | CMake 3.28+ |

---

## Project Structure

```
src/
├── core/                 # Engine (no UI dependency)
│   ├── timeline/         # Timeline, Track, Clip, Keyframe models
│   ├── command/          # Undo/redo command pattern
│   ├── project/          # Serialization, asset database
│   ├── media/            # Decode, frame cache, audio engine, A/V sync
│   ├── spine/            # Spine runtime, model cache, animation
│   ├── effects/          # Effect stack, color grading, blur, LUT, channel fill
│   └── ai/               # whisper.cpp transcription, script matching
├── gpu/                  # Vulkan 1.3 + optional CUDA
│   ├── vulkan/           # Device management, pipelines, buffers, textures
│   ├── cuda/             # CUDA context, NVDEC/NVENC interop
│   ├── render_graph/     # Render-graph orchestration
│   └── (compositor)      # CompositeService/CompositeEngine — frame compositing, staging
├── ui/                   # Qt 6 interface
│   ├── panels/           # Timeline, monitors, effects, audio mixer, project bin
│   ├── dialogs/          # Preferences, shortcuts, project settings, relink media
│   ├── widgets/          # Ruler, transport controls, thumbnail grid, color picker
│   └── viewport/         # GPU viewport, transform overlay, safe areas
├── export/               # Encoder pipeline, muxer, smart render logic
│   └── formats/          # H.264, H.265, AV1, ProRes, DNxHR encoders
shaders/                  # GLSL compute shaders compiled to SPIR-V
tests/                    # Google Test unit tests
assets/                   # Characters, backgrounds, fonts, effect presets
```

---

## Workflow Overview

1. **Create or open a project** — `.rtp` files store all timeline, media, and effect state
2. **Import media** via the Project Bin — video, audio, images, Spine character files (.json + .atlas)
3. **Build your timeline** — drag clips onto the multi-track timeline, arrange with ripple/insert editing
4. **Compose Spine scenes** — use the Shot Composer to lay out multi-character animations with presets
5. **Apply effects & transitions** — drag from the Effects panel; keyframe parameters with bezier curves
6. **Sync audio to script** — transcribe with whisper.cpp, then run script matching to align dialogue
7. **Mix audio** — adjust per-track volume, pan, solo/mute with real-time VU feedback
8. **Export** — choose format and resolution; NVENC acceleration where available

---

## License

GNU Affero General Public License v3.0 — see [LICENSE.txt](LICENSE.txt) for details.

Copyright (C) 2025–2026 ROUNDTABLE TALK.  
This program is free software: you can redistribute it and/or modify it under the terms of the GNU Affero General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
