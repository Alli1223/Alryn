// Entry point for the Alryn sample game. Parses the command line and launches one
// of three modes: the windowed client (default), a dedicated headless server
// (--server), or a headless wandering bot (--bot). The modes themselves live in
// ClientApp / ServerApp / Bot; this file is just the dispatcher.

#include "Bot.h"
#include "ClientApp.h"
#include "ServerApp.h"

#include <cstdlib>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
// (dbghelp.h needs windows.h first)
#include <dbghelp.h>

#include <cstdio>
#pragma comment(lib, "dbghelp.lib")

namespace {
// Last-chance crash reporter: an unhandled exception (an access violation, an uncaught C++ throw) logs
// its code + a symbolized call stack to stderr and crash.log beside the executable, so a crash a
// player hits outside a debugger still says exactly where it happened.
LONG WINAPI report_crash(EXCEPTION_POINTERS* info) {
    static bool reported = false;
    if (reported) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    reported = true;
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string log_path = exe;
    log_path = log_path.substr(0, log_path.find_last_of("\\/") + 1) + "crash.log";
    FILE* log = nullptr;
    fopen_s(&log, log_path.c_str(), "w");
    auto out = [&](const char* line) {
        std::fputs(line, stderr);
        if (log != nullptr) {
            std::fputs(line, log);
        }
    };
    char buf[1024];
    std::snprintf(buf, sizeof(buf), "\n*** Alryn crashed: exception 0x%08lX at %p ***\n",
                  info->ExceptionRecord->ExceptionCode, info->ExceptionRecord->ExceptionAddress);
    out(buf);

    HANDLE proc = GetCurrentProcess();
    HANDLE thread = GetCurrentThread();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(proc, nullptr, TRUE);
    CONTEXT ctx = *info->ContextRecord;
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = ctx.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    alignas(SYMBOL_INFO) char sym_buf[sizeof(SYMBOL_INFO) + 256] = {};
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(sym_buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    for (int depth = 0; depth < 48; ++depth) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, thread, &frame, &ctx, nullptr, SymFunctionTableAccess64,
                         SymGetModuleBase64, nullptr) ||
            frame.AddrPC.Offset == 0) {
            break;
        }
        const DWORD64 pc = frame.AddrPC.Offset;
        DWORD64 disp = 0;
        const char* name = SymFromAddr(proc, pc, &disp, sym) ? sym->Name : "?";
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD ldisp = 0;
        if (SymGetLineFromAddr64(proc, pc, &ldisp, &line)) {
            std::snprintf(buf, sizeof(buf), "  #%02d %s  (%s:%lu)\n", depth, name, line.FileName, line.LineNumber);
        } else {
            std::snprintf(buf, sizeof(buf), "  #%02d %s  [0x%llx]\n", depth, name, static_cast<unsigned long long>(pc));
        }
        out(buf);
    }
    std::snprintf(buf, sizeof(buf), "(also written to %s)\n", log_path.c_str());
    out(buf);
    if (log != nullptr) {
        std::fclose(log);
    }
    return EXCEPTION_CONTINUE_SEARCH; // let Windows / an attached debugger carry on as usual
}
} // namespace
#endif

using namespace alryn;       // u64, f32
using namespace alryn::game; // ServerApp, ClientApp, run_bot

int main(int argc, char** argv) {
#ifdef _WIN32
    SetUnhandledExceptionFilter(report_crash);
#endif
    std::string mode = "client";
    std::string host = "127.0.0.1";
    bool joining = false;
    u64 frames = 0;
    f32 bot_seconds = 60.0f;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--server") {
            mode = "server";
        } else if (arg == "--bot") {
            mode = "bot";
        } else if (arg.rfind("--host=", 0) == 0) {
            host = arg.substr(7);
            joining = true;
        } else {
            frames = std::strtoull(arg.c_str(), nullptr, 10);
            bot_seconds = static_cast<f32>(frames > 0 ? frames : 60);
        }
    }

    if (mode == "server") {
        ServerApp app;
        app.run();
    } else if (mode == "bot") {
        run_bot(host, bot_seconds);
    } else {
        const bool auto_start = joining || frames > 0;
        ClientApp app{host, !joining, frames, auto_start};
        app.run();
    }
    return 0;
}
