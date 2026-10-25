// Remastered host logging implementation.

#include "logging.h"

namespace remastered {

FILE *g_logFile = nullptr;

void logOpen() {
    if (g_logFile)
        return;
    wchar_t path[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", path, MAX_PATH))
        return;
    wcscat_s(path, MAX_PATH, L"\\MarioSoulsProbe");
    CreateDirectoryW(path, nullptr);
    wcscat_s(path, MAX_PATH, L"\\remastered_host.log");
    g_logFile = _wfopen(path, L"w");
    if (g_logFile)
        setvbuf(g_logFile, nullptr, _IOFBF, 1 << 16);
}

void logWrite(const char *fmt, ...) {
    if (!g_logFile)
        return;
    SYSTEMTIME st = {};
    GetSystemTime(&st);
    fprintf(g_logFile, "%02u:%02u:%02u.%03u tid=%-6lu ", st.wHour, st.wMinute,
            st.wSecond, st.wMilliseconds, GetCurrentThreadId());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_logFile, fmt, ap);
    va_end(ap);
    fputc('\n', g_logFile);
    fflush(g_logFile);
}

} // namespace remastered