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
#include "AiExport.h"

#include "utils/JsonParser.h"
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
// AI-HOOK: export system.
//
// A second, distinct LLM call (still routed through AiBridge/gAiBridge, but
// its own AiRequestType values and its own response handler — see
// AiRequest.h) that reformats the conversation so far into a structured
// artifact and writes it to disk via AiExport.h. Decoupled from the chat
// flow: export responses never touch tab->aiConversation as a turn (except
// for the one-line success/failure status message appended for user
// feedback); see WindowTab::aiPendingExportRequestId.
// ---------------------------------------------------------------------------

// Returns true once the current tab has at least one AI (non-user) turn,
// i.e. there is something to export. Drives export button enabled state.
static bool AiTabHasResponse(WindowTab* tab) {
    if (!tab) {
        return false;
    }
    for (AiChatTurn* t : tab->aiConversation) {
        if (!t->isUser && !t->isError) {
            return true;
        }
    }
    return false;
}

static void UpdateAiExportButtonsEnabled(MainWindow* win) {
    if (!win->aiExportNoteButton) {
        return;
    }
    WindowTab* tab = win->CurrentTab();
    bool enabled = AiTabHasResponse(tab) && tab->aiPendingExportRequestId == 0;
    win->aiExportNoteButton->SetIsEnabled(enabled);
    win->aiExportStudySheetButton->SetIsEnabled(enabled);
    win->aiExportQuizButton->SetIsEnabled(enabled);
}

// Appends a one-line status turn (export success/failure) to the
// conversation so the user gets feedback in the same place chat errors show
// up, without pretending it's a chat turn from the model.
static void AppendAiExportStatusTurn(WindowTab* tab, bool isError, const char* text) {
    auto* turn = new AiChatTurn();
    turn->isUser = false;
    turn->isError = isError;
    turn->text.Set(text);
    tab->aiConversation.Append(turn);
}

// Parses a raw LLM response into note.title/body/tags. The model is asked
// (see AiBridge::BuildMessagesJson) to answer with only a JSON object of
// shape {"title":string,"body":string,"tags":[string,...]}, but small local
// models don't always comply exactly, so this is permissive: strips a
// leading/trailing markdown code fence if present, and if title/body are
// still empty after parsing, falls back to a generic title with the whole
// raw response as the body rather than losing the content.
struct NoteJsonVisitor : json::ValueVisitor {
    AiExport::NoteData* note = nullptr;
    bool Visit(const char* path, const char* value, json::Type) override {
        if (str::Eq(path, "/title")) {
            note->title.Set(value);
        } else if (str::Eq(path, "/body")) {
            note->body.Set(value);
        } else if (str::StartsWith(path, "/tags[")) {
            note->tags.Append(new str::Str(value));
        }
        return true;
    }
};

// Same leniency as NoteJsonVisitor's caller, for
// {"questions":[{"question":...,"answer":...,"type":...,"choices":[...]},...]}.
struct QuizJsonVisitor : json::ValueVisitor {
    AiExport::QuizData* quiz = nullptr;
    int lastIndex = -1;
    bool Visit(const char* path, const char* value, json::Type) override {
        const char* p = str::Find(path, "/questions[");
        if (!p) {
            return true;
        }
        p += str::Len("/questions[");
        int idx = atoi(p);
        if (idx != lastIndex) {
            quiz->items.Append(new AiExport::QuizItem());
            lastIndex = idx;
        }
        AiExport::QuizItem* item = quiz->items[quiz->items.len - 1];
        const char* fieldSep = str::Find(p, "]/");
        if (!fieldSep) {
            return true;
        }
        const char* field = fieldSep + 2;
        if (str::StartsWith(field, "question")) {
            item->question.Set(value);
        } else if (str::StartsWith(field, "answer")) {
            item->answer.Set(value);
        } else if (str::StartsWith(field, "type")) {
            item->type.Set(value);
        } else if (str::StartsWith(field, "choices")) {
            item->choices.Append(new str::Str(value));
        }
        return true;
    }
};

// Strips a ```json ... ``` (or bare ``` ... ```) fence around raw if the
// model wrapped its JSON in one despite being told not to.
static const char* StripJsonFenceTemp(const char* raw) {
    TempStr s = str::DupTemp(raw);
    str::TrimWSInPlace(s, str::TrimOpt::Both);
    if (str::StartsWith(s, "```")) {
        const char* nl = str::FindChar(s, '\n');
        s = nl ? str::DupTemp(nl + 1) : str::DupTemp("");
    }
    if (str::EndsWith(s, "```")) {
        s[str::Len(s) - 3] = 0;
    }
    str::TrimWSInPlace(s, str::TrimOpt::Both);
    return s;
}

static void ParseNoteFromLlmText(const char* rawText, AiExport::NoteData& note) {
    const char* json = StripJsonFenceTemp(rawText);
    NoteJsonVisitor visitor;
    visitor.note = &note;
    bool ok = json::Parse(json, &visitor);
    if (!ok || note.title.size() == 0) {
        note.title.Set("Untitled note");
        if (note.body.size() == 0) {
            note.body.Set(rawText);
        }
    }
}

static void ParseQuizFromLlmText(const char* rawText, AiExport::QuizData& quiz) {
    const char* json = StripJsonFenceTemp(rawText);
    QuizJsonVisitor visitor;
    visitor.quiz = &quiz;
    json::Parse(json, &visitor);
    for (AiExport::QuizItem* item : quiz.items) {
        if (item->type.size() == 0) {
            item->type.Set("short_answer");
        }
    }
}

static void FillExportSource(WindowTab* tab, AiExport::Source& source) {
    if (tab->filePath) {
        source.filePath.Set(tab->filePath);
    }
    source.page = tab->aiPendingExportPage;
    source.selectionText.Set(tab->aiPendingExportSelectionText.Get());
}

// Handles WM_AI_RESPONSE_DONE/WM_AI_ERROR when requestId matches a pending
// export (not a chat) request. Returns false (and leaves msg untouched) if
// requestId doesn't match any tab's pending export, so the caller can fall
// back to the normal chat handling in OnAiResponseDone.
static bool OnAiExportResponseDone(MainWindow* win, uint32_t requestId, AiResponseMsg* msg) {
    WindowTab* targetTab = nullptr;
    for (WindowTab* t : win->Tabs()) {
        if (t->aiPendingExportRequestId == requestId) {
            targetTab = t;
            break;
        }
    }
    if (!targetTab) {
        return false;
    }
    targetTab->aiPendingExportRequestId = 0;
    auto kind = (AiRequestType)targetTab->aiPendingExportKind;

    if (msg->isError) {
        AppendAiExportStatusTurn(targetTab, true, msg->text.Get());
    } else if (kind == AiRequestType::Quiz) {
        AiExport::QuizData quiz;
        FillExportSource(targetTab, quiz.source);
        ParseQuizFromLlmText(msg->text.Get(), quiz);
        str::Str path, error;
        if (quiz.items.len == 0) {
            AppendAiExportStatusTurn(targetTab, true, "Could not generate quiz questions from the response.");
        } else if (AiExport::ExportQuiz(quiz, &path, &error)) {
            TempStr status = str::FormatTemp("Quiz exported to %s", path.Get());
            AppendAiExportStatusTurn(targetTab, false, status);
        } else {
            TempStr status = str::FormatTemp("Quiz export failed: %s", error.Get());
            AppendAiExportStatusTurn(targetTab, true, status);
        }
    } else {
        AiExport::NoteData note;
        note.kind = (kind == AiRequestType::StudySheet) ? AiExport::NoteKind::StudySheet : AiExport::NoteKind::Note;
        FillExportSource(targetTab, note.source);
        ParseNoteFromLlmText(msg->text.Get(), note);
        str::Str path, error;
        if (AiExport::ExportNote(note, &path, &error)) {
            const char* label = (note.kind == AiExport::NoteKind::StudySheet) ? "Study sheet" : "Note";
            TempStr status = str::FormatTemp("%s exported to %s", label, path.Get());
            AppendAiExportStatusTurn(targetTab, false, status);
        } else {
            TempStr status = str::FormatTemp("Export failed: %s", error.Get());
            AppendAiExportStatusTurn(targetTab, true, status);
        }
    }

    if (win->CurrentTab() == targetTab) {
        LoadAiConversationIntoSidebar(win);
    }
    delete msg;
    return true;
}

static void SubmitAiExportRequest(MainWindow* win, AiRequestType type) {
    WindowTab* tab = win->CurrentTab();
    if (!tab || !tab->IsDocLoaded() || !AiTabHasResponse(tab) || tab->aiPendingExportRequestId != 0) {
        return;
    }

    if (!gAiBridge || !gAiBridge->IsReady()) {
        AppendAiExportStatusTurn(tab, true,
                                 "AI backend not available — is Ollama running with the configured model pulled?");
        LoadAiConversationIntoSidebar(win);
        return;
    }

    // Snapshot source metadata now: the current page/selection may change
    // before the response arrives.
    tab->aiPendingExportPage = tab->ctrl ? tab->ctrl->CurrentPageNo() : 0;
    tab->aiPendingExportSelectionText.Reset();
    bool isTextOnly = false;
    TempStr sel = GetSelectedTextTemp(tab, "\n", isTextOnly);
    if (sel) {
        tab->aiPendingExportSelectionText.Set(sel);
    }

    str::Str conversationText;
    FormatConversationText(tab, conversationText);

    const char* userMessage = "Create a note from this conversation.";
    if (type == AiRequestType::StudySheet) {
        userMessage = "Create a study sheet from this conversation.";
    } else if (type == AiRequestType::Quiz) {
        userMessage = "Create quiz questions from this conversation.";
    }

    tab->aiPendingExportKind = (int)type;
    uint32_t reqId = gAiBridge->EnqueueRequest(type, AiContextMode::Document, userMessage, conversationText.Get(), "",
                                               win->hwndAiBox);
    if (reqId != 0) {
        tab->aiPendingExportRequestId = reqId;
        UpdateAiExportButtonsEnabled(win);
    }
}

static void OnAiExportNoteClicked(MainWindow* win) {
    SubmitAiExportRequest(win, AiRequestType::Note);
}
static void OnAiExportStudySheetClicked(MainWindow* win) {
    SubmitAiExportRequest(win, AiRequestType::StudySheet);
}
static void OnAiExportQuizClicked(MainWindow* win) {
    SubmitAiExportRequest(win, AiRequestType::Quiz);
}

// ---------------------------------------------------------------------------
// Panel creation / layout
// ---------------------------------------------------------------------------

static void OnAiSendClicked(MainWindow* win) {
    SubmitAiChatMessage(win);
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

    constexpr int kInputH = 26;
    constexpr int kSendW = 60;
    constexpr int kExportRowH = 24;
    bool hasExportRow = win->aiExportNoteButton != nullptr;
    int exportRowH = hasExportRow ? kExportRowH : 0;
    int bottomY = std::max(y, rc.dy - kInputH - exportRowH);

    int historyH = bottomY - y;
    MoveWindow(win->aiHistoryEdit->hwnd, 0, y, dx, historyH, TRUE);

    if (hasExportRow) {
        int exportY = bottomY;
        int colW = dx / 3;
        MoveWindow(win->aiExportNoteButton->hwnd, 0, exportY, colW, kExportRowH, TRUE);
        MoveWindow(win->aiExportStudySheetButton->hwnd, colW, exportY, colW, kExportRowH, TRUE);
        MoveWindow(win->aiExportQuizButton->hwnd, 2 * colW, exportY, dx - 2 * colW, kExportRowH, TRUE);
        bottomY += kExportRowH;
    }

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

        // AiBridge posts these to win->hwndAiBox for both chat (see
        // SubmitAiChatMessage) and export (see SubmitAiExportRequest)
        // requests. Try the export path first since it knows the requestId
        // space it owns (WindowTab::aiPendingExportRequestId); if it isn't
        // an export response, fall back to the chat handler.
        case WM_AI_RESPONSE_DONE:
        case WM_AI_ERROR:
            if (!OnAiExportResponseDone(win, (uint32_t)wp, (AiResponseMsg*)lp)) {
                OnAiResponseDone(hwnd, wp, lp);
            }
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

    // AI-HOOK: export system buttons — enabled once the conversation has an
    // AI response (see UpdateAiExportButtonsEnabled).
    win->aiExportNoteButton =
        CreateButton(win->hwndAiBox, _TRA("Note"), MkFunc0<MainWindow>(OnAiExportNoteClicked, win), IsUIRtl());
    win->aiExportStudySheetButton = CreateButton(win->hwndAiBox, _TRA("Study Sheet"),
                                                 MkFunc0<MainWindow>(OnAiExportStudySheetClicked, win), IsUIRtl());
    win->aiExportQuizButton =
        CreateButton(win->hwndAiBox, _TRA("Quiz"), MkFunc0<MainWindow>(OnAiExportQuizClicked, win), IsUIRtl());

    SubclassAiBox(win);
    LoadAiConversationIntoSidebar(win);
    UpdateControlsColors(win);
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

    UpdateAiExportButtonsEnabled(win);
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
