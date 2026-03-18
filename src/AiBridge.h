/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// AI-HOOK: New file - Phase 1 LLM integration bridge layer

#pragma once

#include "AiRequest.h"

// ---------------------------------------------------------------------------
// AiBridge
//
// Owns the sidecar process lifetime, the async request queue, and all
// communication with llama-server (Phase 1: HTTP POST on loopback).
//
// Threading model:
//   UI thread    : calls EnqueueRequest(), CancelRequest(), Shutdown()
//   Bridge thread: processes queue, sends HTTP, posts WM_AI_* to targetHwnd
//
// The bridge thread NEVER touches Win32 UI. All UI updates go through
// PostMessage so the UI thread drives all rendering.
// ---------------------------------------------------------------------------
class AiBridge {
  public:
    AiBridge();
    ~AiBridge();

    // Call once from WinMain after main window is created.
    // Spawns sidecar process; returns immediately (non-blocking).
    // modelPath: absolute path to .gguf model file
    // port: llama-server port (default 8080)
    bool Init(const char* modelPath, const char* llamaServerExePath, int port = 8080);

    // Call from WM_DESTROY handler on main window.
    // Attempts graceful sidecar shutdown; falls back to forced kill on timeout.
    void Shutdown();

    // Thread-safe. Enqueue a new AI request from the UI thread.
    // Returns assigned request_id (monotonically increasing, never reused).
    // Returns 0 if bridge is not initialized or shutting down.
    uint32_t EnqueueRequest(AiRequestType type, const char* selectedText,
                            const char* pageContext, HWND targetHwnd,
                            RECT anchorRect);

    // Thread-safe. Cancel a request by id.
    // Safe to call even if the request is already complete or not found.
    // Late-arriving WM_AI_RESPONSE_DONE for a canceled id will be dropped
    // by the UI handler via request_id verification.
    void CancelRequest(uint32_t requestId);

    // Returns true if sidecar has signaled ready (health check passed).
    bool IsReady() const;

  private:
    // Bridge worker thread entry point
    static DWORD WINAPI BridgeThreadProc(LPVOID param);
    void RunBridgeLoop();

    // Sidecar process management
    bool SpawnSidecar(const char* llamaServerExePath, const char* modelPath, int port);
    // Graceful shutdown: sends CTRL_BREAK, waits up to timeoutMs, then TerminateProcess
    void ShutdownSidecar(DWORD timeoutMs = 3000);

    // Poll llama-server health endpoint until ready or timeout
    bool WaitForSidecarReady(int maxWaitMs = 15000);

    // Blocking HTTP POST to llama-server on bridge thread
    // Fills responseOut with full completion text; returns false on failure
    bool SendCompletionRequest(const AiRequest& req, str::Str& responseOut);

    // Build prompt string from request type and text
    // Keeps prompts minimal for Phase 1 latency
    void BuildPrompt(const AiRequest& req, str::Str& promptOut) const;

    // Serialize request to JSON body for llama-server /completion endpoint
    void BuildRequestJson(const str::Str& prompt, str::Str& jsonOut) const;

    // Parse response JSON from llama-server; extract "content" field
    bool ParseCompletionResponse(const str::Str& jsonBody, str::Str& contentOut) const;

    // --- Members ---

    // Sidecar process handles (nullptr until SpawnSidecar succeeds)
    HANDLE mSidecarProcess{nullptr};
    HANDLE mSidecarProcessThread{nullptr};

    // Bridge worker thread
    HANDLE mBridgeThread{nullptr};

    // Synchronization: bridge thread waits on both; queue work signals mQueueEvent
    HANDLE mQueueEvent{nullptr};    // auto-reset; signaled when queue has items
    HANDLE mShutdownEvent{nullptr}; // manual-reset; signaled to stop bridge thread

    // Request queue: UI thread pushes, bridge thread pops
    // Protected by mQueueLock
    Mutex        mQueueLock;
    Vec<AiRequest> mQueue;

    // Monotonically increasing request ID counter
    std::atomic<uint32_t> mNextRequestId{1};

    // Set to true once sidecar health check passes
    std::atomic<bool> mSidecarReady{false};

    // Set to true once Shutdown() is called; EnqueueRequest returns 0
    std::atomic<bool> mShuttingDown{false};

    int  mPort{8080};
    str::Str mLlamaServerPath;
    str::Str mModelPath;
};

// Global singleton. Initialized in AppMain / WinMain; destroyed on exit.
// AI-HOOK: extern declared here; defined in AiBridge.cpp
extern AiBridge* gAiBridge;
