// A check for a newer portfix release on GitHub, made once on load in the background.
#pragma once

// This build's version, as its release is tagged on GitHub (without the v).
#ifndef PORTFIX_VERSION
#define PORTFIX_VERSION "1.3.1"
#endif

namespace Update
{
    // Starts the check: one GitHub request, made once per plugin or addon load. A call while one runs does nothing.
    void Start(void);
    // The message to show, once a newer release is found; nullptr until then, after it was taken, or if the check
    // failed or found nothing newer.
    const char* Take(void);
    // True once the check is over (whatever it found).
    bool Done(void);
}
