/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// AI-HOOK: New file - Phase 1 LLM inline response popup implementation

#include "utils/BaseUtil.h"
#include "utils/WinUtil.h"
#include "utils/Log.h"
#include "LlmResponseWnd.h"

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

const WCHAR* LlmResponseWnd::kClassName = L"SumatraPDF_LlmResponseWnd";

// Colors — designed to be readable in both light and dark Sumatra themes
static constexpr COLORREF kBgColor       = RGB(245, 245, 240);
static constexpr COLORREF kBorderColor   = RGB(180, 180, 170);
static constexpr COLORREF kTextColor     = RGB(30,  30,  30);
static constexpr COLORREF kErrorBgColor  = RGB(255, 240, 240);
static constexpr COLORREF kErrorColor    = RGB(180, 30,  30);
static constexpr COLORREF kLoadingColor  = RGB(100, 100, 100);
static constexpr COLORREF kDismissColor  = RGB(120, 120, 120);

// ---------------------------------------------------------------------------
// Global active popup
// ---------------------------------------------------------------------------
LlmResponseWnd* gActiveLlmResponseWnd = nullptr;

// ---------------------------------------------------------------------------
// RegisterWindowClass
// ---------------------------------------------------------------------------

bool LlmResponseWnd::RegisterWindowClass(HINSTANCE hInst) {
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = LlmResponseWnd::WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr; // we paint in WM_PAINT
    wc.lpszClassName = kClassName;
    return RegisterClassExW(&wc) != 0;
}

// ---------------------------------------------------------------------------
// Create
// ---------------------------------------------------------------------------

LlmResponseWnd* LlmResponseWnd::Create(HWND parentHwnd, RECT anchorRect, uint32_t requestId) {
    // Dismiss any existing popup first
    DismissActiveLlmPopup();

    auto* wnd = new LlmResponseWnd();
    wnd->mParentHwnd  = parentHwnd;
    wnd->mAnchorRect  = anchorRect;
    wnd->mRequestId   = requestId;

    // Compute initial position: below the selection, clamped to screen
    POINT pos;
    pos.x = anchorRect.left;
    pos.y = anchorRect.bottom + 6;

    // Get monitor work area to clamp position
    HMONITOR hMon = MonitorFromPoint(pos, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(hMon, &mi);

    // Clamp horizontal
    if (pos.x + kMaxWidth > mi.rcWork.right) {
        pos.x = mi.rcWork.right - kMaxWidth - 4;
    }
    pos.x = std::max(pos.x, mi.rcWork.left + 4);

    // Clamp vertical: if not enough room below, go above
    if (pos.y + kMaxHeight > mi.rcWork.bottom) {
        pos.y = anchorRect.top - kMaxHeight - 6;
    }
    pos.y = std::max(pos.y, mi.rcWork.top + 4);

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    wnd->mHwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,  // don't steal focus
        kClassName,
        nullptr,
        WS_POPUP | WS_BORDER,
        pos.x, pos.y,
        kMaxWidth, kMaxHeight,
        parentHwnd,
        nullptr,
        hInst,
        wnd   // passed as CREATESTRUCT.lpCreateParams
    );

    if (!wnd->mHwnd) {
        delete wnd;
        return nullptr;
    }

    ShowWindow(wnd->mHwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(wnd->mHwnd);

    gActiveLlmResponseWnd = wnd;
    return wnd;
}

// ---------------------------------------------------------------------------
// WndProc
// ---------------------------------------------------------------------------

LRESULT CALLBACK LlmResponseWnd::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    LlmResponseWnd* self = nullptr;

    if (msg == WM_CREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = reinterpret_cast<LlmResponseWnd*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
        self->mHwnd = hwnd;
        return 0;
    }

    self = reinterpret_cast<LlmResponseWnd*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) {
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            self->OnPaint(hdc, rc);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_KEYDOWN:
            self->OnKeyDown(wp);
            return 0;

        case WM_LBUTTONDOWN:
            // Click-through: clicking inside dismisses the popup
            // Phase 2: implement "Copy" button hit testing here
            self->Dismiss();
            return 0;

        case WM_KILLFOCUS:
            // Dismissed when another window takes focus
            // AI-HOOK: WS_EX_NOACTIVATE means this won't fire on normal
            // canvas interaction. Only fires on genuine focus change.
            self->Dismiss();
            return 0;

        case WM_DESTROY:
            if (gActiveLlmResponseWnd == self) {
                gActiveLlmResponseWnd = nullptr;
            }
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            delete self;
            return 0;

        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

// ---------------------------------------------------------------------------
// OnPaint
// ---------------------------------------------------------------------------

void LlmResponseWnd::OnPaint(HDC hdc, RECT& clientRect) {
    // Background
    COLORREF bgColor = mIsError ? kErrorBgColor : kBgColor;
    HBRUSH bgBrush = CreateSolidBrush(bgColor);
    FillRect(hdc, &clientRect, bgBrush);
    DeleteObject(bgBrush);

    // Border
    HPEN borderPen = CreatePen(PS_SOLID, 1, kBorderColor);
    HPEN oldPen    = (HPEN)SelectObject(hdc, borderPen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, clientRect.left, clientRect.top,
              clientRect.right - 1, clientRect.bottom - 1);
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(borderPen);

    // Text area
    RECT textRect = clientRect;
    textRect.left   += kPadding;
    textRect.top    += kPadding;
    textRect.right  -= kPadding;
    textRect.bottom -= kPadding + 18; // leave room for dismiss hint

    SetBkMode(hdc, TRANSPARENT);

    // Body text or loading indicator
    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HFONT oldFont = (HFONT)SelectObject(hdc, hFont);

    if (!mHasResponse) {
        SetTextColor(hdc, kLoadingColor);
        const WCHAR* loadingMsg = L"Asking AI...";
        DrawTextW(hdc, loadingMsg, -1, &textRect, DT_LEFT | DT_TOP | DT_WORDBREAK);
    } else {
        SetTextColor(hdc, mIsError ? kErrorColor : kTextColor);
        TempWStr textW = ToWStrTemp(mResponseText.Get());
        DrawTextW(hdc, textW, -1, &textRect, DT_LEFT | DT_TOP | DT_WORDBREAK);
    }

    // Dismiss hint at bottom
    RECT hintRect = clientRect;
    hintRect.left   += kPadding;
    hintRect.bottom -= 4;
    hintRect.top     = hintRect.bottom - 16;
    hintRect.right  -= kPadding;
    SetTextColor(hdc, kDismissColor);
    DrawTextW(hdc, L"[Esc or click to dismiss]", -1, &hintRect,
              DT_LEFT | DT_BOTTOM | DT_SINGLELINE);

    SelectObject(hdc, oldFont);
}

// ---------------------------------------------------------------------------
// SetResponseText / SetError
// ---------------------------------------------------------------------------

void LlmResponseWnd::SetResponseText(const char* text) {
    mResponseText.Set(text);
    mHasResponse = true;
    mIsError     = false;
    if (mHwnd) InvalidateRect(mHwnd, nullptr, TRUE);
}

void LlmResponseWnd::SetError(const char* errorMsg) {
    mResponseText.Set(errorMsg);
    mHasResponse = true;
    mIsError     = true;
    if (mHwnd) InvalidateRect(mHwnd, nullptr, TRUE);
}

// ---------------------------------------------------------------------------
// OnKeyDown
// ---------------------------------------------------------------------------

void LlmResponseWnd::OnKeyDown(WPARAM vk) {
    if (vk == VK_ESCAPE) {
        Dismiss();
    }
}

// ---------------------------------------------------------------------------
// Dismiss
// ---------------------------------------------------------------------------

void LlmResponseWnd::Dismiss() {
    if (mHwnd) {
        DestroyWindow(mHwnd);
        // mHwnd set to nullptr and gActiveLlmResponseWnd cleared in WM_DESTROY
    }
}

// ---------------------------------------------------------------------------
// DismissActiveLlmPopup (free function)
// ---------------------------------------------------------------------------

void DismissActiveLlmPopup() {
    if (gActiveLlmResponseWnd) {
        gActiveLlmResponseWnd->Dismiss();
        // gActiveLlmResponseWnd set to nullptr by WM_DESTROY handler
    }
}

// ---------------------------------------------------------------------------
// OnAiResponseDone
//
// Called from the canvas WndProc WM_AI_RESPONSE_DONE handler.
// Verifies request_id, creates/updates popup, deletes msg.
// ---------------------------------------------------------------------------

void OnAiResponseDone(HWND canvasHwnd, WPARAM requestId, LPARAM msgPtr) {
    // AI-HOOK: stale-window protection (required even in Phase 1, per review)
    // Take ownership immediately; we are responsible for deletion in all paths.
    AiResponseMsg* msg = reinterpret_cast<AiResponseMsg*>(msgPtr);
    if (!msg) return;

    // Verify the response belongs to the currently active popup.
    // If a new selection was made while inference was running, the popup
    // either no longer exists or tracks a different requestId — drop.
    if (gActiveLlmResponseWnd) {
        if (gActiveLlmResponseWnd->GetRequestId() != (uint32_t)requestId) {
            logf("OnAiResponseDone: dropping stale response for req id=%u "
                 "(active id=%u)\n",
                 (uint32_t)requestId, gActiveLlmResponseWnd->GetRequestId());
            delete msg;
            return;
        }
        if (msg->isError) {
            gActiveLlmResponseWnd->SetError(msg->text.Get());
        } else {
            gActiveLlmResponseWnd->SetResponseText(msg->text.Get());
        }
    }
    // No popup exists (dismissed before response arrived) — drop silently.
    delete msg;
}
