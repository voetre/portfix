// Effect depth bias. A particle element can carry a depth bias (op 0x30 in its generator; Carbuncle's glow discs
// have -0.7). On the PS2 the element is depth tested as if that much nearer the camera: its depth buffer value is
// raised by Z(d) - Z(d - bias) for its view depth d, so a summon's glow lies over its whole body. The PC keeps the
// value but only sorts by it, so the glow is tested at its true place, inside the model, and the near half of the
// body hides it. The fix hooks the element's draw setup and, for an element with a bias, shifts the projection's
// depth by the same amount around that element's own draws (the game sets the projection again between the setup
// and the draw, so it is applied in the draw hooks). The setup is called by two element draw methods, and the bias
// holds only from the setup to the end of that method: other element types, and the scenery drawn after the last
// effect, draw without calling the setup. TECHNICAL.md, "Effect depth".
#include "fixes.h"
#include "debuglog.h"

#include <cstdint>
#include <cstring>

namespace
{
    // The element's draw setup (FUN_052d67e0 in the 2026 client), __fastcall with the element in ECX, RET 4:
    //   sub esp,0D8h / cmp word [esp+0DCh],0FFFFh / push ebx / push ebp / mov ebp,ecx / push esi / mov [esp+..],ebp
    // A function's entry, moved to a trampoline (then a jmp back) and replaced by a jmp to the hook.
    struct Hook
    {
        const char* entry; // bytes the function starts with (the first moved are whole instructions)
        size_t length;
        size_t moved;
        void* to;
        uint8_t* at   = nullptr; // while hooked
        uint8_t* tramp = nullptr;
        uint8_t orig[8]{};
    };

    // The element's draw setup (FUN_052d67e0 in the 2026 client), __fastcall with the element in ECX, RET 4:
    //   sub esp,0D8h / cmp word [esp+0DCh],0FFFFh / push ebx / push ebp / mov ebp,ecx / push esi / mov [esp+..],ebp
    const char SetupEntry[] = "\x81\xEC\xD8\x00\x00\x00\x66\x81\xBC\x24\xDC\x00\x00\x00\xFF\xFF\x53\x55\x8B\xE9\x56\x89\x6C\x24";
    // The two element draw methods that call it (vtable 0x055bb400 in the 2026 client), __fastcall, element in ECX, RET:
    //   FUN_052d6070: sub esp,50h / lea eax,[esp+0Ch] / push ebp / mov ebp,ecx / push ecx / fld [ebp+138h] / fmul [ebp+134h] / lea ecx,[ebp+0F8h]
    //   FUN_052d0e70 (it draws a light volume, then calls the setup): sub esp,28h / push ebx / mov ebx,ecx / mov eax,[ebx+104h] / test eax,eax / jz +123h / mov eax,..
    const char DrawAEntry[] = "\x83\xEC\x50\x8D\x44\x24\x0C\x55\x8B\xE9\x51\xD9\x85\x38\x01\x00\x00\xD8\x8D\x34\x01\x00\x00\x8D\x8D\xF8\x00\x00\x00";
    const char DrawBEntry[] = "\x83\xEC\x28\x53\x8B\xD9\x8B\x83\x04\x01\x00\x00\x85\xC0\x0F\x84\x23\x01\x00\x00\xA1";

    using SetupFn = uint32_t(__fastcall*)(void* element, void* edx, uint32_t arg);
    using DrawFn  = uint32_t(__fastcall*)(void* element, void* edx);

    uint32_t __fastcall SetupHook(void* element, void* edx, uint32_t arg);
    uint32_t __fastcall DrawAHook(void* element, void* edx);
    uint32_t __fastcall DrawBHook(void* element, void* edx);

    Hook s_Setup{SetupEntry, sizeof(SetupEntry) - 1, 6, reinterpret_cast<void*>(&SetupHook)}; // sub esp,0D8h
    Hook s_DrawA{DrawAEntry, sizeof(DrawAEntry) - 1, 7, reinterpret_cast<void*>(&DrawAHook)}; // sub esp,50h / lea eax,[esp+0Ch]
    Hook s_DrawB{DrawBEntry, sizeof(DrawBEntry) - 1, 6, reinterpret_cast<void*>(&DrawBHook)}; // sub esp,28h / push ebx / mov ebx,ecx

    // The element's fields (PC layout, the PS2's in brackets): flags +10Ch (+B4h), bias +128h (+E4h), view depth +13Ch (+E8h).
    // A copy from +10Ch is kept for the draws, so these are offsets into it.
    constexpr size_t Flags = 0x10C - 0x10C, Bias = 0x128 - 0x10C, Depth = 0x13C - 0x10C;
    // Flag bit 22: no bias. Bit 27: the bias is in PS2 depth units, x -2000 (not applied, see BiasFor). Otherwise
    // yalms toward the camera.

    // How far to move the draw's depth in NDC: the PS2's change in Z, which is linear in 1/d as D3D's depth is.
    bool BiasFor(const uint8_t* e, const D3DMATRIX& proj, float* delta)
    {
        uint32_t flags = 0;
        float bias = 0.0f, z = 0.0f;
        std::memcpy(&flags, e + Flags, 4);
        std::memcpy(&bias, e + Bias, 4);
        std::memcpy(&z, e + Depth, 4);
        // a perspective projection, w = _34 * z: the view looks down -z (_34 = -1), so the distance d is w
        // Bit 27 (a bias in raw depth units) is left alone. Southern San d'Oria has two such elements, translucent
        // layers 150-230 yalms out with -15 (30000 PS2 depth units): moved that much they come in front of the town's
        // walls, in the doorways. On the PS2 the same element looks the same at a bias of 0, -15 or -150.
        if ((flags >> 22 & 1) != 0 || (flags >> 27 & 1) != 0 || bias == 0.0f || proj._34 == 0.0f || proj._14 != 0.0f ||
            proj._24 != 0.0f || proj._44 != 0.0f)
            return false;
        const float d = proj._34 * z;
        if (!(d > 0.01f))
            return false;
        if (!(d - bias > 0.01f))
            return false;
        const float inv = 1.0f / d - 1.0f / (d - bias); // change in 1/d
        *delta = proj._43 * inv; // z_ndc = _33 / _34 + _43 / w, w = d
        return *delta != 0.0f;
    }

    // The element whose draws come next: its fields as the setup left them (the draws follow the setup's return).
    bool s_Pending = false;
    uint8_t s_Fields[Depth + 4];

    uint32_t __fastcall SetupHook(void* element, void* edx, uint32_t arg)
    {
        const uint32_t r = reinterpret_cast<SetupFn>(s_Setup.tramp)(element, edx, arg);
        s_Pending = element != nullptr;
        if (s_Pending)
            std::memcpy(s_Fields, static_cast<const uint8_t*>(element) + 0x10C, sizeof(s_Fields));
        return r;
    }

    // An element draw method: no bias until it calls the setup, and the caller's (an element drawing its children)
    // back when it returns.
    uint32_t Draw(const Hook& h, void* element, void* edx)
    {
        const bool pending = s_Pending;
        uint8_t fields[sizeof(s_Fields)];
        std::memcpy(fields, s_Fields, sizeof(fields));
        s_Pending        = false;
        const uint32_t r = reinterpret_cast<DrawFn>(h.tramp)(element, edx);
        s_Pending        = pending;
        std::memcpy(s_Fields, fields, sizeof(fields));
        return r;
    }

    uint32_t __fastcall DrawAHook(void* element, void* edx)
    {
        return Draw(s_DrawA, element, edx);
    }

    uint32_t __fastcall DrawBHook(void* element, void* edx)
    {
        return Draw(s_DrawB, element, edx);
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

    uint8_t* Find(const char* entry, const size_t n)
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
            const uint8_t* end = p + section[s].Misc.VirtualSize - n;
            for (; p < end; ++p)
                if (std::memcmp(p, entry, n) == 0)
                    return const_cast<uint8_t*>(p);
        }
        return nullptr;
    }

    void Unhook(Hook& h)
    {
        if (h.at != nullptr)
            Poke(h.at, h.orig, h.moved);
        h.at = nullptr;
    }

    // False: not found, or couldn't be patched.
    bool Install(Hook& h)
    {
        uint8_t* at = Find(h.entry, h.length);
        if (at == nullptr)
            return false;
        if (h.tramp == nullptr)
        {
            // moved bytes + jmp rel32 back; stays allocated (a draw may be inside it when the hook is removed)
            h.tramp = static_cast<uint8_t*>(::VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
            if (h.tramp == nullptr)
                return false;
        }
        std::memcpy(h.tramp, at, h.moved);
        h.tramp[h.moved] = 0xE9;
        const int32_t back = static_cast<int32_t>(reinterpret_cast<intptr_t>(at + h.moved) - reinterpret_cast<intptr_t>(h.tramp + h.moved + 5));
        std::memcpy(h.tramp + h.moved + 1, &back, 4);
        ::FlushInstructionCache(::GetCurrentProcess(), h.tramp, 16);
        uint8_t jmp[sizeof(h.orig)];
        std::memset(jmp, 0x90, sizeof(jmp));
        jmp[0]         = 0xE9;
        const int32_t to = static_cast<int32_t>(reinterpret_cast<intptr_t>(h.to) - reinterpret_cast<intptr_t>(at + 5));
        std::memcpy(jmp + 1, &to, 4);
        std::memcpy(h.orig, at, h.moved);
        if (!Poke(at, jmp, h.moved))
            return false;
        h.at = at;
        return true;
    }
}

// An element's draws end with the frame at the latest.
void Fixes::DepthPresent(void)
{
    s_Pending = false;
}

// Before a game draw: if it's a 3D draw of an element with a depth bias, set the projection with its depth moved and
// keep the game's in *saved for DepthEnd. False: draw as it is.
bool Fixes::DepthBegin(D3DMATRIX* saved)
{
    if (!m_Depth || !s_Pending || m_Device == nullptr)
        return false;
    DWORD vs = 0;
    m_Device->GetVertexShader(&vs);
    if ((vs & 0xFFFF0000) != 0 || (vs & D3DFVF_POSITION_MASK) != D3DFVF_XYZ) // fixed-function, world-space vertices
        return false;
    if (FAILED(m_Device->GetTransform(D3DTS_PROJECTION, saved)))
        return false;
    float delta = 0.0f;
    if (!BiasFor(s_Fields, *saved, &delta))
        return false;
    D3DMATRIX m = *saved;
    m._13 += delta * m._14, m._23 += delta * m._24, m._33 += delta * m._34, m._43 += delta * m._44;
    m_Device->SetTransform(D3DTS_PROJECTION, &m);
    DebugLog::Once("depth", "depth: first biased draw, z shift %.6f", delta);
    return true;
}

void Fixes::DepthEnd(const D3DMATRIX& saved)
{
    m_Device->SetTransform(D3DTS_PROJECTION, &saved);
}

const char* Fixes::SetDepth(const bool on)
{
    if (on == m_Depth)
        return nullptr;
    if (!on)
    {
        Unhook(s_Setup);
        Unhook(s_DrawA);
        Unhook(s_DrawB);
        s_Pending = false;
        m_Depth   = false;
        PF_DEBUG("depth: off");
        return nullptr;
    }
    // The draw methods first: once the setup is hooked, the bias must end with them.
    if (!Install(s_DrawA) || !Install(s_DrawB) || !Install(s_Setup))
    {
        Unhook(s_Setup);
        Unhook(s_DrawA);
        Unhook(s_DrawB);
        return "the effect draw code wasn't found (or another plugin hooked it)";
    }
    s_Pending = false;
    m_Depth   = true;
    PF_DEBUG("depth: on, hooked at %p, %p, %p", static_cast<void*>(s_Setup.at), static_cast<void*>(s_DrawA.at), static_cast<void*>(s_DrawB.at));
    return nullptr;
}
