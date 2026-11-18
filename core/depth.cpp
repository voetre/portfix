// Effect depth bias. A particle element can carry a depth bias (op 0x30 in its generator; Carbuncle's glow discs
// have -0.7). On the PS2 the element is depth tested as if that much nearer the camera: its depth buffer value is
// raised by Z(d) - Z(d - bias) for its view depth d, so a summon's glow lies over its whole body. The PC keeps the
// value but only sorts by it, so the glow is tested at its true place, inside the model, and the near half of the
// body hides it. The fix hooks the element's draw setup and, for an element with a bias, shifts the projection's
// depth by the same amount around that element's own draws (the game sets the projection again between the setup
// and the draw, so it is applied in the draw hooks). TECHNICAL.md, "Effect depth".
#include "fixes.h"
#include "debuglog.h"

#include <cstdint>
#include <cstring>

namespace
{
    // The element's draw setup (FUN_052d67e0 in the 2026 client), __fastcall with the element in ECX, RET 4:
    //   sub esp,0D8h / cmp word [esp+0DCh],0FFFFh / push ebx / push ebp / mov ebp,ecx / push esi / mov [esp+..],ebp
    const char Entry[]     = "\x81\xEC\xD8\x00\x00\x00\x66\x81\xBC\x24\xDC\x00\x00\x00\xFF\xFF\x53\x55\x8B\xE9\x56\x89\x6C\x24";
    constexpr size_t Moved = 6; // sub esp,0D8h: moved to the trampoline whole

    using SetupFn = uint32_t(__fastcall*)(void* element, void* edx, uint32_t arg);

    uint8_t* s_Entry      = nullptr;  // the setup, while hooked
    uint8_t s_Orig[Moved] = {};
    uint8_t* s_Tramp      = nullptr;  // the moved bytes, then a jmp back

    // The element's fields (PC layout, the PS2's in brackets): flags +10Ch (+B4h), bias +128h (+E4h), view depth +13Ch (+E8h).
    // A copy from +10Ch is kept for the draws, so these are offsets into it.
    constexpr size_t Flags = 0x10C - 0x10C, Bias = 0x128 - 0x10C, Depth = 0x13C - 0x10C;
    // Flag bit 22: no bias. Bit 27: the bias is in PS2 depth units, x -2000. Otherwise yalms toward the camera.
    constexpr float PS2DepthScale = 1677721.6f; // the PS2 projection's depth: Z = 1677721.6 / d - 25.5

    // How far to move the draw's depth in NDC: the PS2's change in Z, which is linear in 1/d as D3D's depth is.
    bool BiasFor(const uint8_t* e, const D3DMATRIX& proj, float* delta)
    {
        uint32_t flags = 0;
        float bias = 0.0f, z = 0.0f;
        std::memcpy(&flags, e + Flags, 4);
        std::memcpy(&bias, e + Bias, 4);
        std::memcpy(&z, e + Depth, 4);
        // a perspective projection, w = _34 * z: the view looks down -z (_34 = -1), so the distance d is w
        if ((flags >> 22 & 1) != 0 || bias == 0.0f || proj._34 == 0.0f || proj._14 != 0.0f || proj._24 != 0.0f || proj._44 != 0.0f)
            return false;
        const float d = proj._34 * z;
        if (!(d > 0.01f))
            return false;
        float inv = 0.0f; // change in 1/d
        if ((flags >> 27 & 1) != 0)
            inv = -bias * 2000.0f / PS2DepthScale;
        else if (d - bias > 0.01f)
            inv = 1.0f / d - 1.0f / (d - bias);
        else
            return false;
        *delta = proj._43 * inv; // z_ndc = _33 / _34 + _43 / w, w = d
        return *delta != 0.0f;
    }

    // The element whose draws come next: its fields as the setup left them (the draws follow the setup's return).
    bool s_Pending = false;
    uint8_t s_Fields[Depth + 4];

    uint32_t __fastcall SetupHook(void* element, void* edx, uint32_t arg)
    {
        const uint32_t r = reinterpret_cast<SetupFn>(s_Tramp)(element, edx, arg);
        s_Pending = element != nullptr;
        if (s_Pending)
            std::memcpy(s_Fields, static_cast<const uint8_t*>(element) + 0x10C, sizeof(s_Fields));
        return r;
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
                if (std::memcmp(p, Entry, n) == 0)
                    return const_cast<uint8_t*>(p);
        }
        return nullptr;
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
        if (s_Entry != nullptr)
            Poke(s_Entry, s_Orig, Moved);
        s_Entry   = nullptr;
        s_Pending = false;
        m_Depth   = false;
        PF_DEBUG("depth: off");
        return nullptr;
    }
    uint8_t* at = Find();
    if (at == nullptr)
        return "the effect draw setup wasn't found (or another plugin hooked it)";
    if (s_Tramp == nullptr)
    {
        // moved bytes + jmp rel32 back; stays allocated (a draw may be inside it when the hook is removed)
        s_Tramp = static_cast<uint8_t*>(::VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (s_Tramp == nullptr)
            return "couldn't allocate the hook";
    }
    std::memcpy(s_Tramp, at, Moved);
    s_Tramp[Moved] = 0xE9;
    const int32_t back = static_cast<int32_t>(reinterpret_cast<intptr_t>(at + Moved) - reinterpret_cast<intptr_t>(s_Tramp + Moved + 5));
    std::memcpy(s_Tramp + Moved + 1, &back, 4);
    ::FlushInstructionCache(::GetCurrentProcess(), s_Tramp, 16);
    uint8_t jmp[Moved] = {0xE9, 0, 0, 0, 0, 0x90};
    const int32_t to = static_cast<int32_t>(reinterpret_cast<intptr_t>(&SetupHook) - reinterpret_cast<intptr_t>(at + 5));
    std::memcpy(jmp + 1, &to, 4);
    std::memcpy(s_Orig, at, Moved);
    if (!Poke(at, jmp, Moved))
        return "couldn't patch the effect draw setup";
    s_Entry = at;
    m_Depth = true;
    PF_DEBUG("depth: on, hooked at %p", static_cast<void*>(at));
    return nullptr;
}
