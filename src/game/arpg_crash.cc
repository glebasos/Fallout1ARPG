// ARPG crash and hang reporter: logs a symbolized stack trace to
// arpg_crash.log on a crash, and to arpg_hang.log (+ arpg_hang.dmp) when the
// main thread freezes, so runs explain their own failures.

#include "game/arpg.h"

#ifdef _WIN32
#include <windows.h>

#include <dbghelp.h>
#include <stdio.h>

#include "plib/gnw/input.h"

#pragma comment(lib, "dbghelp.lib")

namespace fallout {

static bool sym_ready = false;

// Writes a symbolized stack of `thread`, whose registers are in `context`.
static void arpg_write_stack(FILE* stream, HANDLE thread, CONTEXT context)
{
    HANDLE process = GetCurrentProcess();
    if (!sym_ready) {
        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize(process, NULL, TRUE);
        sym_ready = true;
    }

    STACKFRAME64 frame = {};
#ifdef _M_X64
    DWORD machine = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset = context.Rip;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrStack.Offset = context.Rsp;
#else
    DWORD machine = IMAGE_FILE_MACHINE_I386;
    frame.AddrPC.Offset = context.Eip;
    frame.AddrFrame.Offset = context.Ebp;
    frame.AddrStack.Offset = context.Esp;
#endif
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Mode = AddrModeFlat;

    for (int depth = 0; depth < 40; depth++) {
        if (!StackWalk64(machine, process, thread, &frame, &context, NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL)) {
            break;
        }

        DWORD64 address = frame.AddrPC.Offset;
        if (address == 0) {
            break;
        }

        char symbolBuffer[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO* symbol = (SYMBOL_INFO*)symbolBuffer;
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;

        DWORD64 displacement = 0;
        const char* name = "?";
        if (SymFromAddr(process, address, &displacement, symbol)) {
            name = symbol->Name;
        }

        IMAGEHLP_LINE64 line = {};
        line.SizeOfStruct = sizeof(line);
        DWORD lineDisplacement = 0;
        if (SymGetLineFromAddr64(process, address, &lineDisplacement, &line)) {
            fprintf(stream, "#%02d %s  %s:%lu\n", depth, name, line.FileName, line.LineNumber);
        } else {
            fprintf(stream, "#%02d %s  0x%llx\n", depth, name, (unsigned long long)address);
        }
    }
}

static LONG WINAPI arpg_crash_filter(EXCEPTION_POINTERS* info)
{
    FILE* stream = fopen("arpg_crash.log", "wt");
    if (stream == NULL) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    fprintf(stream, "exception 0x%08lX at %p\n",
        info->ExceptionRecord->ExceptionCode,
        info->ExceptionRecord->ExceptionAddress);
    arpg_write_stack(stream, GetCurrentThread(), *info->ContextRecord);

    fclose(stream);
    return EXCEPTION_CONTINUE_SEARCH;
}

// Hang watchdog: when the main thread stops pumping window messages for 5 s
// (a freeze; Windows keeps no dump of those), log its stack to arpg_hang.log
// and write arpg_hang.dmp, then sample the stack again every 5 s (4 samples
// at most) to show where it spins.
static HANDLE main_thread = NULL;

static DWORD WINAPI arpg_watchdog(LPVOID)
{
    // Load symbols now: during a freeze the main thread is suspended and may
    // hold the heap lock.
    if (!sym_ready) {
        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
        SymInitialize(GetCurrentProcess(), NULL, TRUE);
        sym_ready = true;
    }

    unsigned int last = gnw95_pump_count;
    int stuck = 0;
    int reports = 0;
    for (;;) {
        Sleep(1000);
        unsigned int now = gnw95_pump_count;
        if (now != last) {
            last = now;
            stuck = 0;
            reports = 0;
            continue;
        }

        stuck++;
        if (stuck % 5 != 0 || reports >= 4) {
            continue;
        }

        FILE* stream = fopen("arpg_hang.log", reports == 0 ? "wt" : "at");
        if (stream == NULL) {
            continue;
        }

        if (SuspendThread(main_thread) != (DWORD)-1) {
            CONTEXT context = {};
            context.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(main_thread, &context)) {
                fprintf(stream, "main thread not pumping messages for %d s (sample %d):\n", stuck, reports + 1);
                arpg_write_stack(stream, main_thread, context);
            }

            if (reports == 0) {
                HANDLE dump = CreateFileA("arpg_hang.dmp", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                if (dump != INVALID_HANDLE_VALUE) {
                    if (MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dump, MiniDumpWithIndirectlyReferencedMemory, NULL, NULL, NULL)) {
                        fprintf(stream, "minidump: arpg_hang.dmp\n");
                    }
                    CloseHandle(dump);
                }
            }
            ResumeThread(main_thread);
        }

        fclose(stream);
        reports++;
    }
    return 0;
}

void arpg_install_crash_handler()
{
    SetUnhandledExceptionFilter(arpg_crash_filter);

    if (main_thread == NULL) {
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &main_thread, 0, FALSE, DUPLICATE_SAME_ACCESS);
        HANDLE watchdog = CreateThread(NULL, 0, arpg_watchdog, NULL, 0, NULL);
        if (watchdog != NULL) {
            CloseHandle(watchdog);
        }
    }
}

} // namespace fallout

#else

namespace fallout {

void arpg_install_crash_handler()
{
}

} // namespace fallout

#endif
