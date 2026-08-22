[![Build](https://github.com/sumatrapdfreader/sumatrapdf/actions/workflows/build.yml/badge.svg?branch=master)](https://github.com/sumatrapdfreader/sumatrapdf/actions/workflows/build.yml)
## SumatraPDF Reader

SumatraPDF is a multi-format (PDF, EPUB, MOBI, CBZ, CBR, FB2, CHM, XPS, DjVu) reader
for Windows under (A)GPLv3 license, with some code under BSD license (see
AUTHORS).

More Information:
* [Website](https://www.sumatrapdfreader.org/free-pdf-reader)
* [Manual](https://www.sumatrapdfreader.org/manual)
* [Developer Information](https://www.sumatrapdfreader.org/docs/Contribute-to-SumatraPDF)

---

## AI Chat Sidebar (this fork)

This fork of SumatraPDF adds a persistent, dockable **AI chat sidebar** —
developed by **Aethermark Systems LLC** — that lets you ask questions about
the document you're reading. It's backed entirely by a local
[Ollama](https://ollama.com) instance: no document content, selections, or
questions are sent anywhere else. See [NOTICE.md](NOTICE.md) for what
specifically changed relative to upstream SumatraPDF.

### What it does

- A dockable panel on the right side of the window (View menu → **Show AI
  Chat Sidebar**, or right-click a selection)
- Three context modes, switchable from a dropdown in the panel:
  - **Selection** — ask about the currently selected text
  - **Page** — ask about the current page
  - **Document** — ask about the whole document (capped to a few thousand
    characters for now; full chunking/summarization for very large PDFs is
    still a TODO)
- Per-document conversation history — switching tabs shows each document's
  own conversation
- Quick actions on selected text via the right-click context menu: **Ask
  AI...**, **Define**, **Explain Selection**

### Requirements

- Windows (this fork inherits upstream SumatraPDF's Windows-only scope —
  there is no macOS/Linux build)
- [Ollama](https://ollama.com) installed and running
- At least one model pulled, e.g.:
  ```
  ollama pull llama3.2
  ```

### Configuration

Settings are stored in `SumatraPDF-settings.txt`, in the same directory as
the executable (portable installs) or in `%APPDATA%\SumatraPDF\` (installed
mode). Look for the `AiSettings` section:

```
AiSettings [
	Backend = ollama
	OllamaHost = http://localhost:11434
	OllamaModel = llama3.2
	SidebarVisible = false
	SidebarDx = 0
]
```

Edit `OllamaHost` or `OllamaModel` to point at a different Ollama instance or
model, then restart SumatraPDF. There is no in-app settings UI for this yet —
it's a text file edit.

### Usage

1. Open a PDF (or any format SumatraPDF supports)
2. **View menu → Show AI Chat Sidebar** to open the panel, or select some
   text and right-click for a quick action
3. Pick a mode (Selection / Page / Document), type a question, and press
   **Send**
4. If Ollama isn't reachable, the sidebar reports that instead of a response,
   and the selection quick-actions won't appear in the right-click menu

### Building from source

If you'd rather build it yourself than trust a prebuilt binary:

1. Install [Bun](https://bun.sh)
2. Install Visual Studio (2022 or newer) with the "Desktop development with
   C++" workload, so `cl.exe`/`msbuild.exe` are available
3. From a Developer Command Prompt (or with those tools on `PATH`):
   ```
   bun ./cmd/build.ts
   ```
   This produces a **Debug** build at `./out/dbg64/SumatraPDF.exe`. For an
   optimized Release build, invoke msbuild directly instead:
   ```
   msbuild .\vs2022\SumatraPDF.sln /t:SumatraPDF "/p:Configuration=Release;Platform=x64" /m
   ```

A MinGW-w64 cross-compilation path also exists
(`bun cmd/build-with-mingw.ts -debug`) for iterating on non-Windows
machines, with some known divergences from the MSVC build (see commit
history for details) — use the MSVC build above for anything you intend to
actually run day to day.
