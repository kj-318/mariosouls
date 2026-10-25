// Remastered host logging - shared between TUs.

#pragma once

#include <cstdio>
#include <cstdarg>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace remastered {

// Global log file handle
extern FILE *g_logFile;

// Open log file
void logOpen();

// Write to log (thread-safe)
void logWrite(const char *fmt, ...);

} // namespace remastered