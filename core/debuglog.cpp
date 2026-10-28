#define _CRT_SECURE_NO_WARNINGS
#include "debuglog.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

namespace
{
    DebugLog::Sink s_Sink = nullptr;
    bool s_On             = false;
    uint32_t s_Lines      = 0;
    std::set<std::string> s_Once;
    CRITICAL_SECTION s_Lock;
    bool s_LockReady = false;

    // portfix.log next to this dll (the Windower addon folder)
    void FileSink(const char* line)
    {
        static char path[MAX_PATH] = "";
        if (path[0] == '\0')
        {
            HMODULE self = nullptr;
            ::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                 reinterpret_cast<LPCSTR>(&FileSink), &self);
            ::GetModuleFileNameA(self, path, MAX_PATH);
            char* slash = std::strrchr(path, '\\');
            std::snprintf(slash != nullptr ? slash + 1 : path, MAX_PATH - (slash != nullptr ? slash + 1 - path : 0), "portfix.log");
        }
        if (FILE* f = std::fopen(path, "a"))
        {
            SYSTEMTIME t;
            ::GetLocalTime(&t);
            std::fprintf(f, "%02d:%02d:%02d.%03d %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, line);
            std::fclose(f);
        }
    }

    void Write(const char* fmt, va_list args)
    {
        if (!s_LockReady)
            return;
        ::EnterCriticalSection(&s_Lock);
        if (s_Lines < DebugLog::MaxLines)
        {
            char buf[512];
            std::vsnprintf(buf, sizeof(buf), fmt, args);
            if (++s_Lines == DebugLog::MaxLines)
                std::snprintf(buf, sizeof(buf), "debug log: %u lines, stopping here for this session", DebugLog::MaxLines);
            (s_Sink != nullptr ? s_Sink : FileSink)(buf);
        }
        ::LeaveCriticalSection(&s_Lock);
    }
}

void DebugLog::SetSink(const Sink sink)
{
    s_Sink = sink;
}

bool DebugLog::On(void)
{
    return s_On;
}

void DebugLog::Set(const bool on)
{
    if (!s_LockReady)
    {
        ::InitializeCriticalSection(&s_Lock);
        s_LockReady = true;
    }
    s_On = on;
}

void DebugLog::Log(const char* fmt, ...)
{
    if (!s_On)
        return;
    va_list args;
    va_start(args, fmt);
    Write(fmt, args);
    va_end(args);
}

void DebugLog::Once(const char* key, const char* fmt, ...)
{
    if (!s_On || !s_LockReady)
        return;
    ::EnterCriticalSection(&s_Lock);
    const bool fresh = s_Once.insert(key).second;
    ::LeaveCriticalSection(&s_Lock);
    if (!fresh)
        return;
    va_list args;
    va_start(args, fmt);
    Write(fmt, args);
    va_end(args);
}
