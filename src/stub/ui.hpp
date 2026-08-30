// User-facing output: the lazy progress window, error dialogs, and console text.
//
// Nothing here may include <print>, <format> or <iostream>. On this toolchain a
// single std::println call grows the binary from roughly 101 KB to roughly
// 391 KB, and that cost would be paid by every packaged application. Formatting
// goes through wsprintfW instead, which lives in user32 and costs nothing.
#pragma once

#include <atomic>
#include <string>
#include <thread>

#include "error.hpp"
#include "format.hpp"
#include "meta.hpp"

namespace hnx::ui {

// Attaches to the parent console if there is one, so a stub launched from a
// terminal can print to it. The stub is a GUI-subsystem binary, which is what
// avoids a console window flashing up on a normal double-click launch; the cost
// is that stdout is not connected by default.
void attachParentConsole();

// True once attachParentConsole found a console to write to.
[[nodiscard]] bool hasConsole() noexcept;

void writeConsole(std::wstring_view text);
void writeConsoleLine(std::wstring_view text);

// Shows an error. Goes to the console when one is attached, otherwise to a
// message box. Never silent: a stub that fails invisibly looks like a stub that
// did nothing.
void reportError(const Error& e, const Metadata* meta);

// A progress window that only ever appears if extraction takes long enough to be
// worth interrupting the user for.
//
// The window is created on its own thread after a delay. On the warm path -- the
// case this whole system optimises for -- extraction never runs, this class is
// never started, and no window is created at all.
class Progress {
public:
    Progress() = default;
    ~Progress();

    Progress(const Progress&) = delete;
    Progress& operator=(const Progress&) = delete;

    // Starts the delay timer. The window appears only if `delayMs` elapses before
    // finish() is called.
    void start(const std::wstring& appName, unsigned delayMs = 300);

    // Thread-safe; called from extraction workers.
    void update(u64 bytesDone, u64 bytesTotal);

    // Closes the window if it was shown, and stops the thread either way.
    void finish();

    // Set when the user closes the progress window.
    [[nodiscard]] const std::atomic<bool>& cancelled() const noexcept { return cancelled_; }

private:
    std::jthread thread_;
    std::atomic<bool> done_{false};
    std::atomic<bool> cancelled_{false};
    std::atomic<u64> done_bytes_{0};
    std::atomic<u64> total_bytes_{0};
    void* readyEvent_ = nullptr;  // HANDLE, signalled to end the delay early
    std::wstring appName_;
};

}  // namespace hnx::ui
