/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// AI-HOOK: New file - Phase 1 LLM inline response popup window

#pragma once

#include "AiRequest.h"

// ---------------------------------------------------------------------------
// LlmResponseWnd
//
// A lightweight, borderless popup window that appears near the text selection
// and displays the AI response. Follows SumatraPDF's NotificationWnd pattern.
//
// Lifecycle:
//   Created by:  OnAiResponseDone() in Canvas.cpp (or SumatraPDF.cpp)
//   Dismissed by: Esc key, click-outside (WM_KILLFOCUS), or document change
//   Destroyed by: DestroyWindow() — WndProc handles WM_DESTROY cleanup
//
// Phase 1: Displays full response text (no streaming).
// Phase 2: Will support token-by-token append via AppendToken().
// ---------------------------------------------------------------------------
class LlmResponseWnd {
  public:
    // Create and show the popup near anchorRect (screen coordinates).
    // parentHwnd: the canvas HWND (used for positioning and focus tracking).
    // requestId: the id of the request this window was opened for.
    //            Used for stale-drop: WM_AI_RESPONSE_DONE with a different id is ignored.
    static LlmResponseWnd* Create(HWND parentHwnd, RECT anchorRect, uint32_t requestId);

    // Set the full response text and repaint.
    // Phase 1: called once when WM_AI_RESPONSE_DONE arrives.
    void SetResponseText(const char* text);

    // Show an error state (red tint, error message).
    void SetError(const char* errorMsg);

    // Dismiss and destroy the window.
    void Dismiss();

    // Returns the request id this window was opened for.
    uint32_t GetRequestId() const { return mRequestId; }

    HWND GetHwnd() const { return mHwnd; }

    // Register window class. Call once at app startup.
    static bool RegisterWindowClass(HINSTANCE hInst);

  private:
    LlmResponseWnd() = default;
    ~LlmResponseWnd() = default;

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    void OnPaint(HDC hdc, RECT& clientRect);
    void OnKeyDown(WPARAM vk);
    void RepositionNearAnchor();

    HWND      mHwnd{nullptr};
    HWND      mParentHwnd{nullptr};
    uint32_t  mRequestId{0};
    RECT      mAnchorRect{};   // screen rect of selection
    str::Str  mResponseText;
    bool      mIsError{false};
    bool      mHasResponse{false};

    // Window dimensions (auto-sized to text in Phase 2; fixed in Phase 1)
    static constexpr int kMaxWidth  = 420;
    static constexpr int kMaxHeight = 260;
    static constexpr int kPadding   = 12;

    static const WCHAR* kClassName;
};

// ---------------------------------------------------------------------------
// Global active popup (only one at a time in Phase 1).
// Created and destroyed by OnAiResponseDone / tab switch / doc close handlers.
// AI-HOOK: extern declared here; defined in LlmResponseWnd.cpp
// ---------------------------------------------------------------------------
extern LlmResponseWnd* gActiveLlmResponseWnd;

// ---------------------------------------------------------------------------
// Called from WM_AI_RESPONSE_DONE / WM_AI_ERROR handler in Canvas.cpp.
// Verifies request_id matches, creates or updates the popup window.
//
// OWNERSHIP: takes ownership of msg and deletes it before returning.
// ---------------------------------------------------------------------------
void OnAiResponseDone(HWND canvasHwnd, WPARAM requestId, LPARAM msgPtr);

// ---------------------------------------------------------------------------
// Called on tab switch, document close, or new selection to invalidate popup.
// ---------------------------------------------------------------------------
void DismissActiveLlmPopup();
