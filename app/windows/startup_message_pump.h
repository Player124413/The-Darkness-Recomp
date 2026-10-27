#pragma once
#include <windows.h>
#include <algorithm>
#include <exception>
#include <stdexcept>
#include "runtime/native/startup_trace.h"

namespace DarkRecomp::Native {
class NativeStartupMessagePump;
inline thread_local NativeStartupMessagePump* activeStartupMessagePump = nullptr;
struct NativeStartupCanceled final : std::exception {
    int exitCode;
    const char* reason;
    explicit NativeStartupCanceled(int code = 0, const char* why = "window-close") noexcept
        : exitCode(code), reason(why) {}
    const char* what() const noexcept override { return "Native startup canceled before guest execution"; }
};

// Window-thread startup scope only. No callback or pump survives finish().
class NativeStartupMessagePump {
    HWND window_;
    bool active_ = true, canceled_ = false, resizePending_ = false, quitReposted_ = false;
    WPARAM quitCode_ = 0;
    int cancelCode_ = 0;
    const char* cancelReason_ = "window-close";
    bool trace_ = StartupTrace::enabled(), firstPump_ = true, summaryWritten_ = false;
    StartupTrace::PumpSummary summary_{};
    LARGE_INTEGER previousPump_{};
    LARGE_INTEGER budgetFrequency_{};

    long long counter() {
        LARGE_INTEGER now{};
        if (!QueryPerformanceCounter(&now)) {
            canceled_ = true;
            throw NativeStartupCanceled{1,"startup-input-clock-failure"};
        }
        return now.QuadPart;
    }
    bool expired(long long start, unsigned milliseconds) {
        return counter() - start >= budgetFrequency_.QuadPart * milliseconds / 1000;
    }

    void beginTrace() noexcept {
        if (!trace_) return;
        LARGE_INTEGER now{};
        const bool ok = QueryPerformanceCounter(&now) != FALSE;
        summary_.gapClockOk = summary_.gapClockOk && ok;
        if (ok && previousPump_.QuadPart && now.QuadPart >= previousPump_.QuadPart)
            summary_.maxGapQpc = (std::max)(summary_.maxGapQpc, now.QuadPart - previousPump_.QuadPart);
        previousPump_ = now;
        ++summary_.calls;
        if (firstPump_) StartupTrace::mark("startup-pump.first.begin");
    }
    void writeSummary() noexcept {
        if (trace_ && !summaryWritten_) {
            summary_.canceled = canceled_;
            StartupTrace::pumpSummary(summary_);
            summaryWritten_ = true;
        }
    }
    bool take(UINT first, UINT last, MSG& message) noexcept {
        const bool found = PeekMessageW(&message, first == WM_QUIT ? nullptr : window_, first, last, PM_REMOVE) != FALSE;
        if (found && trace_) ++summary_.messages;
        if (found && message.message == WM_QUIT) {
            canceled_ = true;
            quitCode_ = message.wParam;
            cancelCode_ = int(quitCode_); cancelReason_ = "WM_QUIT";
            PostQuitMessage(int(quitCode_)); // Never lose or overwrite the original quit code.
            quitReposted_ = true;
        }
        return found;
    }
public:
    static constexpr unsigned maxMessages = 32;
    static constexpr unsigned maxInputMessages = 1024, inputBudgetMs = 2;
    static constexpr unsigned maxFinishBatches = 128, maxFinishInputMessages = 32768, finishBudgetMs = 250;
    struct Batch { unsigned windowMessages = 0, inputMessages = 0; bool inputPending = false; };
    explicit NativeStartupMessagePump(HWND window) : window_(window) {
        DWORD process = 0;
        if (GetWindowThreadProcessId(window, &process) != GetCurrentThreadId() ||
            process != GetCurrentProcessId() || activeStartupMessagePump)
            throw std::logic_error("Startup pump requires its own window/thread and no nested scope");
        if (!QueryPerformanceFrequency(&budgetFrequency_) || budgetFrequency_.QuadPart <= 0)
            throw std::runtime_error("Startup input budget clock unavailable");
        activeStartupMessagePump = this;
    }
    ~NativeStartupMessagePump() {
        writeSummary();
        // Declared before the display/preview: COM owners unwind before this
        // destroys the HWND. Keep the startup-only window procedure active.
        if (active_ && IsWindow(window_)) DestroyWindow(window_);
        if (activeStartupMessagePump == this) activeStartupMessagePump = nullptr;
    }
    NativeStartupMessagePump(const NativeStartupMessagePump&) = delete;
    NativeStartupMessagePump& operator=(const NativeStartupMessagePump&) = delete;
    bool owns(HWND window) const noexcept { return active_ && window == window_; }
    bool resizePending() const noexcept { return resizePending_; }
    LRESULT windowMessage(UINT message, WPARAM wParam, LPARAM lParam) noexcept {
        switch (message) {
        case WM_CLOSE: canceled_ = true; return 0; // Defer HWND destruction until COM unwinds.
        case WM_DESTROY:
            canceled_ = true;
            if (!quitReposted_) { PostQuitMessage(0); quitReposted_ = true; }
            return 0;
        case WM_NCDESTROY: canceled_ = true; break;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            BeginPaint(window_, &paint); EndPaint(window_, &paint);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_SIZE: if (wParam != SIZE_MINIMIZED) resizePending_ = true; return 0;
        case WM_DISPLAYCHANGE: resizePending_ = true; return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_CLOSE) canceled_ = true;
            return 0; // No modal move/size/menu loop during partial construction.
        case WM_NCLBUTTONDOWN: case WM_NCLBUTTONUP: case WM_NCLBUTTONDBLCLK:
            if (wParam == HTCLOSE) canceled_ = true;
            return 0;
        case WM_SYSKEYDOWN:
            if (wParam == VK_F4 && (lParam & (LPARAM(1) << 29))) canceled_ = true;
            return 0;
        case WM_INPUT:
            // Loading input is deliberately ignored before a guest exists.
            // Raw input still requires Windows cleanup, without NativeInput.
            return DefWindowProcW(window_, message, wParam, lParam);
        }
        if ((message >= WM_KEYFIRST && message <= WM_KEYLAST) ||
            (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) ||
            message == WM_COMMAND || message == WM_HOTKEY || message >= WM_APP)
            return 0; // Sent messages must not enter guest/menu/capture handlers.
        return DefWindowProcW(window_, message, wParam, lParam);
    }
    Batch pump(unsigned inputLimit = maxInputMessages) {
        if (!active_ || activeStartupMessagePump != this) throw std::logic_error("Inactive startup pump");
        beginTrace();
        inputLimit = (std::min)(inputLimit, maxInputMessages);
        Batch batch;
        MSG message{};
        // A full-range queue check supports startup responsiveness without consuming messages.
        PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
        take(WM_QUIT, WM_QUIT, message);
        // Deliberately discard owned loading input, avoiding stale focus/key
        // replay into the first guest frame. Private/thread commands and other
        // windows remain queued. Never read/repost arbitrary raw-input handles.
        constexpr UINT safe[]{WM_CLOSE, WM_SYSCOMMAND, WM_NCLBUTTONDOWN, WM_NCLBUTTONUP,
            WM_NCLBUTTONDBLCLK, WM_PAINT, WM_SIZE, WM_DISPLAYCHANGE, WM_NCPAINT, WM_NCACTIVATE};
        while (!canceled_ && batch.windowMessages < maxMessages) {
            bool found = false;
            for (const auto kind : safe) {
                if (take(kind, kind, message)) {
                    found = true;
                    if (!canceled_) { ++batch.windowMessages; DispatchMessageW(&message); }
                }
                if (canceled_ || batch.windowMessages == maxMessages) break;
            }
            if (!found) break;
        }
        // Drain each input range efficiently; do not rescan ten window-message
        // filters for every raw packet. The QPC budget is sampled every32 inputs.
        constexpr UINT input[][2]{{WM_KEYFIRST,WM_KEYLAST},{WM_MOUSEFIRST,WM_MOUSELAST},{WM_INPUT,WM_INPUT}};
        const auto inputStart = canceled_ ? 0 : counter();
        bool timeLimit = false;
        for (const auto& range : input) {
            while (!canceled_ && !timeLimit && batch.inputMessages < inputLimit &&
                   take(range[0], range[1], message)) {
                if (canceled_) break;
                ++batch.inputMessages;
                if (message.message == WM_INPUT)
                    DefWindowProcW(message.hwnd, message.message, message.wParam, message.lParam);
                else if (message.message == WM_SYSKEYDOWN && message.wParam == VK_F4 &&
                         (message.lParam & (LPARAM(1) << 29))) canceled_ = true;
                if (batch.inputMessages % 32 == 0) timeLimit = expired(inputStart, inputBudgetMs);
            }
            if (canceled_ || timeLimit || batch.inputMessages == inputLimit) break;
        }
        for (const auto& range : input) {
            if (canceled_) break;
            if (PeekMessageW(&message, window_, range[0], range[1], PM_NOREMOVE)) {
                if (message.message == WM_QUIT) take(WM_QUIT, WM_QUIT, message);
                else { batch.inputPending = true; break; }
            }
        }
        if (trace_) {
            if (firstPump_) StartupTrace::mark("startup-pump.first.end", canceled_ ? "canceled" : "returned");
        }
        firstPump_ = false;
        if (canceled_ || !IsWindow(window_)) { canceled_ = true; throw NativeStartupCanceled{cancelCode_,cancelReason_}; }
        return batch;
    }
    void finish() {
        // Capacity exceeds the normal Windows posted-message queue (~10k).
        // A continuously replenished queue still has fixed count/time budgets.
        const auto start = counter();
        bool drained = false;
        unsigned inputs = 0;
        for (unsigned i = 0; i < maxFinishBatches; ++i) {
            const auto batch = pump((std::min)(maxInputMessages, maxFinishInputMessages - inputs));
            inputs += batch.inputMessages;
            if (!batch.inputPending && batch.windowMessages < maxMessages) { drained = true; break; }
            if (inputs >= maxFinishInputMessages || expired(start, finishBudgetMs)) break;
        }
        if (!drained) {
            canceled_ = true;
            throw NativeStartupCanceled{1,"loading-input-overload"};
        }
        writeSummary();
        active_ = false;
        activeStartupMessagePump = nullptr;
    }
};
inline bool handleNativeStartupWindowMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                                             LRESULT& result) noexcept {
    if (!activeStartupMessagePump || !activeStartupMessagePump->owns(window)) return false;
    result = activeStartupMessagePump->windowMessage(message, wParam, lParam);
    return true;
}
}
