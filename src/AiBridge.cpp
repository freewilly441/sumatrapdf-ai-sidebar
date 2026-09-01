/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// AI-HOOK: LLM integration bridge implementation (Ollama backend, Phase 2)

#include "utils/BaseUtil.h"
#include "utils/ThreadUtil.h"
#include "utils/WinUtil.h"
#include "utils/Log.h"
#include "utils/HttpUtil.h"
#include "AiBridge.h"

// wininet.h included via BaseUtil.h; wininet.lib linked via project.
// MinGW note: x86_64-w64-mingw32 ships wininet.h and libwininet.a, so this
// compiles and links the same way under MinGW as under MSVC. Behavior here
// is loopback-only HTTP with no proxy/TLS involved, so MSVC-vs-MinGW runtime
// divergence risk is low; still worth a smoke test on real Windows.

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
// Sufficient for building Ollama /api/chat requests and extracting the
// "message.content" field from responses.
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

// Extracts every value of "<key>":"..." occurring in json, in order, into
// out (e.g. every model's "name" field in an Ollama /api/tags response's
// "models" array). Same "not a full parser" approach as JsonExtractString
// above — good enough for a flat string key that doesn't recur nested inside
// itself, which "name" doesn't in Ollama's response shape.
static void JsonExtractStringArrayValues(const char* json, const char* key, StrVec& out) {
    if (!json || !key) return;

    str::Str pat;
    pat.AppendChar('"');
    pat.Append(key);
    pat.Append("\":\"", 3);

    const char* pos = json;
    for (;;) {
        pos = str::Find(pos, pat.Get());
        if (!pos) break;
        pos += pat.size();

        str::Str val;
        while (*pos && *pos != '"') {
            if (*pos == '\\' && *(pos + 1)) {
                pos++;
                switch (*pos) {
                    case 'n':  val.AppendChar('\n'); break;
                    case 'r':  val.AppendChar('\r'); break;
                    case 't':  val.AppendChar('\t'); break;
                    case '"':  val.AppendChar('"');  break;
                    case '\\': val.AppendChar('\\'); break;
                    default:   val.AppendChar(*pos); break;
                }
            } else {
                val.AppendChar(*pos);
            }
            pos++;
        }
        if (*pos == '"') pos++;
        out.Append(val.Get());
    }
}

// Parses "http://host:port" (scheme optional, port optional) into separate
// host and port. Ollama defaults to port 11434 when none is given.
static void ParseHostUrl(const char* url, str::Str& hostOut, int& portOut) {
    portOut = 11434;
    const char* p = url;
    const char* schemeSep = str::Find(p, "://");
    if (schemeSep) {
        p = schemeSep + 3;
    }
    const char* colon = str::FindChar(p, ':');
    if (colon) {
        hostOut.Append(p, (size_t)(colon - p));
        portOut = atoi(colon + 1);
    } else {
        // strip a trailing path/slash if present
        const char* slash = str::FindChar(p, '/');
        if (slash) {
            hostOut.Append(p, (size_t)(slash - p));
        } else {
            hostOut.Append(p);
        }
    }
}

// ---------------------------------------------------------------------------
// AiHttpRequest
//
// Self-contained WinInet request with full response body capture.
// Called on bridge thread only (blocking), except the initial readiness
// check which also runs on the bridge thread before the loop starts.
// Uses WinInet directly because the existing HttpGet()/HttpPost() in
// HttpUtil.cpp discard the response body.
// ---------------------------------------------------------------------------
static bool AiHttpRequest(const char* server, int port, const char* path, const char* method,
                          const str::Str* body, str::Str& responseOut) {
    bool ok = false;
    HINTERNET hInet = nullptr, hConn = nullptr, hReq = nullptr;
    DWORD respCode = 0;
    DWORD respCodeSize = sizeof(DWORD);
    DWORD timeoutMs = 60 * 1000; // 60s: CPU inference and large-doc prompts can be slow

    WCHAR* serverW = ToWStrTemp(server);
    WCHAR* pathW   = ToWStrTemp(path);
    WCHAR* methodW = ToWStrTemp(method);

    hInet = InternetOpenW(L"SumatraPDF-LLM",
                          INTERNET_OPEN_TYPE_PRECONFIG,
                          nullptr, nullptr, 0);
    if (!hInet) {
        logf("AiHttpRequest: InternetOpen failed (%d)\n", (int)GetLastError());
        goto Exit;
    }

    hConn = InternetConnectW(hInet, serverW, (INTERNET_PORT)port,
                             nullptr, nullptr,
                             INTERNET_SERVICE_HTTP, 0, 0);
    if (!hConn) {
        logf("AiHttpRequest: InternetConnect failed (%d)\n", (int)GetLastError());
        goto Exit;
    }

    hReq = HttpOpenRequestW(hConn, methodW, pathW,
                            nullptr, nullptr, nullptr,
                            INTERNET_FLAG_NO_UI | INTERNET_FLAG_NO_CACHE_WRITE |
                            INTERNET_FLAG_RELOAD,
                            0);
    if (!hReq) {
        logf("AiHttpRequest: HttpOpenRequest failed (%d)\n", (int)GetLastError());
        goto Exit;
    }

    InternetSetOptionW(hReq, INTERNET_OPTION_SEND_TIMEOUT, &timeoutMs, sizeof(timeoutMs));
    InternetSetOptionW(hReq, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeoutMs, sizeof(timeoutMs));

    {
        const char* hdrs    = body ? "Content-Type: application/json\r\n" : nullptr;
        DWORD       hdrsLen = hdrs ? (DWORD)str::Len(hdrs) : 0;
        void*       data    = body ? (void*)body->Get() : nullptr;
        DWORD       dataLen = body ? (DWORD)body->size() : 0;

        if (!HttpSendRequestA(hReq, hdrs, hdrsLen, data, dataLen)) {
            logf("AiHttpRequest: HttpSendRequest failed (%d)\n", (int)GetLastError());
            goto Exit;
        }
    }

    HttpQueryInfoW(hReq,
                   HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                   &respCode, &respCodeSize, nullptr);

    if (respCode != 200) {
        logf("AiHttpRequest: unexpected HTTP status %d\n", (int)respCode);
        goto Exit;
    }

    for (;;) {
        char   buf[4096];
        DWORD  dwRead = 0;
        if (!InternetReadFile(hReq, buf, sizeof(buf), &dwRead)) {
            logf("AiHttpRequest: InternetReadFile failed (%d)\n", (int)GetLastError());
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
    ScopedLock lk(mQueueLock);
    for (size_t i = 0; i < mQueue.len; i++) {
        delete mQueue[i];
    }
    mQueue.Reset();
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

bool AiBridge::Init(const char* host, const char* model) {
    ReportIf(mBridgeThread); // Must not call Init twice

    mHost.Set(host);
    mModel.Set(model);

    mBridgeThread = StartThread(MkMethod0<AiBridge, &AiBridge::RunBridgeLoop>(this), "AiBridge");
    ReportIf(!mBridgeThread);

    logf("AiBridge::Init: bridge thread started (host=%s model=%s)\n", host, model);
    return true;
}

bool AiBridge::WaitForOllamaReady(int maxWaitMs) {
    str::Str host;
    int port;
    ParseHostUrl(mHost.Get(), host, port);

    const int pollMs = 500;
    int elapsed = 0;

    while (elapsed < maxWaitMs) {
        if (AtomicIntGet(&mShuttingDown)) return false;

        str::Str resp;
        if (AiHttpRequest(host.Get(), port, "/api/tags", "GET", nullptr, resp)) {
            logf("AiBridge::WaitForOllamaReady: ready after %dms\n", elapsed);
            AtomicIntSet(&mOllamaReady, 1);
            return true;
        }
        Sleep(pollMs);
        elapsed += pollMs;
    }

    logf("AiBridge::WaitForOllamaReady: timed out after %dms — is Ollama running?\n", maxWaitMs);
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
    logf("AiBridge::Shutdown: complete\n");
}

// ---------------------------------------------------------------------------
// EnqueueRequest / CancelRequest / IsReady
// ---------------------------------------------------------------------------

uint32_t AiBridge::EnqueueRequest(AiRequestType type, AiContextMode contextMode, const char* userMessage,
                                   const char* contextText, const char* historyText, HWND targetHwnd) {
    if (AtomicIntGet(&mShuttingDown)) return 0;

    // Assign ID atomically; start at 1 so 0 is always "invalid"
    uint32_t id = (uint32_t)InterlockedIncrement(&mNextRequestId);

    AiRequest* req    = new AiRequest();
    req->id           = id;
    req->type         = type;
    req->contextMode  = contextMode;
    req->userMessage.Set(userMessage ? userMessage : "");
    req->contextText.Set(contextText ? contextText : "");
    req->historyText.Set(historyText ? historyText : "");
    req->targetHwnd   = targetHwnd;
    req->state        = AiRequestState::Pending;

    {
        ScopedLock lk(mQueueLock);
        mQueue.Append(req);
    }

    SetEvent(mQueueEvent);
    logf("AiBridge::EnqueueRequest: id=%u type=%d mode=%d\n", id, (int)type, (int)contextMode);
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
    return AtomicIntGet(const_cast<AtomicInt*>(&mOllamaReady)) != 0;
}

uint32_t AiBridge::RequestModelListRefresh(HWND targetHwnd) {
    return EnqueueRequest(AiRequestType::RefreshModels, AiContextMode::Selection, "", "", "", targetHwnd);
}

void AiBridge::SetActiveModel(const char* model) {
    ScopedLock lk(mModelLock);
    mModel.Set(model);
    logf("AiBridge::SetActiveModel: now using model=%s\n", model);
}

// ---------------------------------------------------------------------------
// Bridge thread loop
// ---------------------------------------------------------------------------

void AiBridge::RunBridgeLoop() {
    SetThreadName("AiBridge");

    if (!WaitForOllamaReady(20000)) {
        logf("AiBridge::RunBridgeLoop: Ollama never became ready — exiting\n");
        return;
    }

    HANDLE waitHandles[2] = {mShutdownEvent, mQueueEvent};

    while (true) {
        DWORD w = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);

        if (w == WAIT_OBJECT_0) {
            logf("AiBridge::RunBridgeLoop: shutdown signal\n");
            break;
        }
        if (w != WAIT_OBJECT_0 + 1) break; // unexpected

        // Drain all Pending items from the queue
        while (true) {
            if (AtomicIntGet(&mShuttingDown)) goto done;

            AiRequest* req = nullptr;
            {
                ScopedLock lk(mQueueLock);

                for (size_t i = 0; i < mQueue.len; i++) {
                    if (mQueue[i]->state == AiRequestState::Pending) {
                        req = mQueue[i];
                        mQueue.RemoveAt(i);
                        break;
                    }
                }

                for (int i = (int)mQueue.len - 1; i >= 0; i--) {
                    AiRequestState s = mQueue[i]->state;
                    if (s != AiRequestState::Pending) {
                        delete mQueue[i];
                        mQueue.RemoveAt((size_t)i);
                    }
                }
            }

            if (!req) break; // queue empty

            if (req->state != AiRequestState::Pending) {
                logf("AiBridge: req id=%u dropped (state=%d)\n", req->id, (int)req->state);
                delete req;
                continue;
            }

            if (req->type == AiRequestType::RefreshModels) {
                AiModelsMsg* modelsMsg = new AiModelsMsg();
                modelsMsg->requestId   = req->id;
                bool fetchOk           = FetchModelList(modelsMsg->models);
                modelsMsg->isError     = !fetchOk;

                BOOL modelsPosted = PostMessage(req->targetHwnd, WM_AI_MODELS_UPDATED,
                                                (WPARAM)req->id, (LPARAM)modelsMsg);
                if (!modelsPosted) {
                    logf("AiBridge: PostMessage(WM_AI_MODELS_UPDATED) failed for id=%u (HWND gone)\n", req->id);
                    delete modelsMsg;
                }
                delete req;
                continue;
            }

            str::Str responseText;
            bool ok = SendChatRequest(*req, responseText);

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
                             : "AI error — is Ollama running with the configured model pulled?");

            UINT  wm     = ok ? WM_AI_RESPONSE_DONE : WM_AI_ERROR;
            BOOL  posted = PostMessage(req->targetHwnd, wm, (WPARAM)req->id, (LPARAM)msg);
            if (!posted) {
                logf("AiBridge: PostMessage failed for id=%u (HWND gone)\n", req->id);
                delete msg;
            }

            delete req;
        }
    }

done:
    logf("AiBridge::RunBridgeLoop: exited\n");
}

// ---------------------------------------------------------------------------
// Model discovery
// ---------------------------------------------------------------------------

bool AiBridge::FetchModelList(StrVec& modelsOut) {
    str::Str host;
    int port;
    ParseHostUrl(mHost.Get(), host, port);

    str::Str resp;
    if (!AiHttpRequest(host.Get(), port, "/api/tags", "GET", nullptr, resp)) {
        logf("AiBridge::FetchModelList: HTTP GET /api/tags failed\n");
        return false;
    }

    JsonExtractStringArrayValues(resp.Get(), "name", modelsOut);
    logf("AiBridge::FetchModelList: found %d model(s)\n", modelsOut.Size());
    return true;
}

// ---------------------------------------------------------------------------
// HTTP + message logic
// ---------------------------------------------------------------------------

bool AiBridge::SendChatRequest(const AiRequest& req, str::Str& responseOut) {
    str::Str jsonBody;
    BuildMessagesJson(req, jsonBody);

    str::Str host;
    int port;
    ParseHostUrl(mHost.Get(), host, port);

    str::Str rawResponse;
    if (!AiHttpRequest(host.Get(), port, "/api/chat", "POST", &jsonBody, rawResponse)) {
        logf("AiBridge::SendChatRequest: HTTP POST failed\n");
        return false;
    }

    if (!ParseChatResponse(rawResponse, responseOut)) {
        logf("AiBridge::SendChatRequest: parse failed. Body: %s\n", rawResponse.Get());
        return false;
    }

    return true;
}

// Builds the Ollama /api/chat payload:
//   {"model": "...", "stream": false, "messages": [{"role":"system"|"user"|"assistant", "content":"..."}]}
//
// System instruction depends on req.type; contextText (selection/page/document,
// already extracted+capped by the caller) is folded into the first user turn;
// historyText (prior turns, pre-formatted by the sidebar) is sent as a single
// preceding user/assistant-tagged block since we don't keep a structured
// multi-message history across requests yet — cheap and good enough for now,
// revisit if it costs too many tokens on long conversations.
//
// AI-HOOK: Note/StudySheet/Quiz are export requests (see AiRequest.h). Their
// system prompts ask the model to answer with a single raw JSON object
// matching the shape AiSidebarWnd.cpp's response parser expects — that
// parser is intentionally permissive (falls back to a plain note/empty quiz
// on malformed JSON) since small local models don't always follow the
// instruction exactly. The JSON shape here is a prompting convention only;
// it is not the same as the on-disk export schema in docs/export-schema.md
// (AiExport.cpp builds that from the parsed fields, so the final file is
// always well-formed even if the model's JSON isn't).
void AiBridge::BuildMessagesJson(const AiRequest& req, str::Str& jsonOut) const {
    const char* systemPrompt = "You are a helpful reading assistant embedded in a PDF viewer.";
    if (req.type == AiRequestType::Define) {
        systemPrompt = "You are a dictionary. Define the given term in 1-2 sentences. Be concise.";
    } else if (req.type == AiRequestType::Explain) {
        systemPrompt = "You are a reading assistant. Explain the given passage briefly.";
    } else if (req.type == AiRequestType::Note) {
        systemPrompt =
            "You are a note-taking assistant. Read the conversation below and produce a concise note "
            "capturing the key information a reader would want to remember. "
            "Respond with ONLY a single raw JSON object — no markdown, no code fences, no commentary — "
            "matching exactly this shape: "
            "{\"title\": string, \"body\": string, \"tags\": [string, ...]}. "
            "body is plain text (use \\n for line breaks). Provide 2-5 short, lowercase tags.";
    } else if (req.type == AiRequestType::StudySheet) {
        systemPrompt =
            "You are a study-guide assistant. Read the conversation below and condense it into a structured "
            "study sheet: organize the material under clear headings with bullet points for key facts, "
            "definitions and takeaways. "
            "Respond with ONLY a single raw JSON object — no markdown, no code fences, no commentary — "
            "matching exactly this shape: "
            "{\"title\": string, \"body\": string, \"tags\": [string, ...]}. "
            "body is plain text (use \\n for line breaks and \"- \" for bullets). Provide 2-5 short, lowercase tags.";
    } else if (req.type == AiRequestType::Quiz) {
        systemPrompt =
            "You are a quiz-generation assistant. Read the conversation below and write 3-5 quiz questions "
            "that test understanding of the material. "
            "Respond with ONLY a single raw JSON object — no markdown, no code fences, no commentary — "
            "matching exactly this shape: {\"questions\": [{\"question\": string, \"answer\": string, "
            "\"type\": \"short_answer\" or \"multiple_choice\", \"choices\": [string, ...]}]}. "
            "Only include \"choices\" (4 options) when type is \"multiple_choice\"; omit it for \"short_answer\".";
    }

    jsonOut.AppendFmt("{\"model\":\"");
    {
        ScopedLock lk(mModelLock);
        JsonEscapeAppend(mModel.Get(), jsonOut);
    }
    jsonOut.Append("\",\"stream\":false,\"messages\":[");

    jsonOut.Append("{\"role\":\"system\",\"content\":\"");
    JsonEscapeAppend(systemPrompt, jsonOut);
    jsonOut.Append("\"}");

    if (req.historyText.size() > 0) {
        jsonOut.Append(",{\"role\":\"user\",\"content\":\"");
        JsonEscapeAppend("Earlier in this conversation:\n", jsonOut);
        JsonEscapeAppend(req.historyText.Get(), jsonOut);
        jsonOut.Append("\"}");
    }

    jsonOut.Append(",{\"role\":\"user\",\"content\":\"");
    if (req.contextText.size() > 0) {
        const char* label = "Selected text";
        if (req.contextMode == AiContextMode::Page) label = "Current page";
        if (req.contextMode == AiContextMode::Document) label = "Document";
        bool isExport = req.type == AiRequestType::Note || req.type == AiRequestType::StudySheet ||
                        req.type == AiRequestType::Quiz;
        if (isExport) {
            label = "Conversation";
        }
        JsonEscapeAppend(label, jsonOut);
        JsonEscapeAppend(":\n", jsonOut);
        JsonEscapeAppend(req.contextText.Get(), jsonOut);
        JsonEscapeAppend("\n\n", jsonOut);
    }
    JsonEscapeAppend(req.userMessage.Get(), jsonOut);
    jsonOut.Append("\"}");

    jsonOut.Append("]}");
}

bool AiBridge::ParseChatResponse(const str::Str& jsonBody, str::Str& contentOut) const {
    // Ollama /api/chat (stream:false) response: { "message": { "role": "assistant", "content": "..." }, ... }
    return JsonExtractString(jsonBody.Get(), "content", contentOut);
}
