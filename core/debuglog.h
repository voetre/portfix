// portfix debug log (/portfix debug on): off by default and not saved. One line per rare event (a fix that can't
// start, a weapon's draw data rebuilt, a draw the fixes take over for the first time), never per frame, and at most
// MaxLines per session, so a log from a user who has a problem says what portfix was doing without growing.
// Ashita sends the lines to its own session log; without a sink (Windower) they go to portfix.log next to the dll.
#pragma once

#include <cstdint>

namespace DebugLog
{
    constexpr uint32_t MaxLines = 2000;

    using Sink = void (*)(const char* line);
    void SetSink(Sink sink);
    bool On(void);
    void Set(bool on);
    void Log(const char* fmt, ...);
    // Log once per session for this key (first draw a fix takes over, and so on).
    void Once(const char* key, const char* fmt, ...);
}

#define PF_DEBUG(...)                       \
    do                                      \
    {                                       \
        if (DebugLog::On())                 \
            DebugLog::Log(__VA_ARGS__);     \
    } while (0)
