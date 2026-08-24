/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// AI-HOOK: LLM integration request types and lifecycle.
// Phase 2: request/response shapes for the persistent chat sidebar
// (superseded the Phase 1 selection popup; see AiSidebarWnd.h).

#pragma once

// ---------------------------------------------------------------------------
// Request lifecycle state machine
//
//   PENDING --> STREAMING --> COMPLETE
//      |            |
//      v            v
//   CANCELED    CANCELED
//      |
//      v
//    FAILED
//    STALE  (context invalidated: tab switch, doc close, window destroy)
// ---------------------------------------------------------------------------
enum class AiRequestState {
    Pending,   // enqueued, not yet sent to backend
    Streaming, // tokens arriving (not yet implemented; Ollama call is non-streaming for now)
    Complete,  // full response received and posted to UI
    Failed,    // backend returned error or HTTP failed
    Canceled,  // explicitly canceled (new selection, window close, etc.)
    Stale,     // target window or document changed before response arrived
};

// Drives instruction-prefix selection in AiBridge::BuildMessages().
//
// Note/StudySheet/Quiz are export requests (AI-HOOK: export system): a
// *second* LLM call, triggered from the sidebar's export buttons, that asks
// the model to reformat the conversation so far into a structured JSON
// payload instead of a chat reply. They share AiBridge's queue/HTTP
// transport and this enum (for prompt selection), but are dispatched to a
// separate response handler in AiSidebarWnd.cpp — see
// WindowTab::aiPendingExportRequestId — never appended to aiConversation as
// a chat turn. See AiExport.h for the resulting on-disk schema.
enum class AiRequestType {
    Chat,       // freeform message typed in the sidebar (any context mode)
    Define,     // quick action: single-word inline definition of the selection
    Explain,    // quick action: explain the current selection
    Note,       // export: reformat conversation into a note (title/body/tags)
    StudySheet, // export: reformat conversation into a study sheet (same shape as Note)
    Quiz,       // export: generate quiz questions from the conversation
};

// Which part of the document a chat turn's context comes from.
// Selected in the sidebar's mode DropDown; drives text extraction in
// AiSidebarWnd::BuildContextText().
enum class AiContextMode {
    Selection, // currently selected text on the page
    Page,      // full text of the current page
    Document,  // text of the whole document (chunked/capped for large PDFs)
};

// ---------------------------------------------------------------------------
// AiResponseMsg
//
// Heap-allocated; ownership transfers from bridge thread to UI thread
// at the PostMessage() boundary. UI thread is responsible for deletion.
// See AiBridge.cpp: ownership contract comment.
// ---------------------------------------------------------------------------
struct AiResponseMsg {
    uint32_t    requestId{0};
    str::Str    text; // complete response text (no token streaming yet)
    bool        isError{false};
};

// ---------------------------------------------------------------------------
// AiRequest
//
// Created on UI thread, consumed on bridge thread. All context needed to
// build the outgoing message is copied by value at enqueue time (userMessage,
// contextText, historyText) so the bridge thread never touches WindowTab or
// other UI-owned objects, which could be destroyed while the request is
// in flight.
//
// Bridge thread must check state == Pending before processing;
// if Canceled or Stale, drop without sending to the backend.
// ---------------------------------------------------------------------------
struct AiRequest {
    uint32_t       id{0};
    AiRequestType  type{AiRequestType::Chat};
    AiContextMode  contextMode{AiContextMode::Selection};
    str::Str       userMessage; // the new message/question to send
    str::Str       contextText; // extracted selection/page/document text; may be empty
    str::Str       historyText; // prior conversation turns, pre-formatted; may be empty
    HWND           targetHwnd{nullptr}; // AI sidebar HWND - post response here
    AiRequestState state{AiRequestState::Pending};
};

// ---------------------------------------------------------------------------
// A single turn in the sidebar's conversation history (owned by WindowTab so
// each document keeps its own conversation across tab switches).
// ---------------------------------------------------------------------------
struct AiChatTurn {
    bool     isUser{false}; // true: user message, false: AI response (or error)
    bool     isError{false};
    str::Str text;
};

// ---------------------------------------------------------------------------
// WM_USER message IDs for bridge -> UI communication
//
// AI-HOOK: These must not conflict with other WM_USER+N values in the app.
// Current SumatraPDF uses WM_USER+0 through ~+20 range; we start at +100.
// ---------------------------------------------------------------------------
// Existing WM_USER usage in this codebase:
//   WM_USER+101 : kUwmPaintAgain  (previewer, separate HWND)
//   WM_USER+102 : SB_HALF_PAGEUP  (SumatraPDF.h)
//   WM_USER+103 : SB_HALF_PAGEDOWN(SumatraPDF.h)
// We start at +200 to give clear separation.
constexpr UINT WM_AI_RESPONSE_DONE = WM_USER + 200;
// lParam = AiResponseMsg* (isError=false); UI thread takes ownership, must delete
// wParam = request_id (verify against active request before rendering)

constexpr UINT WM_AI_ERROR = WM_USER + 201;
// lParam = AiResponseMsg* (isError=true); same ownership rules
// wParam = request_id
