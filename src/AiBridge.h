/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// AI-HOOK: Phase 1 LLM integration bridge layer

#pragma once

// Callers must include utils/BaseUtil.h and utils/ThreadUtil.h before this header.
#include "AiRequest.h"

// ---------------------------------------------------------------------------
// AiBridge
//
// Owns sidecar process lifetime, async request queue, and all communication
// with llama-server (Phase 1: HTTP POST on loopback via WinInet).
//
// Threading model:
//   UI thread    : EnqueueRequest(), CancelRequest(), Shutdown()
//   Bridge thread: dequeues, sends HTTP, posts WM_AI_RESPONSE_DONE to canvas
//
// The bridge thread NEVER calls Win32 UI functions directly.
// All UI updates go through PostMessage so the UI thread drives rendering.
// ---------------------------------------------------------------------------
class AiBridge {
  public:
    AiBridge();
    ~AiBridge();

    // Call once from WinMain after main window is created.
    // Spawns sidecar; returns immediately (non-blocking).
    // modelPath          : absolute path to .gguf model file
    // llamaServerExePath : absolute path to llama-server.exe
    // port               : llama-server port (default 8080)
    bool Init(const char* modelPath, const char* llamaServerExePath, int port = 8080);

    // Call from main window WM_DESTROY.
    // Attempts graceful sidecar shutdown; forces kill on timeout.
    void Shutdown();

    // Thread-safe. Enqueue request from UI thread.
    // Returns assigned request_id (monotonic, never reused in session).
    // Returns 0 if bridge is not initialized or shutting down.
    uint32_t EnqueueRequest(AiRequestType type, const char* selectedText,
                            const char* pageContext, HWND targetHwnd,
                            RECT anchorRect);

    // Thread-safe. Mark a queued request as canceled.
    // No-op if already complete, failed, or not found.
    void CancelRequest(uint32_t requestId);

    // Returns true once sidecar health check has passed.
    bool IsReady() const;

  private:
    void RunBridgeLoop();

    // Sidecar process management
    bool SpawnSidecar(const char* exePath, const char* modelPath, int port);
    void ShutdownSidecar(DWORD gracefulTimeoutMs = 3000);
    bool WaitForSidecarReady(int maxWaitMs = 20000);

    // HTTP POST to llama-server /completion; fills responseOut with body
    bool SendCompletionRequest(const AiRequest& req, str::Str& responseOut);

    // Prompt construction
    void BuildPrompt(const AiRequest& req, str::Str& out) const;
    void BuildRequestJson(const str::Str& prompt, str::Str& jsonOut) const;
    bool ParseCompletionResponse(const str::Str& jsonBody, str::Str& contentOut) const;

    // --- Sidecar handles ---
    HANDLE mSidecarProcess{nullptr};
    HANDLE mSidecarThread{nullptr};

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
    AtomicInt mSidecarReady{0};
    AtomicInt mShuttingDown{0};

    int      mPort{8080};
    str::Str mLlamaServerPath;
    str::Str mModelPath;
};

// Global singleton — initialized in app startup; destroyed on exit.
// AI-HOOK: defined in AiBridge.cpp
extern AiBridge* gAiBridge;
