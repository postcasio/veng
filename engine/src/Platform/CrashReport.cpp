#include <Veng/Platform/CrashReport.h>

#if defined(_WIN32)

#include <cstdio>

#include <windows.h>

#include <dbghelp.h>

namespace Veng
{
    namespace
    {
        // Raw fprintf on purpose: the process is mid-crash, so the report leans on nothing that
        // allocates or locks in engine code — stderr and dbghelp only.
        LONG WINAPI ReportCrash(EXCEPTION_POINTERS* info)
        {
            const EXCEPTION_RECORD& record = *info->ExceptionRecord;
            std::fprintf(stderr, "fatal exception 0x%08lX at %p\n",
                         static_cast<unsigned long>(record.ExceptionCode),
                         record.ExceptionAddress);

            const HANDLE process = GetCurrentProcess();
            SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
            SymInitialize(process, nullptr, TRUE);

            // Walk from the exception's own context, not the handler's, so the first frame is the
            // faulting instruction. The walk mutates the context, so it works on a copy.
            CONTEXT context = *info->ContextRecord;
            STACKFRAME64 frame = {};
            frame.AddrPC.Offset = context.Rip;
            frame.AddrPC.Mode = AddrModeFlat;
            frame.AddrFrame.Offset = context.Rbp;
            frame.AddrFrame.Mode = AddrModeFlat;
            frame.AddrStack.Offset = context.Rsp;
            frame.AddrStack.Mode = AddrModeFlat;

            for (int depth = 0; depth < 64; ++depth)
            {
                if (StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame,
                                &context, nullptr, SymFunctionTableAccess64, SymGetModuleBase64,
                                nullptr) == FALSE ||
                    frame.AddrPC.Offset == 0)
                {
                    break;
                }

                const DWORD64 address = frame.AddrPC.Offset;

                char moduleName[MAX_PATH] = "?";
                const DWORD64 moduleBase = SymGetModuleBase64(process, address);
                if (moduleBase != 0)
                {
                    GetModuleFileNameA(reinterpret_cast<HMODULE>(moduleBase), moduleName,
                                       MAX_PATH);
                }

                char symbolBuffer[sizeof(SYMBOL_INFO) + 256] = {};
                auto* const symbol = reinterpret_cast<SYMBOL_INFO*>(symbolBuffer);
                symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
                symbol->MaxNameLen = 255;
                DWORD64 symbolOffset = 0;
                const bool named = SymFromAddr(process, address, &symbolOffset, symbol) != FALSE;

                IMAGEHLP_LINE64 line = {};
                line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
                DWORD lineOffset = 0;
                const bool located = SymGetLineFromAddr64(process, address, &lineOffset, &line) != FALSE;

                if (named && located)
                {
                    std::fprintf(stderr, "  %2d %s + 0x%llx (%s:%lu)\n", depth, symbol->Name,
                                 static_cast<unsigned long long>(symbolOffset), line.FileName,
                                 static_cast<unsigned long>(line.LineNumber));
                }
                else if (named)
                {
                    std::fprintf(stderr, "  %2d %s + 0x%llx\n", depth, symbol->Name,
                                 static_cast<unsigned long long>(symbolOffset));
                }
                else
                {
                    std::fprintf(stderr, "  %2d %s + 0x%llx\n", depth, moduleName,
                                 static_cast<unsigned long long>(address - moduleBase));
                }
            }

            std::fflush(stderr);

            // Hand the exception on so Windows Error Reporting still records it (and a debugger,
            // when one is attached, still catches it).
            return EXCEPTION_CONTINUE_SEARCH;
        }
    }

    void InstallCrashReporter()
    {
        SetUnhandledExceptionFilter(&ReportCrash);
    }
}

#else

namespace Veng
{
    void InstallCrashReporter()
    {
    }
}

#endif
