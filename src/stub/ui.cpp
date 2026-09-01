#include "ui.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>

#include "paths.hpp"

namespace hnx::ui {
namespace {

bool g_hasConsole = false;
HANDLE g_conOut = INVALID_HANDLE_VALUE;

constexpr wchar_t kWindowClass[] = L"HNxProgressWindow";
constexpr int kWidth = 420;
constexpr int kHeight = 120;

// The Common Controls v6 manifest embedded by stub.rc. Kept out of the process
// manifest so that an image lacking the assembly -- WinPE -- costs us the
// theming rather than the process.
constexpr WORD kComCtl6ManifestId = 2;

struct WindowState {
    Progress* owner = nullptr;
    HWND bar = nullptr;
    HWND label = nullptr;
};

// Activates the Common Controls v6 assembly for the calling thread, for as long
// as this object lives. Every failure is silent and leaves the thread with
// whatever comctl32 the system provides.
class ComCtl6Context {
public:
    ComCtl6Context() {
        ACTCTXW ctx{};
        ctx.cbSize = sizeof(ctx);
        ctx.dwFlags = ACTCTX_FLAG_RESOURCE_NAME_VALID | ACTCTX_FLAG_HMODULE_VALID;
        ctx.hModule = ::GetModuleHandleW(nullptr);
        ctx.lpResourceName = MAKEINTRESOURCEW(kComCtl6ManifestId);

        handle_ = ::CreateActCtxW(&ctx);
        if (handle_ != INVALID_HANDLE_VALUE)
            activated_ = ::ActivateActCtx(handle_, &cookie_) != FALSE;
    }

    ~ComCtl6Context() {
        if (activated_) ::DeactivateActCtx(0, cookie_);
        if (handle_ != INVALID_HANDLE_VALUE) ::ReleaseActCtx(handle_);
    }

    ComCtl6Context(const ComCtl6Context&) = delete;
    ComCtl6Context& operator=(const ComCtl6Context&) = delete;

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    ULONG_PTR cookie_ = 0;
    bool activated_ = false;
};

// Owns a LoadLibraryW reference for a scope.
class ModuleGuard {
public:
    explicit ModuleGuard(const wchar_t* name) : module_(::LoadLibraryW(name)) {}
    ~ModuleGuard() {
        if (module_) ::FreeLibrary(module_);
    }

    ModuleGuard(const ModuleGuard&) = delete;
    ModuleGuard& operator=(const ModuleGuard&) = delete;

    [[nodiscard]] HMODULE get() const noexcept { return module_; }

private:
    HMODULE module_ = nullptr;
};

LRESULT CALLBACK progressProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<WindowState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_CLOSE:
            // Treated as cancellation rather than ignored: a user who closes the
            // window expects the extraction to stop, not to keep running headless.
            if (st && st->owner) {
                // const_cast is confined here; the accessor exposes the flag as
                // const to extraction, which must only read it.
                const_cast<std::atomic<bool>&>(st->owner->cancelled()).store(true);
            }
            return 0;

        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

void attachParentConsole() {
    // Only attach; never allocate. A GUI stub that called AllocConsole would pop
    // a console window on every double-click launch.
    if (::AttachConsole(ATTACH_PARENT_PROCESS)) {
        g_conOut = ::GetStdHandle(STD_OUTPUT_HANDLE);
        g_hasConsole = g_conOut != INVALID_HANDLE_VALUE && g_conOut != nullptr;
    }
}

bool hasConsole() noexcept { return g_hasConsole; }

void writeConsole(std::wstring_view text) {
    if (!g_hasConsole || text.empty()) return;
    DWORD written = 0;
    if (!::WriteConsoleW(g_conOut, text.data(), static_cast<DWORD>(text.size()),
                         &written, nullptr)) {
        // Redirected to a file or a pipe: WriteConsoleW fails there, so fall back
        // to a byte write of the UTF-8 form.
        const std::string utf8 = paths::narrow(text);
        ::WriteFile(g_conOut, utf8.data(), static_cast<DWORD>(utf8.size()), &written,
                    nullptr);
    }
}

void writeConsoleLine(std::wstring_view text) {
    writeConsole(text);
    writeConsole(L"\r\n");
}

void reportError(const Error& e, const Metadata* meta) {
    const std::string msg = e.message();
    std::wstring wmsg = paths::widen(msg);

    std::wstring title = L"HNx";
    if (meta && !meta->appName.empty()) {
        title = meta->appName;
    } else if (meta && !meta->appId.empty()) {
        title = meta->appId;
    }

    if (g_hasConsole) {
        writeConsoleLine(title + L": " + wmsg);
        return;
    }

    if (meta && meta->ui == UiMode::Silent) {
        // Even in silent mode a failure is shown: the alternative is an
        // application that appears to do nothing at all when double-clicked.
        // Silent governs progress, not errors.
    }

    ::MessageBoxW(nullptr, wmsg.c_str(), title.c_str(),
                  MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

// ---------------------------------------------------------------------------
// Progress
// ---------------------------------------------------------------------------

Progress::~Progress() { finish(); }

void Progress::start(const std::wstring& appName, unsigned delayMs) {
    appName_ = appName;

    readyEvent_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!readyEvent_) return;

    thread_ = std::jthread([this, delayMs] {
        // Wait out the delay first. If extraction finishes inside it -- which is
        // the common case for a small update -- no window is ever created and the
        // user sees nothing flash.
        const DWORD w = ::WaitForSingleObject(readyEvent_, delayMs);
        if (w == WAIT_OBJECT_0 || done_.load(std::memory_order_relaxed)) return;

        // comctl32 is loaded here rather than imported, for the same reason its
        // manifest dependency is activated rather than declared: a missing
        // comctl32 must cost this window its progress bar, not cost the whole
        // stub its ability to load. The activation context comes first so that
        // the load resolves to the v6 assembly where one exists.
        const ComCtl6Context comctl6;

        ModuleGuard comctl(L"comctl32.dll");
        bool haveProgressBar = false;
        if (comctl.get()) {
            using InitCommonControlsExFn = BOOL(WINAPI*)(const INITCOMMONCONTROLSEX*);
            const auto init = reinterpret_cast<InitCommonControlsExFn>(
                reinterpret_cast<void*>(
                    ::GetProcAddress(comctl.get(), "InitCommonControlsEx")));
            if (init) {
                INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_PROGRESS_CLASS};
                haveProgressBar = init(&icc) != FALSE;
            }
        }

        HINSTANCE inst = ::GetModuleHandleW(nullptr);

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = progressProc;
        wc.hInstance = inst;
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = kWindowClass;
        wc.hIcon = ::LoadIconW(inst, MAKEINTRESOURCEW(1));
        ::RegisterClassExW(&wc);

        const int x = (::GetSystemMetrics(SM_CXSCREEN) - kWidth) / 2;
        const int y = (::GetSystemMetrics(SM_CYSCREEN) - kHeight) / 2;

        std::wstring title = appName_.empty() ? L"Starting" : appName_;

        HWND hwnd = ::CreateWindowExW(
            WS_EX_APPWINDOW, kWindowClass, title.c_str(),
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, x, y, kWidth, kHeight, nullptr,
            nullptr, inst, nullptr);
        if (!hwnd) return;

        WindowState st;
        st.owner = this;

        st.label = ::CreateWindowExW(0, L"STATIC", L"Preparing files...",
                                     WS_CHILD | WS_VISIBLE, 16, 12, kWidth - 48, 20,
                                     hwnd, nullptr, inst, nullptr);
        // Without comctl32 there is no bar, and the label alone carries the
        // percentage. That is a worse window, not a broken one.
        if (haveProgressBar) {
            st.bar = ::CreateWindowExW(0, PROGRESS_CLASSW, nullptr,
                                       WS_CHILD | WS_VISIBLE, 16, 40, kWidth - 48, 22,
                                       hwnd, nullptr, inst, nullptr);
            ::SendMessageW(st.bar, PBM_SETRANGE32, 0, 1000);
        }

        // The default GUI font; without this the label renders in the ancient
        // bitmap system font.
        HFONT font = reinterpret_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
        ::SendMessageW(st.label, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&st));
        ::ShowWindow(hwnd, SW_SHOWNORMAL);
        ::UpdateWindow(hwnd);

        // Poll rather than have workers post messages: the workers are hot, and a
        // cross-thread PostMessage per file would cost more than a 60 Hz refresh.
        while (!done_.load(std::memory_order_relaxed)) {
            MSG msg;
            while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) break;
                ::TranslateMessage(&msg);
                ::DispatchMessageW(&msg);
            }

            const u64 total = total_bytes_.load(std::memory_order_relaxed);
            const u64 done = done_bytes_.load(std::memory_order_relaxed);
            if (st.bar) {
                const int pos = total > 0
                                    ? static_cast<int>((done * 1000) / total)
                                    : 0;
                ::SendMessageW(st.bar, PBM_SETPOS, static_cast<WPARAM>(pos), 0);
            }

            wchar_t buf[128];
            const unsigned pct = total > 0 ? static_cast<unsigned>((done * 100) / total) : 0;
            ::wsprintfW(buf, L"Preparing files... %u%%  (%u of %u MB)", pct,
                        static_cast<unsigned>(done / (1024 * 1024)),
                        static_cast<unsigned>(total / (1024 * 1024)));
            ::SetWindowTextW(st.label, buf);

            ::WaitForSingleObject(readyEvent_, 16);
        }

        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        ::DestroyWindow(hwnd);
        ::UnregisterClassW(kWindowClass, inst);
    });
}

void Progress::update(u64 bytesDone, u64 bytesTotal) {
    done_bytes_.store(bytesDone, std::memory_order_relaxed);
    total_bytes_.store(bytesTotal, std::memory_order_relaxed);
}

void Progress::finish() {
    done_.store(true, std::memory_order_relaxed);
    if (readyEvent_) ::SetEvent(readyEvent_);

    if (thread_.joinable()) thread_.join();

    if (readyEvent_) {
        ::CloseHandle(readyEvent_);
        readyEvent_ = nullptr;
    }
}

}  // namespace hnx::ui
