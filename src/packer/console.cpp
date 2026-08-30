// Console window management.
//
// hnxpack is a console-subsystem program, which is the right choice for a build
// tool: cmd.exe waits for a console application and returns the prompt
// immediately for a GUI one, so a GUI subsystem would silently break
// `hnxpack build ... && next-step`.
//
// The cost is that launching it from Explorer or a shortcut -- which is exactly
// how the no-argument interactive flow gets used -- makes Windows create a
// console window that then sits behind the dialogs with nothing useful in it.
#include "packer.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace hnx::pack {
namespace {

bool g_consoleHidden = false;

}  // namespace

bool ownsConsole() {
    // The distinction that matters: did Windows create this console for us, or
    // did we inherit one from a shell?
    //
    // A console created for us has exactly one process attached -- this one. A
    // console inherited from cmd.exe or a terminal has at least two. Getting this
    // backwards would mean hiding the user's own terminal window, which is a far
    // worse outcome than leaving a stray console visible.
    DWORD pids[2]{};
    const DWORD count = ::GetConsoleProcessList(pids, 2);
    return count == 1;
}

bool hideOwnConsole() {
    if (!ownsConsole()) return false;

    HWND console = ::GetConsoleWindow();
    if (!console) return false;

    ::ShowWindow(console, SW_HIDE);
    g_consoleHidden = true;
    return true;
}

bool consoleHidden() { return g_consoleHidden; }

}  // namespace hnx::pack
