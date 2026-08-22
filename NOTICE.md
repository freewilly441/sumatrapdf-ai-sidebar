# Notice

This repository is a fork of [SumatraPDF](https://github.com/sumatrapdfreader/sumatrapdf).

## Original work

SumatraPDF is Copyright the SumatraPDF project authors (see [AUTHORS](AUTHORS))
and is licensed under the GNU General Public License v3 (GPLv3), with some
components under a BSD-style license — see [COPYING](COPYING) and
[COPYING.BSD](COPYING.BSD) respectively. The original project is developed at
<https://github.com/sumatrapdfreader/sumatrapdf> and
<https://www.sumatrapdfreader.org>.

This fork carries the original GPLv3 license forward unmodified (see
[LICENSE](LICENSE) / [COPYING](COPYING)). Nothing in this fork relicenses,
weakens, or adds conflicting terms to any part of the original work.

## Fork-specific work

The AI chat sidebar feature in this fork — a dockable panel that lets a
reader ask questions about a PDF's current selection, page, or full document
via a local [Ollama](https://ollama.com) instance — was developed by
**[Aethermark Systems LLC](https://aethermarksystems.com)**, as a
modification to SumatraPDF distributed under the same GPLv3 terms as the
original.

High-level summary of what changed for this feature:

**New files:**
- `src/AiBridge.h` / `src/AiBridge.cpp` — async request queue and HTTP bridge
  to a local Ollama instance (`/api/chat`)
- `src/AiRequest.h` — request/response types and the chat-turn/state-machine
  shapes shared between the bridge and the sidebar UI
- `src/AiSidebarWnd.h` / `src/AiSidebarWnd.cpp` — the dockable AI chat sidebar
  panel (selection/page/document mode switching, conversation history, input)

**Modified files (non-exhaustive; see git history for full detail):**
- `src/Menu.cpp` — added and gated the "Ask AI...", "Define", and "Explain
  Selection" selection context-menu commands
- `src/Canvas.cpp` — minor wiring adjustments for the sidebar
- `src/SumatraPDF.cpp` — window layout changes to dock the sidebar, command
  dispatch for the new AI commands
- `src/SumatraStartup.cpp` — starts the Ollama bridge at startup, reading
  configuration from settings instead of hardcoded paths
- `src/Settings.h` — added `AiSettings` (Ollama host/model, sidebar
  visibility/width) to the persisted settings schema
- `src/MainWindow.h` / `src/MainWindow.cpp`, `src/WindowTab.h` /
  `src/WindowTab.cpp` — added sidebar widget ownership and per-document
  conversation history
- `cmd/build-with-mingw.ts`, `premake5.files.lua`, `vs2022/SumatraPDF.vcxproj`
  — build-system updates so the new files are included in every build path

No data from documents processed by the AI chat sidebar is sent anywhere
other than the Ollama instance configured in settings (`http://localhost:11434`
by default) — there is no telemetry, cloud API, or third-party service
involved in this feature.
