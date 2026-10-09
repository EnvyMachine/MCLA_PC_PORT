#pragma once

#include <cstdint>

struct PPCContext;

void LogInit(const char* path);
void LogWrite(const char* fmt, ...);
void LogFlush();

// Set MCLA_TRACE=1 to log every implemented import call with its first arguments.
extern bool g_traceImports;

// Remembers the most recent import per thread so crash reports can show it.
void NoteImportCall(const char* name);
const char* LastImportCall();

void TraceImport(const char* name, PPCContext& ctx);

#define LOGI(...) LogWrite(__VA_ARGS__)
#define TRACE(name) TraceImport(name, ctx)
