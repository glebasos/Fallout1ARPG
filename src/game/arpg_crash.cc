// ARPG autotest crash reporter: logs a symbolized stack trace to arpg.log so
// automated runs explain their own crashes.

#include "game/arpg.h"

#ifdef _WIN32
#include <windows.h>

#include <dbghelp.h>
#include <stdio.h>

#pragma comment(lib, "dbghelp.lib")

namespace fallout {

static LONG WINAPI arpg_crash_filter(EXCEPTION_POINTERS* info)
{
    FILE* stream = fopen("arpg_crash.log", "wt");
    if (stream == NULL) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    fprintf(stream, "exception 0x%08lX at %p\n",
        info->ExceptionRecord->ExceptionCode,
        info->ExceptionRecord->ExceptionAddress);

    HANDLE process = GetCurrentProcess();
    HANDLE thread = GetCurrentThread();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(process, NULL, TRUE);

    CONTEXT context = *info->ContextRecord;
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

    fclose(stream);
    return EXCEPTION_CONTINUE_SEARCH;
}

void arpg_install_crash_handler()
{
    SetUnhandledExceptionFilter(arpg_crash_filter);
}

} // namespace fallout

#else

namespace fallout {

void arpg_install_crash_handler()
{
}

} // namespace fallout

#endif
