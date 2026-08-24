/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// AI-HOOK: local delivery mechanism for AI-generated study artifacts (v1:
// write a JSON file per artifact to the configured export directory). See
// AiExport.h for the module contract and docs/export-schema.md for the
// on-disk schema this writes.

#include "utils/BaseUtil.h"
#include "utils/FileUtil.h"
#include "utils/WinUtil.h"
#include "utils/Log.h"

#include "Settings.h"
#include "AppTools.h"
#include "GlobalPrefs.h"

#include "AiExport.h"

// ---------------------------------------------------------------------------
// Minimal JSON string escaping — deliberately independent from AiBridge.cpp's
// copy of the same ~15 lines. Both modules are small and meant to stay
// decoupled (AiExport doesn't know about AiBridge/LLM requests at all); this
// module never emits LLM-authored JSON verbatim; all field values here come
// from already-parsed, already-owned strings, so a shared JSON-writer header
// is not worth introducing yet.
// ---------------------------------------------------------------------------
static void JsonEscapeAppend(const char* s, str::Str& out) {
    if (!s) {
        return;
    }
    for (const char* p = s; *p; p++) {
        switch (*p) {
            case '"':
                out.Append("\\\"", 2);
                break;
            case '\\':
                out.Append("\\\\", 2);
                break;
            case '\n':
                out.Append("\\n", 2);
                break;
            case '\r':
                out.Append("\\r", 2);
                break;
            case '\t':
                out.Append("\\t", 2);
                break;
            default:
                if ((unsigned char)*p < 0x20) {
                    out.AppendFmt("\\u%04x", (unsigned int)*p);
                } else {
                    out.AppendChar(*p);
                }
                break;
        }
    }
}

static void JsonAppendStringField(str::Str& out, const char* key, const char* value, bool comma = true) {
    out.AppendChar('"');
    out.Append(key);
    out.Append("\":\"");
    JsonEscapeAppend(value, out);
    out.AppendChar('"');
    if (comma) {
        out.AppendChar(',');
    }
}

static void JsonAppendStringArray(str::Str& out, const char* key, const Vec<str::Str*>& items, bool comma = true) {
    out.AppendChar('"');
    out.Append(key);
    out.Append("\":[");
    for (size_t i = 0; i < items.len; i++) {
        if (i > 0) {
            out.AppendChar(',');
        }
        out.AppendChar('"');
        JsonEscapeAppend(items[i]->Get(), out);
        out.AppendChar('"');
    }
    out.AppendChar(']');
    if (comma) {
        out.AppendChar(',');
    }
}

// ---------------------------------------------------------------------------
// created_at / filename helpers
// ---------------------------------------------------------------------------

// ISO 8601 UTC, e.g. "2026-08-24T15:30:12Z".
static void FormatIso8601Utc(str::Str& out) {
    SYSTEMTIME st{};
    GetSystemTime(&st);
    out.AppendFmt("%04d-%02d-%02dT%02d:%02d:%02dZ", (int)st.wYear, (int)st.wMonth, (int)st.wDay, (int)st.wHour,
                  (int)st.wMinute, (int)st.wSecond);
}

// Filename-safe timestamp, e.g. "20260824-153012".
static void FormatTimestampForFileName(str::Str& out) {
    SYSTEMTIME st{};
    GetSystemTime(&st);
    out.AppendFmt("%04d%02d%02d-%02d%02d%02d", (int)st.wYear, (int)st.wMonth, (int)st.wDay, (int)st.wHour,
                  (int)st.wMinute, (int)st.wSecond);
}

// Lowercases, replaces anything that isn't [a-z0-9] with '-', collapses runs
// of '-', trims leading/trailing '-', caps length. Empty/all-punctuation
// titles fall back to "untitled".
static void SlugifyForFileName(const char* title, str::Str& out) {
    bool lastWasDash = false;
    for (const char* p = title; p && *p && out.size() < 60; p++) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        bool isAlNum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (isAlNum) {
            out.AppendChar(c);
            lastWasDash = false;
        } else if (!lastWasDash && out.size() > 0) {
            out.AppendChar('-');
            lastWasDash = true;
        }
    }
    while (out.size() > 0 && out.Get()[out.size() - 1] == '-') {
        out.RemoveAt(out.size() - 1, 1);
    }
    if (out.size() == 0) {
        out.Append("untitled");
    }
}

// ---------------------------------------------------------------------------
// Export directory
// ---------------------------------------------------------------------------

namespace AiExport {

TempStr GetExportDirTemp() {
    char* configured = gGlobalPrefs ? gGlobalPrefs->aiSettings.exportDir : nullptr;
    if (!str::IsEmptyOrWhiteSpace(configured)) {
        return str::DupTemp(configured);
    }
    return GetPathInAppDataDirTemp("AiExports");
}

static void AppendSourceJson(str::Str& out, const Source& source) {
    out.Append("\"source\":{");
    JsonAppendStringField(out, "filePath", source.filePath.Get());
    out.AppendFmt("\"page\":%d,", source.page);
    JsonAppendStringField(out, "selectionText", source.selectionText.Get(), false);
    out.Append("},");
}

static bool WriteJsonFile(const char* fileNamePrefix, const char* title, const str::Str& json, str::Str* pathOut,
                          str::Str* errorOut) {
    TempStr dir = GetExportDirTemp();
    if (!dir::CreateAll(dir)) {
        if (errorOut) {
            errorOut->AppendFmt("Could not create export directory: %s", dir);
        }
        return false;
    }

    str::Str slug;
    if (!str::IsEmptyOrWhiteSpace(title)) {
        SlugifyForFileName(title, slug);
    }
    str::Str timestamp;
    FormatTimestampForFileName(timestamp);
    str::Str fileName;
    fileName.Append(fileNamePrefix);
    if (slug.size() > 0) {
        fileName.AppendChar('-');
        fileName.Append(slug.Get());
    }
    fileName.AppendChar('-');
    fileName.Append(timestamp.Get());
    fileName.Append(".json");

    TempStr path = path::JoinTemp(dir, fileName.Get());
    ByteSlice data((const u8*)json.Get(), json.size());
    if (!file::WriteFile(path, data)) {
        if (errorOut) {
            errorOut->AppendFmt("Could not write file: %s", path);
        }
        return false;
    }

    logf("AiExport: wrote %s\n", path);
    if (pathOut) {
        pathOut->Set(path);
    }
    return true;
}

bool ExportNote(const NoteData& note, str::Str* pathOut, str::Str* errorOut) {
    const char* type = (note.kind == NoteKind::StudySheet) ? "study_sheet" : "note";
    const char* fileNamePrefix = (note.kind == NoteKind::StudySheet) ? "study-sheet" : "note";

    str::Str json;
    json.AppendFmt("{\"schema_version\":%d,", kSchemaVersion);
    JsonAppendStringField(json, "type", type);
    JsonAppendStringField(json, "title", note.title.Get());
    JsonAppendStringField(json, "body", note.body.Get());
    JsonAppendStringArray(json, "tags", note.tags);
    AppendSourceJson(json, note.source);
    str::Str createdAt;
    FormatIso8601Utc(createdAt);
    JsonAppendStringField(json, "created_at", createdAt.Get(), false);
    json.AppendChar('}');

    return WriteJsonFile(fileNamePrefix, note.title.Get(), json, pathOut, errorOut);
}

bool ExportQuiz(const QuizData& quiz, str::Str* pathOut, str::Str* errorOut) {
    str::Str json;
    json.AppendFmt("{\"schema_version\":%d,", kSchemaVersion);
    JsonAppendStringField(json, "type", "quiz");
    json.Append("\"questions\":[");
    for (size_t i = 0; i < quiz.items.len; i++) {
        if (i > 0) {
            json.AppendChar(',');
        }
        QuizItem* it = quiz.items[i];
        json.AppendChar('{');
        JsonAppendStringField(json, "question", it->question.Get());
        JsonAppendStringField(json, "answer", it->answer.Get());
        bool hasChoices = it->choices.len > 0;
        JsonAppendStringField(json, "type", it->type.Get(), hasChoices);
        if (hasChoices) {
            JsonAppendStringArray(json, "choices", it->choices, false);
        }
        json.AppendChar('}');
    }
    json.Append("],");
    AppendSourceJson(json, quiz.source);
    str::Str createdAt;
    FormatIso8601Utc(createdAt);
    JsonAppendStringField(json, "created_at", createdAt.Get(), false);
    json.AppendChar('}');

    return WriteJsonFile("quiz", nullptr, json, pathOut, errorOut);
}

} // namespace AiExport
