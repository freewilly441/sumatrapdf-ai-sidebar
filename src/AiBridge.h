/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// AI-HOOK: LLM integration bridge layer.
// Phase 2: switched backend from a llama-server.exe sidecar to Ollama's
// /api/chat. Ollama runs as its own persistent background service (not
// something we spawn/own), so this class no longer manages a child process —
// it only owns the async request queue and the bridge thread that talks to
// it over loopback HTTP via WinInet.

#pragma once

// Callers must include utils/BaseUtil.h and utils/ThreadUtil.h before this header.
#include "AiRequest.h"

// ---------------------------------------------------------------------------
// AiBridge
//
// Owns the async request queue and all communication with Ollama (HTTP POST
// on loopback via WinInet).
//
// Threading model:
//   UI thread    : EnqueueRequest(), CancelRequest(), Shutdown()
//   Bridge thread: dequeues, sends HTTP, posts WM_AI_RESPONSE_DONE to the sidebar
//
// The bridge thread NEVER calls Win32 UI functions directly.
// All UI updates go through PostMessage so the UI thread drives rendering.
// ---------------------------------------------------------------------------
class AiBridge {
  public:
    AiBridge();
    ~AiBridge();

    // Call once from WinMain after main window is created.
    // Does not block: kicks off a background health check against Ollama's
    // /api/tags endpoint. IsReady() flips to true once that succeeds.
    // host  : e.g. "http://localhost:11434"
    // model : Ollama model name, e.g. "llama3.2" (must already be pulled)
    bool Init(const char* host, const char* model);

    // Call from main window WM_DESTROY. Stops the bridge thread.
    void Shutdown();

    // Thread-safe. Enqueue request from UI thread.
    // Returns assigned request_id (monotonic, never reused in session).
    // Returns 0 if bridge is not initialized or shutting down.
    uint32_t EnqueueRequest(AiRequestType type, AiContextMode contextMode, const char* userMessage,
                            const char* contextText, const char* historyText, HWND targetHwnd);

    // Thread-safe. Mark a queued request as canceled.
    // No-op if already complete, failed, or not found.
    void CancelRequest(uint32_t requestId);

    // Returns true once the Ollama health check has passed.
    bool IsReady() const;

    // Thread-safe. Enqueue a GET /api/tags via the same request queue/bridge
    // thread used for chat; posts WM_AI_MODELS_UPDATED to targetHwnd with the
    // discovered model names. Returns assigned request_id, or 0 if the bridge
    // is not initialized/shutting down (same as EnqueueRequest).
    uint32_t RequestModelListRefresh(HWND targetHwnd);

    // Thread-safe. Changes the model used for chat requests sent after this
    // call returns (requests already in flight keep using the old model).
    void SetActiveModel(const char* model);

  private:
    void RunBridgeLoop();

    // GET /api/tags; parses the "models" array's "name" fields into modelsOut.
    // Returns false only on HTTP/transport failure — an empty (but successful)
    // list means Ollama is reachable but has no models pulled.
    bool FetchModelList(StrVec& modelsOut);

    // GET /api/tags; used both as a startup health check and to confirm the
    // configured model is actually available.
    bool WaitForOllamaReady(int maxWaitMs = 20000);

    // POST host/api/chat; fills responseOut with the assistant's reply text.
    bool SendChatRequest(const AiRequest& req, str::Str& responseOut);

    // Message construction
    void BuildMessagesJson(const AiRequest& req, str::Str& jsonOut) const;
    bool ParseChatResponse(const str::Str& jsonBody, str::Str& contentOut) const;

    // --- Bridge thread handle ---
    HANDLE mBridgeThread{nullptr};

    // --- Synchronization ---
    // mQueueEvent  : auto-reset; signaled when queue gains a new item
    // mShutdownEvent: manual-reset; signaled to stop bridge thread
    HANDLE mQueueEvent{nullptr};
    HANDLE mShutdownEvent{nullptr};
    Mutex  mQueueLock;

    // Queue stores heap-allocated AiRequest* (AiRequest contains str::Str
    // so is non-POD; must not be stored by value in Vec)
    Vec<AiRequest*> mQueue;

    // Monotonic request ID; incremented via InterlockedIncrement
    volatile LONG mNextRequestId{0};

    // AtomicInt (volatile LONG): 0 = false, 1 = true
    AtomicInt mOllamaReady{0};
    AtomicInt mShuttingDown{0};

    str::Str mHost;  // e.g. "http://localhost:11434"; not changed after Init()

    // mModel: written from the UI thread (SetActiveModel, called when the
    // user picks a model in the sidebar), read from the bridge thread
    // (BuildMessagesJson) — needs its own lock since it's no longer
    // write-once-before-thread-start like mHost.
    mutable Mutex mModelLock;
    str::Str mModel; // e.g. "llama3.2"
};

// Global singleton — initialized in app startup; destroyed on exit.
// AI-HOOK: defined in AiBridge.cpp
extern AiBridge* gAiBridge;
