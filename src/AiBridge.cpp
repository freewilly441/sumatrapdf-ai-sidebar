/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// AI-HOOK: Phase 1 LLM integration bridge implementation

#include "utils/BaseUtil.h"
#include "utils/ThreadUtil.h"
#include "utils/WinUtil.h"
#include "utils/Log.h"
#include "utils/HttpUtil.h"
#include "AiBridge.h"

// wininet.h included via BaseUtil.h; wininet.lib linked via project

// ---------------------------------------------------------------------------
// Global singleton
// ---------------------------------------------------------------------------
AiBridge* gAiBridge = nullptr;

// ---------------------------------------------------------------------------
// Simple RAII lock guard (no ScopedMutexLock exists in this codebase)
// ---------------------------------------------------------------------------
struct ScopedLock {
    Mutex& m;
    explicit ScopedLock(Mutex& m) : m(m) { m.Lock(); }
    ~ScopedLock() { m.Unlock(); }
};

// ---------------------------------------------------------------------------
// Minimal JSON helpers — no external dependency.
// Sufficient for building llama-server /completion requests and
// extracting the "content" field from responses.
// ---------------------------------------------------------------------------

static void JsonEscapeAppend(const char* s, str::Str& out) {
    if (!s) return;
    for (const char* p = s; *p; p++) {
        switch (*p) {
            case '"':  out.Append("\\\"", 2); break;
            case '\\': out.Append("\\\\", 2); break;
            case '\n': out.Append("\\n",  2); break;
            case '\r': out.Append("\\r",  2); break;
            case '\t': out.Append("\\t",  2); break;
            default:   out.AppendChar(*p);    break;
        }
    }
}

// Extracts a JSON string field value from a flat JSON object.
// Handles basic escape sequences. Not a full parser.
static bool JsonExtractString(const char* json, const char* key, str::Str& out) {
    if (!json || !key) return false;

    // Build search pattern: "key":"
    str::Str pat;
    pat.AppendChar('"');
    pat.Append(key);
    pat.Append("\":\"", 3);

    const char* pos = str::Find(json, pat.Get());
    if (!pos) {
        // Also try without space: some servers emit "key" : "
        // (not needed for llama-server but defensive)
        return false;
    }
    pos += pat.size();

    while (*pos && *pos != '"') {
        if (*pos == '\\' && *(pos + 1)) {
            pos++;
            switch (*pos) {
                case 'n':  out.AppendChar('\n'); break;
                case 'r':  out.AppendChar('\r'); break;
                case 't':  out.AppendChar('\t'); break;
                case '"':  out.AppendChar('"');  break;
                case '\\': out.AppendChar('\\'); break;
                default:   out.AppendChar(*pos); break;
            }
        } else {
            out.AppendChar(*pos);
        }
        pos++;
    }
    return true;
}

// ---------------------------------------------------------------------------
// AiHttpPost
//
// Self-contained WinInet POST with full response body capture.
// Called on bridge thread only (blocking). Uses WinInet directly because
// the existing HttpPost() in HttpUtil.cpp discards the response body.
//
// Sets a 60-second receive timeout to accommodate slow CPU inference.
// ---------------------------------------------------------------------------
static bool AiHttpPost(const char* server, int port, const char* path,
                       const str::Str& body, str::Str& responseOut) {
    bool ok = false;
    HINTERNET hInet = nullptr, hConn = nullptr, hReq = nullptr;
    DWORD respCode = 0;
    DWORD respCodeSize = sizeof(DWORD);
    DWORD timeoutMs = 60 * 1000;  // 60s: CPU inference can be slow

    WCHAR* serverW = ToWStrTemp(server);
    WCHAR* pathW   = ToWStrTemp(path);

    hInet = InternetOpenW(L"SumatraPDF-LLM",
                          INTERNET_OPEN_TYPE_PRECONFIG,
                          nullptr, nullptr, 0);
    if (!hInet) {
        logf("AiHttpPost: InternetOpen failed (%d)\n", (int)GetLastError());
        goto Exit;
    }

    hConn = InternetConnectW(hInet, serverW, (INTERNET_PORT)port,
                             nullptr, nullptr,
                             INTERNET_SERVICE_HTTP, 0, 0);
    if (!hConn) {
        logf("AiHttpPost: InternetConnect failed (%d)\n", (int)GetLastError());
        goto Exit;
    }

    hReq = HttpOpenRequestW(hConn, L"POST", pathW,
                            nullptr, nullptr, nullptr,
                            INTERNET_FLAG_NO_UI | INTERNET_FLAG_NO_CACHE_WRITE |
                            INTERNET_FLAG_RELOAD,
                            0);
    if (!hReq) {
        logf("AiHttpPost: HttpOpenRequest failed (%d)\n", (int)GetLastError());
        goto Exit;
    }

    // Set generous timeouts — bridge thread is allowed to block
    InternetSetOptionW(hReq, INTERNET_OPTION_SEND_TIMEOUT,
                       &timeoutMs, sizeof(timeoutMs));
    InternetSetOptionW(hReq, INTERNET_OPTION_RECEIVE_TIMEOUT,
                       &timeoutMs, sizeof(timeoutMs));

    {
        const char* hdrs    = "Content-Type: application/json\r\n";
        DWORD       hdrsLen = (DWORD)str::Len(hdrs);
        void*       data    = (void*)body.Get();
        DWORD       dataLen = (DWORD)body.size();

        if (!HttpSendRequestA(hReq, hdrs, hdrsLen, data, dataLen)) {
            logf("AiHttpPost: HttpSendRequest failed (%d)\n", (int)GetLastError());
            goto Exit;
        }
    }

    HttpQueryInfoW(hReq,
                   HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                   &respCode, &respCodeSize, nullptr);

    if (respCode != 200) {
        logf("AiHttpPost: unexpected HTTP status %d\n", (int)respCode);
        goto Exit;
    }

    // Read response body in chunks
    for (;;) {
        char   buf[4096];
        DWORD  dwRead = 0;
        if (!InternetReadFile(hReq, buf, sizeof(buf), &dwRead)) {
            logf("AiHttpPost: InternetReadFile failed (%d)\n", (int)GetLastError());
            goto Exit;
        }
        if (dwRead == 0) break;
        responseOut.Append(buf, (size_t)dwRead);
    }

    ok = true;

Exit:
    if (hReq)  InternetCloseHandle(hReq);
    if (hConn) InternetCloseHandle(hConn);
    if (hInet) InternetCloseHandle(hInet);
    return ok;
}

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

AiBridge::AiBridge() {
    mQueueEvent    = CreateEventW(nullptr, FALSE, FALSE, nullptr); // auto-reset
    mShutdownEvent = CreateEventW(nullptr, TRUE,  FALSE, nullptr); // manual-reset
    ReportIf(!mQueueEvent || !mShutdownEvent);
}

AiBridge::~AiBridge() {
    // Shutdown() should have been called already.
    // Belt-and-suspenders: free anything remaining.
    if (mSidecarProcess) {
        TerminateProcess(mSidecarProcess, 0);
        CloseHandle(mSidecarProcess);
        mSidecarProcess = nullptr;
    }
    if (mSidecarThread) {
        CloseHandle(mSidecarThread);
        mSidecarThread = nullptr;
    }
    if (mBridgeThread) {
        CloseHandle(mBridgeThread);
        mBridgeThread = nullptr;
    }
    if (mQueueEvent) {
        CloseHandle(mQueueEvent);
        mQueueEvent = nullptr;
    }
    if (mShutdownEvent) {
        CloseHandle(mShutdownEvent);
        mShutdownEvent = nullptr;
    }
    // Drain any remaining queued requests
    ScopedLock lk(mQueueLock);
    for (size_t i = 0; i < mQueue.len; i++) {
        delete mQueue[i];
    }
    mQueue.Reset();
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

bool AiBridge::Init(const char* modelPath, const char* llamaServerExePath, int port) {
    ReportIf(mBridgeThread);  // Must not call Init twice

    mPort = port;
    mModelPath.Set(modelPath);
    mLlamaServerPath.Set(llamaServerExePath);

    if (!SpawnSidecar(llamaServerExePath, modelPath, port)) {
        logf("AiBridge::Init: SpawnSidecar failed\n");
        return false;
    }

    // StartThread() from ThreadUtil.h: returns HANDLE, takes Func0 (not lambda)
    mBridgeThread = StartThread(MkMethod0<AiBridge, &AiBridge::RunBridgeLoop>(this), "AiBridge");
    ReportIf(!mBridgeThread);

    logf("AiBridge::Init: bridge thread started (port %d)\n", port);
    return true;
}

// ---------------------------------------------------------------------------
// Sidecar management
// ---------------------------------------------------------------------------

bool AiBridge::SpawnSidecar(const char* exePath, const char* modelPath, int port) {
    // Command line:
    // llama-server.exe -m "<model>" --port <N> --host 127.0.0.1 -np 1 -c 2048 --log-disable
    str::Str cmdLine;
    cmdLine.AppendChar('"');
    cmdLine.Append(exePath);
    cmdLine.Append("\" -m \"");
    cmdLine.Append(modelPath);
    cmdLine.AppendFmt("\" --port %d --host 127.0.0.1 -np 1 -c 2048 --log-disable", port);

    TempWStr cmdLineW = ToWStrTemp(cmdLine.Get());

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    // Do NOT set STARTF_USESTDHANDLES — passing INVALID_HANDLE_VALUE as stdio
    // handles causes CreateProcessW to fail with ERROR_INVALID_HANDLE.
    // CREATE_NO_WINDOW already suppresses any console window.

    PROCESS_INFORMATION pi{};
    BOOL created = CreateProcessW(
        nullptr, cmdLineW,
        nullptr, nullptr,
        FALSE, CREATE_NO_WINDOW,
        nullptr, nullptr,
        &si, &pi);

    if (!created) {
        logf("AiBridge::SpawnSidecar: CreateProcess failed (%d)\n",
             (int)GetLastError());
        return false;
    }

    mSidecarProcess = pi.hProcess;
    mSidecarThread  = pi.hThread;
    logf("AiBridge::SpawnSidecar: PID=%d\n", (int)pi.dwProcessId);
    return true;
}

void AiBridge::ShutdownSidecar(DWORD gracefulTimeoutMs) {
    if (!mSidecarProcess) return;

    // Graceful path: CTRL_BREAK lets llama-server flush state
    GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, GetProcessId(mSidecarProcess));

    DWORD result = WaitForSingleObject(mSidecarProcess, gracefulTimeoutMs);
    if (result != WAIT_OBJECT_0) {
        logf("AiBridge::ShutdownSidecar: graceful timeout — forcing kill\n");
        TerminateProcess(mSidecarProcess, 0);
        WaitForSingleObject(mSidecarProcess, 2000);
    }

    CloseHandle(mSidecarProcess);
    mSidecarProcess = nullptr;
    if (mSidecarThread) {
        CloseHandle(mSidecarThread);
        mSidecarThread = nullptr;
    }
    logf("AiBridge::ShutdownSidecar: done\n");
}

bool AiBridge::WaitForSidecarReady(int maxWaitMs) {
    str::Str url;
    url.AppendFmt("http://127.0.0.1:%d/health", mPort);

    const int pollMs = 500;
    int elapsed = 0;

    while (elapsed < maxWaitMs) {
        if (AtomicIntGet(&mShuttingDown)) return false;

        HttpRsp rsp;
        if (HttpGet(url.Get(), &rsp) && IsHttpRspOk(&rsp)) {
            logf("AiBridge::WaitForSidecarReady: ready after %dms\n", elapsed);
            AtomicIntSet(&mSidecarReady, 1);
            return true;
        }
        Sleep(pollMs);
        elapsed += pollMs;
    }

    logf("AiBridge::WaitForSidecarReady: timed out after %dms\n", maxWaitMs);
    return false;
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

void AiBridge::Shutdown() {
    // Guard against double-call
    if (AtomicIntSet(&mShuttingDown, 1) == 1) return;

    logf("AiBridge::Shutdown: signaling bridge thread\n");
    SetEvent(mShutdownEvent);

    if (mBridgeThread) {
        WaitForSingleObject(mBridgeThread, 5000);
        CloseHandle(mBridgeThread);
        mBridgeThread = nullptr;
    }

    ShutdownSidecar(3000);
    logf("AiBridge::Shutdown: complete\n");
}

// ---------------------------------------------------------------------------
// EnqueueRequest / CancelRequest / IsReady
// ---------------------------------------------------------------------------

uint32_t AiBridge::EnqueueRequest(AiRequestType type, const char* selectedText,
                                   const char* pageContext, HWND targetHwnd,
                                   RECT anchorRect) {
    if (AtomicIntGet(&mShuttingDown)) return 0;

    // Assign ID atomically; start at 1 so 0 is always "invalid"
    uint32_t id = (uint32_t)InterlockedIncrement(&mNextRequestId);

    AiRequest* req   = new AiRequest();
    req->id          = id;
    req->type        = type;
    req->selectedText.Set(selectedText ? selectedText : "");
    req->pageContext.Set(pageContext   ? pageContext   : "");
    req->targetHwnd  = targetHwnd;
    req->anchorRect  = anchorRect;
    req->state       = AiRequestState::Pending;

    {
        ScopedLock lk(mQueueLock);
        mQueue.Append(req);
    }

    SetEvent(mQueueEvent);
    logf("AiBridge::EnqueueRequest: id=%u type=%d\n", id, (int)type);
    return id;
}

void AiBridge::CancelRequest(uint32_t requestId) {
    ScopedLock lk(mQueueLock);
    for (size_t i = 0; i < mQueue.len; i++) {
        AiRequest* req = mQueue[i];
        if (req->id == requestId && req->state == AiRequestState::Pending) {
            req->state = AiRequestState::Canceled;
            logf("AiBridge::CancelRequest: id=%u canceled\n", requestId);
            return;
        }
    }
}

bool AiBridge::IsReady() const {
    return AtomicIntGet(const_cast<AtomicInt*>(&mSidecarReady)) != 0;
}

// ---------------------------------------------------------------------------
// Bridge thread loop
// ---------------------------------------------------------------------------

void AiBridge::RunBridgeLoop() {
    SetThreadName("AiBridge");

    if (!WaitForSidecarReady(20000)) {
        logf("AiBridge::RunBridgeLoop: sidecar never ready — exiting\n");
        return;
    }

    HANDLE waitHandles[2] = {mShutdownEvent, mQueueEvent};

    while (true) {
        DWORD w = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);

        if (w == WAIT_OBJECT_0) {
            // Shutdown event — exit loop
            logf("AiBridge::RunBridgeLoop: shutdown signal\n");
            break;
        }
        if (w != WAIT_OBJECT_0 + 1) break;  // unexpected

        // Drain all Pending items from the queue
        while (true) {
            if (AtomicIntGet(&mShuttingDown)) goto done;

            // Pop one Pending request under lock; prune dead entries
            AiRequest* req = nullptr;
            {
                ScopedLock lk(mQueueLock);

                // Find oldest Pending
                for (size_t i = 0; i < mQueue.len; i++) {
                    if (mQueue[i]->state == AiRequestState::Pending) {
                        req = mQueue[i];
                        mQueue.RemoveAt(i);
                        break;
                    }
                }

                // Prune non-Pending entries (Canceled, Complete, Failed)
                for (int i = (int)mQueue.len - 1; i >= 0; i--) {
                    AiRequestState s = mQueue[i]->state;
                    if (s != AiRequestState::Pending) {
                        delete mQueue[i];
                        mQueue.RemoveAt((size_t)i);
                    }
                }
            }

            if (!req) break;  // queue empty

            // Guard: state may have been set to Canceled while we were waiting
            if (req->state != AiRequestState::Pending) {
                logf("AiBridge: req id=%u dropped (state=%d)\n",
                     req->id, (int)req->state);
                delete req;
                continue;
            }

            // --- Send to sidecar ---
            str::Str responseText;
            bool ok = SendCompletionRequest(*req, responseText);

            // -------------------------------------------------------------------
            // Memory ownership contract:
            //   AiResponseMsg allocated here on bridge thread.
            //   PostMessage transfers ownership to UI thread.
            //   If PostMessage fails (HWND destroyed), free here.
            //   UI thread MUST delete in WM_AI_RESPONSE_DONE handler.
            // -------------------------------------------------------------------
            AiResponseMsg* msg = new AiResponseMsg();
            msg->requestId     = req->id;
            msg->isError       = !ok;
            msg->text.Set(ok ? responseText.Get()
                             : "AI error — is the model loaded?");

            UINT  wm     = ok ? WM_AI_RESPONSE_DONE : WM_AI_ERROR;
            BOOL  posted = PostMessage(req->targetHwnd, wm,
                                       (WPARAM)req->id, (LPARAM)msg);
            if (!posted) {
                logf("AiBridge: PostMessage failed for id=%u (HWND gone)\n",
                     req->id);
                delete msg;
            }

            delete req;
        }
    }

done:
    logf("AiBridge::RunBridgeLoop: exited\n");
}

// ---------------------------------------------------------------------------
// HTTP + prompt logic
// ---------------------------------------------------------------------------

bool AiBridge::SendCompletionRequest(const AiRequest& req, str::Str& responseOut) {
    str::Str prompt;
    BuildPrompt(req, prompt);

    str::Str jsonBody;
    BuildRequestJson(prompt, jsonBody);

    str::Str rawResponse;
    if (!AiHttpPost("127.0.0.1", mPort, "/completion", jsonBody, rawResponse)) {
        logf("AiBridge::SendCompletionRequest: HTTP POST failed\n");
        return false;
    }

    if (!ParseCompletionResponse(rawResponse, responseOut)) {
        logf("AiBridge::SendCompletionRequest: parse failed. Body: %s\n",
             rawResponse.Get());
        return false;
    }

    return true;
}

void AiBridge::BuildPrompt(const AiRequest& req, str::Str& out) const {
    // Phi-3 / Llama-3.2 instruction format.
    // Tokens: kept minimal for Phase 1 latency. Define: ~100 in / ~80 out.
    switch (req.type) {
        case AiRequestType::Define:
            out.Append("<|system|>You are a dictionary. "
                       "Define the given term in 1-2 sentences. Be concise.<|end|>\n"
                       "<|user|>Define: ");
            out.Append(req.selectedText.Get());
            out.Append("<|end|>\n<|assistant|>");
            break;

        case AiRequestType::Explain:
            out.Append("<|system|>You are a reading assistant. "
                       "Explain the following passage briefly.<|end|>\n"
                       "<|user|>");
            if (req.pageContext.size() > 0) {
                out.Append("Document context:\n");
                // Cap page context to stay within Phase 1 token budget
                size_t ctxLen = (req.pageContext.size() < 1200)
                              ? req.pageContext.size() : 1200;
                out.Append(req.pageContext.Get(), ctxLen);
                out.Append("\n\nNow explain this selection:\n");
            } else {
                out.Append("Explain: ");
            }
            out.Append(req.selectedText.Get());
            out.Append("<|end|>\n<|assistant|>");
            break;

        case AiRequestType::Ask:
        default:
            out.Append("<|system|>You are a helpful reading assistant "
                       "for PDF documents.<|end|>\n<|user|>");
            if (req.pageContext.size() > 0) {
                out.Append("Document context:\n");
                size_t ctxLen = (req.pageContext.size() < 1200)
                              ? req.pageContext.size() : 1200;
                out.Append(req.pageContext.Get(), ctxLen);
                out.Append("\n\nQuestion about the selection:\n");
            }
            out.Append(req.selectedText.Get());
            out.Append("<|end|>\n<|assistant|>");
            break;
    }
}

void AiBridge::BuildRequestJson(const str::Str& prompt, str::Str& jsonOut) const {
    // llama-server /completion payload
    // n_predict: 256 — sufficient for definitions and short explanations
    // stream: false — Phase 1 is single-shot response only
    jsonOut.Append("{\"prompt\":\"");
    JsonEscapeAppend(prompt.Get(), jsonOut);
    jsonOut.Append("\","
                   "\"n_predict\":256,"
                   "\"stream\":false,"
                   "\"temperature\":0.7,"
                   "\"stop\":[\"<|end|>\",\"<|user|>\"]}");
}

bool AiBridge::ParseCompletionResponse(const str::Str& jsonBody,
                                        str::Str& contentOut) const {
    // llama-server /completion response: { "content": "...", ... }
    return JsonExtractString(jsonBody.Get(), "content", contentOut);
}
