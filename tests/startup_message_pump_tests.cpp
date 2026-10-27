#include "app/windows/startup_message_pump.h"
#include "app/windows/native_shutdown.h"
#include "renderer/engine/scene_work.h"
#include <cstdio>
#include <iterator>
#include <string_view>

using namespace DarkRecomp::Native;
static void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct State {
    unsigned paints = 0, guestCalls = 0, capDispatches = 0;
    bool resourceAlive = false, destroyedBeforeUnwind = false, capRegression = false, capClosePosted = false;
};
static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM key, LPARAM detail) {
    auto* state = reinterpret_cast<State*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        state = static_cast<State*>(reinterpret_cast<CREATESTRUCTW*>(detail)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (state && message == WM_PAINT) ++state->paints;
    if (state && state->capRegression && message == WM_SIZE &&
        ++state->capDispatches == NativeStartupMessagePump::maxMessages)
        state->capClosePosted = PostMessageW(window, WM_CLOSE, 0, 0) != FALSE;
    if (state && message == WM_DESTROY && state->resourceAlive) state->destroyedBeforeUnwind = true;
    LRESULT result = 0;
    if (handleNativeStartupWindowMessage(window, message, key, detail, result)) return result;
    if (state && (message == WM_KEYDOWN || message == WM_KEYUP || message == WM_SYSKEYDOWN || message == WM_LBUTTONDOWN ||
                  message == WM_INPUT || message == WM_COMMAND || message >= WM_APP)) ++state->guestCalls;
    if (message == WM_SYSKEYDOWN) return 0; // Mock normal consumer; no system command/UI.
    return DefWindowProcW(window, message, key, detail);
}
struct Window {
    State state;
    HWND handle;
    Window() : handle(CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        L"DarkRecompStartupPumpContract", L"hidden CPU fixture", WS_POPUP,
        0, 0, 32, 32, nullptr, nullptr, GetModuleHandleW(nullptr), &state)) {
        check(handle != nullptr && !IsWindowVisible(handle), "fixture must remain hidden");
    }
    ~Window() { if (IsWindow(handle)) DestroyWindow(handle); }
};
static bool queued(HWND window, UINT kind, MSG& message, UINT flags = PM_NOREMOVE) {
    return PeekMessageW(&message, window, kind, kind, flags) != FALSE;
}
static void consumeQuit(WPARAM expected) {
    MSG message{};
    unsigned count = 0;
    while (queued(nullptr, WM_QUIT, message, PM_REMOVE)) {
        check(message.message == WM_QUIT && message.wParam == expected, "quit/code lost or overwritten");
        check(++count < 8, "unbounded duplicate quit messages");
    }
    check(count == 1, "quit repost duplicated or lost");
}
static void normalRouting() {
    Window window, other;
    NativeStartupMessagePump pump(window.handle);
    const UINT input[]{WM_KEYDOWN, WM_KEYUP, WM_SYSKEYDOWN, WM_LBUTTONDOWN, WM_INPUT};
    for (const auto kind : input) check(PostMessageW(window.handle, kind,
        kind == WM_INPUT ? RIM_INPUT : kind == WM_SYSKEYDOWN ? VK_MENU : VK_F5,
        kind == WM_SYSKEYDOWN ? LPARAM(1) << 29 : 0) != 0, "cannot queue ordinary input");
    check(PostMessageW(other.handle, WM_APP + 2, 9, 0) != 0, "cannot queue other-window sentinel");
    check(PostThreadMessageW(GetCurrentThreadId(), WM_APP + 3, 7, 0) != 0, "cannot queue thread sentinel");
    check(PostMessageW(window.handle, WM_COMMAND, 19, 0) != 0 &&
          PostMessageW(window.handle, WM_APP + 1, 21, 0) != 0, "cannot queue private commands");
    SendMessageW(window.handle, WM_KEYDOWN, VK_F1, 0);
    SendMessageW(window.handle, WM_LBUTTONDOWN, 0, 0);
    SendMessageW(window.handle, WM_KILLFOCUS, 0, 0);
    SendMessageW(window.handle, WM_SETFOCUS, 0, 0); // No actual focus APIs or activation.
    check(window.state.guestCalls == 0, "sent input entered partial-startup guest/capture routing");
    check(PostMessageW(window.handle, WM_PAINT, 0, 0) != 0, "cannot queue paint");
    RECT initialized{}; check(GetClientRect(window.handle, &initialized) != 0, "cannot sample initialized client");
    // A real hidden client-size change during simulated preparation must
    // survive the startup handoff even if initial display sampling was earlier.
    check(SetWindowPos(window.handle, nullptr, 0, 0, 64, 48,
        SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOMOVE) != 0, "cannot resize hidden fixture during preparation");
    RECT prepared{}; check(GetClientRect(window.handle, &prepared) != 0 &&
        prepared.right != initialized.right && !IsWindowVisible(window.handle), "fixture did not change hidden client size");
    const auto batch = pump.pump();
    check(batch.windowMessages <= NativeStartupMessagePump::maxMessages &&
          batch.inputMessages <= NativeStartupMessagePump::maxInputMessages, "pump exceeded separate bounds");
    check(window.state.paints != 0 && pump.resizePending() && window.state.guestCalls == 0, "safe paint/deferred resize failed");
    MSG message{};
    for (const auto kind : input) check(!queued(window.handle, kind, message), "loading input was retained for stale replay");
    check(queued(other.handle, WM_APP + 2, message) && message.wParam == 9, "other window queue consumed");
    check(queued(nullptr, WM_APP + 3, message) && !message.hwnd && message.wParam == 7, "thread command consumed");
    pump.finish();
    check(pump.resizePending(), "resize received during preparation lost at handoff");
    check(activeStartupMessagePump == nullptr, "startup route retained after finish");
    check(window.state.guestCalls == 0, "loading input replayed at handoff");
    check(queued(window.handle, WM_COMMAND, message, PM_REMOVE) && message.wParam == 19, "private command consumed");
    check(queued(window.handle, WM_APP + 1, message, PM_REMOVE) && message.wParam == 21, "private application command consumed");
    SendMessageW(window.handle, WM_KEYDOWN, VK_F5, 0);
    SendMessageW(window.handle, WM_KEYUP, VK_F5, 0);
    check(window.state.guestCalls == 2, "fresh normal input routing not restored");
    check(queued(other.handle, WM_APP + 2, message, PM_REMOVE), "missing other-window sentinel");
    check(queued(nullptr, WM_APP + 3, message, PM_REMOVE), "missing thread sentinel");
    std::puts("PASS loading policy: paint/actual hidden resize retained; stale key/mouse/raw input ignored through focus transitions; private/foreign/thread commands retained; only fresh post-handoff input routed.");
}
struct Resource {
    State& state;
    explicit Resource(State& value) : state(value) { state.resourceAlive = true; }
    ~Resource() { state.resourceAlive = false; }
};
static void cancellation(UINT message, WPARAM key, LPARAM detail = 0, bool sent = false, bool flood = false) {
    Window window;
    bool canceled = false, guestStarted = false;
    try {
        NativeStartupMessagePump pump(window.handle);
        Resource resource(window.state); // Same destruction order as main's COM-owning renderer.
        if (flood) {
            for (unsigned i = 0; i < 200; ++i) {
                check(PostMessageW(window.handle, WM_SIZE, SIZE_RESTORED, 0) != 0, "cannot queue safe flood");
                check(PostMessageW(window.handle, WM_KEYDOWN, VK_F5, 0) != 0, "cannot queue input flood");
            }
            check(PostMessageW(window.handle, WM_PAINT, 0, 0) != 0, "cannot queue flood paint");
            check(pump.pump().windowMessages == NativeStartupMessagePump::maxMessages, "safe flood was not bounded at 32");
            check(window.state.paints != 0 && window.state.guestCalls == 0, "flood starved paint or routed guest input");
        }
        if (!sent && message == WM_SYSKEYDOWN)
            check(PostMessageW(window.handle, WM_SYSKEYDOWN, VK_MENU, 0) != 0, "cannot post preceding physical Alt key");
        if (sent) SendMessageW(window.handle, message, key, detail);
        else check(PostMessageW(window.handle, message, key, detail) != 0, "cannot post close request");
        pump.pump();
        guestStarted = true;
        pump.finish();
    } catch (const NativeStartupCanceled& cancel) { canceled = true; check(cancel.exitCode == 0, "explicit close must exit zero"); }
    check(canceled && !guestStarted && !IsWindow(window.handle), "startup cancellation did not unwind before guest");
    check(!window.state.resourceAlive && !window.state.destroyedBeforeUnwind, "window destroyed before renderer resources unwound");
    check(window.state.guestCalls == 0 && activeStartupMessagePump == nullptr, "cancel leaked input or startup route");
    consumeQuit(0);
    std::printf("PASS cancel: message=%u sent=%u flood=%u; resources precede HWND destruction; no guest start.\n", message, unsigned(sent), unsigned(flood));
}
static void quitPriority() {
    Window window;
    bool canceled = false;
    try {
        NativeStartupMessagePump pump(window.handle);
        Resource resource(window.state);
        check(PostMessageW(window.handle, WM_SIZE, SIZE_RESTORED, 0) != 0, "cannot post prequit message");
        check(PostThreadMessageW(GetCurrentThreadId(), WM_APP + 4, 11, 0) != 0, "cannot post later command");
        check(PostThreadMessageW(GetCurrentThreadId(), WM_QUIT, 37, 0) != 0, "cannot deliver quit message");
        pump.pump();
    } catch (const NativeStartupCanceled& cancel) { canceled = true; check(cancel.exitCode == 37, "delivered quit code not carried to cancellation"); }
    check(canceled && !IsWindow(window.handle) && !window.state.destroyedBeforeUnwind, "quit did not cancel safely");
    MSG message{};
    check(queued(nullptr, WM_APP + 4, message, PM_REMOVE) && message.wParam == 11, "later message overwrote quit or was drained");
    consumeQuit(37);
    std::puts("PASS quit: WM_QUIT priority/code preserved; later thread command untouched; safe unwind.");
}
static void virtualQuitEventually() {
    Window window;
    NativeStartupMessagePump pump(window.handle);
    check(PostThreadMessageW(GetCurrentThreadId(), WM_APP + 5, 13, 0) != 0, "cannot post deferred private command");
    PostQuitMessage(41);
    // Windows does not materialize this virtual low-priority flag while the
    // unrelated posted command is retained. This approved limitation is explicit.
    pump.finish();
    MSG message{};
    check(takeNativeMessage(message) && message.message == WM_APP + 5 && message.wParam == 13,
          "private command not retained for normal routing");
    check(!takeNativeMessage(message) && message.message == WM_QUIT && message.wParam == 41,
          "virtual quit/code did not eventually reach normal routing");
    check(!queued(nullptr, WM_QUIT, message, PM_REMOVE), "virtual quit duplicated");
    std::puts("PASS virtual quit: deferred flag/code eventually delivered after private command; no full-queue stash or raw repost.");
}
static void inputBurst() {
    Window window;
    NativeStartupMessagePump pump(window.handle);
    for (unsigned i = 0; i < 5000; ++i)
        check(PostMessageW(window.handle, WM_INPUT, RIM_INPUT, 0) != 0, "cannot queue 5000-message owned input burst");
    pump.finish();
    MSG message{};
    check(!queued(window.handle, WM_INPUT, message) && window.state.guestCalls == 0 &&
          IsWindow(window.handle) && !IsWindowVisible(window.handle), "ordinary input burst canceled or replayed into guest");
    std::puts("PASS burst: 5000 owned input messages (>1024) drained at handoff without guest callbacks or cancellation; synthetic packet placeholders, no hardware rate claim.");
}
static thread_local HWND replenishedWindow = nullptr;
static LRESULT CALLBACK replenishInput(int code, WPARAM mode, LPARAM detail) {
    const auto* message = reinterpret_cast<const MSG*>(detail);
    if (code >= 0 && mode == PM_REMOVE && message->hwnd == replenishedWindow && message->message == WM_INPUT)
        PostMessageW(replenishedWindow, WM_INPUT, RIM_INPUT, 0);
    return CallNextHookEx(nullptr, code, mode, detail);
}
static void inputOverload() {
    Window window;
    bool canceled = false;
    try {
        NativeStartupMessagePump pump(window.handle);
        Resource resource(window.state);
        // A fixture-only WH_GETMESSAGE hook on this exact test thread posts a
        // replacement after each removal. This deterministically keeps the real
        // owned queue nonempty without hardware, foreign hooks or timing races.
        struct Replenisher {
            HHOOK hook;
            explicit Replenisher(HWND window) {
                replenishedWindow = window;
                hook = SetWindowsHookExW(WH_GETMESSAGE, replenishInput, nullptr, GetCurrentThreadId());
                check(hook != nullptr, "cannot install owned-thread fixture hook");
            }
            ~Replenisher() { UnhookWindowsHookEx(hook); replenishedWindow = nullptr; }
        } replenisher(window.handle);
        check(PostMessageW(window.handle, WM_INPUT, RIM_INPUT, 0) != 0, "cannot seed persistent owned input");
        pump.finish();
    } catch (const NativeStartupCanceled& cancel) {
        canceled = true;
        check(cancel.exitCode == 1 && std::string_view(cancel.reason) == "loading-input-overload", "overload not explicit/nonzero");
    }
    check(canceled && window.state.guestCalls == 0 && !IsWindow(window.handle) &&
          !window.state.destroyedBeforeUnwind, "overload handed stale input to a guest or failed cleanup");
    consumeQuit(0);
    std::puts("PASS overload: continuously replenished owned input fails closed within independent count/time/batch budgets; own-thread fixture hook removed before unwind; no guest/input replay.");
}
static void closeAtWindowCap() {
    Window window;
    bool canceled = false, guestHandoff = false;
    try {
        NativeStartupMessagePump pump(window.handle);
        Resource resource(window.state);
        pump.pump(); // Clear only fixture creation messages before arming the regression.
        window.state.capRegression = true;
        for (unsigned i = 0; i < NativeStartupMessagePump::maxMessages; ++i)
            check(PostMessageW(window.handle, WM_SIZE, SIZE_RESTORED, 0) != 0, "cannot queue exact safe-window cap");
        pump.finish();
        guestHandoff = true; // Must never be reached after the32nd dispatch posts close.
    } catch (const NativeStartupCanceled& cancel) {
        canceled = true;
        check(cancel.exitCode == 0, "close at safe-window cap must exit zero");
    }
    check(window.state.capClosePosted && window.state.capDispatches == NativeStartupMessagePump::maxMessages,
          "regression did not post close during the32nd safe dispatch");
    check(canceled && !guestHandoff && !IsWindow(window.handle) && !window.state.destroyedBeforeUnwind,
          "capped safe-window batch handed off before checking its newly posted close");
    consumeQuit(0);
    std::puts("PASS safe-window cap: WM_CLOSE posted during32nd safe dispatch cancels finish before guest handoff; safe unwind and single quit.");
}
static unsigned retainedTimerCalls = 0;
static void CALLBACK retainedTimer(HWND, UINT, UINT_PTR, DWORD) { ++retainedTimerCalls; }
static void callbackTimerRetention() {
    for (const bool threadTimer : {false, true}) {
        Window window;
        NativeStartupMessagePump pump(window.handle);
        retainedTimerCalls = 0;
        const auto callback = reinterpret_cast<LPARAM>(&retainedTimer);
        const auto queue = threadTimer ? reinterpret_cast<HWND>(INT_PTR(-1)) : window.handle;
        struct RegisteredTimer {
            HWND window;
            UINT_PTR identity;
            explicit RegisteredTimer(HWND target) : window(target),
                identity(SetTimer(target, target ? 101 : 0, 20, retainedTimer)) {
                check(identity != 0, "cannot register process-local callback timer fixture");
            }
            ~RegisteredTimer() { KillTimer(window, identity); }
        } timer(threadTimer ? nullptr : window.handle);
        const auto identity = timer.identity;
        // Let Windows generate a real registered timer message at the queue
        // head. A manually posted callback pointer is not a valid timer setup.
        Sleep(40);
        pump.pump();
        MSG message{};
        check(queued(queue, WM_TIMER, message) && message.wParam == identity && message.lParam == callback,
              "startup heartbeat consumed or changed callback timer");
        check(retainedTimerCalls == 0, "startup heartbeat dispatched callback timer");
        pump.finish();
        check(retainedTimerCalls == 0 && queued(queue, WM_TIMER, message, PM_REMOVE) &&
              message.wParam == identity && message.lParam == callback, "callback timer not retained through handoff");
        DispatchMessageW(&message); // Deliberate normal routing after startup has finished.
        check(retainedTimerCalls == 1, "retained callback timer not executable by normal routing");
    }
    std::puts("PASS callback timers: owned-window and thread timers remain unchanged/unexecuted through heartbeat/handoff, then execute exactly once by deliberate normal dispatch.");
}
static void workerInitializationCancel() {
    Window window;
    SceneWorkPool pool(2);
    check(pool.workers() == 2, "cannot create two CPU scene workers for cancellation test");
    std::vector<unsigned> values(64);
    check(pool.run(values.size(), 1, [&](size_t first, size_t last) noexcept {
        for (size_t i = first; i < last; ++i) values[i] = unsigned(i + 1); return true;
    }), "CPU scene work fixture failed");
    for (size_t i = 0; i < values.size(); ++i) check(values[i] == i + 1, "CPU scene work result incomplete");
    bool stopped = false, canceled = false, guestStarted = false;
    try {
        NativeStartupMessagePump pump(window.handle);
        Resource resource(window.state);
        struct ArmedStop {
            SceneWorkPool& pool; bool& stopped;
            ~ArmedStop() { pool.stopForStartupCancellation(); stopped = true; }
        } guard{pool, stopped};
        check(PostMessageW(window.handle, WM_CLOSE, 0, 0) != 0, "cannot close after worker initialization");
        pump.finish();
        guestStarted = true;
    } catch (const NativeStartupCanceled& cancel) { canceled = true; check(cancel.exitCode == 0, "worker-init close exit code changed"); }
    check(canceled && stopped && !guestStarted && !IsWindow(window.handle) &&
          !window.state.destroyedBeforeUnwind, "worker-init cancellation did not stop/join before resource unwind");
    pool.stopForStartupCancellation(); // The terminal operation, and later destructor, are idempotent.
    consumeQuit(0);
    std::puts("PASS worker gap: real two-thread CPU pool stopped/joined by armed cleanup before resource/window unwind; repeated stop safe; guest never started.");
}
int main(int argc, char** argv) {
    try {
        const bool trace = argc == 2 && std::string_view(argv[1]) == "--trace";
        check(argc == 1 || trace, "unsupported fixture argument");
        SetEnvironmentVariableA("DARK_STARTUP_TRACE", trace ? "1" : nullptr);
        WNDCLASSW type{}; type.lpfnWndProc = procedure; type.hInstance = GetModuleHandleW(nullptr);
        type.lpszClassName = L"DarkRecompStartupPumpContract";
        check(RegisterClassW(&type) != 0, "cannot register hidden fixture class");
        normalRouting();
        cancellation(WM_CLOSE, 0, 0, false, true);
        cancellation(WM_SYSCOMMAND, SC_CLOSE);
        cancellation(WM_NCLBUTTONDOWN, HTCLOSE);
        cancellation(WM_CLOSE, 0, 0, true);
        cancellation(WM_SYSKEYDOWN, VK_F4, LPARAM(1) << 29);
        cancellation(WM_SYSKEYDOWN, VK_F4, LPARAM(1) << 29, true);
        quitPriority();
        virtualQuitEventually();
        inputBurst();
        inputOverload();
        closeAtWindowCap();
        callbackTimerRetention();
        workerInitializationCancel();
        check(UnregisterClassW(type.lpszClassName, type.hInstance) != 0, "hidden fixture windows still live");
        std::puts("StartupMessagePumpContract PASS: 14 cases; hidden owned windows/CPU workers only; no GPU/guest/game/visible UI/focus calls.");
        return 0;
    } catch (const NativeStartupCanceled& cancel) {
        std::fprintf(stderr, "StartupMessagePumpContract FAIL: unexpected cancellation code=%d reason=%s\n",
                     cancel.exitCode, cancel.reason);
        return 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "StartupMessagePumpContract FAIL: %s\n", error.what()); return 1;
    }
}
