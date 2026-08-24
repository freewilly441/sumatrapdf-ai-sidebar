/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// AI-HOOK: local delivery mechanism for AI-generated study artifacts.
//
// This module is the "pipe" side of the export system: it knows nothing
// about LLM requests, prompts or the sidebar UI (see AiSidebarWnd.cpp for
// that — it builds these structs from a parsed LLM response and calls into
// here). All this module does is serialize NoteData/QuizData to the
// on-disk schema documented in docs/export-schema.md and write it to the
// configured export directory.
//
// v1 delivery is "write a JSON file to disk". Swapping that for e.g. a POST
// to a local HTTP endpoint or an Obsidian-vault writer later should only
// require changing ExportNote()/ExportQuiz()'s implementation, not their
// callers or the NoteData/QuizData shapes.

#pragma once

// Callers must include utils/BaseUtil.h before this header.

namespace AiExport {

// Schema version written into every exported file's "schema_version" field.
// Bump when the JSON shape changes in a way a consumer needs to branch on.
// See docs/export-schema.md.
constexpr int kSchemaVersion = 1;

// Where an exported artifact was generated from. All fields optional/best
// effort: filePath may be empty for documents with no path (shouldn't
// normally happen, the sidebar requires a loaded doc), selectionText is
// empty unless the user had a selection active at export time.
struct Source {
    str::Str filePath;
    int page{0};
    str::Str selectionText;
};

enum class NoteKind {
    Note,
    StudySheet,
};

// Owned: tags are heap-allocated (Vec<T> in this codebase doesn't run
// destructors for non-POD T, so str::Str can't be stored by value — same
// convention as AiChatTurn/AiRequest).
struct NoteData {
    NoteKind kind{NoteKind::Note};
    str::Str title;
    str::Str body;
    Vec<str::Str*> tags;
    Source source;

    ~NoteData() {
        for (str::Str* t : tags) {
            delete t;
        }
    }
};

struct QuizItem {
    str::Str question;
    str::Str answer;
    str::Str type; // "short_answer" | "multiple_choice"
    Vec<str::Str*> choices; // empty unless type == "multiple_choice"

    ~QuizItem() {
        for (str::Str* c : choices) {
            delete c;
        }
    }
};

struct QuizData {
    Vec<QuizItem*> items;
    Source source;

    ~QuizData() {
        for (QuizItem* it : items) {
            delete it;
        }
    }
};

// Directory exported files are written to: gGlobalPrefs->aiSettings.exportDir
// if set and non-empty, else an "AiExports" folder next to
// SumatraPDF-settings.txt. Does not create the directory (ExportNote/
// ExportQuiz do that via dir::CreateAll before writing).
TempStr GetExportDirTemp();

// Writes a "note"/"study_sheet" artifact (per note.kind) as a JSON file in
// the export directory (see docs/export-schema.md). Returns true on
// success. On failure, and if errorOut is non-null, fills it with a
// human-readable message. On success, if pathOut is non-null, fills it with
// the full path written.
bool ExportNote(const NoteData& note, str::Str* pathOut = nullptr, str::Str* errorOut = nullptr);

// Same contract as ExportNote, for a "quiz" artifact.
bool ExportQuiz(const QuizData& quiz, str::Str* pathOut = nullptr, str::Str* errorOut = nullptr);

} // namespace AiExport
