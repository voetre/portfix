// Lighting of models with an aura: summons, the Prime avatars and some story characters. Their DAT has a type 0x5E
// block (the aura: the colours of the halo's copies, and a flags byte first). The PS2 lights such a model by the
// block's flags: 1 alone (Carbuncle, Selh'teus...) switches its lights off and sets its ambient to 2.0, so it's drawn
// at full white whatever the zone and hour; with 2 as well (Titan, Ifrit, Fenrir...) it's lit by the zone like anyone
// else. The PC's actor lighting sets a fixed grey ambient, 0x707070, for either kind instead: the unlit ones at 88% of
// the PS2's brightness (Carbuncle a darker, greener teal), and the lit ones a neutral grey instead of the zone's light,
// bluish at night. The fix rewrites that branch to the PS2's rule. TECHNICAL.md, "Aura lighting".
#include "fixes.h"
#include "debuglog.h"

#include <cstdint>
#include <cstring>

namespace
{
    // In the actor's lighting (FUN_0535b4e0 in the 2026 client), with EAX the 0x5E block and its flags at +30h:
    //   test byte [eax+30h],2 / jnz set / mov eax,<lights> / l: mov dword [eax],0 / add eax,68h / cmp eax,<lights+1A0h> /
    //   jl l / set: mov dword [<ambient>],80707070h / jmp <on> / zone: (the ambient from the zone's light)
    constexpr size_t Size = 0x27;
    const char Pattern[] = "\xF6\x40\x30\x02\x75\x15\xB8????\xC7\x00\x00\x00\x00\x00\x83\xC0\x68\x3D????\x7C\xF0\xC7\x05????\x70\x70\x70\x80\xEB\x1C";

    uint8_t* s_At = nullptr;  // the branch, while patched
    uint8_t s_Orig[Size];     // its bytes before

    const uint8_t* Find(void)
    {
        const auto base = reinterpret_cast<const uint8_t*>(::GetModuleHandleA("FFXiMain.dll"));
        if (base == nullptr)
            return nullptr;
        const auto nt      = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
        const auto section = IMAGE_FIRST_SECTION(nt);
        for (WORD s = 0; s < nt->FileHeader.NumberOfSections; ++s)
        {
            if ((section[s].Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0)
                continue;
            const uint8_t* p   = base + section[s].VirtualAddress;
            const uint8_t* end = p + section[s].Misc.VirtualSize - Size;
            for (; p < end; ++p)
            {
                size_t n = 0;
                while (n < Size && (Pattern[n] == '?' || p[n] == static_cast<uint8_t>(Pattern[n])))
                    ++n;
                if (n == Size)
                    return p;
            }
        }
        return nullptr;
    }

    bool Poke(uint8_t* at, const uint8_t* data)
    {
        DWORD old = 0;
        if (!::VirtualProtect(at, Size, PAGE_EXECUTE_READWRITE, &old))
            return false;
        std::memcpy(at, data, Size);
        ::VirtualProtect(at, Size, old, &old);
        ::FlushInstructionCache(::GetCurrentProcess(), at, Size);
        return true;
    }
}

const char* Fixes::SetAura(const bool on)
{
    if (on == m_Aura)
        return nullptr;
    if (!on)
    {
        if (s_At != nullptr)
            Poke(s_At, s_Orig);
        s_At   = nullptr;
        m_Aura = false;
        PF_DEBUG("aura: off");
        return nullptr;
    }
    auto at = const_cast<uint8_t*>(Find());
    if (at == nullptr)
        return "the actor lighting code wasn't found";
    uint32_t lights = 0, end = 0, ambient = 0;
    std::memcpy(&lights, at + 0x07, 4);
    std::memcpy(&end, at + 0x15, 4);
    std::memcpy(&ambient, at + 0x1D, 4);
    if (end - lights != 0x1A0) // the loop below counts four 68h-byte lights by the low byte
        return "the actor lighting code has a different light table";
    // mov cl,[eax+30h] / and cl,3 / cmp cl,1 / jne zone            (an aura that keeps its lights, or none: the zone's)
    // xor eax,eax / l: and dword [eax+<lights>],0 / add eax,68h / cmp al,0A0h / jne l      (lights off)
    // mov dword [<ambient>],80FFFFFFh / jmp <on>                   (2.0, clamped: full white after the part's scale)
    uint8_t code[Size] = {0x8A, 0x48, 0x30, 0x80, 0xE1, 0x03, 0x80, 0xF9, 0x01, 0x75, 0x1C, 0x31, 0xC0, 0x83, 0xA0, 0, 0, 0, 0, 0x00,
                          0x83, 0xC0, 0x68, 0x3C, 0xA0, 0x75, 0xF2, 0xC7, 0x05, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0x80, 0xEB, 0x1C};
    std::memcpy(code + 0x0F, &lights, 4);
    std::memcpy(code + 0x1D, &ambient, 4);
    std::memcpy(s_Orig, at, Size);
    if (!Poke(at, code))
        return "couldn't patch the actor lighting code";
    s_At   = at;
    m_Aura = true;
    PF_DEBUG("aura: on, patched at %p (lights %08lx, ambient %08lx)", static_cast<void*>(at), static_cast<unsigned long>(lights), static_cast<unsigned long>(ambient));
    return nullptr;
}
