/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// AI-HOOK: New file - Phase 1 LLM integration request types and lifecycle

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
    Pending,   // enqueued, not yet sent to sidecar
    Streaming, // tokens arriving (Phase 2+; unused in Phase 1)
    Complete,  // full response received and posted to UI
    Failed,    // sidecar returned error or HTTP failed
    Canceled,  // explicitly canceled (new selection, window close, etc.)
    Stale,     // target window or document changed before response arrived
};

// Drives prompt template selection in AiBridge::BuildPrompt()
enum class AiRequestType {
    Ask,     // freeform "Ask AI about this selection"
    Define,  // single-word inline definition
    Explain, // multi-word / paragraph explanation
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
    str::Str    text;      // complete response text (Phase 1: always full)
    bool        isError{false};
    // Phase 1: no token streaming; text is always complete on WM_AI_RESPONSE_DONE
};

// ---------------------------------------------------------------------------
// AiRequest
//
// Created on UI thread, consumed on bridge thread.
// Bridge thread must check state == Pending before processing;
// if Canceled or Stale, drop without sending to sidecar.
// ---------------------------------------------------------------------------
struct AiRequest {
    uint32_t       id{0};
    AiRequestType  type{AiRequestType::Ask};
    str::Str       selectedText;
    str::Str       pageContext;  // current page text; may be empty
    HWND           targetHwnd{nullptr};  // canvas HWND - post response here
    RECT           anchorRect{};         // screen rect of selection for popup
    AiRequestState state{AiRequestState::Pending};
};

// ---------------------------------------------------------------------------
// WM_USER message IDs for bridge -> UI communication
//
// AI-HOOK: These must not conflict with other WM_USER+N values in the app.
// Current SumatraPDF uses WM_USER+0 through ~+20 range; we start at +100.
// ---------------------------------------------------------------------------
constexpr UINT WM_AI_RESPONSE_DONE = WM_USER + 100;
// lParam = AiResponseMsg* (isError=false); UI thread takes ownership, must delete
// wParam = request_id (verify against active request before rendering)

constexpr UINT WM_AI_ERROR = WM_USER + 101;
// lParam = AiResponseMsg* (isError=true); same ownership rules
// wParam = request_id
