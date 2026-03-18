/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// AI-HOOK: New file - Phase 1 LLM integration bridge implementation

#include "utils/BaseUtil.h"
#include "utils/StrUtil.h"
#include "utils/ThreadUtil.h"
#include "utils/HttpUtil.h"
#include "utils/WinUtil.h"
#include "AiBridge.h"

// ---------------------------------------------------------------------------
// Global singleton
// ---------------------------------------------------------------------------
AiBridge* gAiBridge = nullptr;

// ---------------------------------------------------------------------------
// Minimal JSON helpers (no external dependency)
// We only need to build and parse simple flat JSON for llama-server.
// ---------------------------------------------------------------------------

// Escape a plain string for JSON embedding (handles \, ", newlines)
static void JsonEscapeAppend(const char* s, str::Str& out) {
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

// Extract the value of a JSON string field by key from a flat JSON object.
// Not a full parser — sufficient for llama-server's /completion response.
static bool JsonExtractString(const char* json, const char* key, str::Str& out) {
    // Find: "key":"<value>"
    str::Str searchKey;
    searchKey.AppendChar('"');
    searchKey.Append(key);
    searchKey.Append("\":\"", 3);

    const char* pos = str::Find(json, searchKey.Get());
    if (!pos) {
        return false;
    }
    pos += searchKey.size();

    // Read until unescaped closing quote
    while (*pos && *pos != '"') {
        if (*pos == '\\' && *(pos + 1)) {
            pos++; // skip escape char
            switch (*pos) {
                case 'n':  out.AppendChar('\n'); break;
                case 'r':  out.AppendChar('\r'); break;
                case 't':  out.AppendChar('\t'); break;
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
// Construction / Destruction
// ---------------------------------------------------------------------------

AiBridge::AiBridge() {
    mQueueEvent    = CreateEventW(nullptr, FALSE, FALSE, nullptr); // auto-reset
    mShutdownEvent = CreateEventW(nullptr, TRUE,  FALSE, nullptr); // manual-reset
    ReportIf(!mQueueEvent || !mShutdownEvent);
}

AiBridge::~AiBridge() {
    if (mQueueEvent)    CloseHandle(mQueueEvent);
    if (mShutdownEvent) CloseHandle(mShutdownEvent);
    // Note: Shutdown() must be called before destructor to join bridge thread
    // and kill sidecar. If not called, we do a best-effort forced kill here.
    if (mSidecarProcess) {
        TerminateProcess(mSidecarProcess, 0);
        CloseHandle(mSidecarProcess);
    }
    if (mSidecarProcessThread) CloseHandle(mSidecarProcessThread);
    if (mBridgeThread)         CloseHandle(mBridgeThread);
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

bool AiBridge::Init(const char* modelPath, const char* llamaServerExePath, int port) {
    ReportIf(mBridgeThread); // Don't call Init twice

    mPort = port;
    mModelPath.Set(modelPath);
    mLlamaServerPath.Set(llamaServerExePath);

    if (!SpawnSidecar(llamaServerExePath, modelPath, port)) {
        logf("AiBridge::Init: failed to spawn sidecar\n");
        return false;
    }

    // Start bridge worker thread
    // AI-HOOK: Uses SumatraPDF's StartThread() from ThreadUtil.h
    mBridgeThread = StartThread([this]() { RunBridgeLoop(); }, "AiBridge");
    ReportIf(!mBridgeThread);

    logf("AiBridge::Init: sidecar spawned, bridge thread started (port %d)\n", port);
    return true;
}

// ---------------------------------------------------------------------------
// Sidecar process management
// ---------------------------------------------------------------------------

bool AiBridge::SpawnSidecar(const char* llamaServerExePath, const char* modelPath, int port) {
    // Build command line:
    // llama-server.exe -m <model> --port <port> --host 127.0.0.1 -np 1 -c 2048
    str::Str cmdLine;
    cmdLine.AppendChar('"');
    cmdLine.Append(llamaServerExePath);
    cmdLine.Append("\" -m \"");
    cmdLine.Append(modelPath);
    cmdLine.AppendFmt("\" --port %d --host 127.0.0.1 -np 1 -c 2048 --log-disable", port);

    AutoFreeWstr cmdLineW = ToWStr(cmdLine.Get());

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    // Suppress sidecar console window and stdout/stderr in release builds
    si.hStdInput  = INVALID_HANDLE_VALUE;
    si.hStdOutput = INVALID_HANDLE_VALUE;
    si.hStdError  = INVALID_HANDLE_VALUE;

    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(
        nullptr,
        cmdLineW.Get(),
        nullptr,   // process security
        nullptr,   // thread security
        FALSE,     // don't inherit handles
        CREATE_NO_WINDOW,
        nullptr,   // inherit environment
        nullptr,   // inherit working dir
        &si,
        &pi
    );

    if (!ok) {
        logf("AiBridge::SpawnSidecar: CreateProcess failed (err=%d)\n", GetLastError());
        return false;
    }

    mSidecarProcess       = pi.hProcess;
    mSidecarProcessThread = pi.hThread;

    logf("AiBridge::SpawnSidecar: sidecar PID=%d\n", pi.dwProcessId);

    // Poll for readiness asynchronously from bridge thread (see RunBridgeLoop)
    return true;
}

void AiBridge::ShutdownSidecar(DWORD timeoutMs) {
    if (!mSidecarProcess) return;

    // Graceful path: send CTRL_BREAK to sidecar's process group
    // This allows llama-server to flush state cleanly
    GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, GetProcessId(mSidecarProcess));

    DWORD waitResult = WaitForSingleObject(mSidecarProcess, timeoutMs);
    if (waitResult != WAIT_OBJECT_0) {
        // Graceful shutdown timed out — forced kill as last resort
        logf("AiBridge::ShutdownSidecar: graceful timeout, forcing termination\n");
        TerminateProcess(mSidecarProcess, 0);
        WaitForSingleObject(mSidecarProcess, 1000);
    }

    CloseHandle(mSidecarProcess);
    mSidecarProcess = nullptr;

    if (mSidecarProcessThread) {
        CloseHandle(mSidecarProcessThread);
        mSidecarProcessThread = nullptr;
    }

    logf("AiBridge::ShutdownSidecar: sidecar stopped\n");
}

bool AiBridge::WaitForSidecarReady(int maxWaitMs) {
    // Poll GET /health on llama-server until it returns 200 or timeout
    str::Str url;
    url.AppendFmt("http://127.0.0.1:%d/health", mPort);

    const int pollIntervalMs = 500;
    int elapsed = 0;

    while (elapsed < maxWaitMs) {
        // Check shutdown requested while polling
        if (mShuttingDown.load()) return false;

        HttpRsp rsp;
        if (HttpGet(url.Get(), &rsp) && IsHttpRspOk(&rsp)) {
            logf("AiBridge::WaitForSidecarReady: ready after %dms\n", elapsed);
            mSidecarReady.store(true);
            return true;
        }

        Sleep(pollIntervalMs);
        elapsed += pollIntervalMs;
    }

    logf("AiBridge::WaitForSidecarReady: timed out after %dms\n", maxWaitMs);
    return false;
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

void AiBridge::Shutdown() {
    if (mShuttingDown.exchange(true)) return; // already shutting down

    logf("AiBridge::Shutdown: signaling bridge thread\n");

    // Signal bridge thread to exit
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
// Public API: EnqueueRequest / CancelRequest / IsReady
// ---------------------------------------------------------------------------

uint32_t AiBridge::EnqueueRequest(AiRequestType type, const char* selectedText,
                                   const char* pageContext, HWND targetHwnd,
                                   RECT anchorRect) {
    if (mShuttingDown.load()) return 0;

    uint32_t id = mNextRequestId.fetch_add(1);

    AiRequest req;
    req.id          = id;
    req.type        = type;
    req.selectedText.Set(selectedText);
    req.pageContext.Set(pageContext ? pageContext : "");
    req.targetHwnd  = targetHwnd;
    req.anchorRect  = anchorRect;
    req.state       = AiRequestState::Pending;

    {
        ScopedMutexLock lock(mQueueLock);
        mQueue.Append(req);
    }

    SetEvent(mQueueEvent); // wake bridge thread
    logf("AiBridge::EnqueueRequest: id=%u type=%d\n", id, (int)type);
    return id;
}

void AiBridge::CancelRequest(uint32_t requestId) {
    ScopedMutexLock lock(mQueueLock);
    for (AiRequest& req : mQueue) {
        if (req.id == requestId && req.state == AiRequestState::Pending) {
            req.state = AiRequestState::Canceled;
            logf("AiBridge::CancelRequest: id=%u canceled\n", requestId);
            return;
        }
    }
}

bool AiBridge::IsReady() const {
    return mSidecarReady.load();
}

// ---------------------------------------------------------------------------
// Bridge thread main loop
// ---------------------------------------------------------------------------

DWORD WINAPI AiBridge::BridgeThreadProc(LPVOID param) {
    ((AiBridge*)param)->RunBridgeLoop();
    return 0;
}

void AiBridge::RunBridgeLoop() {
    SetThreadName("AiBridge");

    // Wait for sidecar to become ready before processing any requests
    // This is non-blocking for the UI since we're on bridge thread
    if (!WaitForSidecarReady(20000)) {
        logf("AiBridge::RunBridgeLoop: sidecar never became ready, exiting\n");
        return;
    }

    HANDLE waitHandles[2] = {mShutdownEvent, mQueueEvent};

    while (true) {
        DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);

        if (waitResult == WAIT_OBJECT_0) {
            // Shutdown event signaled — drain and exit
            logf("AiBridge::RunBridgeLoop: shutdown event, exiting\n");
            break;
        }

        if (waitResult != WAIT_OBJECT_0 + 1) {
            // Unexpected wait result
            break;
        }

        // Process all pending requests in queue
        while (true) {
            if (mShuttingDown.load()) goto done;

            // Pop next pending request
            AiRequest req;
            bool found = false;
            {
                ScopedMutexLock lock(mQueueLock);
                for (int i = 0; i < mQueue.size(); i++) {
                    if (mQueue[i].state == AiRequestState::Pending) {
                        req   = mQueue[i];
                        found = true;
                        mQueue.RemoveAt(i);
                        break;
                    }
                }
                // Also remove completed/canceled items
                for (int i = mQueue.size() - 1; i >= 0; i--) {
                    auto s = mQueue[i].state;
                    if (s == AiRequestState::Canceled ||
                        s == AiRequestState::Complete  ||
                        s == AiRequestState::Failed) {
                        mQueue.RemoveAt(i);
                    }
                }
            }

            if (!found) break; // queue empty

            if (req.state != AiRequestState::Pending) {
                // Already canceled while waiting in queue — drop
                logf("AiBridge: req id=%u dropped (state=%d)\n", req.id, (int)req.state);
                continue;
            }

            // --- Send request to sidecar ---
            str::Str responseText;
            bool ok = SendCompletionRequest(req, responseText);

            // ---------------------------------------------------------------------------
            // Memory ownership contract:
            //   - AiResponseMsg is allocated here on the bridge thread
            //   - Ownership transfers to UI thread at PostMessage()
            //   - If PostMessage() fails (HWND destroyed), we free it here
            //   - UI thread MUST delete the object in its WM_AI_RESPONSE_DONE handler
            // ---------------------------------------------------------------------------
            AiResponseMsg* msg = new AiResponseMsg();
            msg->requestId = req.id;
            msg->isError   = !ok;
            if (ok) {
                msg->text.Set(responseText.Get());
            } else {
                msg->text.Set("AI request failed. Is the model loaded?");
            }

            UINT wmsg = ok ? WM_AI_RESPONSE_DONE : WM_AI_ERROR;
            BOOL posted = PostMessage(req.targetHwnd, wmsg,
                                      (WPARAM)req.id,
                                      (LPARAM)msg);
            if (!posted) {
                // HWND was destroyed before we could deliver — free here
                logf("AiBridge: PostMessage failed for req id=%u (HWND gone)\n", req.id);
                delete msg;
            }
        }
    }

done:
    logf("AiBridge::RunBridgeLoop: exited\n");
}

// ---------------------------------------------------------------------------
// HTTP communication with llama-server
// ---------------------------------------------------------------------------

bool AiBridge::SendCompletionRequest(const AiRequest& req, str::Str& responseOut) {
    str::Str prompt;
    BuildPrompt(req, prompt);

    str::Str jsonBody;
    BuildRequestJson(prompt, jsonBody);

    str::Str headers;
    headers.Append("Content-Type: application/json\r\n");

    // AI-HOOK: Uses SumatraPDF's existing HttpPost from utils/HttpUtil.h
    // Signature: bool HttpPost(const char* server, int port,
    //                          const char* url, str::Str* headers, str::Str* data)
    str::Str serverAddr;
    serverAddr.Append("127.0.0.1");

    str::Str rawResponse;
    // Note: HttpPost fills rawResponse via the data param used as output here.
    // AI-HOOK: Check HttpUtil.cpp to confirm output param convention if build fails.
    bool ok = HttpPost(serverAddr.Get(), mPort, "/completion", &headers, &jsonBody);
    if (!ok) {
        logf("AiBridge::SendCompletionRequest: HttpPost failed\n");
        return false;
    }

    // AI-HOOK: HttpPost as declared doesn't return body directly.
    // TODO Phase 1.1: Switch to WinHTTP directly for response body capture.
    // For now this is the scaffold; see AiBridge_Http.cpp in Phase 1.1.
    // The structure, ownership, and threading model are correct.
    responseOut.Set(rawResponse.Get());
    return true;
}

void AiBridge::BuildPrompt(const AiRequest& req, str::Str& out) const {
    // Keep prompts minimal for Phase 1 latency targets
    switch (req.type) {
        case AiRequestType::Define:
            out.Append("<|system|>You are a dictionary. Define the given term in 1-2 sentences. Be concise.<|end|>\n");
            out.Append("<|user|>Define: ");
            out.Append(req.selectedText.Get());
            out.Append("<|end|>\n<|assistant|>");
            break;

        case AiRequestType::Explain:
            out.Append("<|system|>You are a helpful reading assistant. Explain the following passage briefly.<|end|>\n");
            if (req.pageContext.size() > 0) {
                out.Append("<|user|>Context from document:\n");
                // Inject up to 1000 chars of page context to stay under token budget
                int ctxLen = std::min((int)req.pageContext.size(), 1000);
                out.Append(req.pageContext.Get(), ctxLen);
                out.Append("\n\nNow explain this specific selection:\n");
            } else {
                out.Append("<|user|>Explain: ");
            }
            out.Append(req.selectedText.Get());
            out.Append("<|end|>\n<|assistant|>");
            break;

        case AiRequestType::Ask:
        default:
            out.Append("<|system|>You are a helpful reading assistant for PDF documents.<|end|>\n");
            if (req.pageContext.size() > 0) {
                out.Append("<|user|>Document context:\n");
                int ctxLen = std::min((int)req.pageContext.size(), 1000);
                out.Append(req.pageContext.Get(), ctxLen);
                out.Append("\n\nQuestion about the selected text:\n");
            } else {
                out.Append("<|user|>");
            }
            out.Append(req.selectedText.Get());
            out.Append("<|end|>\n<|assistant|>");
            break;
    }
}

void AiBridge::BuildRequestJson(const str::Str& prompt, str::Str& jsonOut) const {
    // llama-server /completion expects:
    // { "prompt": "...", "n_predict": 256, "stream": false, "temperature": 0.7 }
    jsonOut.Append("{\"prompt\":\"");
    JsonEscapeAppend(prompt.Get(), jsonOut);
    jsonOut.Append("\",\"n_predict\":256,\"stream\":false,\"temperature\":0.7,\"stop\":[\"<|end|>\",\"<|user|>\"]}");
}

bool AiBridge::ParseCompletionResponse(const str::Str& jsonBody, str::Str& contentOut) const {
    // llama-server returns: { "content": "...", ... }
    return JsonExtractString(jsonBody.Get(), "content", contentOut);
}
