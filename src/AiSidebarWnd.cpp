/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// AI-HOOK: persistent, dockable AI chat sidebar implementation.
// Structure mirrors TableOfContents.cpp/Favorites.cpp: a WC_STATIC container
// (win->hwndAiBox) subclassed for WM_SIZE, holding a LabelWithCloseWnd header
// plus this panel's own controls (mode dropdown, history view, input, send).

#include "utils/BaseUtil.h" // already pulls in utils/StrconvUtil.h (ToUtf8); don't include it again, it has no include guard
#include "utils/ThreadUtil.h" // Mutex/AtomicInt, required before AiBridge.h
#include "utils/ScopedWin.h"
#include "utils/WinUtil.h"

#include "wingui/UIModels.h"
#include "wingui/Layout.h"
#include "wingui/WinGui.h"
#include "wingui/LabelWithCloseWnd.h"

#include "Settings.h"
#include "AppSettings.h"
#include "DocController.h"
#include "EngineBase.h"
#include "GlobalPrefs.h"
#include "Annotation.h"
#include "SumatraPDF.h"
#include "MainWindow.h"
#include "DisplayModel.h"
#include "WindowTab.h"
#include "Selection.h"
#include "resource.h"
#include "Commands.h"
#include "Translations.h"

#include "AiRequest.h"
#include "AiBridge.h"
#include "AiSidebarWnd.h"

#include "utils/Log.h"

namespace AiSidebarWnd {
void RegisterWindowClass(HINSTANCE) {
    // No custom window class to register: the panel is built entirely out of
    // stock controls (WC_STATIC container + LabelWithCloseWnd/DropDown/Edit/
    // Button), same as the ToC/Favorites panels. Kept as a call-site-compatible
    // no-op so SumatraStartup.cpp's startup sequence doesn't need special-casing.
}
} // namespace AiSidebarWnd

// ---------------------------------------------------------------------------
// Context extraction (selection/page/document) — reuses the same engine and
// selection APIs the search/selection/copy features already use.
// ---------------------------------------------------------------------------

// Document mode sends whole-document text to a small local model, which
// doesn't have a large context window. Cap how much we extract so a huge
// PDF doesn't produce a multi-megabyte prompt or block the UI thread for a
// long time walking every page.
// TODO: replace with real chunking (summarize-then-ask over page ranges)
// once a document is bigger than this budget; for now the tail of large
// documents is simply not sent.
constexpr size_t kDocContextCharBudget = 12000;

static void BuildContextText(WindowTab* tab, AiContextMode mode, str::Str& out) {
    if (!tab || !tab->ctrl) {
        return;
    }
    switch (mode) {
        case AiContextMode::Selection: {
            bool isTextOnly = false;
            TempStr sel = GetSelectedTextTemp(tab, "\n", isTextOnly);
            if (sel) {
                out.Append(sel);
            }
        } break;

        case AiContextMode::Page: {
            DisplayModel* dm = tab->AsFixed();
            if (!dm) {
                break;
            }
            int pageNo = tab->ctrl->CurrentPageNo();
            PageText pt = dm->GetEngine()->ExtractPageText(pageNo);
            if (pt.text) {
                AutoFreeStr utf8(ToUtf8(pt.text));
                out.Append(utf8.Get());
            }
            FreePageText(&pt);
        } break;

        case AiContextMode::Document: {
            DisplayModel* dm = tab->AsFixed();
            if (!dm) {
                break;
            }
            EngineBase* engine = dm->GetEngine();
            int n = engine->PageCount();
            for (int i = 1; i <= n && out.size() < kDocContextCharBudget; i++) {
                PageText pt = engine->ExtractPageText(i);
                if (pt.text) {
                    AutoFreeStr utf8(ToUtf8(pt.text));
                    out.Append(utf8.Get());
                    out.Append("\n");
                }
                FreePageText(&pt);
            }
        } break;
    }
}

// Flattens the conversation to "You: ...\n\nAI: ...\n\n" text. With
// charBudget == (size_t)-1 (default) returns the whole conversation, for
// display; with a real budget, keeps only the most recent turns that fit,
// for the (smaller) prompt sent to the model.
static void FormatConversationText(WindowTab* tab, str::Str& out, size_t charBudget = (size_t)-1) {
    if (!tab) {
        return;
    }
    Vec<AiChatTurn*>& conv = tab->aiConversation;

    size_t start = 0;
    if (charBudget != (size_t)-1) {
        size_t total = 0;
        for (size_t i = conv.len; i > 0; i--) {
            total += conv[i - 1]->text.size() + 8;
            if (total > charBudget) {
                start = i;
                break;
            }
        }
    }

    for (size_t i = start; i < conv.len; i++) {
        AiChatTurn* t = conv[i];
        const char* label = t->isUser ? "You: " : (t->isError ? "AI (error): " : "AI: ");
        out.Append(label);
        out.Append(t->text.Get());
        out.Append("\n\n");
    }
}

// ---------------------------------------------------------------------------
// Panel creation / layout
// ---------------------------------------------------------------------------

static void OnAiSendClicked(MainWindow* win) {
    SubmitAiChatMessage(win);
}

// Message shown in the model dropdown itself (not the history pane) when
// there's nothing real to show there. Mirrors the wording of the existing
// "AI backend not available" chat error in SubmitAiChatMessage().
static void SetAiModelDropDownPlaceholder(MainWindow* win, const char* text) {
    if (!win->aiModelDropDown) {
        return;
    }
    win->aiModelDropDown->SetItemsSeqStrings(text);
    win->aiModelDropDown->SetCurrentSelection(0);
}

static void OnAiModelRefreshClicked(MainWindow* win) {
    RefreshAiModelList(win);
}

// User picked a different model from the dropdown: switch the bridge over
// immediately and persist the choice, the same way SetAiSidebarVisibility()
// persists SidebarVisible — just write gGlobalPrefs directly, no explicit
// SaveSettings() call (that happens on the app's existing save points).
static void OnAiModelSelectionChanged(MainWindow* win) {
    int idx = win->aiModelDropDown->GetCurrentSelection();
    if (idx < 0 || idx >= win->aiModelDropDown->items.Size()) {
        return;
    }
    const char* model = win->aiModelDropDown->items.At(idx);
    str::ReplaceWithCopy(&gGlobalPrefs->aiSettings.ollamaModel, model);
    if (gAiBridge) {
        gAiBridge->SetActiveModel(model);
    }
}

void LayoutAiContainer(MainWindow* win) {
    HWND box = win->hwndAiBox;
    if (!box || !win->aiLabelWithClose) {
        return;
    }
    Rect rc = ClientRect(box);
    int dx = rc.dx;
    int y = 0;

    Size labelSize = win->aiLabelWithClose->GetIdealSize();
    MoveWindow(win->aiLabelWithClose->hwnd, 0, y, dx, labelSize.dy, TRUE);
    y += labelSize.dy;

    int ddH = std::max(win->aiModeDropDown->GetIdealSize().dy, 24);
    MoveWindow(win->aiModeDropDown->hwnd, 0, y, dx, ddH, TRUE);
    y += ddH;

    constexpr int kModelRefreshW = 60;
    int modelDdH = std::max(win->aiModelDropDown->GetIdealSize().dy, 24);
    int modelDdW = std::max(0, dx - kModelRefreshW);
    MoveWindow(win->aiModelDropDown->hwnd, 0, y, modelDdW, modelDdH, TRUE);
    MoveWindow(win->aiModelRefreshButton->hwnd, modelDdW, y, dx - modelDdW, modelDdH, TRUE);
    y += modelDdH;

    constexpr int kInputH = 26;
    constexpr int kSendW = 60;
    int bottomY = std::max(y, rc.dy - kInputH);

    int historyH = bottomY - y;
    MoveWindow(win->aiHistoryEdit->hwnd, 0, y, dx, historyH, TRUE);

    MoveWindow(win->aiInputEdit->hwnd, 0, bottomY, std::max(0, dx - kSendW), kInputH, TRUE);
    MoveWindow(win->aiSendButton->hwnd, std::max(0, dx - kSendW), bottomY, kSendW, kInputH, TRUE);
}

static LRESULT CALLBACK WndProcAiBox(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    MainWindow* win = FindMainWindowByHwnd(hwnd);
    if (!win) {
        return DefSubclassProc(hwnd, msg, wp, lp);
    }

    LRESULT res = TryReflectMessages(hwnd, msg, wp, lp);
    if (res) {
        return res;
    }

    switch (msg) {
        case WM_SIZE:
            LayoutAiContainer(win);
            break;

        case WM_COMMAND:
            if (LOWORD(wp) == IDC_AI_LABEL_WITH_CLOSE) {
                ToggleAiSidebar(win);
            }
            break;

        // AiBridge posts these to win->hwndAiBox (see SubmitAiChatMessage).
        case WM_AI_RESPONSE_DONE:
        case WM_AI_ERROR:
            OnAiResponseDone(hwnd, wp, lp);
            break;

        // AiBridge posts this to win->hwndAiBox (see RefreshAiModelList).
        case WM_AI_MODELS_UPDATED:
            OnAiModelsUpdated(hwnd, wp, lp);
            break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static void SubclassAiBox(MainWindow* win) {
    if (win->aiBoxSubclassId == 0) {
        win->aiBoxSubclassId = NextSubclassId();
        BOOL ok = SetWindowSubclass(win->hwndAiBox, WndProcAiBox, win->aiBoxSubclassId, (DWORD_PTR)win);
        ReportIf(!ok);
    }
}

void CreateAiSidebar(MainWindow* win) {
    HMODULE hmod = GetModuleHandle(nullptr);
    int dx = gGlobalPrefs->aiSettings.sidebarDx;
    DWORD style = WS_CHILD | WS_CLIPCHILDREN;
    HWND parent = win->hwndFrame;
    win->hwndAiBox = CreateWindowExW(0, WC_STATIC, L"", style, 0, 0, dx, 0, parent, nullptr, hmod, nullptr);

    auto label = new LabelWithCloseWnd();
    {
        LabelWithCloseWnd::CreateArgs args;
        args.parent = win->hwndAiBox;
        args.cmdId = IDC_AI_LABEL_WITH_CLOSE;
        args.isRtl = IsUIRtl();
        args.font = GetDefaultGuiFont(true, false);
        label->Create(args);
    }
    win->aiLabelWithClose = label;
    label->SetPaddingXY(2, 2);
    label->SetLabel(_TRA("AI Chat"));

    auto dropDown = new DropDown();
    {
        DropDown::CreateArgs args;
        args.parent = win->hwndAiBox;
        args.font = GetAppTreeFont();
        args.isRtl = IsUIRtl();
        dropDown->Create(args);
    }
    dropDown->SetItemsSeqStrings("Selection\0Page\0Document\0");
    dropDown->SetCurrentSelection(0);
    win->aiModeDropDown = dropDown;

    auto modelDropDown = new DropDown();
    {
        DropDown::CreateArgs args;
        args.parent = win->hwndAiBox;
        args.font = GetAppTreeFont();
        args.isRtl = IsUIRtl();
        modelDropDown->Create(args);
    }
    modelDropDown->onSelectionChanged = MkFunc0<MainWindow>(OnAiModelSelectionChanged, win);
    win->aiModelDropDown = modelDropDown;
    SetAiModelDropDownPlaceholder(win, "Loading models...\0");

    win->aiModelRefreshButton =
        CreateButton(win->hwndAiBox, _TRA("Refresh"), MkFunc0<MainWindow>(OnAiModelRefreshClicked, win), IsUIRtl());

    auto historyEdit = new Edit();
    {
        Edit::CreateArgs args;
        args.parent = win->hwndAiBox;
        args.isMultiLine = true;
        args.withBorder = true;
        args.font = GetAppTreeFont();
        args.isRtl = IsUIRtl();
        historyEdit->Create(args);
    }
    SendMessageW(historyEdit->hwnd, EM_SETREADONLY, TRUE, 0);
    win->aiHistoryEdit = historyEdit;

    auto inputEdit = new Edit();
    {
        Edit::CreateArgs args;
        args.parent = win->hwndAiBox;
        args.isMultiLine = false;
        args.withBorder = true;
        args.cueText = "Ask a question...";
        args.font = GetAppTreeFont();
        args.isRtl = IsUIRtl();
        inputEdit->Create(args);
    }
    win->aiInputEdit = inputEdit;

    win->aiSendButton = CreateButton(win->hwndAiBox, _TRA("Send"), MkFunc0<MainWindow>(OnAiSendClicked, win), IsUIRtl());

    SubclassAiBox(win);
    LoadAiConversationIntoSidebar(win);
    UpdateControlsColors(win);

    // AI-HOOK: kick off model discovery for this window's dropdown. On the
    // very first window this is a no-op (gAiBridge doesn't exist yet at this
    // point in startup) — SumatraStartup.cpp triggers the initial refresh
    // itself once the bridge is up. For any later window (Ctrl+N) the bridge
    // already exists, so this is what populates it.
    RefreshAiModelList(win);
}

void SetAiSidebarVisibility(MainWindow* win, bool visible) {
    if (!win->hwndAiBox) {
        return;
    }
    win->aiVisible = visible;
    gGlobalPrefs->aiSettings.sidebarVisible = visible;
    if (win->aiSplitter) {
        HwndSetVisibility(win->aiSplitter->hwnd, visible);
    }
    HwndSetVisibility(win->hwndAiBox, visible);
    if (visible) {
        LoadAiConversationIntoSidebar(win);
    }
    RelayoutWindow(win);
}

void ToggleAiSidebar(MainWindow* win) {
    SetAiSidebarVisibility(win, !win->aiVisible);
}

void LoadAiConversationIntoSidebar(MainWindow* win) {
    if (!win->aiHistoryEdit) {
        return;
    }
    WindowTab* tab = win->CurrentTab();
    str::Str text;
    if (tab) {
        FormatConversationText(tab, text);
    }
    if (text.size() == 0) {
        win->aiHistoryEdit->SetText(_TRA("Ask a question about this document to get started."));
    } else {
        // multiline Edit controls need \r\n, not bare \n
        TempStr s = str::ReplaceTemp(text.Get(), "\r\n", "\n");
        s = str::ReplaceTemp(s, "\n", "\r\n");
        win->aiHistoryEdit->SetText(s);
    }
    // scroll to the end so the latest turn is visible
    HWND h = win->aiHistoryEdit->hwnd;
    SendMessageW(h, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
    SendMessageW(h, EM_SCROLLCARET, 0, 0);
}

// ---------------------------------------------------------------------------
// Sending / receiving
// ---------------------------------------------------------------------------

void SubmitAiChatMessage(MainWindow* win, AiRequestType type) {
    WindowTab* tab = win->CurrentTab();
    if (!tab || !tab->IsDocLoaded()) {
        return;
    }
    TempStr input = win->aiInputEdit->GetTextTemp();
    if (!input || str::Len(input) == 0) {
        return;
    }

    int sel = win->aiModeDropDown->GetCurrentSelection();
    AiContextMode mode = AiContextMode::Selection;
    if (sel == 1) {
        mode = AiContextMode::Page;
    } else if (sel == 2) {
        mode = AiContextMode::Document;
    }

    // Record the user's turn and clear the input immediately so the UI
    // feels responsive even before the backend replies.
    auto* userTurn = new AiChatTurn();
    userTurn->isUser = true;
    userTurn->text.Set(input);
    tab->aiConversation.Append(userTurn);
    win->aiInputEdit->SetText("");
    LoadAiConversationIntoSidebar(win);

    if (!gAiBridge || !gAiBridge->IsReady()) {
        auto* errTurn = new AiChatTurn();
        errTurn->isUser = false;
        errTurn->isError = true;
        errTurn->text.Set("AI backend not available — is Ollama running with the configured model pulled? "
                          "(check Settings for the host/model, or the Advanced Settings file)");
        tab->aiConversation.Append(errTurn);
        LoadAiConversationIntoSidebar(win);
        return;
    }

    str::Str contextText;
    BuildContextText(tab, mode, contextText);

    // historyText should cover turns *before* the one we just appended above
    // (that one is sent separately as userMessage); format the conversation
    // then strip the trailing "You: <input>\n\n" we just added.
    str::Str historyText;
    FormatConversationText(tab, historyText, 4000);
    {
        str::Str trailing;
        trailing.Append("You: ");
        trailing.Append(input);
        trailing.Append("\n\n");
        if (str::EndsWith(historyText.Get(), trailing.Get())) {
            historyText.RemoveAt(historyText.size() - trailing.size(), trailing.size());
        }
    }

    uint32_t reqId = gAiBridge->EnqueueRequest(type, mode, input, contextText.Get(), historyText.Get(), win->hwndAiBox);
    if (reqId != 0) {
        tab->aiPendingRequestId = reqId;
    }
}

void AiQuickAction(MainWindow* win, AiRequestType type) {
    WindowTab* tab = win->CurrentTab();
    if (!tab || !tab->IsDocLoaded()) {
        return;
    }
    bool hasSelection = win->showSelection && tab->selectionOnPage;
    if (!hasSelection) {
        return;
    }

    SetAiSidebarVisibility(win, true);
    win->aiModeDropDown->SetCurrentSelection(0); // Selection

    if (type == AiRequestType::Chat) {
        // "Ask AI..." - focus the input and let the user type their question.
        SetFocus(win->aiInputEdit->hwnd);
        return;
    }

    // Define/Explain: auto-submit a canned instruction; BuildContextText()
    // picks up the current selection itself when SubmitAiChatMessage() runs.
    const char* instruction = (type == AiRequestType::Define) ? "Define this." : "Explain this.";
    win->aiInputEdit->SetText(instruction);
    SubmitAiChatMessage(win, type);
}

void OnAiResponseDone(HWND aiBoxHwnd, WPARAM requestIdParam, LPARAM msgPtr) {
    auto* msg = (AiResponseMsg*)msgPtr;
    uint32_t requestId = (uint32_t)requestIdParam;

    MainWindow* win = FindMainWindowByHwnd(aiBoxHwnd);
    if (!win) {
        delete msg;
        return;
    }

    WindowTab* targetTab = nullptr;
    for (WindowTab* t : win->Tabs()) {
        if (t->aiPendingRequestId == requestId) {
            targetTab = t;
            break;
        }
    }
    if (!targetTab) {
        // Tab was closed (or request was superseded) while in flight.
        delete msg;
        return;
    }
    targetTab->aiPendingRequestId = 0;

    auto* turn = new AiChatTurn();
    turn->isUser = false;
    turn->isError = msg->isError;
    turn->text.Set(msg->text.Get());
    targetTab->aiConversation.Append(turn);

    if (win->CurrentTab() == targetTab) {
        LoadAiConversationIntoSidebar(win);
    }
    delete msg;
}

// ---------------------------------------------------------------------------
// Model discovery
// ---------------------------------------------------------------------------

void RefreshAiModelList(MainWindow* win) {
    if (!win->hwndAiBox) {
        return;
    }
    if (!gAiBridge) {
        // Same situation SubmitAiChatMessage() reports for chat requests:
        // Ollama isn't reachable (or the bridge failed to start). Leave the
        // dropdown in an explicit, non-broken placeholder state rather than
        // empty/disabled.
        SetAiModelDropDownPlaceholder(win, "AI backend not available\0");
        return;
    }
    gAiBridge->RequestModelListRefresh(win->hwndAiBox);
}

void OnAiModelsUpdated(HWND aiBoxHwnd, WPARAM /* requestId */, LPARAM msgPtr) {
    auto* msg = (AiModelsMsg*)msgPtr;

    MainWindow* win = FindMainWindowByHwnd(aiBoxHwnd);
    if (!win || !win->aiModelDropDown) {
        delete msg;
        return;
    }

    if (msg->isError) {
        SetAiModelDropDownPlaceholder(win, "AI backend not available\0");
        delete msg;
        return;
    }
    if (msg->models.IsEmpty()) {
        // Ollama is reachable but nothing has been pulled yet.
        SetAiModelDropDownPlaceholder(win, "No models pulled\0");
        delete msg;
        return;
    }

    const char* configured = gGlobalPrefs->aiSettings.ollamaModel;
    int idx = msg->models.Find(configured ? configured : "");
    if (idx < 0) {
        // Previously selected model is gone (e.g. removed via "ollama rm",
        // or never matched what's actually pulled) — fall back to the first
        // available model and persist that as the new default, the same way
        // a user's manual selection is persisted.
        idx = 0;
        const char* fallback = msg->models.At(0);
        logf("AiSidebarWnd: configured model '%s' not found; falling back to '%s'\n",
             configured ? configured : "(null)", fallback);
        str::ReplaceWithCopy(&gGlobalPrefs->aiSettings.ollamaModel, fallback);
        gAiBridge->SetActiveModel(fallback);
    }

    win->aiModelDropDown->SetItems(msg->models);
    win->aiModelDropDown->SetCurrentSelection(idx);
    LayoutAiContainer(win);

    delete msg;
}
