// Night lighting. A zone's lighting for each hour has a sun colour and a moon colour. The PS2 lights a zone with the
// moon only where its zone setup allows it (zone ids under 100 or over 539, and thirteen others); elsewhere, Port
// Bastok among them, nights have the ambient light alone. The PC dropped that zone test, so every zone gets the moon
// at night: a second, downward light that brightens the ground and anything facing up, and casts the moon's shadows,
// about twice the PS2's brightness on the ground, and a moonlit side and shadow on every character. The fix hooks the
// environment's hourly update and sets the moon's colours (the scenery's and the characters') to 0 in the zones the
// PS2 doesn't light with it. TECHNICAL.md, "Night lighting".
#include "fixes.h"
#include "debuglog.h"

#include <cstdint>
#include <cmath>
#include <cstring>

namespace
{
    // The environment's update from its hour's record (FUN_05418190 in the 2026 client), __thiscall with the
    // environment in ECX, RET 4. It starts with mov eax,[<the zone environment>], then push ebp / push esi /
    // mov esi,[esp+0Ch] / push edi / mov edi,ecx / test eax,eax / mov ebp,3F800000h.
    const char Entry[]     = "\xA1????\x55\x56\x8B\x74\x24\x0C\x57\x8B\xF9\x85\xC0\xBD\x00\x00\x80\x3F";
    constexpr size_t Moved = 5; // mov eax,[abs]: moved to the trampoline whole

    using UpdateFn = void(__fastcall*)(void* env, void* edx, const void* record);

    uint8_t* s_Entry      = nullptr;
    uint8_t s_Orig[Moved] = {};
    uint8_t* s_Tramp      = nullptr;
    uint8_t** s_Zone      = nullptr; // the zone environment's pointer (its zone id at +1B4h)

    // The PS2's zone setup (0x3341c0): the moon lights zones under 100 or over 539 and these.
    bool Moonlit(const int zone)
    {
        static const int listed[] = {129, 136, 137, 138, 155, 156, 164, 171, 175, 182, 189, 199, 219};
        if (zone < 100 || zone > 539)
            return true;
        for (const int z : listed)
            if (z == zone)
                return true;
        return false;
    }

    void __fastcall UpdateHook(void* env, void* edx, const void* record)
    {
        reinterpret_cast<UpdateFn>(s_Tramp)(env, edx, record);
        const uint8_t* main = s_Zone != nullptr ? *s_Zone : nullptr;
        if (main == nullptr || env == nullptr)
            return;
        int zone = 0;
        std::memcpy(&zone, main + 0x1B4, 4);
        if (DebugLog::On() && env == main)
        {
            const auto* e = static_cast<const uint8_t*>(env);
            auto v = [e](size_t o) { return reinterpret_cast<const float*>(e + o); };
            const uint8_t* rec = *reinterpret_cast<uint8_t* const*>(e + 0x18);
            const float intensity = rec != nullptr ? *reinterpret_cast<const float*>(rec + 0x44) : -1.0f;
            static float last = -1.0f;
            static int lines  = 0;
            if (lines < 40 && std::fabs(v(0x104)[0] + v(0xE4)[0] - last) > 0.01f)
            {
                ++lines, last = v(0x104)[0] + v(0xE4)[0];
                PF_DEBUG("moon: zone %d env sun %.3f %.3f %.3f moon %.3f %.3f %.3f ambient %.3f %.3f %.3f chosen %.3f %.3f %.3f second %.3f %.3f %.3f intensity %.3f", zone,
                           v(0xE4)[0], v(0xE4)[1], v(0xE4)[2], v(0xF4)[0], v(0xF4)[1], v(0xF4)[2], v(0x104)[0], v(0x104)[1], v(0x104)[2], v(0x16C)[0], v(0x16C)[1],
                           v(0x16C)[2], v(0x19C)[0], v(0x19C)[1], v(0x19C)[2], intensity);
            }
        }
        if (Moonlit(zone))
            return;
        // The moon's colour for the scenery (+F4h) and for characters (+74h), r g b then 1.0: the update sets both from
        // the hour's record, or both to 0 when the record has no moon.
        float* moon  = reinterpret_cast<float*>(static_cast<uint8_t*>(env) + 0xF4);
        float* actor = reinterpret_cast<float*>(static_cast<uint8_t*>(env) + 0x74);
        if (moon[0] != 0.0f || moon[1] != 0.0f || moon[2] != 0.0f)
            DebugLog::Once("moon", "moon: zone %d isn't lit by the moon on the PS2, moon colour %.3f %.3f %.3f (characters %.3f %.3f %.3f) set to 0", zone, moon[0],
                           moon[1], moon[2], actor[0], actor[1], actor[2]);
        moon[0] = moon[1] = moon[2] = 0.0f;
        actor[0] = actor[1] = actor[2] = 0.0f;
    }

    bool Poke(uint8_t* at, const void* data, size_t n)
    {
        DWORD old = 0;
        if (!::VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old))
            return false;
        std::memcpy(at, data, n);
        ::VirtualProtect(at, n, old, &old);
        ::FlushInstructionCache(::GetCurrentProcess(), at, n);
        return true;
    }

    uint8_t* Find(void)
    {
        const auto base = reinterpret_cast<const uint8_t*>(::GetModuleHandleA("FFXiMain.dll"));
        if (base == nullptr)
            return nullptr;
        const auto nt      = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
        const auto section = IMAGE_FIRST_SECTION(nt);
        const size_t n     = sizeof(Entry) - 1;
        for (WORD s = 0; s < nt->FileHeader.NumberOfSections; ++s)
        {
            if ((section[s].Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0)
                continue;
            const uint8_t* p   = base + section[s].VirtualAddress;
            const uint8_t* end = p + section[s].Misc.VirtualSize - n;
            for (; p < end; ++p)
            {
                size_t k = 0;
                while (k < n && (Entry[k] == '?' || p[k] == static_cast<uint8_t>(Entry[k])))
                    ++k;
                if (k == n)
                    return const_cast<uint8_t*>(p);
            }
        }
        return nullptr;
    }
}

const char* Fixes::SetNight(const bool on)
{
    if (on == m_Night)
        return nullptr;
    if (!on)
    {
        if (s_Entry != nullptr)
            Poke(s_Entry, s_Orig, Moved);
        s_Entry = nullptr;
        m_Night = false;
        PF_DEBUG("night: off");
        return nullptr;
    }
    uint8_t* at = Find();
    if (at == nullptr)
        return "the environment's hourly update wasn't found (or another plugin hooked it)";
    if (s_Tramp == nullptr)
    {
        s_Tramp = static_cast<uint8_t*>(::VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (s_Tramp == nullptr)
            return "couldn't allocate the hook";
    }
    std::memcpy(&s_Zone, at + 1, 4);
    std::memcpy(s_Tramp, at, Moved); // its address is absolute, so it runs the same from here
    s_Tramp[Moved] = 0xE9;
    const int32_t back = static_cast<int32_t>(reinterpret_cast<intptr_t>(at + Moved) - reinterpret_cast<intptr_t>(s_Tramp + Moved + 5));
    std::memcpy(s_Tramp + Moved + 1, &back, 4);
    ::FlushInstructionCache(::GetCurrentProcess(), s_Tramp, 16);
    uint8_t jmp[Moved] = {0xE9, 0, 0, 0, 0};
    const int32_t to = static_cast<int32_t>(reinterpret_cast<intptr_t>(&UpdateHook) - reinterpret_cast<intptr_t>(at + 5));
    std::memcpy(jmp + 1, &to, 4);
    std::memcpy(s_Orig, at, Moved);
    if (!Poke(at, jmp, Moved))
        return "couldn't patch the environment's hourly update";
    s_Entry = at;
    m_Night = true;
    uint8_t* main = *s_Zone; // now, not only from the next hourly update
    int zone      = 0;
    if (main != nullptr)
    {
        std::memcpy(&zone, main + 0x1B4, 4);
        if (!Moonlit(zone))
            std::memset(main + 0xF4, 0, 12), std::memset(main + 0x74, 0, 12);
    }
    PF_DEBUG("night: on, hooked at %p", static_cast<void*>(at));
    return nullptr;
}
