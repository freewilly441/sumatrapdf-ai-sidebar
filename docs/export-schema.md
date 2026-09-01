# AI export schema

AI-HOOK: this document is the contract for the "export" pipe described in
`src/AiExport.h` — the on-disk format any future consuming app (a
dedicated note-taking app, a script, Obsidian via a plugin, etc.) should
read. Treat it as an external API: changes should bump `schema_version`
rather than silently altering the shape of existing fields.

## Where files live

Exported files are written as individual `*.json` files in a directory
configured by `Settings > AiSettings > ExportDir` (see
`gGlobalPrefs->aiSettings.exportDir` in `src/Settings.h`). If unset, they
go to an `AiExports` folder next to `SumatraPDF-settings.txt` (i.e. in the
app's per-user AppData directory — see `AiExport::GetExportDirTemp()` in
`src/AiExport.cpp`). The directory is created on first write if it doesn't
exist.

Filenames are `<kind>-<title-slug>-<timestamp>.json` (quiz omits the slug),
e.g. `note-photosynthesis-overview-20260824-153012.json`. Filenames are
not part of the contract — a consumer should not parse them for metadata
that's already in the file (`type`, `created_at`, etc.); the timestamp
suffix only exists to keep runs from colliding.

## Format choice: JSON, not markdown+frontmatter (for now)

v1 writes raw JSON rather than markdown-with-YAML-frontmatter. Reasoning:

- These files are *constructed by SumatraPDF from already-parsed fields*,
  never handed through verbatim from the model — see
  `AiSidebarWnd.cpp`'s `ParseNoteFromLlmText`/`ParseQuizFromLlmText`. JSON
  we build ourselves is trivially well-formed; there's no YAML-escaping
  surface to get wrong.
- The schema explicitly needs typed/nested data (a `tags` array, a
  `questions` array of objects with an optional `choices` array) that maps
  onto JSON far more naturally than frontmatter, which is really designed
  for flat scalar metadata with a single markdown body underneath.
- A quiz especially doesn't have one natural "body" — it's structured data
  through and through.

See the Phase 4 assessment (this file's companion note in the PR/commit
history, or ask the assistant) for the tradeoff against markdown+frontmatter
if the eventual consumer turns out to be Obsidian itself; that may justify
adding a markdown mirror later without changing this JSON schema.

## Versioning

Every file has a top-level `"schema_version"` integer (currently `1`,
`AiExport::kSchemaVersion` in `src/AiExport.h`). Bump it when a change
would require a consumer to branch on it (renaming/removing a field,
changing a field's type or meaning). Purely additive fields (a new
optional key) don't require a bump but should be documented here.

## Common `source` object

Both artifact types embed the same `source` object describing where the
content came from:

```jsonc
"source": {
  "filePath": "C:\\Users\\...\\paper.pdf", // path of the open document
  "page": 4,                                // current page at export time, 0 if not meaningful
  "selectionText": ""                       // selected text at export time, may be empty
}
```

## Note / Study Sheet

`"type"` is `"note"` or `"study_sheet"` — the two share one shape; a study
sheet just asks the model for headings/bullet-point-organized content
instead of a short summary (see the two system prompts in
`AiBridge::BuildMessagesJson`).

```jsonc
{
  "schema_version": 1,
  "type": "note",                 // or "study_sheet"
  "title": "Photosynthesis overview",
  "body": "Plants convert light energy...\n\n- Key point 1\n- Key point 2",
  "tags": ["biology", "photosynthesis", "energy"],
  "source": { "filePath": "...", "page": 4, "selectionText": "" },
  "created_at": "2026-08-24T15:30:12Z"   // ISO 8601, UTC
}
```

- `title`: string, always non-empty (falls back to `"Untitled note"` if the
  model's response couldn't be parsed as JSON — see below).
- `body`: string. Plain text with `\n` line breaks; may contain `- ` bullet
  markers as literal text (not markdown-rendered by this app). Free-form,
  no further structure guaranteed.
- `tags`: array of short lowercase strings. May be empty if the model
  didn't produce a parseable `tags` array.

## Quiz

```jsonc
{
  "schema_version": 1,
  "type": "quiz",
  "questions": [
    {
      "question": "What pigment absorbs light in photosynthesis?",
      "answer": "Chlorophyll",
      "type": "short_answer"
    },
    {
      "question": "Which of these is a product of photosynthesis?",
      "answer": "Oxygen",
      "type": "multiple_choice",
      "choices": ["Oxygen", "Nitrogen", "Methane", "Ozone"]
    }
  ],
  "source": { "filePath": "...", "page": 0, "selectionText": "" },
  "created_at": "2026-08-24T15:31:02Z"
}
```

- `questions`: non-empty array (a response that yields zero questions is
  treated as a failure and no file is written — see
  `OnAiExportResponseDone` in `AiSidebarWnd.cpp`).
- `type` per question: `"short_answer"` or `"multiple_choice"`.
- `choices`: present only when `type` is `"multiple_choice"`; absent
  otherwise (not an empty array).

## How the LLM's raw response maps to this schema

The model is prompted (see `AiBridge::BuildMessagesJson`) to reply with a
single raw JSON object matching a shape close to (but not identical to —
no `schema_version`, `source`, `created_at`, or note/study_sheet framing)
the final files above. Small local models don't always follow the
instruction exactly — extra prose, a wrapping ` ```json ` code fence, or a
missing field are all observed in practice — so
`AiSidebarWnd.cpp`'s parsers are deliberately permissive:

- A leading/trailing markdown code fence is stripped before parsing.
- Parsing uses the project's existing streaming JSON parser
  (`src/utils/JsonParser.h`), which visits `(path, value)` pairs rather
  than requiring a strict schema match, so extra/reordered fields don't
  break parsing.
- If a note/study-sheet response can't be parsed as JSON at all, the whole
  raw response text becomes the `body` and the title falls back to
  `"Untitled note"` rather than losing the content.
- If a quiz response yields no parseable questions, nothing is written and
  the sidebar reports a failure instead of producing an empty/misleading
  file.

This means **the schema above is always well-formed on disk** even when
the model's own output wasn't — the LLM's JSON is a prompting convention,
not the contract; this document (and the JSON actually written by
`src/AiExport.cpp`) is the contract.
