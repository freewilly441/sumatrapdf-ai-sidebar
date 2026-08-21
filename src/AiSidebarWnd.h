/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// AI-HOOK: persistent, dockable AI chat sidebar.
// Phase 2: replaces the Phase 1 LlmResponseWnd selection popup. Follows the
// same container-HWND + subclass pattern as TableOfContents.cpp/Favorites.cpp
// (a WC_STATIC box holding a LabelWithCloseWnd header + child controls),
// rather than introducing a one-off window class, so it docks and resizes
// the same way the existing ToC/Favorites panels do.

#pragma once

// Callers must include utils/BaseUtil.h (and utils/ThreadUtil.h, transitively
// required by AiRequest.h) before this header, same as AiBridge.h.
#include "AiRequest.h"

struct MainWindow;

// Registers the WM_AI_RESPONSE_DONE/WM_AI_ERROR-aware window class pieces
// this panel depends on. Call once at startup (mirrors
// LlmResponseWnd::RegisterWindowClass in Phase 1; kept as a free function
// name for call-site continuity, even though today it's a no-op beyond
// asserting the label control class is available).
namespace AiSidebarWnd {
void RegisterWindowClass(HINSTANCE hInstance);
}

// Creates hwndAiBox and its children (label, mode dropdown, history view,
// input box, send button); mirrors CreateToc()/CreateFavorites().
void CreateAiSidebar(MainWindow* win);

// Shows/hides the AI sidebar, persists the choice to
// gGlobalPrefs->aiSettings.sidebarVisible, and refreshes the conversation
// display for the current tab if becoming visible.
void SetAiSidebarVisibility(MainWindow* win, bool visible);

// Toggles visibility (bound to the close button and CmdToggleAiSidebar).
void ToggleAiSidebar(MainWindow* win);

// Refreshes the history view from win->CurrentTab()->aiConversation.
// Call on tab switch/document load, same spot LoadTocTree() is called.
void LoadAiConversationIntoSidebar(MainWindow* win);

// Positions the header/dropdown/history/input/send controls within
// win->hwndAiBox; called from the container's WM_SIZE subclass handler.
void LayoutAiContainer(MainWindow* win);

// Sends the text currently in the input box as a request using the mode
// selected in the dropdown ("type" is Chat for freeform sidebar messages,
// or Define/Explain for the quick-action commands below). Appends the
// user's turn to the conversation immediately; the AI's turn is appended
// when the response (or error) arrives via OnAiResponseDone().
void SubmitAiChatMessage(MainWindow* win, AiRequestType type = AiRequestType::Chat);

// Entry point for the "Ask AI...", "Define" and "Explain Selection" context
// menu commands (CmdAiAskSelection/CmdAiDefine/CmdAiExplain). Makes sure the
// sidebar is visible and in Selection mode; Define/Explain auto-submit a
// canned instruction, AskSelection just focuses the input for the user to
// type a freeform question.
void AiQuickAction(MainWindow* win, AiRequestType type);

// Handles WM_AI_RESPONSE_DONE/WM_AI_ERROR, posted to win->hwndAiBox.
// Takes ownership of msgPtr (AiResponseMsg*) and deletes it before returning.
void OnAiResponseDone(HWND aiBoxHwnd, WPARAM requestId, LPARAM msgPtr);
